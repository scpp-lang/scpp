module;

// Official llvm::LLVM-C (llvm-c/*.h) is itself already a stable, extern "C"
// interface -- the DataLayout and type-construction operations in this
// file go through module `llvm`'s own `:target` partition's functions
// directly below (Core.h's own functions come from its `:core` partition
// instead) rather than any native llvm::LLVM C++ header, all reached via the
// single `import llvm;` below (module `llvm` re-exports every partition,
// see libs/llvm/llvm.cpp). See libs/README.md for why this project binds
// straight to llvm::LLVM-C wherever it already covers what's needed --
// including pointer ABI alignment (see pointer_abi_alignment_for_as
// below): a rigorous, function-by-function empirical audit found llvm::LLVM-C
// fully covers every llvm::LLVM operation this project's codegen needs, so
// there is no custom wrapper of any kind here.

module scpp.compiler.codegen:layout;

import std;
import llvm;
import :api;

namespace scpp {

namespace {

llvm::LLVMTargetDataRef data_layout_ref(llvm::LLVMModuleRef module) { return llvm::LLVMGetModuleDataLayout(module); }

// llvm::DataLayout::getPointerABIAlignment(address_space).value() has no
// function in llvm-c/Target.h with this exact shape (a data layout plus a
// bare address space), but llvm::LLVMABIAlignmentOfType() queried against an
// opaque pointer type *for that same address space* reads the exact same
// per-address-space entry in the DataLayout's own pointer-alignment
// table -- empirically confirmed identical (via a standalone llvm::LLVM-C++ vs.
// llvm::LLVM-C comparison program) across several representative data layouts
// and address spaces, including a synthetic one with an unusual, non-
// default alignment. So this composes two already-official llvm::LLVM-C calls
// instead of needing any wrapper of our own.
unsigned pointer_abi_alignment_for_as(llvm::LLVMModuleRef module, unsigned address_space) {
    return llvm::LLVMABIAlignmentOfType(llvm::LLVMGetModuleDataLayout(module),
                                  llvm::LLVMPointerTypeInContext(llvm::LLVMGetModuleContext(module), address_space));
}

} // namespace

[[nodiscard]] bool Codegen::is_scalar_type_name(const std::string& name)
{
    return scpp::is_scalar_type_name(std::string_view{name});
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
        return TargetLayoutInfo{llvm::LLVMPointerSizeForAS(data_layout_ref(module_), 0),
                                pointer_abi_alignment_for_as(module_, 0)};
    }


    [[nodiscard]] std::size_t Codegen::align_up(std::size_t value, std::size_t alignment)
{
        if (alignment <= 1) return value;
        return ((value + alignment - 1) / alignment) * alignment;
    }


    [[nodiscard]] std::expected<std::size_t, CodegenError> Codegen::alignment_bytes_for_type(const Type& type) const
{
        if (program_ != nullptr) {
            std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, type, current_target_layout_info());
            if (layout.has_value()) return layout->abi_align_bytes;
        }
        if (type.kind == TypeKind::Named) {
            auto it = structs_.find(type.name);
            if (it != structs_.end()) return it->second.abi_align;
        }
        auto llvm_type_result = const_cast<Codegen*>(this)->to_llvm_type(type);
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        return llvm::LLVMABIAlignmentOfType(data_layout_ref(module_), std::move(llvm_type_result).value());
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_sizeof_value(const Expr& expr)
{
        Type queried_type;
        if (expr.sizeof_operand_is_type) {
            queried_type = expr.type;
        } else {
            std::optional<Type> inferred = infer_type(*expr.lhs);
            if (!inferred.has_value()) {
                return std::unexpected(CodegenError("cannot apply 'sizeof' to this expression: its type could not be inferred", current_loc_));
            }
            queried_type = *inferred;
        }
        if (program_ == nullptr) return std::unexpected(CodegenError("internal error: sizeof requires program type information", current_loc_));
        std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, queried_type, current_target_layout_info());
        if (!layout.has_value()) {
            return std::unexpected(CodegenError("cannot apply 'sizeof' to this type in this version", current_loc_));
        }
        auto size_t_type = to_llvm_type(named_type("size_t"));
        if (!size_t_type.has_value()) return std::unexpected(std::move(size_t_type).error());
        return llvm::LLVMConstInt(std::move(size_t_type).value(), layout->size_bytes, /*SignExtend=*/0);
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_alignof_value(const Expr& expr)
{
        if (program_ == nullptr) return std::unexpected(CodegenError("internal error: alignof requires program type information", current_loc_));
        std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, expr.type, current_target_layout_info());
        if (!layout.has_value()) {
            return std::unexpected(CodegenError("cannot apply 'alignof' to this type in this version", current_loc_));
        }
        auto size_t_type = to_llvm_type(named_type("size_t"));
        if (!size_t_type.has_value()) return std::unexpected(std::move(size_t_type).error());
        return llvm::LLVMConstInt(std::move(size_t_type).value(), layout->abi_align_bytes, /*SignExtend=*/0);
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::validate_trivial(const Type& type, std::vector<std::string>& in_progress)
{
        switch (type.kind) {
            case TypeKind::Pointer:
            case TypeKind::FunctionPointer:
                return {};
            case TypeKind::Function:
                return std::unexpected(CodegenError("a bare function type cannot be a struct field in this version", current_loc_));
            case TypeKind::Reference:
                return std::unexpected(CodegenError("a reference cannot be a struct field in this version",
                    current_loc_));
            case TypeKind::Span:
                return std::unexpected(CodegenError("a std::span cannot be a struct field in this version (it is a "
                                    "lifetime-checked borrowed view; use class instead)",
                    current_loc_));
            case TypeKind::Array:
                return validate_trivial(*type.element, in_progress);
            case TypeKind::Named: {
                if (is_scalar_type_name(type.name)) return {};
                if (find_enum_def(program_, type.name) != nullptr) return {};
                if (type.name == "void") {
                    return std::unexpected(CodegenError("'void' cannot be a struct field (only a return type or a "
                                        "pointer's pointee -- 'void*' -- may be 'void')",
                        current_loc_));
                }
                if (find_class_def(type.name) != nullptr) {
                    return std::unexpected(CodegenError("a class type '" + type.name + "' cannot be a struct field; use class instead",
                        current_loc_));
                }
                const StructDef* def = find_struct_def(type.name);
                if (def == nullptr) {
                    return std::unexpected(CodegenError("unknown type '" + type.name + "'",
                        current_loc_));
                }
                if (std::find(in_progress.begin(), in_progress.end(), type.name) != in_progress.end()) {
                    return std::unexpected(CodegenError("struct '" + type.name + "' cannot contain itself by value "
                                                                 "(did you mean a pointer '" +
                                        type.name + "*'?)",
                        current_loc_));
                }
                in_progress.push_back(type.name);
                for (const StructField& field : def->fields) {
                    if (auto result = validate_trivial(field.type, in_progress); !result.has_value()) {
                        return std::unexpected(std::move(result).error());
                    }
                }
                in_progress.pop_back();
                return {};
            }
        }
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::declare_struct(const StructDef& def)
{
        if (def.is_forward_declaration) {
            return std::unexpected(CodegenError("forward-declared struct '" + def.name + "' requires a full definition before layout is needed",
                current_loc_));
        }
        if (declaring_aggregates_.contains(def.name)) return {};
        declaring_aggregates_.insert(def.name);
        StructInfo info;
        info.is_union = def.is_union;
        info.is_packed = def.is_packed;
        std::vector<std::string> in_progress;
        std::vector<llvm::LLVMTypeRef> llvm_field_types;
        llvm_field_types.reserve(def.fields.size() * 2);
        if (!def.is_union) {
            std::size_t offset = 0;
            std::size_t overall_align = std::max<std::size_t>(1, def.resolved_alignment == 0 ? 1 : static_cast<std::size_t>(def.resolved_alignment));
            for (const StructField& field : def.fields) {
                if (auto validate_result = validate_trivial(field.type, in_progress); !validate_result.has_value()) {
                    return std::unexpected(CodegenError(std::string(def.is_union ? "union '" : "struct '") + def.name + "' field '" +
                                            field.name + "': " + validate_result.error().what() +
                                            " (only scalars, pointers, trivial structs/unions, and fixed-size arrays "
                                            "of trivial types are allowed here; see spec ch04)",
                        current_loc_));
                }
                auto field_type_result = to_llvm_type(field.type);
                if (!field_type_result.has_value()) return std::unexpected(std::move(field_type_result).error());
                llvm::LLVMTypeRef field_type = std::move(field_type_result).value();
                std::size_t field_size = llvm::LLVMABISizeOfType(data_layout_ref(module_), field_type);
                std::size_t field_align = 1;
                if (!def.is_packed) {
                    auto align_result = alignment_bytes_for_type(field.type);
                    if (!align_result.has_value()) return std::unexpected(std::move(align_result).error());
                    field_align = std::max(std::move(align_result).value(), static_cast<std::size_t>(field.resolved_alignment));
                }
                std::size_t aligned_offset = align_up(offset, field_align);
                if (aligned_offset > offset) {
                    llvm_field_types.push_back(
                        llvm::LLVMArrayType2(llvm::LLVMInt8TypeInContext(context_), aligned_offset - offset));
                    offset = aligned_offset;
                }
                info.field_names.push_back(field.name);
                info.field_types.push_back(field.type);
                info.field_alignments.push_back(static_cast<unsigned>(field_align));
                info.field_physical_indices.push_back(llvm_field_types.size());
                llvm_field_types.push_back(field_type);
                offset += field_size;
                overall_align = def.is_packed ? 1 : std::max(overall_align, field_align);
            }
            std::size_t final_align = def.is_packed ? 1 : overall_align;
            std::size_t final_size = align_up(offset, final_align);
            if (final_size > offset) {
                llvm_field_types.push_back(llvm::LLVMArrayType2(llvm::LLVMInt8TypeInContext(context_), final_size - offset));
            }
            llvm::LLVMTypeRef struct_type = llvm::LLVMStructCreateNamed(context_, ("struct." + def.name).c_str());
            llvm::LLVMStructSetBody(struct_type, llvm_field_types.data(), static_cast<unsigned>(llvm_field_types.size()),
                              def.is_packed);
            info.llvm_type = struct_type;
            info.abi_align = static_cast<unsigned>(final_align);
        } else {
            for (const StructField& field : def.fields) {
                if (auto validate_result = validate_trivial(field.type, in_progress); !validate_result.has_value()) {
                    return std::unexpected(CodegenError(std::string(def.is_union ? "union '" : "struct '") + def.name + "' field '" +
                                            field.name + "': " + validate_result.error().what() +
                                            " (only scalars, pointers, trivial structs/unions, and fixed-size arrays "
                                            "of trivial types are allowed here; see spec ch04)",
                        current_loc_));
                }
                info.field_names.push_back(field.name);
                info.field_types.push_back(field.type);
                std::size_t union_field_align = 1;
                if (!def.is_packed) {
                    auto align_result = alignment_bytes_for_type(field.type);
                    if (!align_result.has_value()) return std::unexpected(std::move(align_result).error());
                    union_field_align = std::max(std::move(align_result).value(), static_cast<std::size_t>(field.resolved_alignment));
                }
                info.field_alignments.push_back(static_cast<unsigned>(union_field_align));
                info.field_physical_indices.push_back(0);
                auto field_type_result = to_llvm_type(field.type);
                if (!field_type_result.has_value()) return std::unexpected(std::move(field_type_result).error());
                llvm_field_types.push_back(std::move(field_type_result).value());
            }
            if (llvm_field_types.empty()) {
                return std::unexpected(CodegenError("union '" + def.name + "' must declare at least one field",
                    current_loc_));
            }
            std::size_t align_value = def.is_packed ? 1 : std::max<std::size_t>(1, def.resolved_alignment);
            std::size_t max_size = 0;
            std::size_t max_rep_align = 0;
            llvm::LLVMTypeRef rep_type = llvm_field_types[0];
            std::size_t rep_size = llvm::LLVMABISizeOfType(data_layout_ref(module_), rep_type);
            for (std::size_t i = 0; i < llvm_field_types.size(); ++i) {
                llvm::LLVMTypeRef field_type = llvm_field_types[i];
                std::size_t field_size = llvm::LLVMABISizeOfType(data_layout_ref(module_), field_type);
                std::size_t field_align = info.field_alignments[i];
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
            std::size_t union_size = ((max_size + align_value - 1) / align_value) * align_value;
            std::vector<llvm::LLVMTypeRef> storage_fields;
            storage_fields.push_back(rep_type);
            if (union_size > rep_size) {
                storage_fields.push_back(llvm::LLVMArrayType2(llvm::LLVMInt8TypeInContext(context_), union_size - rep_size));
            }
            llvm::LLVMTypeRef union_type = llvm::LLVMStructCreateNamed(context_, ("union." + def.name).c_str());
            llvm::LLVMStructSetBody(union_type, storage_fields.data(), static_cast<unsigned>(storage_fields.size()),
                              def.is_packed);
            info.llvm_type = union_type;
            info.abi_align = static_cast<unsigned>(align_value);
        }
        structs_[def.name] = std::move(info);
        declaring_aggregates_.erase(def.name);
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::declare_class(const ClassDef& def)
{
        if (def.is_forward_declaration) {
            return std::unexpected(CodegenError("forward-declared class '" + def.name + "' requires a full definition before layout is needed",
                current_loc_));
        }
        if (declaring_aggregates_.contains(def.name)) return {};
        declaring_aggregates_.insert(def.name);
        StructInfo info;
        info.has_ordinary_vtable = !def.is_interface;
        std::vector<llvm::LLVMTypeRef> llvm_field_types;
        std::size_t offset = 0;
        std::size_t overall_align = std::max<std::size_t>(1, def.resolved_alignment == 0 ? 1 : static_cast<std::size_t>(def.resolved_alignment));
        if (auto base = def.direct_ordinary_base()) {
            const StructInfo& base_info = structs_.at(base->get().base_type.name);
            info.field_names = base_info.field_names;
            info.field_types = base_info.field_types;
            info.field_alignments = base_info.field_alignments;
            info.field_physical_indices = base_info.field_physical_indices;
            unsigned base_field_count = llvm::LLVMCountStructElementTypes(base_info.llvm_type);
            llvm_field_types.resize(base_field_count);
            llvm::LLVMGetStructElementTypes(base_info.llvm_type, llvm_field_types.data());
            offset = llvm::LLVMABISizeOfType(data_layout_ref(module_), base_info.llvm_type);
            overall_align = std::max(overall_align, static_cast<std::size_t>(base_info.abi_align));
        }
        if (info.has_ordinary_vtable && llvm_field_types.empty()) {
            llvm_field_types.push_back(llvm::LLVMPointerTypeInContext(context_, 0));
            offset = llvm::LLVMABISizeOfType(data_layout_ref(module_), llvm_field_types.back());
            overall_align = std::max(overall_align,
                                     static_cast<std::size_t>(pointer_abi_alignment_for_as(module_, 0)));
        }
        for (const ClassField& field : def.fields) {
            auto field_type_result = to_llvm_type(field.type);
            if (!field_type_result.has_value()) return std::unexpected(std::move(field_type_result).error());
            llvm::LLVMTypeRef field_type = std::move(field_type_result).value();
            std::size_t field_size = llvm::LLVMABISizeOfType(data_layout_ref(module_), field_type);
            auto align_result = alignment_bytes_for_type(field.type);
            if (!align_result.has_value()) return std::unexpected(std::move(align_result).error());
            std::size_t field_align = std::max(std::move(align_result).value(), static_cast<std::size_t>(field.resolved_alignment));
            std::size_t aligned_offset = align_up(offset, field_align);
            if (aligned_offset > offset) {
                llvm_field_types.push_back(
                    llvm::LLVMArrayType2(llvm::LLVMInt8TypeInContext(context_), aligned_offset - offset));
                offset = aligned_offset;
            }
            info.field_names.push_back(field.name);
            info.field_types.push_back(field.type);
            info.field_alignments.push_back(static_cast<unsigned>(field_align));
            info.field_physical_indices.push_back(llvm_field_types.size());
            llvm_field_types.push_back(field_type);
            offset += field_size;
            overall_align = std::max(overall_align, field_align);
        }
        std::size_t final_size = align_up(offset, overall_align);
        if (final_size > offset) {
            llvm_field_types.push_back(llvm::LLVMArrayType2(llvm::LLVMInt8TypeInContext(context_), final_size - offset));
        }
        llvm::LLVMTypeRef class_type = llvm::LLVMStructCreateNamed(context_, ("class." + def.name).c_str());
        llvm::LLVMStructSetBody(class_type, llvm_field_types.data(), static_cast<unsigned>(llvm_field_types.size()),
                          /*Packed=*/0);
        info.llvm_type = class_type;
        info.abi_align = static_cast<unsigned>(overall_align);
        structs_[def.name] = std::move(info);
        declaring_aggregates_.erase(def.name);
        return {};
    }


    [[nodiscard]] std::expected<llvm::LLVMTypeRef, CodegenError> Codegen::to_llvm_type(const Type& type)
{
        switch (type.kind) {
            case TypeKind::Function:
                return std::unexpected(CodegenError("a bare function type cannot be lowered directly; only function pointers are runtime values",
                                   current_loc_));
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
                return llvm::LLVMPointerTypeInContext(context_, 0);
            case TypeKind::FunctionPointer: {
                std::vector<llvm::LLVMTypeRef> params;
                params.reserve(type.function_params.size());
                for (const Type& param : type.function_params) {
                    auto param_result = to_llvm_type(param);
                    if (!param_result.has_value()) return std::unexpected(std::move(param_result).error());
                    params.push_back(std::move(param_result).value());
                }
                return llvm::LLVMPointerTypeInContext(context_, 0);
            }
            case TypeKind::Span: {
                // A non-owning {data pointer, element count} pair -- a
                // literal (unnamed) two-word llvm::LLVM struct, not registered
                // in `structs_` (span isn't a user-visible aggregate the
                // way a `struct` is; Member/Subscript access to it is
                // special-cased directly on TypeKind::Span in
                // codegen_lvalue, not routed through StructInfo). llvm::LLVM
                // deduplicates identical literal struct types itself, so
                // there's no need to cache this beyond calling
                // llvm::LLVMStructTypeInContext each time.
                llvm::LLVMTypeRef fields[] = {llvm::LLVMPointerTypeInContext(context_, 0), llvm::LLVMInt64TypeInContext(context_)};
                return llvm::LLVMStructTypeInContext(context_, fields, 2, /*Packed=*/0);
            }
            case TypeKind::Array: {
                auto element_result = to_llvm_type(*type.element);
                if (!element_result.has_value()) return std::unexpected(std::move(element_result).error());
                return llvm::LLVMArrayType2(std::move(element_result).value(), type.array_size);
            }
            case TypeKind::Named:
                // ch06 §6: `nullptr_t`, the type of the `nullptr`
                // literal. It has exactly one value, so nothing about it
                // needs to be *read* at runtime -- but it is a real
                // object type (a `nullptr_t` local/field/parameter has
                // storage), and real C++ gives it the size and alignment
                // of a pointer, so it lowers to an opaque pointer whose
                // only ever value is null.
                if (is_nullptr_type(type)) return llvm::LLVMPointerTypeInContext(context_, 0);
                // Every scalar's LLVM type follows from `scpp.ast`'s
                // scalar type model -- see `scalar_type_info`, which is
                // the only place the twenty names of ch06 §6 are listed.
                // This file supplies the one thing that model cannot
                // know: the target's pointer width, which `size_t` and
                // `ptrdiff_t` track. (`long`/`unsigned long` are always
                // 64-bit regardless of target -- ch06's deliberate
                // anti-LP64/LLP64-pitfall fix -- so they are not
                // pointer-sized and need nothing from here.)
                //
                // LLVM draws no signed/unsigned distinction at the type
                // level, only at the instruction level, so each
                // signed/unsigned pair maps to the same integer type;
                // see is_unsigned_scalar_type_name and
                // codegen_checked_arith for where signedness is actually
                // consulted. `bool` is a full byte (i8), matching real
                // C++'s sizeof(bool)==1 and ch06's false=0/true=1
                // invariant -- not i1, even though LLVM's icmp/br/select
                // require i1; see bool_to_i1/i1_to_bool below for the
                // narrow choke point that bridges the two. `char` is a
                // signed 8-bit scalar distinct from both `int8_t` and
                // `uint8_t`, with no implicit promotion to or from
                // `int`, so i8 is its only representation.
                if (std::optional<ScalarTypeInfo> scalar = scalar_type_info(std::string_view{type.name}); scalar.has_value()) {
                    int width = scalar->is_pointer_sized
                                    ? static_cast<int>(8 * llvm::LLVMPointerSizeForAS(data_layout_ref(module_), 0))
                                    : scalar->bit_width;
                    if (scalar->category == ScalarCategory::Floating) {
                        return width == 32 ? llvm::LLVMFloatTypeInContext(context_) : llvm::LLVMDoubleTypeInContext(context_);
                    }
                    return llvm::LLVMIntTypeInContext(context_, static_cast<unsigned>(width));
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
                if (type.name == "void") return llvm::LLVMVoidTypeInContext(context_);
                {
                    auto it = structs_.find(type.name);
                    if (it != structs_.end()) return it->second.llvm_type;
                }
                if (!declaring_aggregates_.contains(type.name) && program_ != nullptr) {
                    const StructDef* struct_def = nullptr;
                    for (const StructDef& def : program_->structs) {
                        if (def.name != type.name || !def.template_params.empty()) continue;
                        if (!def.is_forward_declaration) {
                            struct_def = &def;
                            break;
                        }
                        if (struct_def == nullptr) struct_def = &def;
                    }
                    if (struct_def != nullptr) {
                        if (auto declare_result = declare_struct(*struct_def); !declare_result.has_value()) {
                            return std::unexpected(std::move(declare_result).error());
                        }
                        auto it = structs_.find(type.name);
                        if (it != structs_.end()) return it->second.llvm_type;
                    }
                    const ClassDef* class_def = nullptr;
                    for (const ClassDef& def : program_->classes) {
                        if (def.name != type.name || !def.template_params.empty() || def.is_synthetic_check_only ||
                            def.is_concept_witness) {
                            continue;
                        }
                        if (!def.is_forward_declaration) {
                            class_def = &def;
                            break;
                        }
                        if (class_def == nullptr) class_def = &def;
                    }
                    if (class_def != nullptr) {
                        if (auto declare_result = declare_class(*class_def); !declare_result.has_value()) {
                            return std::unexpected(std::move(declare_result).error());
                        }
                        auto it = structs_.find(type.name);
                        if (it != structs_.end()) return it->second.llvm_type;
                    }
                }
                return std::unexpected(CodegenError("unsupported type '" + type.name + "'",
                    current_loc_));
        }
        return std::unexpected(CodegenError("unhandled type kind",
            current_loc_));
    }


    [[nodiscard]] std::optional<unsigned> Codegen::alignment_for_type(const Type& type) const
{
        if (program_ != nullptr) {
            std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, type, current_target_layout_info());
            if (layout.has_value()) return static_cast<unsigned>(layout->abi_align_bytes);
        }
        if (type.kind != TypeKind::Named) return std::nullopt;
        auto it = structs_.find(type.name);
        if (it == structs_.end()) return std::nullopt;
        return it->second.abi_align;
    }


    llvm::LLVMValueRef Codegen::create_load(llvm::LLVMTypeRef type, llvm::LLVMValueRef ptr, std::optional<unsigned> alignment,
                                const std::string& name)
{
        llvm::LLVMValueRef load = llvm::LLVMBuildLoad2(builder_, type, ptr, name.c_str());
        if (alignment.has_value()) llvm::LLVMSetAlignment(load, *alignment);
        return load;
    }


    llvm::LLVMValueRef Codegen::create_store(llvm::LLVMValueRef value, llvm::LLVMValueRef ptr, std::optional<unsigned> alignment)
{
        llvm::LLVMValueRef store = llvm::LLVMBuildStore(builder_, value, ptr);
        if (alignment.has_value()) llvm::LLVMSetAlignment(store, *alignment);
        return store;
    }


    llvm::LLVMValueRef Codegen::build_call(llvm::LLVMValueRef callee, std::vector<llvm::LLVMValueRef> args, const std::string& name)
{
        return build_call(llvm::LLVMGlobalGetValueType(callee), callee, std::move(args), name);
    }


    llvm::LLVMValueRef Codegen::build_call(llvm::LLVMTypeRef fn_type, llvm::LLVMValueRef callee, std::vector<llvm::LLVMValueRef> args,
                            const std::string& name)
{
        return llvm::LLVMBuildCall2(builder_, fn_type, callee, args.data(), static_cast<unsigned>(args.size()), name.c_str());
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::zero_initialize_storage(llvm::LLVMValueRef ptr, const Type& type, std::optional<unsigned> alignment)
{
        auto llvm_type_result = to_llvm_type(type);
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
        switch (type.kind) {
            case TypeKind::Named:
            case TypeKind::Array:
            case TypeKind::Span: {
                std::uint64_t size = llvm::LLVMABISizeOfType(data_layout_ref(module_), llvm_type);
                llvm::LLVMBuildMemSet(builder_, ptr,
                                llvm::LLVMConstInt(llvm::LLVMInt8TypeInContext(context_), 0, /*SignExtend=*/0),
                                llvm::LLVMConstInt(llvm::LLVMInt64TypeInContext(context_), size, /*SignExtend=*/0),
                                alignment.value_or(1));
                if (type.kind == TypeKind::Named && class_has_ordinary_vtable(type.name)) {
                    if (auto init_result = initialize_ordinary_vtable_pointer(type.name, ptr); !init_result.has_value()) {
                        return std::unexpected(std::move(init_result).error());
                    }
                }
                return {};
            }
            default:
                create_store(llvm::LLVMConstNull(llvm_type), ptr, alignment);
                return {};
        }
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::validate_reference_pointee(const Type& pointee)
{
        if (pointee.kind == TypeKind::Reference) {
            return std::unexpected(CodegenError("a reference to a reference is not supported",
                current_loc_));
        }
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::validate_reference_return_elision(const Function& fn)
{
        if (fn.return_lifetime.present()) return {};
        // ch04 §4.2/ch05 §5.9/spec §6.5: `this` (always params[0] when
        // present) is always the elision source when the function is a
        // method, regardless of how many other reference parameters it
        // also takes -- see movecheck's resolve_elided_param_index for
        // the full rationale (this is its structural codegen-side
        // counterpart, kept in sync).
        if (!fn.params.empty() && fn.params[0].name == "this" && fn.params[0].type.kind == TypeKind::Reference) {
            if (fn.return_type.is_mutable_ref && !fn.params[0].type.is_mutable_ref) {
                return std::unexpected(CodegenError("function '" + fn.name +
                                    "' returns a mutable reference ('T&') but its 'this' is a read-only "
                                    "('const') receiver",
                    current_loc_));
            }
            return {};
        }
        const Param* found = nullptr;
        for (const Param& param : fn.params) {
            // ch03/ch05 §5.11: exclude a `T&&` parameter -- see
            // movecheck's resolve_elided_param_index (this function's
            // structural counterpart) for why it's never a sound elision
            // source.
            if (param.type.kind != TypeKind::Reference || param.type.is_rvalue_ref) continue;
            if (found != nullptr) {
                return std::unexpected(CodegenError("function '" + fn.name +
                                    "' returns a reference but has more than one reference parameter; scpp "
                                    "v0.1 can only infer a returned reference's lifetime when there is exactly "
                                    "one (spec ch05.3)",
                    current_loc_));
            }
            found = &param;
        }
        if (found == nullptr) {
            return std::unexpected(CodegenError("function '" + fn.name +
                                "' returns a reference but has no reference parameter to infer its lifetime "
                                "from (spec ch05.3)",
                current_loc_));
        }
        if (fn.return_type.is_mutable_ref && !found->type.is_mutable_ref) {
            return std::unexpected(CodegenError("function '" + fn.name +
                                "' returns a mutable reference ('T&') but its sole reference parameter '" +
                                found->name + "' is a shared reference ('const T&')",
                current_loc_));
        }
        return {};
    }


    [[nodiscard]] bool Codegen::is_bare_void(const Type& type)
{
        return type.kind == TypeKind::Named && type.name == "void";
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::validate_c_abi_compatible(const Type& type, const std::string& fn_name,
                                    const std::string& context_description)
{
        if (is_interface_representation_type(type)) {
            return std::unexpected(CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                " cannot be an interface-typed pointer or reference -- it has no defined C "
                                "representation (spec ch02 §2.1 / §11.3)",
                current_loc_));
        }
        switch (type.kind) {
            case TypeKind::Function:
                return std::unexpected(CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be a bare function type -- use a function pointer instead (spec ch02 §2.1)",
                    current_loc_));
            case TypeKind::Named: {
                if (find_class_def(type.name) != nullptr) {
                    return std::unexpected(CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                        " cannot be class type '" + type.name +
                                        "' -- class types have no defined C representation "
                                        "(spec ch02 §2.1)",
                        current_loc_));
                }
                if (is_scalar_type_name(type.name)) return {};
            }
            case TypeKind::Pointer:
            case TypeKind::FunctionPointer:
            case TypeKind::Array:
                return {};
            case TypeKind::Reference:
                return std::unexpected(CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be a reference -- it has no defined C representation (spec "
                                    "ch02 §2.1)",
                    current_loc_));
            case TypeKind::Span:
                return std::unexpected(CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be std::span -- it has no defined C representation (spec "
                                    "ch02 §2.1)",
                    current_loc_));
        }
        return {};
    }

} // namespace scpp
