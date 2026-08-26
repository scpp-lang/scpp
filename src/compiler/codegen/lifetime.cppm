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
        // A braced list has no value to load and store: it initializes
        // the temporary in place, which is what makes `f({1, 2})` work
        // for a `const S&` parameter. Handled before the rvalue path
        // below because that one goes through codegen_expr, which a
        // list -- having no type of its own -- cannot go through.
        if (expr.kind == ExprKind::BracedInitList) {
            auto llvm_type_result = to_llvm_type(target_type);
            if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
            llvm::LLVMValueRef temp = create_entry_block_alloca(std::move(llvm_type_result).value(), "listreftmp");
            LValue target{temp, target_type, alignment_for_type(target_type)};
            if (auto r = initialize_storage_from_brace_args(target, expr.args); !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
            return temp;
        }
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
        std::optional<std::size_t> index = find_destructor_function_index(class_name, *program_);
        if (!index.has_value()) return nullptr;
        const Function* fn = &program_->functions[*index];
        return llvm::LLVMGetNamedFunction(module_, overload_names_.at(fn).c_str());
    }


    [[nodiscard]] const Function* Codegen::find_destructor_ast(const std::string& class_name) const
{
        std::optional<std::size_t> index = find_destructor_function_index(class_name, *program_);
        if (!index.has_value()) return nullptr;
        return &program_->functions[*index];
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
            if (fn.member_owner_class != class_name || fn.params.size() != 2) continue;
            if (!is_constructor_function(fn)) continue;
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


    // Deliberately *no* reference-member exception -- see movecheck's
    // is_copy_constructible, whose answer this one must agree with:
    // §6.4(3)/§6.5(3) carve out a reference member because it cannot be
    // re-seated by assignment; §6.4(2)/§6.5(2) do not, because copy
    // construction binds it once.
    [[nodiscard]] bool Codegen::is_copy_constructible(const std::string& class_name)
{
        if (const Function* user_copy = find_user_declared_copy_ctor_ast(class_name); user_copy != nullptr) {
            // [dcl.fct.def.delete]/1, mirroring movecheck's is_copy_constructible.
            return !user_copy->is_deleted;
        }
        if (find_user_declared_copy_assign_ast(class_name) != nullptr) {
            return false;
        }
        auto it = structs_.find(class_name);
        if (it == structs_.end()) return false;
        if (has_user_declared_dtor(class_name)) return false;
        for (const Type& field_type : it->second.field_types) {
            if (!is_field_copy_constructible(field_type)) return false;
        }
        return true;
    }


    [[nodiscard]] bool Codegen::is_copy_assignable(const std::string& class_name)
{
        if (const Function* user_assign = find_user_declared_copy_assign_ast(class_name); user_assign != nullptr) {
            return !user_assign->is_deleted;
        }
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
            type_needs_elementwise_copy(field_type)) {
            auto array_llvm_type_result = to_llvm_type(field_type);
            if (!array_llvm_type_result.has_value()) return std::unexpected(std::move(array_llvm_type_result).error());
            llvm::LLVMTypeRef array_llvm_type = std::move(array_llvm_type_result).value();
            return emit_array_element_loop(
                field_type, dest_ptr, /*reverse=*/false, /*begin_index=*/0,
                [&, this](llvm::LLVMValueRef dest_element, llvm::LLVMValueRef index) -> std::expected<void, CodegenError> {
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
            type_needs_elementwise_copy(field_type)) {
            auto array_llvm_type_result = to_llvm_type(field_type);
            if (!array_llvm_type_result.has_value()) return std::unexpected(std::move(array_llvm_type_result).error());
            llvm::LLVMTypeRef array_llvm_type = std::move(array_llvm_type_result).value();
            return emit_array_element_loop(
                field_type, dest_ptr, /*reverse=*/false, /*begin_index=*/0,
                [&, this](llvm::LLVMValueRef dest_element, llvm::LLVMValueRef index) -> std::expected<void, CodegenError> {
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


    [[nodiscard]] bool Codegen::record_needs_teardown(const std::string& record_name)
{
        return scpp::record_needs_teardown(record_name, *program_);
    }


    [[nodiscard]] bool Codegen::class_has_destructor_in_chain(const std::string& class_name)
{
        return class_destruction_chain_has_destructor(class_name, *program_);
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
                                                         llvm::LLVMValueRef moved_flag, const Place* place)
{
        if (class_has_destructor_in_chain(class_name)) {
            // The destructor is another function; it cannot be told to
            // skip one member. The move checker knows that (see
            // place_teardown_is_emitted_here) and refuses to leave a
            // subobject under such an object moved out at teardown, so
            // there is never a descendant flag to honour here.
            codegen_call_destructor_chain_unless_moved(class_name, ptr, moved_flag);
            return;
        }
        auto struct_it = structs_.find(class_name);
        if (struct_it == structs_.end()) return;
        const StructInfo& info = struct_it->second;
        (void)get_or_declare_free();
        // A record with no destructor of its own is torn down by walking
        // its fields here, so "this whole object was moved out" has to be
        // honoured here too -- the destructor-chain branch above is the
        // only place that used to consume `moved_flag`, which left a
        // moved-out struct's owning members released anyway.
        emit_unless_moved(moved_flag, [&, this] {
            for (std::size_t i = 0; i < info.field_types.size(); i++) {
                const Type& field_type = info.field_types[i];
                if (!type_needs_subobject_teardown(field_type)) continue;
                llvm::LLVMValueRef field_ptr = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, ptr, info.physical_field_index(i),
                                                             info.field_names[i].c_str());
                if (place != nullptr) {
                    Place field_place = projected_field(*place, info.field_names[i]);
                    codegen_destroy_old_state_for_move_assign(field_type, field_ptr, /*moved_flag=*/nullptr, &field_place);
                    continue;
                }
                codegen_destroy_old_state_for_move_assign(field_type, field_ptr);
            }
        });
    }


    [[nodiscard]] bool Codegen::type_has_destructor(const Type& type)
{
        return type_needs_teardown(type, *program_);
    }


    [[nodiscard]] bool Codegen::type_needs_nontrivial_default_init(const Type& type)
{
        if (type.kind == TypeKind::Array) {
            return type.element != nullptr && type.array_size > 0 && type_needs_nontrivial_default_init(*type.element);
        }
        // Spec §6.1(5): "Because a reference has no empty state, a
        // constructor for a class or struct with a reference member is
        // ill-formed unless that member is initialized ...". Value-
        // initializing a reference member is therefore not the no-op a
        // zero fill would make it -- it is ill-formed, which is what
        // initialize_storage_from_brace_args says when it is handed no
        // initializer for a Reference target. Answering "false" here
        // skipped that call and left the member holding a null.
        if (type.kind == TypeKind::Reference) return true;
        if (type.kind != TypeKind::Named) return false;
        if (find_class_def(type.name) != nullptr) return true;
        // A struct's default member initializers are as real as a
        // class's; they are simply not reachable through a ClassDef. The
        // recursion matters because a struct field need not carry an
        // initializer of its own: `struct A { int x = 7; }; struct B { A a; };`
        // makes B non-trivial to default-initialize even though nothing
        // is written on `a`. A struct cannot contain itself, so this
        // terminates.
        if (const StructDef* struct_def = find_struct_def(type.name)) {
            for (const StructField& field : struct_def->fields) {
                if (field.default_initializer.has_value()) return true;
                if (type_needs_nontrivial_default_init(field.type)) return true;
            }
        }
        return false;
    }


    [[nodiscard]] bool Codegen::type_needs_elementwise_copy(const Type& type)
{
        if (type.kind == TypeKind::Array) {
            return type.element != nullptr && type.array_size > 0 && type_needs_elementwise_copy(*type.element);
        }
        return type.kind == TypeKind::Named && find_class_def(type.name) != nullptr;
    }


    void Codegen::emit_storage_destruction(const Type& type, llvm::LLVMValueRef ptr, const Place* place)
{
        // The guard for `place` itself is emitted here and nowhere else.
        // Every other teardown entry point funnels through this one, so
        // consulting the flag in more than one of them produced two
        // nested branches on the same i1.
        emit_unless_moved(place != nullptr ? moved_flag_for_place(*place) : nullptr,
                          [&, this] { emit_storage_destruction_unguarded(type, ptr, place); });
    }


    void Codegen::emit_storage_destruction_unguarded(const Type& type, llvm::LLVMValueRef ptr, const Place* place)
{
        if (type.kind == TypeKind::Array) {
            if (!type_has_destructor(type)) return;
            // A moved-out element has to be stepped over, and the runtime
            // induction variable of emit_array_element_loop cannot be
            // compared against a projection path. Move state only ever
            // records constant subscripts (place_of declines the rest),
            // so where an element actually was moved out the bound is a
            // compile-time constant and the loop can be unrolled --
            // giving each element its own place. Every other array keeps
            // the loop, unchanged.
            if (place != nullptr && place_has_moved_descendants(*place)) {
                (void)emit_unrolled_array_teardown(type, ptr, *place, /*old_state=*/false);
                return;
            }
            // Reverse of construction order, exactly as for a derived
            // object's base subobjects.
            (void)emit_array_element_loop(type, ptr, /*reverse=*/true, /*begin_index=*/0,
                                          [&, this](llvm::LLVMValueRef element_ptr, llvm::LLVMValueRef) -> std::expected<void, CodegenError> {
                                              emit_storage_destruction(*type.element, element_ptr);
                                              return {};
                                          });
            return;
        }
        if (type.kind != TypeKind::Named) return;
        // Not emit_destructor_chain_calls directly: a record with no
        // destructor of its own still has to release whatever its members
        // own (spec §6.3 destroys every subobject). A `class` always
        // reaches its members through the virtual destructor spec §11.5(1)
        // makes it declare, so this only ever mattered for a `struct` --
        // and asking about the destructor alone left a struct's
        // record-typed members never destroyed at all.
        codegen_destroy_old_class_state_for_move_assign(ptr, type.name, /*moved_flag=*/nullptr, place);
    }


    void Codegen::codegen_destroy_storage_unless_moved(const Type& type, llvm::LLVMValueRef ptr, llvm::LLVMValueRef moved_flag,
                                                       const Place* place)
{
        if (!type_has_destructor(type)) return;
        // An explicitly supplied flag is the caller's own; a place-derived
        // one belongs to emit_storage_destruction, which consults it.
        if (moved_flag == nullptr) {
            emit_storage_destruction(type, ptr, place);
            return;
        }
        emit_unless_moved(moved_flag, [&, this] { emit_storage_destruction_unguarded(type, ptr, place); });
    }


    void Codegen::codegen_destroy_old_state_for_move_assign(const Type& type, llvm::LLVMValueRef ptr, llvm::LLVMValueRef moved_flag,
                                                            const Place* place)
{
        if (moved_flag == nullptr && place != nullptr) moved_flag = moved_flag_for_place(*place);
        if (type.kind == TypeKind::Array) {
            if (!type_has_destructor(type)) return;
            if (place != nullptr && place_has_moved_descendants(*place)) {
                emit_unless_moved(moved_flag, [&, this] {
                    (void)emit_unrolled_array_teardown(type, ptr, *place, /*old_state=*/true);
                });
                return;
            }
            (void)emit_array_element_loop(type, ptr, /*reverse=*/true, /*begin_index=*/0,
                                          [&, this](llvm::LLVMValueRef element_ptr, llvm::LLVMValueRef) -> std::expected<void, CodegenError> {
                                              codegen_destroy_old_state_for_move_assign(*type.element, element_ptr);
                                              return {};
                                          });
            return;
        }
        if (type.kind != TypeKind::Named) return;
        codegen_destroy_old_class_state_for_move_assign(ptr, type.name, moved_flag, place);
    }


    llvm::LLVMValueRef Codegen::create_moved_flag_if_type_has_destructor(const Type& type)
{
        if (!type_has_destructor(type)) return nullptr;
        llvm::LLVMValueRef flag = create_entry_block_alloca(llvm::LLVMInt1TypeInContext(context_), "movedflag");
        llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, /*SignExtend=*/0), flag);
        return flag;
    }


    llvm::LLVMValueRef Codegen::create_zeroed_flag_in_entry_block(const std::string& name)
{
        llvm::LLVMTypeRef i1 = llvm::LLVMInt1TypeInContext(context_);
        llvm::LLVMValueRef flag = create_entry_block_alloca(i1, name);
        llvm::LLVMValueRef zero = llvm::LLVMConstInt(i1, 0, /*SignExtend=*/0);
        llvm::LLVMBasicBlockRef current_block = llvm::LLVMGetInsertBlock(builder_);
        if (current_block == nullptr) {
            llvm::LLVMBuildStore(builder_, zero, flag);
            return flag;
        }
        // The `false` has to dominate every teardown that reads the
        // flag, and a flag for a subobject is created where that
        // subobject is first moved -- which may be inside a branch. Put
        // the initialization next to the alloca, in the entry block.
        llvm::LLVMBasicBlockRef entry = llvm::LLVMGetEntryBasicBlock(llvm::LLVMGetBasicBlockParent(current_block));
        llvm::LLVMMetadataRef saved_dbg = llvm::LLVMGetCurrentDebugLocation2(builder_);
        llvm::LLVMValueRef insert_before = llvm::LLVMGetNextInstruction(flag);
        if (insert_before != nullptr) {
            llvm::LLVMPositionBuilderBefore(builder_, insert_before);
        } else {
            llvm::LLVMPositionBuilderAtEnd(builder_, entry);
        }
        llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        llvm::LLVMBuildStore(builder_, zero, flag);
        llvm::LLVMPositionBuilderAtEnd(builder_, current_block);
        llvm::LLVMSetCurrentDebugLocation2(builder_, saved_dbg);
        return flag;
    }


    void Codegen::emit_unless_moved(llvm::LLVMValueRef moved_flag, const std::function<void()>& body)
{
        if (moved_flag == nullptr) {
            body();
            return;
        }
        llvm::LLVMValueRef was_moved = llvm::LLVMBuildLoad2(builder_, llvm::LLVMInt1TypeInContext(context_), moved_flag, "wasmoved");
        llvm::LLVMValueRef current_fn = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
        llvm::LLVMBasicBlockRef then_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "dtorcall");
        llvm::LLVMBasicBlockRef merge_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "dtorskip");
        llvm::LLVMBuildCondBr(builder_, was_moved, merge_bb, then_bb);
        llvm::LLVMPositionBuilderAtEnd(builder_, then_bb);
        body();
        llvm::LLVMBuildBr(builder_, merge_bb);
        llvm::LLVMPositionBuilderAtEnd(builder_, merge_bb);
    }


    std::expected<void, CodegenError> Codegen::emit_unrolled_array_teardown(const Type& type, llvm::LLVMValueRef ptr,
                                                                            const Place& place, bool old_state)
{
        auto llvm_type_result = to_llvm_type(type);
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        llvm::LLVMTypeRef array_llvm_type = std::move(llvm_type_result).value();
        llvm::LLVMTypeRef i64 = llvm::LLVMInt64TypeInContext(context_);
        for (std::int64_t i = type.array_size; i > 0; i--) {
            Place element = projected_index(place, i - 1);
            llvm::LLVMValueRef element_ptr = build_array_element_gep(
                array_llvm_type, ptr, llvm::LLVMConstInt(i64, static_cast<unsigned long long>(i - 1), /*SignExtend=*/0));
            if (old_state) {
                codegen_destroy_old_state_for_move_assign(*type.element, element_ptr, /*moved_flag=*/nullptr, &element);
            } else {
                codegen_destroy_storage_unless_moved(*type.element, element_ptr, /*moved_flag=*/nullptr, &element);
            }
        }
        return {};
    }


    [[nodiscard]] llvm::LLVMValueRef Codegen::moved_flag_for_place(const Place& place)
{
        auto it = locals_.find(place.local);
        if (it == locals_.end()) return nullptr;
        return it->second.moved_flag_for(place.path);
    }


    [[nodiscard]] bool Codegen::place_has_moved_descendants(const Place& place)
{
        auto it = locals_.find(place.local);
        if (it == locals_.end()) return false;
        for (const MovedFlag& entry : it->second.moved_flags) {
            Place candidate{place.local, entry.path};
            if (candidate.is_strictly_under(place)) return true;
        }
        return false;
    }


    void Codegen::set_place_moved(const Place& place, const Type& place_type)
{
        // Only a place with something to release needs a bit: the bit
        // exists solely so teardown can skip it (spec §6.3(1)).
        if (!type_has_destructor(place_type) && !type_needs_subobject_teardown(place_type)) return;
        auto it = locals_.find(place.local);
        if (it == locals_.end()) return;
        LocalSlot& slot = it->second;
        llvm::LLVMValueRef flag = slot.moved_flag_for(place.path);
        if (flag == nullptr) {
            flag = create_zeroed_flag_in_entry_block("movedflag");
            slot.set_moved_flag(place.path, flag);
        }
        llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 1, /*SignExtend=*/0), flag);
        // Moving a whole object moves everything under it; a strict
        // descendant's own bit would otherwise stay behind and guard a
        // teardown that no longer happens. Mirrors the move checker's
        // mark_place_moved_out, which erases the same entries.
        clear_strict_descendant_flags(place);
    }


    void Codegen::clear_place_moved(const Place& place)
{
        auto it = locals_.find(place.local);
        if (it == locals_.end()) return;
        llvm::LLVMValueRef flag = it->second.moved_flag_for(place.path);
        if (flag != nullptr) {
            llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, /*SignExtend=*/0), flag);
        }
        clear_strict_descendant_flags(place);
    }


    void Codegen::clear_strict_descendant_flags(const Place& place)
{
        auto it = locals_.find(place.local);
        if (it == locals_.end()) return;
        llvm::LLVMValueRef zero = llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, /*SignExtend=*/0);
        for (const MovedFlag& entry : it->second.moved_flags) {
            Place candidate{place.local, entry.path};
            if (!candidate.is_strictly_under(place)) continue;
            if (entry.flag != nullptr) llvm::LLVMBuildStore(builder_, zero, entry.flag);
        }
    }


    [[nodiscard]] std::optional<Place> Codegen::codegen_place_of(const Expr& expr)
{
        std::optional<Place> place = place_of(expr, [this](const Expr& e) -> std::optional<LocalId> {
            if (!has_resolved_local(e)) return std::optional<LocalId>{};
            LocalId id = resolved_local_of(e);
            if (!locals_.contains(id)) return std::optional<LocalId>{};
            return id;
        });
        if (!place.has_value()) return place;
        // Splice a reference root onto what it was bound to, so the place
        // names the object rather than the binding. Bounded by the number
        // of reference locals, which cannot form a cycle: a reference is
        // bound at its declaration, so its target is always declared
        // before it.
        std::size_t hops = 0;
        while (hops < reference_bound_places_.size() + 1) {
            auto it = reference_bound_places_.find(place->local);
            if (it == reference_bound_places_.end()) break;
            Place spliced = it->second;
            spliced.path.insert(spliced.path.end(), place->path.begin(), place->path.end());
            place = std::move(spliced);
            hops++;
        }
        return place;
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
