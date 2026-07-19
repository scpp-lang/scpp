module;

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

module scpp.compiler.codegen:lifetime;

import std;
import :api;

namespace scpp {

    llvm::Value* Codegen::codegen_materialize_rvalue_reference_source(const Expr& expr)
{
        if (expr.kind == ExprKind::Lambda) return codegen_expr(expr);
        // Also reuses std::move's own codegen unchanged, including its
        // "null out the source slot" side effect when the moved value is
        // itself a std::unique_ptr/class.
        llvm::Value* value = codegen_expr(expr);
        llvm::AllocaInst* temp = create_entry_block_alloca(value->getType(), "rvaluetmp");
        builder_->CreateStore(value, temp);
        return temp;
    }


    llvm::Value* Codegen::codegen_materialize_const_reference_source(const Expr& expr, const Type& target_type)
{
        if (produces_rvalue_of_type(expr, target_type)) {
            return codegen_materialize_rvalue_reference_source(expr);
        }
        llvm::Type* llvm_type = to_llvm_type(target_type);
        llvm::AllocaInst* temp = create_entry_block_alloca(llvm_type, "constreftmp");
        if (is_named_record_type(target_type)) {
            create_store(codegen_class_value_for_boundary(expr, target_type, /*allow_implicit_converting_ctor=*/true), temp,
                         alignment_for_type(target_type));
        } else {
            create_store(codegen_value_for_target(expr, target_type), temp, alignment_for_type(target_type));
        }
        return temp;
    }


    void Codegen::codegen_copy_construct_class(llvm::Value* dest_ptr, llvm::Value* src_ptr, const std::string& class_name)
{
        if (const Function* user_ctor = find_user_declared_copy_ctor_ast(class_name)) {
            llvm::Function* ctor = module_->getFunction(overload_names_.at(user_ctor));
            builder_->CreateCall(ctor, {dest_ptr, src_ptr});
        } else {
            codegen_memberwise_copy_construct(dest_ptr, src_ptr, class_name);
        }
    }


    llvm::Function* Codegen::find_destructor(const std::string& class_name)
{
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_delete") || fn.params.size() != 1) continue;
            const Type& this_param = fn.params[0].type;
            if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
                this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
                continue;
            }
            return module_->getFunction(overload_names_.at(&fn));
        }
        return nullptr;
    }


    [[nodiscard]] const Function* Codegen::find_destructor_ast(const std::string& class_name) const
{
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_delete") || fn.params.size() != 1) continue;
            const Type& this_param = fn.params[0].type;
            if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
                this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
                continue;
            }
            return &fn;
        }
        return nullptr;
    }


    void Codegen::emit_interface_destructor_dispatch_call(const std::string& interface_name, llvm::Value* interface_value)
{
        const Function* destructor = find_destructor_ast(interface_name);
        if (destructor == nullptr) return;
        std::optional<std::size_t> slot_index = interface_method_slot_index(interface_name, *destructor);
        if (!slot_index.has_value()) {
            throw CodegenError("missing destructor dispatch slot for interface '" + interface_name + "'", current_loc_);
        }
        llvm::Value* object_ptr = extract_interface_object_ptr(interface_value);
        llvm::Value* dispatch_ptr = extract_interface_dispatch_ptr(interface_value);
        llvm::FunctionType* thunk_type = interface_dispatch_function_type(*destructor);
        llvm::ArrayType* table_type = interface_dispatch_table_type(interface_name);
        llvm::Value* slot_ptr = builder_->CreateConstGEP2_32(table_type, dispatch_ptr, 0,
                                                             static_cast<unsigned>(*slot_index), "iface.dtor.slot");
        llvm::Value* target_ptr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), slot_ptr, "iface.dtor.target");
        builder_->CreateCall(thunk_type, target_ptr, {object_ptr});
    }


    [[nodiscard]] const Function* Codegen::find_user_declared_copy_ctor_ast(const std::string& class_name)
{
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_new") || fn.params.size() != 2) continue;
            const Type& this_param = fn.params[0].type;
            if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
                this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
                continue;
            }
            const Type& p = fn.params[1].type;
            if (p.kind == TypeKind::Reference && !p.is_rvalue_ref && !p.is_mutable_ref && p.pointee &&
                p.pointee->kind == TypeKind::Named && p.pointee->name == class_name) {
                return &fn;
            }
        }
        return nullptr;
    }


    [[nodiscard]] const Function* Codegen::find_user_declared_copy_assign_ast(const std::string& class_name)
{
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_operator_assign") || fn.params.size() != 2) continue;
            const Type& this_param = fn.params[0].type;
            if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
                this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
                continue;
            }
            const Type& p = fn.params[1].type;
            if (p.kind == TypeKind::Reference && !p.is_rvalue_ref && !p.is_mutable_ref && p.pointee &&
                p.pointee->kind == TypeKind::Named && p.pointee->name == class_name) {
                return &fn;
            }
        }
        return nullptr;
    }


    [[nodiscard]] bool Codegen::has_user_declared_dtor(const std::string& class_name)
{
        return find_destructor(class_name) != nullptr;
    }


    [[nodiscard]] bool Codegen::is_copy_constructible(const std::string& class_name)
{
        if (find_user_declared_copy_ctor_ast(class_name) != nullptr) return true;
        if (has_user_declared_dtor(class_name) || find_user_declared_copy_assign_ast(class_name) != nullptr) {
            return false;
        }
        auto it = structs_.find(class_name);
        if (it == structs_.end()) return false;
        for (const Type& field_type : it->second.field_types) {
            if (!is_field_copy_constructible(field_type)) return false;
        }
        return true;
    }


    [[nodiscard]] bool Codegen::is_copy_assignable(const std::string& class_name)
{
        if (find_user_declared_copy_assign_ast(class_name) != nullptr) return true;
        if (has_user_declared_dtor(class_name) || find_user_declared_copy_ctor_ast(class_name) != nullptr) {
            return false;
        }
        auto it = structs_.find(class_name);
        if (it == structs_.end()) return false;
        for (const Type& field_type : it->second.field_types) {
            if (field_type.kind == TypeKind::Reference) return false;
            if (!is_field_copy_assignable(field_type)) return false;
        }
        return true;
    }


    [[nodiscard]] bool Codegen::is_field_copy_constructible(const Type& type)
{
        if (type.kind == TypeKind::Reference) return true;
        if (type.kind == TypeKind::Array) return is_field_copy_constructible(*type.element);
        if (type.kind == TypeKind::Named && structs_.contains(type.name)) return is_copy_constructible(type.name);
        return true;
    }


    [[nodiscard]] bool Codegen::is_field_copy_assignable(const Type& type)
{
        if (type.kind == TypeKind::Reference) return false;
        if (type.kind == TypeKind::Array) return is_field_copy_assignable(*type.element);
        if (type.kind == TypeKind::Named && structs_.contains(type.name)) return is_copy_assignable(type.name);
        return true;
    }


    void Codegen::codegen_memberwise_copy_construct(llvm::Value* dest_ptr, llvm::Value* src_ptr,
                                            const std::string& class_name)
{
        const StructInfo& info = structs_.at(class_name);
        if (info.has_ordinary_vtable) initialize_ordinary_vtable_pointer(class_name, dest_ptr);
        for (std::size_t i = 0; i < info.field_names.size(); i++) {
            const Type& field_type = info.field_types[i];
            llvm::Value* dest_field =
                builder_->CreateStructGEP(info.llvm_type, dest_ptr, info.physical_field_index(i), info.field_names[i]);
            llvm::Value* src_field =
                builder_->CreateStructGEP(info.llvm_type, src_ptr, info.physical_field_index(i), info.field_names[i]);
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                if (const Function* user_ctor = find_user_declared_copy_ctor_ast(field_type.name)) {
                    llvm::Function* ctor = module_->getFunction(overload_names_.at(user_ctor));
                    builder_->CreateCall(ctor, {dest_field, src_field});
                } else {
                    codegen_memberwise_copy_construct(dest_field, src_field, field_type.name);
                }
            } else {
                llvm::Type* llvm_field_type = to_llvm_type(field_type);
                llvm::Value* value = builder_->CreateLoad(llvm_field_type, src_field, "copiedfield");
                create_store(value, dest_field, std::nullopt);
            }
        }
    }


    void Codegen::codegen_memberwise_copy_assign(llvm::Value* dest_ptr, llvm::Value* src_ptr, const std::string& class_name)
{
        const StructInfo& info = structs_.at(class_name);
        for (std::size_t i = 0; i < info.field_names.size(); i++) {
            const Type& field_type = info.field_types[i];
            llvm::Value* dest_field =
                builder_->CreateStructGEP(info.llvm_type, dest_ptr, info.physical_field_index(i), info.field_names[i]);
            llvm::Value* src_field =
                builder_->CreateStructGEP(info.llvm_type, src_ptr, info.physical_field_index(i), info.field_names[i]);
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                if (const Function* user_assign = find_user_declared_copy_assign_ast(field_type.name)) {
                    llvm::Function* op = module_->getFunction(overload_names_.at(user_assign));
                    builder_->CreateCall(op, {dest_field, src_field});
                } else {
                    codegen_memberwise_copy_assign(dest_field, src_field, field_type.name);
                }
            } else {
                llvm::Type* llvm_field_type = to_llvm_type(field_type);
                llvm::Value* value = builder_->CreateLoad(llvm_field_type, src_field, "copiedfield");
                create_store(value, dest_field, std::nullopt);
            }
        }
    }


    [[nodiscard]] bool Codegen::class_has_destructor_in_chain(const std::string& class_name)
{
        if (find_destructor(class_name) != nullptr) return true;
        const ClassDef* def = find_class_def(class_name);
        if (def == nullptr) return false;
        if (const BaseSpecifier* base = def->direct_ordinary_base()) {
            if (class_has_destructor_in_chain(base->base_type.name)) return true;
        }
        for (const ClassDef* interface_def : collect_virtual_interface_bases_in_construction_order(*def)) {
            if (interface_def != nullptr && find_destructor(interface_def->name) != nullptr) return true;
        }
        return false;
    }


    void Codegen::emit_destructor_chain_calls(const std::string& class_name, llvm::Value* object_ptr)
{
        if (llvm::Function* dtor = find_destructor(class_name)) {
            builder_->CreateCall(dtor, {object_ptr});
        }
        const ClassDef* def = find_class_def(class_name);
        if (def != nullptr) {
            if (const BaseSpecifier* base = def->direct_ordinary_base()) {
                emit_destructor_chain_calls(base->base_type.name, object_ptr);
            }
            std::vector<const ClassDef*> interface_bases = collect_virtual_interface_bases_in_construction_order(*def);
            for (auto it = interface_bases.rbegin(); it != interface_bases.rend(); ++it) {
                if (*it == nullptr) continue;
                if (llvm::Function* dtor = find_destructor((*it)->name)) {
                    builder_->CreateCall(dtor, {object_ptr});
                }
            }
        }
    }


    llvm::AllocaInst* Codegen::create_moved_flag_if_has_destructor(const std::string& class_name)
{
        if (!class_has_destructor_in_chain(class_name)) return nullptr;
        llvm::AllocaInst* flag = create_entry_block_alloca(llvm::Type::getInt1Ty(*context_), "movedflag");
        builder_->CreateStore(llvm::ConstantInt::getFalse(*context_), flag);
        return flag;
    }


    void Codegen::codegen_call_destructor_chain_unless_moved(const std::string& class_name, llvm::Value* object_ptr,
                                                    llvm::AllocaInst* moved_flag)
{
        if (moved_flag == nullptr) {
            emit_destructor_chain_calls(class_name, object_ptr);
            return;
        }
        llvm::Value* was_moved = builder_->CreateLoad(llvm::Type::getInt1Ty(*context_), moved_flag, "wasmoved");
        llvm::Function* current_fn = builder_->GetInsertBlock()->getParent();
        llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(*context_, "dtorcall", current_fn);
        llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(*context_, "dtorskip", current_fn);
        builder_->CreateCondBr(was_moved, merge_bb, then_bb);
        builder_->SetInsertPoint(then_bb);
        emit_destructor_chain_calls(class_name, object_ptr);
        builder_->CreateBr(merge_bb);
        builder_->SetInsertPoint(merge_bb);
    }


    void Codegen::codegen_destroy_old_class_state_for_move_assign(llvm::Value* ptr, const std::string& class_name,
                                                         llvm::AllocaInst* moved_flag)
{
        if (class_has_destructor_in_chain(class_name)) {
            codegen_call_destructor_chain_unless_moved(class_name, ptr, moved_flag);
            return;
        }
        auto struct_it = structs_.find(class_name);
        if (struct_it == structs_.end()) return;
        const StructInfo& info = struct_it->second;
        (void)get_or_declare_free();
        for (std::size_t i = 0; i < info.field_types.size(); i++) {
            const Type& field_type = info.field_types[i];
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                llvm::Value* field_ptr =
                    builder_->CreateStructGEP(info.llvm_type, ptr, info.physical_field_index(i), info.field_names[i]);
                codegen_destroy_old_class_state_for_move_assign(field_ptr, field_type.name);
            }
        }
    }

} // namespace scpp
