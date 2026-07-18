module;

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Support/raw_ostream.h>


module scpp.compiler.codegen:layout;

import :api;

namespace scpp {

[[nodiscard]] bool Codegen::is_scalar_type_name(const std::string& name)
{
    static const std::unordered_set<std::string> scalar_names = {
        "bool", "char", "int", "long", "unsigned int", "unsigned long", "int8_t", "int16_t", "int32_t",
        "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "float", "double", "float32_t", "float64_t",
        "size_t", "ptrdiff_t"};
    return scalar_names.contains(name);
}


[[nodiscard]] const EnumDef* Codegen::find_enum_def(const Program* program, const std::string& name)
{
    if (program == nullptr) return nullptr;
    for (const EnumDef& def : program->enums) {
        if (def.name == name) return &def;
    }
    return nullptr;
}


[[nodiscard]] const EnumVariant* Codegen::find_enum_variant(const Program* program, const std::string& name,
                                                   const EnumDef** owning_enum)
{
    if (program == nullptr) return nullptr;
    for (const EnumDef& def : program->enums) {
        for (const EnumVariant& variant : def.variants) {
            if (variant.name == name) {
                if (owning_enum != nullptr) *owning_enum = &def;
                return &variant;
            }
        }
    }
    return nullptr;
}


    [[nodiscard]] TargetLayoutInfo Codegen::current_target_layout_info() const
{
        return TargetLayoutInfo{module_->getDataLayout().getPointerSize(),
                                module_->getDataLayout().getPointerABIAlignment(0).value()};
    }


    [[nodiscard]] size_t Codegen::align_up(size_t value, size_t alignment)
{
        if (alignment <= 1) return value;
        return ((value + alignment - 1) / alignment) * alignment;
    }


    [[nodiscard]] size_t Codegen::alignment_bytes_for_type(const Type& type) const
{
        if (program_ != nullptr) {
            std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, type, current_target_layout_info());
            if (layout.has_value()) return layout->abi_align_bytes;
        }
        if (type.kind == TypeKind::Named) {
            auto it = structs_.find(type.name);
            if (it != structs_.end()) return it->second.abi_align.value();
        }
        llvm::Type* llvm_type = const_cast<Codegen*>(this)->to_llvm_type(type);
        return module_->getDataLayout().getABITypeAlign(llvm_type).value();
    }


    llvm::Value* Codegen::codegen_sizeof_value(const Expr& expr)
{
        Type queried_type;
        if (expr.sizeof_operand_is_type) {
            queried_type = expr.type;
        } else {
            std::optional<Type> inferred = infer_type(*expr.lhs);
            if (!inferred.has_value()) {
                throw CodegenError("cannot apply 'sizeof' to this expression: its type could not be inferred", current_loc_);
            }
            queried_type = *inferred;
        }
        if (program_ == nullptr) throw CodegenError("internal error: sizeof requires program type information", current_loc_);
        std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, queried_type, current_target_layout_info());
        if (!layout.has_value()) {
            throw CodegenError("cannot apply 'sizeof' to this type in this version", current_loc_);
        }
        return llvm::ConstantInt::get(to_llvm_type(named_type("size_t")), layout->size_bytes, /*isSigned=*/false);
    }


    llvm::Value* Codegen::codegen_alignof_value(const Expr& expr)
{
        if (program_ == nullptr) throw CodegenError("internal error: alignof requires program type information", current_loc_);
        std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, expr.type, current_target_layout_info());
        if (!layout.has_value()) {
            throw CodegenError("cannot apply 'alignof' to this type in this version", current_loc_);
        }
        return llvm::ConstantInt::get(to_llvm_type(named_type("size_t")), layout->abi_align_bytes, /*isSigned=*/false);
    }


    void Codegen::validate_trivial(const Type& type, std::vector<std::string>& in_progress)
{
        switch (type.kind) {
            case TypeKind::Pointer:
            case TypeKind::FunctionPointer:
                return;
            case TypeKind::Function:
                throw CodegenError("a bare function type cannot be a struct field in this version", current_loc_);
            case TypeKind::Reference:
                throw CodegenError("a reference cannot be a struct field in this version",
                    current_loc_);
            case TypeKind::Span:
                throw CodegenError("a std::span cannot be a struct field in this version (it is a "
                                    "lifetime-checked borrowed view; use class instead)",
                    current_loc_);
            case TypeKind::Array:
                validate_trivial(*type.element, in_progress);
                return;
            case TypeKind::Named: {
                if (is_scalar_type_name(type.name)) return;
                if (find_enum_def(program_, type.name) != nullptr) return;
                if (type.name == "void") {
                    throw CodegenError("'void' cannot be a struct field (only a return type or a "
                                        "pointer's pointee -- 'void*' -- may be 'void')",
                        current_loc_);
                }
                if (find_class_def(type.name) != nullptr) {
                    throw CodegenError("a class type '" + type.name + "' cannot be a struct field; use class instead",
                        current_loc_);
                }
                const StructDef* def = find_struct_def(type.name);
                if (def == nullptr) {
                    throw CodegenError("unknown type '" + type.name + "'",
                        current_loc_);
                }
                if (std::find(in_progress.begin(), in_progress.end(), type.name) != in_progress.end()) {
                    throw CodegenError("struct '" + type.name + "' cannot contain itself by value "
                                                                 "(did you mean a pointer '" +
                                        type.name + "*'?)",
                        current_loc_);
                }
                in_progress.push_back(type.name);
                for (const StructField& field : def->fields) {
                    validate_trivial(field.type, in_progress);
                }
                in_progress.pop_back();
                return;
            }
        }
    }


    void Codegen::declare_struct(const StructDef& def)
{
        if (declaring_aggregates_.contains(def.name)) return;
        declaring_aggregates_.insert(def.name);
        StructInfo info;
        info.is_union = def.is_union;
        info.is_packed = def.is_packed;
        std::vector<std::string> in_progress;
        std::vector<llvm::Type*> llvm_field_types;
        llvm_field_types.reserve(def.fields.size() * 2);
        if (!def.is_union) {
            size_t offset = 0;
            size_t overall_align = std::max<size_t>(1, def.resolved_alignment == 0 ? 1 : static_cast<size_t>(def.resolved_alignment));
            for (const StructField& field : def.fields) {
                try {
                    validate_trivial(field.type, in_progress);
                } catch (const CodegenError& e) {
                    throw CodegenError(std::string(def.is_union ? "union '" : "struct '") + def.name + "' field '" +
                                            field.name + "': " + e.what() +
                                            " (only scalars, pointers, trivial structs/unions, and fixed-size arrays "
                                            "of trivial types are allowed here; see spec ch04)",
                        current_loc_);
                }
                llvm::Type* field_type = to_llvm_type(field.type);
                size_t field_size = module_->getDataLayout().getTypeAllocSize(field_type);
                size_t field_align = def.is_packed ? 1
                                                   : std::max(alignment_bytes_for_type(field.type),
                                                              static_cast<size_t>(field.resolved_alignment));
                size_t aligned_offset = align_up(offset, field_align);
                if (aligned_offset > offset) {
                    llvm_field_types.push_back(
                        llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), aligned_offset - offset));
                    offset = aligned_offset;
                }
                info.field_names.push_back(field.name);
                info.field_types.push_back(field.type);
                info.field_alignments.push_back(llvm::Align(field_align));
                info.field_physical_indices.push_back(llvm_field_types.size());
                llvm_field_types.push_back(field_type);
                offset += field_size;
                overall_align = def.is_packed ? 1 : std::max(overall_align, field_align);
            }
            size_t final_align = def.is_packed ? 1 : overall_align;
            size_t final_size = align_up(offset, final_align);
            if (final_size > offset) {
                llvm_field_types.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), final_size - offset));
            }
            info.llvm_type = llvm::StructType::create(*context_, llvm_field_types, "struct." + def.name, def.is_packed);
            info.abi_align = llvm::Align(final_align);
        } else {
            for (const StructField& field : def.fields) {
                try {
                    validate_trivial(field.type, in_progress);
                } catch (const CodegenError& e) {
                    throw CodegenError(std::string(def.is_union ? "union '" : "struct '") + def.name + "' field '" +
                                            field.name + "': " + e.what() +
                                            " (only scalars, pointers, trivial structs/unions, and fixed-size arrays "
                                            "of trivial types are allowed here; see spec ch04)",
                        current_loc_);
                }
                info.field_names.push_back(field.name);
                info.field_types.push_back(field.type);
                info.field_alignments.push_back(
                    llvm::Align(def.is_packed ? 1
                                              : std::max(alignment_bytes_for_type(field.type),
                                                         static_cast<size_t>(field.resolved_alignment))));
                info.field_physical_indices.push_back(0);
                llvm_field_types.push_back(to_llvm_type(field.type));
            }
            if (llvm_field_types.empty()) {
                throw CodegenError("union '" + def.name + "' must declare at least one field",
                    current_loc_);
            }
            size_t align_value = def.is_packed ? 1 : std::max<size_t>(1, def.resolved_alignment);
            size_t max_size = 0;
            size_t max_rep_align = 0;
            llvm::Type* rep_type = llvm_field_types[0];
            size_t rep_size = module_->getDataLayout().getTypeAllocSize(rep_type);
            for (size_t i = 0; i < llvm_field_types.size(); ++i) {
                llvm::Type* field_type = llvm_field_types[i];
                size_t field_size = module_->getDataLayout().getTypeAllocSize(field_type);
                size_t field_align = info.field_alignments[i].value();
                if (field_size > max_size) max_size = field_size;
                if (field_align > align_value) align_value = field_align;
                if (field_align > max_rep_align ||
                    (field_align == max_rep_align && field_size > rep_size)) {
                    rep_type = field_type;
                    rep_size = field_size;
                    max_rep_align = field_align;
                }
            }
            if (align_value == 0) align_value = 1;
            size_t union_size = ((max_size + align_value - 1) / align_value) * align_value;
            std::vector<llvm::Type*> storage_fields;
            storage_fields.push_back(rep_type);
            if (union_size > rep_size) {
                storage_fields.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), union_size - rep_size));
            }
            info.llvm_type = llvm::StructType::create(*context_, storage_fields, "union." + def.name, def.is_packed);
            info.abi_align = llvm::Align(align_value);
        }
        structs_[def.name] = std::move(info);
        declaring_aggregates_.erase(def.name);
    }


    void Codegen::declare_class(const ClassDef& def)
{
        if (declaring_aggregates_.contains(def.name)) return;
        declaring_aggregates_.insert(def.name);
        StructInfo info;
        info.has_ordinary_vtable = !def.is_interface;
        std::vector<llvm::Type*> llvm_field_types;
        size_t offset = 0;
        size_t overall_align = std::max<size_t>(1, def.resolved_alignment == 0 ? 1 : static_cast<size_t>(def.resolved_alignment));
        if (const BaseSpecifier* base = def.direct_ordinary_base()) {
            const StructInfo& base_info = structs_.at(base->base_type.name);
            info.field_names = base_info.field_names;
            info.field_types = base_info.field_types;
            info.field_alignments = base_info.field_alignments;
            info.field_physical_indices = base_info.field_physical_indices;
            llvm_field_types.assign(base_info.llvm_type->elements().begin(), base_info.llvm_type->elements().end());
            offset = module_->getDataLayout().getTypeAllocSize(base_info.llvm_type);
            overall_align = std::max(overall_align, static_cast<size_t>(base_info.abi_align.value()));
        }
        if (info.has_ordinary_vtable && llvm_field_types.empty()) {
            llvm_field_types.push_back(llvm::PointerType::getUnqual(*context_));
            offset = module_->getDataLayout().getTypeAllocSize(llvm_field_types.back());
            overall_align = std::max(overall_align,
                                     static_cast<size_t>(module_->getDataLayout().getPointerABIAlignment(0).value()));
        }
        for (const ClassField& field : def.fields) {
            llvm::Type* field_type = to_llvm_type(field.type);
            size_t field_size = module_->getDataLayout().getTypeAllocSize(field_type);
            size_t field_align = std::max(alignment_bytes_for_type(field.type), static_cast<size_t>(field.resolved_alignment));
            size_t aligned_offset = align_up(offset, field_align);
            if (aligned_offset > offset) {
                llvm_field_types.push_back(
                    llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), aligned_offset - offset));
                offset = aligned_offset;
            }
            info.field_names.push_back(field.name);
            info.field_types.push_back(field.type);
            info.field_alignments.push_back(llvm::Align(field_align));
            info.field_physical_indices.push_back(llvm_field_types.size());
            llvm_field_types.push_back(field_type);
            offset += field_size;
            overall_align = std::max(overall_align, field_align);
        }
        size_t final_size = align_up(offset, overall_align);
        if (final_size > offset) {
            llvm_field_types.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), final_size - offset));
        }
        info.llvm_type = llvm::StructType::create(*context_, llvm_field_types, "class." + def.name);
        info.abi_align = llvm::Align(overall_align);
        structs_[def.name] = std::move(info);
        declaring_aggregates_.erase(def.name);
    }


    llvm::Type* Codegen::to_llvm_type(const Type& type)
{
        switch (type.kind) {
            case TypeKind::Function:
                throw CodegenError("a bare function type cannot be lowered directly; only function pointers are runtime values",
                                   current_loc_);
            case TypeKind::Pointer:
            case TypeKind::Reference:
                if (is_interface_representation_type(type)) {
                    return interface_representation_type();
                }
                // A unique_ptr is just a possibly-null owning pointer at the
                // ABI/codegen level in v0.1: no destructor/drop codegen
                // exists yet (that's M3's "drop insertion"), so it lowers
                // identically to a raw pointer. The move checker is what
                // enforces the ownership discipline on top of this.
                // A reference is likewise ABI-identical to a pointer (same
                // as Clang lowers C++ references): the frontend is what
                // makes it auto-dereference on every use (see
                // codegen_lvalue's Identifier case) and enforces borrow
                // discipline (scpp.movecheck), not the IR shape itself.
                return llvm::PointerType::getUnqual(*context_);
            case TypeKind::FunctionPointer: {
                std::vector<llvm::Type*> params;
                params.reserve(type.function_params.size());
                for (const Type& param : type.function_params) params.push_back(to_llvm_type(param));
                return llvm::PointerType::get(*context_, 0);
            }
            case TypeKind::Span:
                // A non-owning {data pointer, element count} pair -- a
                // literal (unnamed) two-word LLVM struct, not registered
                // in `structs_` (span isn't a user-visible aggregate the
                // way a `struct` is; Member/Subscript access to it is
                // special-cased directly on TypeKind::Span in
                // codegen_lvalue, not routed through StructInfo). LLVM
                // deduplicates identical literal struct types itself, so
                // there's no need to cache this beyond calling
                // StructType::get each time.
                return llvm::StructType::get(*context_,
                                              {llvm::PointerType::getUnqual(*context_), llvm::Type::getInt64Ty(*context_)});
            case TypeKind::Array:
                return llvm::ArrayType::get(to_llvm_type(*type.element), type.array_size);
            case TypeKind::Named:
                if (type.name == "int") return llvm::Type::getInt32Ty(*context_);
                // A full byte (i8), matching real C++'s sizeof(bool)==1
                // and the spec's false=0/true=1 invariant (ch06) -- not
                // i1, even though LLVM's own icmp/br/select instructions
                // require i1. See bool_to_i1/i1_to_bool below for the
                // narrow choke point that bridges the two: every
                // consumer that needs a branch/select condition truncates
                // down to i1 first, and every comparison/logical result
                // gets zext'd back up to i8 before being stored, passed,
                // or returned as an ordinary bool value.
                if (type.name == "bool") return llvm::Type::getInt8Ty(*context_);
                // A signed 8-bit scalar, matching the common (e.g.
                // x86-64 Linux/Clang) default for plain `char` -- no
                // implicit promotion to/from `int` exists yet (matching
                // the same pre-existing lack of promotion between `bool`
                // and `int`), so this is the type's only representation.
                if (type.name == "char") return llvm::Type::getInt8Ty(*context_);
                // ch06 §6: the rest of the numeric family -- LLVM natively
                // supports arbitrary-width integers, so every fixed-width
                // signed/unsigned pair just maps to the same-width
                // integer type (LLVM itself draws no signed/unsigned
                // distinction at the type level, only at the
                // instruction level -- see is_unsigned_scalar_type/
                // codegen_checked_arith's own signedness dispatch).
                // `long`/`unsigned long` are always 64-bit regardless of
                // target (ch06's own deliberate anti-LP64/LLP64-pitfall
                // fix), unlike `size_t`/`ptrdiff_t` below, which are
                // meant to track the pointer width.
                if (type.name == "int8_t" || type.name == "uint8_t") return llvm::Type::getInt8Ty(*context_);
                if (type.name == "int16_t" || type.name == "uint16_t") return llvm::Type::getInt16Ty(*context_);
                if (type.name == "int32_t" || type.name == "uint32_t" || type.name == "unsigned int") {
                    return llvm::Type::getInt32Ty(*context_);
                }
                if (type.name == "int64_t" || type.name == "uint64_t" || type.name == "long" ||
                    type.name == "unsigned long") {
                    return llvm::Type::getInt64Ty(*context_);
                }
                if (type.name == "float" || type.name == "float32_t") return llvm::Type::getFloatTy(*context_);
                if (type.name == "double" || type.name == "float64_t") return llvm::Type::getDoubleTy(*context_);
                if (type.name == "size_t" || type.name == "ptrdiff_t") {
                    return llvm::Type::getIntNTy(*context_, module_->getDataLayout().getPointerSizeInBits());
                }
                if (const EnumDef* enum_def = find_enum_def(program_, type.name)) {
                    return to_llvm_type(enum_def->underlying_type);
                }
                // `void` (ch02 §2.1): only meaningful as a function return
                // type or a pointer's pointee (`void*`, whose own
                // to_llvm_type case above never even inspects the
                // pointee, so nothing further is needed there). Callers
                // that would put it somewhere nonsensical (a bare local,
                // parameter, struct field, or array element) reject it
                // *before* reaching here -- see declare_function's
                // parameter loop and codegen_stmt's VarDecl case -- so
                // this is never actually asked to allocate storage for a
                // void value.
                if (type.name == "void") return llvm::Type::getVoidTy(*context_);
                {
                    auto it = structs_.find(type.name);
                    if (it != structs_.end()) return it->second.llvm_type;
                }
                if (!declaring_aggregates_.contains(type.name) && program_ != nullptr) {
                    for (const StructDef& def : program_->structs) {
                        if (def.name == type.name && def.template_params.empty()) {
                            declare_struct(def);
                            auto it = structs_.find(type.name);
                            if (it != structs_.end()) return it->second.llvm_type;
                        }
                    }
                    for (const ClassDef& def : program_->classes) {
                        if (def.name == type.name && def.template_params.empty() && !def.is_synthetic_check_only &&
                            !def.is_concept_witness) {
                            declare_class(def);
                            auto it = structs_.find(type.name);
                            if (it != structs_.end()) return it->second.llvm_type;
                        }
                    }
                }
                throw CodegenError("unsupported type '" + type.name + "'",
                    current_loc_);
        }
        throw CodegenError("unhandled type kind",
            current_loc_);
    }


    [[nodiscard]] std::optional<llvm::Align> Codegen::alignment_for_type(const Type& type) const
{
        if (program_ != nullptr) {
            std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, type, current_target_layout_info());
            if (layout.has_value()) return llvm::Align(layout->abi_align_bytes);
        }
        if (type.kind != TypeKind::Named) return std::nullopt;
        auto it = structs_.find(type.name);
        if (it == structs_.end()) return std::nullopt;
        return it->second.abi_align;
    }


    llvm::LoadInst* Codegen::create_load(llvm::Type* type, llvm::Value* ptr, std::optional<llvm::Align> alignment,
                                const llvm::Twine& name)
{
        llvm::LoadInst* load = builder_->CreateLoad(type, ptr, name);
        if (alignment.has_value()) load->setAlignment(*alignment);
        return load;
    }


    llvm::StoreInst* Codegen::create_store(llvm::Value* value, llvm::Value* ptr, std::optional<llvm::Align> alignment)
{
        llvm::StoreInst* store = builder_->CreateStore(value, ptr);
        if (alignment.has_value()) store->setAlignment(*alignment);
        return store;
    }


    void Codegen::zero_initialize_storage(llvm::Value* ptr, const Type& type, std::optional<llvm::Align> alignment)
{
        llvm::Type* llvm_type = to_llvm_type(type);
        switch (type.kind) {
            case TypeKind::Named:
            case TypeKind::Array:
            case TypeKind::Span: {
                llvm::TypeSize size = module_->getDataLayout().getTypeAllocSize(llvm_type);
                builder_->CreateMemSet(ptr,
                                       llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 0),
                                       size,
                                       alignment.value_or(llvm::Align(1)));
                if (type.kind == TypeKind::Named && class_has_ordinary_vtable(type.name)) {
                    initialize_ordinary_vtable_pointer(type.name, ptr);
                }
                return;
            }
            default:
                create_store(llvm::Constant::getNullValue(llvm_type), ptr, alignment);
                return;
        }
    }


    void Codegen::validate_reference_pointee(const Type& pointee)
{
        if (pointee.kind == TypeKind::Reference) {
            throw CodegenError("a reference to a reference is not supported",
                current_loc_);
        }
    }


    void Codegen::validate_reference_return_elision(const Function& fn)
{
        if (fn.return_lifetime.present()) return;
        // ch04 §4.2/ch05 §5.9/spec §6.5: `this` (always params[0] when
        // present) is always the elision source when the function is a
        // method, regardless of how many other reference parameters it
        // also takes -- see movecheck's resolve_elided_param_index for
        // the full rationale (this is its structural codegen-side
        // counterpart, kept in sync).
        if (!fn.params.empty() && fn.params[0].name == "this" && fn.params[0].type.kind == TypeKind::Reference) {
            if (fn.return_type.is_mutable_ref && !fn.params[0].type.is_mutable_ref) {
                throw CodegenError("function '" + fn.name +
                                    "' returns a mutable reference ('T&') but its 'this' is a read-only "
                                    "('const') receiver",
                    current_loc_);
            }
            return;
        }
        const Param* found = nullptr;
        for (const Param& param : fn.params) {
            // ch03/ch05 §5.11: exclude a `T&&` parameter -- see
            // movecheck's resolve_elided_param_index (this function's
            // structural counterpart) for why it's never a sound elision
            // source.
            if (param.type.kind != TypeKind::Reference || param.type.is_rvalue_ref) continue;
            if (found != nullptr) {
                throw CodegenError("function '" + fn.name +
                                    "' returns a reference but has more than one reference parameter; scpp "
                                    "v0.1 can only infer a returned reference's lifetime when there is exactly "
                                    "one (spec ch05.3)",
                    current_loc_);
            }
            found = &param;
        }
        if (found == nullptr) {
            throw CodegenError("function '" + fn.name +
                                "' returns a reference but has no reference parameter to infer its lifetime "
                                "from (spec ch05.3)",
                current_loc_);
        }
        if (fn.return_type.is_mutable_ref && !found->type.is_mutable_ref) {
            throw CodegenError("function '" + fn.name +
                                "' returns a mutable reference ('T&') but its sole reference parameter '" +
                                found->name + "' is a shared reference ('const T&')",
                current_loc_);
        }
    }


    [[nodiscard]] bool Codegen::is_bare_void(const Type& type)
{
        return type.kind == TypeKind::Named && type.name == "void";
    }


    void Codegen::validate_c_abi_compatible(const Type& type, const std::string& fn_name,
                                    const std::string& context_description)
{
        if (is_interface_representation_type(type)) {
            throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                " cannot be an interface-typed pointer or reference -- it has no defined C "
                                "representation (spec ch02 §2.1 / §11.3)",
                current_loc_);
        }
        switch (type.kind) {
            case TypeKind::Function:
                throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be a bare function type -- use a function pointer instead (spec ch02 §2.1)",
                    current_loc_);
            case TypeKind::Named: {
                if (find_class_def(type.name) != nullptr) {
                    throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                        " cannot be class type '" + type.name +
                                        "' -- class types have no defined C representation "
                                        "(spec ch02 §2.1)",
                        current_loc_);
                }
                if (is_scalar_type_name(type.name)) return;
            }
            case TypeKind::Pointer:
            case TypeKind::FunctionPointer:
            case TypeKind::Array:
                return;
            case TypeKind::Reference:
                throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be a reference -- it has no defined C representation (spec "
                                    "ch02 §2.1)",
                    current_loc_);
            case TypeKind::Span:
                throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be std::span -- it has no defined C representation (spec "
                                    "ch02 §2.1)",
                    current_loc_);
        }
    }

} // namespace scpp
