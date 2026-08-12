module;

module scpp.compiler.codegen:lifetime;

import std;
import llvm;
import :api;

namespace scpp {

    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_materialize_rvalue_reference_source(const Expr& expr)
{
        if (expr.kind == ExprKind::Lambda) return codegen_expr(expr);
        // Also reuses std::move's own codegen unchanged, including its
        // "null out the source slot" side effect when the moved value is
        // itself a std::unique_ptr/class.
        auto value_result = codegen_expr(expr);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        llvm::LLVMValueRef value = std::move(value_result).value();
        llvm::LLVMValueRef temp = create_entry_block_alloca(llvm::LLVMTypeOf(value), "rvaluetmp");
        llvm::LLVMBuildStore(builder_, value, temp);
        return temp;
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_materialize_const_reference_source(const Expr& expr, const Type& target_type)
{
        if (produces_rvalue_of_type(expr, target_type)) {
            return codegen_materialize_rvalue_reference_source(expr);
        }
        auto llvm_type_result = to_llvm_type(target_type);
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
        llvm::LLVMValueRef temp = create_entry_block_alloca(llvm_type, "constreftmp");
        if (is_named_record_type(target_type)) {
            auto value_result = codegen_class_value_for_boundary(expr, target_type, /*allow_implicit_converting_ctor=*/true);
            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
            create_store(std::move(value_result).value(), temp, alignment_for_type(target_type));
        } else {
            auto value_result = codegen_value_for_target(expr, target_type);
            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
            create_store(std::move(value_result).value(), temp, alignment_for_type(target_type));
        }
        return temp;
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::codegen_copy_construct_class(llvm::LLVMValueRef dest_ptr, llvm::LLVMValueRef src_ptr, const std::string& class_name)
{
        if (const Function* user_ctor = find_user_declared_copy_ctor_ast(class_name)) {
            llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(user_ctor).c_str());
            build_call(ctor, {dest_ptr, src_ptr});
            return {};
        }
        return codegen_memberwise_copy_construct(dest_ptr, src_ptr, class_name);
    }


    llvm::LLVMValueRef Codegen::find_destructor(const std::string& class_name)
{
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_delete") || fn.params.size() != 1) continue;
            if (fn.member_owner_class != class_name) continue;
            const Type& this_param = fn.params[0].type;
            if (!is_special_member_this_param(this_param, class_name)) continue;
            return llvm::LLVMGetNamedFunction(module_, overload_names_.at(&fn).c_str());
        }
        return nullptr;
    }


    [[nodiscard]] const Function* Codegen::find_destructor_ast(const std::string& class_name) const
{
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_delete") || fn.params.size() != 1) continue;
            if (fn.member_owner_class != class_name) continue;
            const Type& this_param = fn.params[0].type;
            if (!is_special_member_this_param(this_param, class_name)) continue;
            return &fn;
        }
        return nullptr;
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::emit_interface_destructor_dispatch_call(const std::string& interface_name, llvm::LLVMValueRef interface_value)
{
        const Function* destructor = find_destructor_ast(interface_name);
        if (destructor == nullptr) return {};
        auto slot_index_result = interface_method_slot_index(interface_name, *destructor);
        if (!slot_index_result.has_value()) return std::unexpected(std::move(slot_index_result).error());
        std::optional<std::size_t> slot_index = std::move(slot_index_result).value();
        if (!slot_index.has_value()) {
            return std::unexpected(CodegenError("missing destructor dispatch slot for interface '" + interface_name + "'", current_loc_));
        }
        llvm::LLVMValueRef object_ptr = extract_interface_object_ptr(interface_value);
        llvm::LLVMValueRef dispatch_ptr = extract_interface_dispatch_ptr(interface_value);
        auto thunk_type_result = interface_dispatch_function_type(*destructor);
        if (!thunk_type_result.has_value()) return std::unexpected(std::move(thunk_type_result).error());
        llvm::LLVMTypeRef thunk_type = std::move(thunk_type_result).value();
        auto table_type_result = interface_dispatch_table_type(interface_name);
        if (!table_type_result.has_value()) return std::unexpected(std::move(table_type_result).error());
        llvm::LLVMTypeRef table_type = std::move(table_type_result).value();
        llvm::LLVMTypeRef i32_ty = llvm::LLVMInt32TypeInContext(context_);
        llvm::LLVMValueRef slot_indices[] = {llvm::LLVMConstInt(i32_ty, 0, /*SignExtend=*/0),
                                       llvm::LLVMConstInt(i32_ty, static_cast<unsigned>(*slot_index), /*SignExtend=*/0)};
        llvm::LLVMValueRef slot_ptr = llvm::LLVMBuildGEP2(builder_, table_type, dispatch_ptr, slot_indices, 2, "iface.dtor.slot");
        llvm::LLVMValueRef target_ptr = llvm::LLVMBuildLoad2(builder_, llvm::LLVMPointerTypeInContext(context_, 0), slot_ptr, "iface.dtor.target");
        build_call(thunk_type, target_ptr, {object_ptr});
        return {};
    }


    [[nodiscard]] const Function* Codegen::find_user_declared_copy_ctor_ast(const std::string& class_name)
{
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_new") || fn.params.size() != 2) continue;
            if (fn.member_owner_class != class_name) continue;
            const Type& this_param = fn.params[0].type;
            if (!is_special_member_this_param(this_param, class_name)) continue;
            const Type& p = fn.params[1].type;
            if (is_special_member_const_lvalue_self_param(p, class_name)) return &fn;
        }
        return nullptr;
    }


    [[nodiscard]] const Function* Codegen::find_user_declared_copy_assign_ast(const std::string& class_name)
{
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_operator_assign") || fn.params.size() != 2) continue;
            if (fn.member_owner_class != class_name) continue;
            const Type& this_param = fn.params[0].type;
            if (!is_special_member_this_param(this_param, class_name)) continue;
            const Type& p = fn.params[1].type;
            if (is_special_member_const_lvalue_self_param(p, class_name)) return &fn;
        }
        return nullptr;
    }


    [[nodiscard]] bool Codegen::has_user_declared_dtor(const std::string& class_name)
{
        return find_destructor(class_name) != nullptr;
    }


    [[nodiscard]] bool Codegen::is_copy_constructible(const std::string& class_name)
{
        auto has_direct_reference_field = [&](const std::vector<Type>& field_types) {
            for (const Type& field_type : field_types) {
                if (field_type.kind == TypeKind::Reference) return true;
            }
            return false;
        };
        if (find_user_declared_copy_ctor_ast(class_name) != nullptr) return true;
        if (find_user_declared_copy_assign_ast(class_name) != nullptr) {
            return false;
        }
        auto it = structs_.find(class_name);
        if (it == structs_.end()) return false;
        if (has_user_declared_dtor(class_name) && !has_direct_reference_field(it->second.field_types)) return false;
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


    [[nodiscard]] std::expected<void, CodegenError> Codegen::codegen_memberwise_copy_construct(llvm::LLVMValueRef dest_ptr, llvm::LLVMValueRef src_ptr,
                                            const std::string& class_name)
{
        const StructInfo& info = structs_.at(class_name);
        if (info.has_ordinary_vtable) {
            if (auto vtable_result = initialize_ordinary_vtable_pointer(class_name, dest_ptr); !vtable_result.has_value()) {
                return std::unexpected(std::move(vtable_result).error());
            }
        }
        for (std::size_t i = 0; i < info.field_names.size(); i++) {
            const Type& field_type = info.field_types[i];
            llvm::LLVMValueRef dest_field = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, dest_ptr, info.physical_field_index(i),
                                                          info.field_names[i].c_str());
            llvm::LLVMValueRef src_field = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, src_ptr, info.physical_field_index(i),
                                                         info.field_names[i].c_str());
            if (auto r = codegen_copy_construct_field(dest_field, src_field, field_type); !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
        }
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::codegen_copy_construct_field(llvm::LLVMValueRef dest_ptr, llvm::LLVMValueRef src_ptr,
                                            const Type& field_type)
{
        // is_field_copy_constructible already looks *through* an array to
        // its element type when deciding whether a field is copyable, so
        // emission has to look through it too: a bitwise load/store of a
        // `Owner[3]` field would bypass the very copy constructor that
        // check just approved.
        if (field_type.kind == TypeKind::Array && field_type.element != nullptr &&
            type_needs_nontrivial_default_init(field_type)) {
            auto array_llvm_type_result = to_llvm_type(field_type);
            if (!array_llvm_type_result.has_value()) return std::unexpected(std::move(array_llvm_type_result).error());
            llvm::LLVMTypeRef array_llvm_type = std::move(array_llvm_type_result).value();
            return emit_array_element_loop(
                field_type, dest_ptr, /*reverse=*/false,
                [&](llvm::LLVMValueRef dest_element, llvm::LLVMValueRef index) -> std::expected<void, CodegenError> {
                    return codegen_copy_construct_field(dest_element, build_array_element_gep(array_llvm_type, src_ptr, index),
                                                        *field_type.element);
                });
        }
        if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
            if (const Function* user_ctor = find_user_declared_copy_ctor_ast(field_type.name)) {
                llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(user_ctor).c_str());
                build_call(ctor, {dest_ptr, src_ptr});
                return {};
            }
            return codegen_memberwise_copy_construct(dest_ptr, src_ptr, field_type.name);
        }
        auto llvm_field_type_result = to_llvm_type(field_type);
        if (!llvm_field_type_result.has_value()) return std::unexpected(std::move(llvm_field_type_result).error());
        llvm::LLVMTypeRef llvm_field_type = std::move(llvm_field_type_result).value();
        llvm::LLVMValueRef value = llvm::LLVMBuildLoad2(builder_, llvm_field_type, src_ptr, "copiedfield");
        create_store(value, dest_ptr, std::nullopt);
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::codegen_copy_assign_field(llvm::LLVMValueRef dest_ptr, llvm::LLVMValueRef src_ptr,
                                            const Type& field_type)
{
        if (field_type.kind == TypeKind::Array && field_type.element != nullptr &&
            type_needs_nontrivial_default_init(field_type)) {
            auto array_llvm_type_result = to_llvm_type(field_type);
            if (!array_llvm_type_result.has_value()) return std::unexpected(std::move(array_llvm_type_result).error());
            llvm::LLVMTypeRef array_llvm_type = std::move(array_llvm_type_result).value();
            return emit_array_element_loop(
                field_type, dest_ptr, /*reverse=*/false,
                [&](llvm::LLVMValueRef dest_element, llvm::LLVMValueRef index) -> std::expected<void, CodegenError> {
                    return codegen_copy_assign_field(dest_element, build_array_element_gep(array_llvm_type, src_ptr, index),
                                                     *field_type.element);
                });
        }
        if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
            if (const Function* user_assign = find_user_declared_copy_assign_ast(field_type.name)) {
                llvm::LLVMValueRef op = llvm::LLVMGetNamedFunction(module_, overload_names_.at(user_assign).c_str());
                build_call(op, {dest_ptr, src_ptr});
                return {};
            }
            return codegen_memberwise_copy_assign(dest_ptr, src_ptr, field_type.name);
        }
        auto llvm_field_type_result = to_llvm_type(field_type);
        if (!llvm_field_type_result.has_value()) return std::unexpected(std::move(llvm_field_type_result).error());
        llvm::LLVMTypeRef llvm_field_type = std::move(llvm_field_type_result).value();
        llvm::LLVMValueRef value = llvm::LLVMBuildLoad2(builder_, llvm_field_type, src_ptr, "copiedfield");
        create_store(value, dest_ptr, std::nullopt);
        return {};
    }


    llvm::LLVMValueRef Codegen::build_array_element_gep(llvm::LLVMTypeRef array_llvm_type, llvm::LLVMValueRef array_ptr,
                                               llvm::LLVMValueRef index)
{
        llvm::LLVMValueRef indices[] = {llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), 0, /*SignExtend=*/0), index};
        return llvm::LLVMBuildGEP2(builder_, array_llvm_type, array_ptr, indices, 2, "arrayelem");
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::codegen_memberwise_copy_assign(llvm::LLVMValueRef dest_ptr, llvm::LLVMValueRef src_ptr, const std::string& class_name)
{
        const StructInfo& info = structs_.at(class_name);
        for (std::size_t i = 0; i < info.field_names.size(); i++) {
            const Type& field_type = info.field_types[i];
            llvm::LLVMValueRef dest_field = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, dest_ptr, info.physical_field_index(i),
                                                          info.field_names[i].c_str());
            llvm::LLVMValueRef src_field = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, src_ptr, info.physical_field_index(i),
                                                         info.field_names[i].c_str());
            if (auto r = codegen_copy_assign_field(dest_field, src_field, field_type); !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
        }
        return {};
    }


    [[nodiscard]] bool Codegen::class_has_destructor_in_chain(const std::string& class_name)
{
        if (find_destructor(class_name) != nullptr) return true;
        const ClassDef* def = find_class_def(class_name);
        if (def == nullptr) return false;
        if (auto base = def->direct_ordinary_base()) {
            if (class_has_destructor_in_chain(base->get().base_type.name)) return true;
        }
        for (const ClassDef* interface_def : collect_virtual_interface_bases_in_construction_order(*def)) {
            if (interface_def != nullptr && find_destructor(interface_def->name) != nullptr) return true;
        }
        return false;
    }


    void Codegen::emit_destructor_chain_calls(const std::string& class_name, llvm::LLVMValueRef object_ptr)
{
        if (llvm::LLVMValueRef dtor = find_destructor(class_name)) {
            build_call(dtor, {object_ptr});
        }
        const ClassDef* def = find_class_def(class_name);
        if (def != nullptr) {
            if (auto base = def->direct_ordinary_base()) {
                emit_destructor_chain_calls(base->get().base_type.name, object_ptr);
            }
            std::vector<const ClassDef*> interface_bases = collect_virtual_interface_bases_in_construction_order(*def);
            for (auto it = interface_bases.rbegin(); it != interface_bases.rend(); ++it) {
                if (*it == nullptr) continue;
                if (llvm::LLVMValueRef dtor = find_destructor((*it)->name)) {
                    build_call(dtor, {object_ptr});
                }
            }
        }
    }


    llvm::LLVMValueRef Codegen::create_moved_flag_if_has_destructor(const std::string& class_name)
{
        if (!class_has_destructor_in_chain(class_name)) return nullptr;
        llvm::LLVMValueRef flag = create_entry_block_alloca(llvm::LLVMInt1TypeInContext(context_), "movedflag");
        llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, /*SignExtend=*/0), flag);
        return flag;
    }


    void Codegen::codegen_call_destructor_chain_unless_moved(const std::string& class_name, llvm::LLVMValueRef object_ptr,
                                                    llvm::LLVMValueRef moved_flag)
{
        if (moved_flag == nullptr) {
            emit_destructor_chain_calls(class_name, object_ptr);
            return;
        }
        llvm::LLVMValueRef was_moved = llvm::LLVMBuildLoad2(builder_, llvm::LLVMInt1TypeInContext(context_), moved_flag, "wasmoved");
        llvm::LLVMValueRef current_fn = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
        llvm::LLVMBasicBlockRef then_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "dtorcall");
        llvm::LLVMBasicBlockRef merge_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "dtorskip");
        llvm::LLVMBuildCondBr(builder_, was_moved, merge_bb, then_bb);
        llvm::LLVMPositionBuilderAtEnd(builder_, then_bb);
        emit_destructor_chain_calls(class_name, object_ptr);
        llvm::LLVMBuildBr(builder_, merge_bb);
        llvm::LLVMPositionBuilderAtEnd(builder_, merge_bb);
    }


    void Codegen::codegen_destroy_old_class_state_for_move_assign(llvm::LLVMValueRef ptr, const std::string& class_name,
                                                         llvm::LLVMValueRef moved_flag)
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
            if (!type_needs_subobject_teardown(field_type)) continue;
            llvm::LLVMValueRef field_ptr = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, ptr, info.physical_field_index(i),
                                                         info.field_names[i].c_str());
            codegen_destroy_old_state_for_move_assign(field_type, field_ptr);
        }
    }


    [[nodiscard]] bool Codegen::type_has_destructor(const Type& type)
{
        if (type.kind == TypeKind::Array) {
            return type.element != nullptr && type.array_size > 0 && type_has_destructor(*type.element);
        }
        return type.kind == TypeKind::Named && class_has_destructor_in_chain(type.name);
    }


    [[nodiscard]] bool Codegen::type_needs_nontrivial_default_init(const Type& type)
{
        if (type.kind == TypeKind::Array) {
            return type.element != nullptr && type.array_size > 0 && type_needs_nontrivial_default_init(*type.element);
        }
        return type.kind == TypeKind::Named && find_class_def(type.name) != nullptr;
    }


    void Codegen::emit_storage_destruction(const Type& type, llvm::LLVMValueRef ptr)
{
        if (type.kind == TypeKind::Array) {
            if (!type_has_destructor(type)) return;
            // Reverse of construction order, exactly as for a derived
            // object's base subobjects.
            (void)emit_array_element_loop(type, ptr, /*reverse=*/true,
                                          [&](llvm::LLVMValueRef element_ptr, llvm::LLVMValueRef) -> std::expected<void, CodegenError> {
                                              emit_storage_destruction(*type.element, element_ptr);
                                              return {};
                                          });
            return;
        }
        if (type.kind != TypeKind::Named) return;
        emit_destructor_chain_calls(type.name, ptr);
    }


    void Codegen::codegen_destroy_storage_unless_moved(const Type& type, llvm::LLVMValueRef ptr, llvm::LLVMValueRef moved_flag)
{
        if (!type_has_destructor(type)) return;
        if (moved_flag == nullptr) {
            emit_storage_destruction(type, ptr);
            return;
        }
        llvm::LLVMValueRef was_moved = llvm::LLVMBuildLoad2(builder_, llvm::LLVMInt1TypeInContext(context_), moved_flag, "wasmoved");
        llvm::LLVMValueRef current_fn = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
        llvm::LLVMBasicBlockRef then_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "dtorcall");
        llvm::LLVMBasicBlockRef merge_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "dtorskip");
        llvm::LLVMBuildCondBr(builder_, was_moved, merge_bb, then_bb);
        llvm::LLVMPositionBuilderAtEnd(builder_, then_bb);
        emit_storage_destruction(type, ptr);
        llvm::LLVMBuildBr(builder_, merge_bb);
        llvm::LLVMPositionBuilderAtEnd(builder_, merge_bb);
    }


    void Codegen::codegen_destroy_old_state_for_move_assign(const Type& type, llvm::LLVMValueRef ptr, llvm::LLVMValueRef moved_flag)
{
        if (type.kind == TypeKind::Array) {
            if (!type_has_destructor(type)) return;
            (void)emit_array_element_loop(type, ptr, /*reverse=*/true,
                                          [&](llvm::LLVMValueRef element_ptr, llvm::LLVMValueRef) -> std::expected<void, CodegenError> {
                                              codegen_destroy_old_state_for_move_assign(*type.element, element_ptr);
                                              return {};
                                          });
            return;
        }
        if (type.kind != TypeKind::Named) return;
        codegen_destroy_old_class_state_for_move_assign(ptr, type.name, moved_flag);
    }


    llvm::LLVMValueRef Codegen::create_moved_flag_if_type_has_destructor(const Type& type)
{
        if (!type_has_destructor(type)) return nullptr;
        llvm::LLVMValueRef flag = create_entry_block_alloca(llvm::LLVMInt1TypeInContext(context_), "movedflag");
        llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, /*SignExtend=*/0), flag);
        return flag;
    }


    [[nodiscard]] bool Codegen::type_needs_subobject_teardown(const Type& type)
{
        // Deliberately broader than type_has_destructor for a Named type:
        // a record field whose own class has *no* destructor can still own
        // subobjects that must be released (a std::unique_ptr member, or a
        // nested record that in turn owns one), and
        // codegen_destroy_old_state_for_move_assign recurses into exactly
        // that flattened layout. Answering "no" here for such a field would
        // leak it.
        if (type.kind == TypeKind::Array) return type_has_destructor(type);
        return type.kind == TypeKind::Named && structs_.contains(type.name);
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::emit_class_member_teardown(const std::string& class_name,
                                                                                        llvm::LLVMValueRef object_ptr)
{
        auto info_it = structs_.find(class_name);
        if (info_it == structs_.end()) {
            return std::unexpected(CodegenError("destructor of unknown class '" + class_name + "'", current_loc_));
        }
        const StructInfo& info = info_it->second;
        // A derived class's StructInfo carries its base's fields flattened
        // in front of its own (declare_class copies base_info.field_names/
        // field_types, then appends `def.fields`), so the class's own
        // members are exactly the last def.fields.size() entries. Walking
        // the whole vector here would destroy every inherited member a
        // second time, because the base's own destructor -- which the
        // caller reaches through emit_destructor_chain_calls -- already
        // destroys them. This mirrors construction, where
        // emit_constructor_member_initializers walks class_def->fields
        // (own fields only) and delegates the base subobject to the base
        // constructor. Note the deliberate contrast with
        // codegen_destroy_old_class_state_for_move_assign, which *does*
        // walk the flattened layout: it only runs when no class in the
        // chain has a destructor, so no base destructor exists to reach
        // the inherited fields and it must reach them itself.
        const ClassDef* class_def = find_class_def(class_name);
        std::size_t own_field_count = class_def != nullptr ? class_def->fields.size() : info.field_types.size();
        if (own_field_count > info.field_types.size()) own_field_count = info.field_types.size();
        std::size_t first_own = info.field_types.size() - own_field_count;
        for (std::size_t i = info.field_types.size(); i > first_own; --i) {
            const Type& field_type = info.field_types[i - 1];
            if (!type_needs_subobject_teardown(field_type)) continue;
            llvm::LLVMValueRef field_ptr = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, object_ptr,
                                                                     info.physical_field_index(i - 1),
                                                                     info.field_names[i - 1].c_str());
            codegen_destroy_old_state_for_move_assign(field_type, field_ptr);
        }
        return {};
    }

} // namespace scpp
