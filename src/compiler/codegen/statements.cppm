module;

module scpp.compiler.codegen:statements;

import std;
import llvm;
import :api;

namespace scpp {

    [[nodiscard]] std::expected<void, CodegenError> Codegen::codegen_stmt(const Stmt& stmt, llvm::LLVMValueRef current_function)
{
        // Refreshed on every call (including each recursive call for a
        // nested statement) so a CodegenError thrown while handling
        // `stmt` points at `stmt` itself -- see current_loc_ and
        // codegen_expr's identical opening comment.
        refresh_debug_location(stmt.loc);
        switch (stmt.kind) {
            case StmtKind::Block:
                push_scope();
                // ch01 §1.3 / ch05 §5.8: an `unsafe { }` block raises the
                // depth counter for its own statements (and anything
                // transitively nested inside it) so codegen_binary knows
                // to emit plain, guaranteed-wrapping arithmetic instead of
                // the overflow-checked form -- mirrors movecheck's own
                // UnsafeEnter/UnsafeExit MIR statements.
                if (stmt.is_unsafe) unsafe_depth_++;
                for (const auto& s : stmt.statements) {
                    // Once a block has a terminator (return), skip anything
                    // after it: unreachable code shouldn't be lowered.
                    if (llvm::LLVMGetBasicBlockTerminator(llvm::LLVMGetInsertBlock(builder_)) != nullptr) break;
                    if (auto r = codegen_stmt(*s, current_function); !r.has_value()) return std::unexpected(std::move(r).error());
                }
                if (stmt.is_unsafe) unsafe_depth_--;
                pop_scope();
                return {};

            case StmtKind::VarDecl:
                return [&, this]() -> std::expected<void, CodegenError> {
                    // Every declaration is resolved (resolve_program_locals runs
                    // over the whole program before this point, and again after
                    // monomorphization synthesizes new functions), so an
                    // unresolved one means resolution was skipped for this body
                    // -- fail here rather than silently binding storage to a
                    // nonsense id that no use will ever match.
                    if (!has_declared_local(stmt)) {
                        return std::unexpected(CodegenError(
                            "internal error: declaration of '" + stmt.var_name + "' was never resolved to a local", stmt.loc));
                    }
                    if (auto r = validate_type_is_inhabitable(stmt.type, "local variable '" + stmt.var_name + "'");
                        !r.has_value()) {
                        return std::unexpected(std::move(r).error());
                    }
                    std::optional<unsigned> declared_alignment = alignment_for_type(stmt.type);
                    if (stmt.resolved_alignment != 0) {
                        unsigned explicit_align = stmt.resolved_alignment;
                        if (!declared_alignment.has_value() || explicit_align > *declared_alignment) {
                            declared_alignment = explicit_align;
                        }
                    }
                    if (stmt.is_static_local) {
                        if (current_function_def_ == nullptr) {
                            return std::unexpected(CodegenError("internal error: function-local static outside a function body", current_loc_));
                        }
                        auto add_internal_global = [&, this](llvm::LLVMTypeRef llvm_type, const std::string& name,
                                                       llvm::LLVMValueRef init) {
                            llvm::LLVMValueRef global = llvm::LLVMAddGlobal(module_, llvm_type, name.c_str());
                            llvm::LLVMSetLinkage(global, llvm::LLVMInternalLinkage);
                            llvm::LLVMSetGlobalConstant(global, /*IsConstant=*/0);
                            llvm::LLVMSetInitializer(global, init);
                            return global;
                        };
                        auto get_or_declare_runtime_fn = [&, this](const std::string& name, llvm::LLVMTypeRef fn_type) {
                            llvm::LLVMValueRef fn = llvm::LLVMGetNamedFunction(module_, name.c_str());
                            if (fn == nullptr) fn = llvm::LLVMAddFunction(module_, name.c_str(), fn_type);
                            return fn;
                        };
                        auto define_static_local_destructor_helper =
                            [&, this](const std::string& helper_name, llvm::LLVMValueRef storage,
                                llvm::LLVMValueRef moved_flag) -> llvm::LLVMValueRef {
                            llvm::LLVMTypeRef fn_type =
                                llvm::LLVMFunctionType(llvm::LLVMVoidTypeInContext(context_), nullptr, 0, /*IsVarArg=*/0);
                            llvm::LLVMValueRef fn = llvm::LLVMGetNamedFunction(module_, helper_name.c_str());
                            if (fn != nullptr) return fn;
                            fn = llvm::LLVMAddFunction(module_, helper_name.c_str(), fn_type);
                            llvm::LLVMSetLinkage(fn, llvm::LLVMInternalLinkage);
                            llvm::LLVMBasicBlockRef saved_bb = llvm::LLVMGetInsertBlock(builder_);
                            const Function* saved_function = current_function_def_;
                            SourceLocation saved_loc = current_loc_;
                            llvm::LLVMBasicBlockRef entry =
                                llvm::LLVMAppendBasicBlockInContext(context_, fn, "entry");
                            llvm::LLVMPositionBuilderAtEnd(builder_, entry);
                            current_function_def_ = nullptr;
                            current_loc_ = stmt.loc;
                            codegen_destroy_storage_unless_moved(stmt.type, storage, moved_flag);
                            llvm::LLVMBuildRetVoid(builder_);
                            current_function_def_ = saved_function;
                            current_loc_ = saved_loc;
                            if (saved_bb != nullptr) llvm::LLVMPositionBuilderAtEnd(builder_, saved_bb);
                            refresh_debug_location(stmt.loc);
                            return fn;
                        };

                        std::string static_base = "__scpp_static_local." + overload_names_.at(current_function_def_) + "." +
                                                  stmt.var_name + "." + std::to_string(stmt.loc.line) + "." +
                                                  std::to_string(stmt.loc.column);
                        llvm::LLVMTypeRef storage_type;
                        if (stmt.type.kind == TypeKind::Reference) {
                            storage_type = llvm::LLVMPointerTypeInContext(context_, 0);
                        } else {
                            auto storage_type_result = to_llvm_type(stmt.type);
                            if (!storage_type_result.has_value()) return std::unexpected(std::move(storage_type_result).error());
                            storage_type = std::move(storage_type_result).value();
                        }
                        llvm::LLVMValueRef storage =
                            add_internal_global(storage_type, static_base + ".storage", llvm::LLVMConstNull(storage_type));
                        if (declared_alignment.has_value()) llvm::LLVMSetAlignment(storage, *declared_alignment);
                        llvm::LLVMTypeRef guard_type = llvm::LLVMInt64TypeInContext(context_);
                        llvm::LLVMValueRef guard = add_internal_global(
                            guard_type, static_base + ".guard",
                            llvm::LLVMConstInt(guard_type, 0, /*SignExtend=*/0));
                        llvm::LLVMValueRef moved_flag = nullptr;
                        if (type_has_destructor(stmt.type)) {
                            llvm::LLVMTypeRef moved_flag_type = llvm::LLVMInt1TypeInContext(context_);
                            moved_flag = add_internal_global(
                                moved_flag_type, static_base + ".moved",
                                llvm::LLVMConstInt(moved_flag_type, 0, /*SignExtend=*/0));
                        }

                        llvm::LLVMTypeRef guard_ptr_type = llvm::LLVMPointerTypeInContext(context_, 0);
                        llvm::LLVMTypeRef guard_acquire_params[] = {guard_ptr_type};
                        llvm::LLVMValueRef guard_acquire = get_or_declare_runtime_fn(
                            "__cxa_guard_acquire",
                            llvm::LLVMFunctionType(llvm::LLVMInt32TypeInContext(context_), guard_acquire_params, 1,
                                                   /*IsVarArg=*/0));
                        llvm::LLVMValueRef guard_release = get_or_declare_runtime_fn(
                            "__cxa_guard_release",
                            llvm::LLVMFunctionType(llvm::LLVMVoidTypeInContext(context_), guard_acquire_params, 1,
                                                   /*IsVarArg=*/0));
                        llvm::LLVMValueRef atexit_fn = get_or_declare_runtime_fn(
                            "atexit",
                            [&, this] {
                                llvm::LLVMTypeRef atexit_params[] = {guard_ptr_type};
                                return llvm::LLVMFunctionType(llvm::LLVMInt32TypeInContext(context_), atexit_params,
                                                              1, /*IsVarArg=*/0);
                            }());

                        llvm::LLVMValueRef acquired = build_call(guard_acquire, {guard});
                        llvm::LLVMValueRef should_init = llvm::LLVMBuildICmp(
                            builder_, llvm::LLVMIntNE, acquired,
                            llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), 0, /*SignExtend=*/0),
                            "staticguard");
                        llvm::LLVMValueRef current_fn = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
                        llvm::LLVMBasicBlockRef init_bb =
                            llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "static.init");
                        llvm::LLVMBasicBlockRef cont_bb =
                            llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "static.cont");
                        llvm::LLVMBuildCondBr(builder_, should_init, init_bb, cont_bb);

                        llvm::LLVMPositionBuilderAtEnd(builder_, init_bb);
                        refresh_debug_location(stmt.loc);
                        if (stmt.type.kind == TypeKind::Reference) {
                            if (!stmt.init) {
                                return std::unexpected(CodegenError("reference '" + stmt.var_name +
                                                       "' must be initialized (bound to a variable) at declaration",
                                    current_loc_));
                            }
                            if (auto r = validate_reference_pointee(*stmt.type.pointee); !r.has_value()) return std::unexpected(std::move(r).error());
                            llvm::LLVMValueRef referent_addr;
                            if (!stmt.type.is_mutable_ref && produces_rvalue_of_type(*stmt.init, *stmt.type.pointee)) {
                                auto addr_result = codegen_materialize_rvalue_reference_source(*stmt.init);
                                if (!addr_result.has_value()) return std::unexpected(std::move(addr_result).error());
                                referent_addr = std::move(addr_result).value();
                            } else {
                                auto lvalue_result = codegen_lvalue(*stmt.init);
                                if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                                referent_addr = std::move(lvalue_result).value().ptr;
                            }
                            llvm::LLVMBuildStore(builder_, referent_addr, storage);
                        } else if (stmt.type.kind == TypeKind::Span) {
                            if (!stmt.init) {
                                return std::unexpected(CodegenError("span '" + stmt.var_name +
                                                       "' must be initialized (bound to an array) at declaration",
                                    current_loc_));
                            }
                            auto span_value_result = codegen_span_value_for_target(*stmt.init, stmt.type);
                            if (!span_value_result.has_value()) return std::unexpected(std::move(span_value_result).error());
                            llvm::LLVMValueRef span_value = std::move(span_value_result).value();
                            llvm::LLVMBuildStore(builder_, span_value, storage);
                        } else if (stmt.init && stmt.init->kind == ExprKind::Lambda) {
                            if (auto r = codegen_construct_lambda(*stmt.init, storage); !r.has_value()) return std::unexpected(std::move(r).error());
                        } else if (stmt.has_ctor_args) {
                            if (auto r = zero_initialize_storage(storage, stmt.type, declared_alignment); !r.has_value()) return std::unexpected(std::move(r).error());
                            if (stmt.type.kind != TypeKind::Named || !structs_.contains(stmt.type.name)) {
                                if (auto r = initialize_storage_from_brace_args(LValue{storage, stmt.type, declared_alignment}, stmt.ctor_args); !r.has_value()) return std::unexpected(std::move(r).error());
                            } else {
                                auto same_type_result = try_initialize_class_storage_from_same_type_source(
                                           LValue{storage, stmt.type, declared_alignment}, stmt.ctor_args);
                                if (!same_type_result.has_value()) return std::unexpected(std::move(same_type_result).error());
                                if (!std::move(same_type_result).value()) {
                                std::string ctor_name = stmt.type.name + "_new";
                                const Function* ctor_def = stmt.type.name == "std::thread"
                                                               ? resolve_constructor_overload_exact(stmt.type.name, stmt.ctor_args)
                                                               : resolve_overload_by_type(ctor_name, stmt.ctor_args, /*param_offset=*/1);
                                if (ctor_def == nullptr) {
                                    if (stmt.ctor_args.empty() &&
                                        record_is_implicitly_default_initializable(stmt.type.name)) {
                                        if (auto r = emit_default_initializers_for_record_storage(
                                            storage, stmt.type.name, /*initialize_virtual_interface_bases=*/true); !r.has_value()) return std::unexpected(std::move(r).error());
                                    } else if (stmt.ctor_args.empty() && find_class_def(stmt.type.name) == nullptr &&
                                               find_struct_def(stmt.type.name) == nullptr) {
                                    } else if (!stmt.ctor_args.empty() && find_struct_def(stmt.type.name) != nullptr &&
                                               find_class_def(stmt.type.name) == nullptr) {
                                    // A braced list on a struct is
                                    // handed to the one routine that
                                    // decides what it means -- copy,
                                    // constructor, or [dcl.init.aggr]
                                    // aggregate initialization -- for
                                    // every other position a braced list
                                    // appears in. Deciding it here too
                                    // would be a second copy of that rule.
                                        if (auto r = initialize_storage_from_brace_args(
                                                LValue{storage, stmt.type, declared_alignment}, stmt.ctor_args); !r.has_value())
                                            return std::unexpected(std::move(r).error());
                                    } else {
                                        return std::unexpected(CodegenError(
                                            describe_constructor_resolution_failure(stmt.type.name, stmt.ctor_args),
                                            current_loc_));
                                    }
                                } else if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                                    auto value_result =
                                        codegen_constructed_class_value(stmt.type.name, stmt.ctor_args, ctor_def);
                                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                                    create_store(std::move(value_result).value(), storage, declared_alignment);
                                } else {
                                    llvm::LLVMValueRef ctor =
                                        llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
                                    if (ctor == nullptr) {
                                        return std::unexpected(CodegenError("class '" + stmt.type.name + "' has no constructor matching this call",
                                            current_loc_));
                                    }
                                    auto args_result = emit_constructor_arguments_and_virtual_bases(
                                        stmt.type.name, ctor_def, stmt.ctor_args, storage);
                                    if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                                    std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                                    args.insert(args.begin(), storage);
                                    build_call(ctor, args);
                                }
                                }
                            }
                        } else if (stmt.init) {
                            if (stmt.type.kind == TypeKind::Named && structs_.contains(stmt.type.name) &&
                                stmt.init->kind == ExprKind::Identifier) {
                                const Expr& source_expr =
                                    *stmt.init;
                                auto src_result = codegen_lvalue(source_expr);
                                if (!src_result.has_value()) return std::unexpected(std::move(src_result).error());
                                LValue src = std::move(src_result).value();
                                if (const Function* user_ctor = find_user_declared_copy_ctor_ast(stmt.type.name)) {
                                    llvm::LLVMValueRef ctor =
                                        llvm::LLVMGetNamedFunction(module_, overload_names_.at(user_ctor).c_str());
                                    build_call(ctor, {storage, src.ptr});
                                } else {
                                    if (auto r = codegen_memberwise_copy_construct(storage, src.ptr, stmt.type.name); !r.has_value()) return std::unexpected(std::move(r).error());
                                }
                            } else if (stmt.init->kind == ExprKind::BracedInitList) {
                                // In place, not materialize-then-copy: an
                                // array or record target has no single
                                // loadable value (see the same dispatch in
                                // define_global_initializers).
                                LValue target{storage, stmt.type, declared_alignment};
                                if (auto r = initialize_storage_from_brace_args(target, stmt.init->args); !r.has_value())
                                    return std::unexpected(std::move(r).error());
                            } else {
                                auto init_value_result = codegen_value_for_target(*stmt.init, stmt.type);
                                if (!init_value_result.has_value()) return std::unexpected(std::move(init_value_result).error());
                                llvm::LLVMValueRef init_value = std::move(init_value_result).value();
                                refresh_debug_location(stmt.loc);
                                if (auto r = check_store_type(init_value, storage_type, "variable '" + stmt.var_name + "'"); !r.has_value()) return std::unexpected(std::move(r).error());
                                create_store(init_value, storage, declared_alignment);
                            }
                        } else if (stmt.type.kind == TypeKind::Named && structs_.contains(stmt.type.name)) {
                            if (auto r = zero_initialize_storage(storage, stmt.type, declared_alignment); !r.has_value()) return std::unexpected(std::move(r).error());
                            const ClassDef* class_def = find_class_def(stmt.type.name);
                            std::vector<ExprPtr> no_args;
                            std::string ctor_name = stmt.type.name + "_new";
                            const Function* ctor_def = resolve_overload_by_type(ctor_name, no_args, /*param_offset=*/1);
                            if (ctor_def != nullptr) {
                                llvm::LLVMValueRef ctor =
                                    llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
                                if (ctor == nullptr) {
                                    return std::unexpected(CodegenError("class '" + stmt.type.name + "' has no constructor matching this call",
                                        current_loc_));
                                }
                                if (auto r = emit_constructor_arguments_and_virtual_bases(stmt.type.name, ctor_def,
                                                                                          no_args, storage);
                                    !r.has_value()) return std::unexpected(std::move(r).error());
                                build_call(ctor, {storage});
                            } else if (record_is_implicitly_default_initializable(stmt.type.name)) {
                                if (auto r = emit_default_initializers_for_record_storage(
                                    storage, stmt.type.name, /*initialize_virtual_interface_bases=*/true); !r.has_value()) return std::unexpected(std::move(r).error());
                            } else if (class_def != nullptr) {
                                return std::unexpected(CodegenError(
                                    describe_constructor_resolution_failure(stmt.type.name, no_args), current_loc_));
                            }
                        } else {
                            if (auto r = initialize_storage_from_brace_args(LValue{storage, stmt.type, declared_alignment}, {}); !r.has_value()) return std::unexpected(std::move(r).error());
                        }

                        if (moved_flag != nullptr) {
                            llvm::LLVMBuildStore(builder_,
                                                 llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0), moved_flag);
                            llvm::LLVMValueRef dtor_helper = define_static_local_destructor_helper(
                                static_base + ".dtor", storage, moved_flag);
                            build_call(atexit_fn, {dtor_helper});
                        }
                        build_call(guard_release, {guard});
                        llvm::LLVMBuildBr(builder_, cont_bb);

                        llvm::LLVMPositionBuilderAtEnd(builder_, cont_bb);
                        refresh_debug_location(stmt.loc);
                        locals_[declared_local_of(stmt)] = LocalSlot{storage, stmt.type};
                        locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                        locals_[declared_local_of(stmt)].is_static_storage = true;
                        locals_[declared_local_of(stmt)].moved_flag = moved_flag;
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(declared_local_of(stmt));
                        }
                        return {};
                    }
                    if (stmt.type.kind == TypeKind::Reference) {
                        // Real C++ references must be bound at declaration
                        // (there's no such thing as a later-bound or
                        // "null" reference) -- unlike every other type, which
                        // zero-initializes when no initializer is given.
                        if (!stmt.init) {
                            return std::unexpected(CodegenError("reference '" + stmt.var_name +
                                                "' must be initialized (bound to a variable) at declaration",
                                current_loc_));
                        }
                        if (is_interface_reference_type(stmt.type)) {
                            auto slot_type_result = to_llvm_type(stmt.type);
                            if (!slot_type_result.has_value()) return std::unexpected(std::move(slot_type_result).error());
                            llvm::LLVMValueRef slot =
                                create_entry_block_alloca(std::move(slot_type_result).value(), stmt.var_name, declared_alignment);
                            auto interface_value_result = codegen_interface_value_for_target(*stmt.init, stmt.type);
                            if (!interface_value_result.has_value()) return std::unexpected(std::move(interface_value_result).error());
                            create_store(std::move(interface_value_result).value(), slot, alignment_for_type(stmt.type));
                            locals_[declared_local_of(stmt)] = LocalSlot{slot, stmt.type};
                            locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                            if (auto r = maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc); !r.has_value()) return std::unexpected(std::move(r).error());
                            if (!scope_stack_.empty()) {
                                scope_stack_.back().push_back(declared_local_of(stmt));
                            }
                            return {};
                        }
                        if (auto r = validate_reference_pointee(*stmt.type.pointee); !r.has_value()) return std::unexpected(std::move(r).error());
                        // ch05 §5.x: a *const* reference may bind directly to
                        // a fresh rvalue initializer (a literal, std::move/
                        // std::make_unique, a lambda literal, or a call not
                        // itself returning by reference) -- movecheck has
                        // already validated this (produces_rvalue_of_type,
                        // only ever for a non-mutable reference), so it only
                        // remains to materialize a temporary and use *its*
                        // address, exactly like codegen_call_args' identical
                        // handling of the same shapes for a reference call
                        // argument. Otherwise (the overwhelmingly common
                        // case): codegen_lvalue on the initializer gives the
                        // address directly, and also enforces it resolves to
                        // a real, addressable place (a plain variable, or a
                        // further member/subscript chain off one).
                        llvm::LLVMValueRef referent_addr;
                        if (!stmt.type.is_mutable_ref && produces_rvalue_of_type(*stmt.init, *stmt.type.pointee)) {
                            auto addr_result = codegen_materialize_rvalue_reference_source(*stmt.init);
                            if (!addr_result.has_value()) return std::unexpected(std::move(addr_result).error());
                            referent_addr = std::move(addr_result).value();
                        } else {
                            auto lvalue_result = codegen_lvalue(*stmt.init);
                            if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                            referent_addr = std::move(lvalue_result).value().ptr;
                        }
                        llvm::LLVMValueRef slot =
                            create_entry_block_alloca(llvm::LLVMPointerTypeInContext(context_, 0), stmt.var_name, declared_alignment);
                        llvm::LLVMBuildStore(builder_, referent_addr, slot);
                        locals_[declared_local_of(stmt)] = LocalSlot{slot, stmt.type};
                        locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                        if (auto r = maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc); !r.has_value()) return std::unexpected(std::move(r).error());
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(declared_local_of(stmt));
                        }
                        return {};
                    }

                    if (stmt.type.kind == TypeKind::Span) {
                        // Like a reference, a std::span<T> must be bound to a
                        // place at declaration -- v0.1 only supports
                        // constructing one from a fixed-size array (spec
                        // ch06/M6; std::vector doesn't exist yet).
                        if (!stmt.init) {
                            return std::unexpected(CodegenError("span '" + stmt.var_name +
                                                "' must be initialized (bound to an array) at declaration",
                                current_loc_));
                        }
                        auto span_type_result = to_llvm_type(stmt.type);
                        if (!span_type_result.has_value()) return std::unexpected(std::move(span_type_result).error());
                        llvm::LLVMTypeRef span_type = std::move(span_type_result).value();
                        auto span_value_result = codegen_span_value_for_target(*stmt.init, stmt.type);
                        if (!span_value_result.has_value()) return std::unexpected(std::move(span_value_result).error());
                        llvm::LLVMValueRef span_value = std::move(span_value_result).value();
                        llvm::LLVMValueRef slot = create_entry_block_alloca(span_type, stmt.var_name, declared_alignment);
                        llvm::LLVMBuildStore(builder_, span_value, slot);
                        locals_[declared_local_of(stmt)] = LocalSlot{slot, stmt.type};
                        locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                        if (auto r = maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc); !r.has_value()) return std::unexpected(std::move(r).error());
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(declared_local_of(stmt));
                        }
                        return {};
                    }

                    // ch05 §5.12: `auto f = [...];` -- the only spelling
                    // that gives a class-typed VarDecl a plain `= expr`
                    // initializer rather than `ClassName name{args};`'s own
                    // constructor-call syntax (movecheck's closure-
                    // resolution pass gives a synthesized closure class no
                    // constructor at all). A Lambda literal's own codegen
                    // (codegen_construct_lambda) already allocates and fully
                    // populates its own fresh instance -- exactly the
                    // storage `f` itself should use -- so `f` is aliased
                    // directly to that address rather than allocating a
                    // *second*, separate slot and trying to copy into it
                    // (which would be wrong regardless: a class-typed
                    // value's own codegen representation is always its
                    // address, never a loadable/storable flat value, unlike
                    // every scalar/struct/array/pointer type the general
                    // path below handles).
                    if (stmt.init && stmt.init->kind == ExprKind::Lambda) {
                        auto closure_type_result = to_llvm_type(stmt.type);
                        if (!closure_type_result.has_value()) return std::unexpected(std::move(closure_type_result).error());
                        llvm::LLVMValueRef closure_ptr =
                            create_entry_block_alloca(std::move(closure_type_result).value(), stmt.var_name, declared_alignment);
                        if (auto r = codegen_construct_lambda(*stmt.init, closure_ptr); !r.has_value()) return std::unexpected(std::move(r).error());
                        locals_[declared_local_of(stmt)] = LocalSlot{closure_ptr, stmt.type};
                        locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                        if (auto r = maybe_emit_local_debug_decl(stmt.var_name, stmt.type, closure_ptr, stmt.loc); !r.has_value()) return std::unexpected(std::move(r).error());
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(declared_local_of(stmt));
                        }
                        return {};
                    }

                    auto llvm_type_result = to_llvm_type(stmt.type);
                    if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
                    llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
                    llvm::LLVMValueRef slot = create_entry_block_alloca(llvm_type, stmt.var_name, declared_alignment);
                    if (stmt.has_ctor_args) {
                        // `ClassName name{args};` (ch04 §4.2 / spec §6.1):
                        // direct-
                        // initialization via an explicit constructor call.
                        // Storage is zero-initialized first -- same as every
                        // other VarDecl with no initializer at all (scpp has
                        // no concept of "uninitialized" memory, ch05.4) --
                        // then the synthesized `ClassName_new(&name, args...)`
                        // constructor runs in place: the same caller-
                        // allocates/constructor-initializes-in-place ABI shape
                        // real C++ itself already uses, so this needs no new
                        // storage-layout logic beyond what every other
                        // Named-type VarDecl already does above.
                        if (auto r = zero_initialize_storage(slot, stmt.type, declared_alignment); !r.has_value()) return std::unexpected(std::move(r).error());
                        locals_[declared_local_of(stmt)] = LocalSlot{slot, stmt.type};
                        locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                        locals_[declared_local_of(stmt)].moved_flag = create_moved_flag_if_type_has_destructor(stmt.type);
                        if (auto r = maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc); !r.has_value()) return std::unexpected(std::move(r).error());
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(declared_local_of(stmt));
                        }
                        if (stmt.type.kind != TypeKind::Named || !structs_.contains(stmt.type.name)) {
                            if (auto r = initialize_storage_from_brace_args(LValue{slot, stmt.type, declared_alignment}, stmt.ctor_args); !r.has_value()) return std::unexpected(std::move(r).error());
                            return {};
                        }
                        auto same_type_result = try_initialize_class_storage_from_same_type_source(LValue{slot, stmt.type, declared_alignment},
                                                                               stmt.ctor_args);
                        if (!same_type_result.has_value()) return std::unexpected(std::move(same_type_result).error());
                        if (std::move(same_type_result).value()) {
                            return {};
                        }
                        std::string ctor_name = stmt.type.name + "_new";
                        // ch05 §5.10: a class may declare multiple
                        // constructors (all synthesized as "ClassName_new"),
                        // resolved by exact argument-type match exactly like
                        // any other overloaded name.
                        const Function* ctor_def = stmt.type.name == "std::thread"
                                                       ? resolve_constructor_overload_exact(stmt.type.name, stmt.ctor_args)
                                                       : resolve_overload_by_type(ctor_name, stmt.ctor_args, /*param_offset=*/1);
                        if (ctor_def == nullptr) {
                            if (stmt.ctor_args.empty() && record_is_implicitly_default_initializable(stmt.type.name)) {
                                if (auto r = emit_default_initializers_for_record_storage(slot, stmt.type.name, /*initialize_virtual_interface_bases=*/true); !r.has_value()) return std::unexpected(std::move(r).error());
                                return {};
                            }
                            if (stmt.ctor_args.empty() && find_class_def(stmt.type.name) == nullptr &&
                                find_struct_def(stmt.type.name) == nullptr) {
                                return {};
                            }
                            if (!stmt.ctor_args.empty() && find_struct_def(stmt.type.name) != nullptr &&
                                find_class_def(stmt.type.name) == nullptr) {
                                // See the static-local path's own note: one
                                // routine decides what a braced list on a
                                // struct means, reached from every position.
                                return initialize_storage_from_brace_args(
                                    LValue{slot, stmt.type, declared_alignment}, stmt.ctor_args);
                            }
                            // spec §6.5: `ClassName y{x};` with no matching
                            // user-declared constructor found by ordinary
                            // resolution just above (which would already
                            // have found a user-declared copy constructor,
                            // if one exists, since it's registered like any
                            // other overload) -- if this is a bare (non-
                            // move) same-type single argument and the class
                            // is copy-constructible, synthesize the
                            // compiler-provided recursive memberwise copy
                            // directly, exactly like move construction's own
                            // analogous fallback above.
                            return std::unexpected(CodegenError(
                                describe_constructor_resolution_failure(stmt.type.name, stmt.ctor_args), current_loc_));
                        }
                        if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                            auto value_result = codegen_constructed_class_value(stmt.type.name, stmt.ctor_args, ctor_def);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            create_store(std::move(value_result).value(), slot, declared_alignment);
                            return {};
                        }
                        llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
                        if (ctor == nullptr) {
                            return std::unexpected(CodegenError("class '" + stmt.type.name + "' has no constructor matching this call",
                                current_loc_));
                        }
                        auto args_result =
                            emit_constructor_arguments_and_virtual_bases(stmt.type.name, ctor_def, stmt.ctor_args, slot);
                        if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                        std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                        args.insert(args.begin(), slot);
                        build_call(ctor, args);
                        return {};
                    }
                    if (stmt.init) {
                        if (stmt.type.kind == TypeKind::Named && structs_.contains(stmt.type.name) &&
                            stmt.init->kind == ExprKind::Identifier) {
                            // spec §6.5: `ClassName y = x;` -- copy
                            // construction (movecheck has already verified
                            // `x` is the exact same class type and that the
                            // class is copy-constructible). Dispatch to the
                            // user-declared copy constructor if one exists
                            // (a real function call, so any side effects --
                            // e.g. incrementing a reference count, spec
                            // §6.5's own worked example -- actually run,
                            // unlike a blind byte copy); otherwise the
                            // compiler-provided recursive memberwise copy.
                            const Expr& source_expr = *stmt.init;
                            auto src_result = codegen_lvalue(source_expr);
                            if (!src_result.has_value()) return std::unexpected(std::move(src_result).error());
                            LValue src = std::move(src_result).value();
                            if (const Function* user_ctor = find_user_declared_copy_ctor_ast(stmt.type.name)) {
                                llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(user_ctor).c_str());
                                build_call(ctor, {slot, src.ptr});
                            } else {
                                if (auto r = codegen_memberwise_copy_construct(slot, src.ptr, stmt.type.name); !r.has_value()) return std::unexpected(std::move(r).error());
                            }
                            locals_[declared_local_of(stmt)] = LocalSlot{slot, stmt.type};
                            locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                            locals_[declared_local_of(stmt)].moved_flag = create_moved_flag_if_has_destructor(stmt.type.name);
                            if (auto r = maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc); !r.has_value()) return std::unexpected(std::move(r).error());
                            if (!scope_stack_.empty()) {
                                scope_stack_.back().push_back(declared_local_of(stmt));
                            }
                            return {};
                        }
                        if (stmt.init->kind == ExprKind::BracedInitList) {
                            // See the static-storage path above: a braced
                            // list initializes the slot in place.
                            LValue target{slot, stmt.type, declared_alignment};
                            if (auto r = initialize_storage_from_brace_args(target, stmt.init->args); !r.has_value())
                                return std::unexpected(std::move(r).error());
                            locals_[declared_local_of(stmt)] = LocalSlot{slot, stmt.type};
                            locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                            locals_[declared_local_of(stmt)].moved_flag = create_moved_flag_if_has_destructor(stmt.type.name);
                            if (auto r = maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc); !r.has_value())
                                return std::unexpected(std::move(r).error());
                            if (!scope_stack_.empty()) scope_stack_.back().push_back(declared_local_of(stmt));
                            return {};
                        }
                        auto init_value_result = codegen_value_for_target(*stmt.init, stmt.type);
                        if (!init_value_result.has_value()) return std::unexpected(std::move(init_value_result).error());
                        llvm::LLVMValueRef init_value = std::move(init_value_result).value();
                        // Refresh to `stmt`'s own position: codegen_expr just
                        // recursed through `stmt.init` (possibly a compound
                        // expression like `a + b`), leaving current_loc_ at
                        // whichever sub-expression it last visited rather
                        // than the statement check_store_type is actually
                        // about.
                        refresh_debug_location(stmt.loc);
                        if (auto r = check_store_type(init_value, llvm_type, "variable '" + stmt.var_name + "'"); !r.has_value()) return std::unexpected(std::move(r).error());
                        create_store(init_value, slot, declared_alignment);
                    } else {
                        // scpp has no concept of an uninitialized variable: a
                        // local declared without an initializer is always
                        // zero-initialized (0 / false / null / all-zero
                        // fields), for every type -- scalars and raw pointers
                        // included, not just struct/array/unique_ptr. Only an
                        // array reaches here with a record element type (a
                        // non-array record local is required to be written
                        // `T name{...}`), and its elements are objects that
                        // need their own default member initializers, not a
                        // zero fill -- which is the same question
                        // initialize_storage_from_brace_args answers for a
                        // field, so it answers it here too rather than a
                        // second copy deciding differently.
                        if (auto r = initialize_storage_from_brace_args(LValue{slot, stmt.type, declared_alignment}, {}); !r.has_value()) return std::unexpected(std::move(r).error());
                    }
                    locals_[declared_local_of(stmt)] = LocalSlot{slot, stmt.type};
                    locals_[declared_local_of(stmt)].is_const = stmt.is_const || stmt.is_constexpr;
                    locals_[declared_local_of(stmt)].moved_flag = create_moved_flag_if_type_has_destructor(stmt.type);
                    if (auto r = maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc); !r.has_value()) return std::unexpected(std::move(r).error());
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(declared_local_of(stmt));
                    }
                    return {};
                }();

            case StmtKind::Return:
                return [&, this]() -> std::expected<void, CodegenError> {
                    // Evaluate the return value *before* freeing owned locals:
                    // `return std::move(a);` nulls out `a`'s slot as a side
                    // effect of the move, so by the time we free every
                    // unique_ptr local below, an already-moved-from one is
                    // safely a no-op (free(NULL) is well-defined) while a
                    // still-owning one is correctly released.
                    //
                    // When *this* function's own return type is a reference,
                    // the returned expression is an addressable place
                    // (movecheck's dangling check -- see
                    // resolve_borrow_source_root -- only allows an
                    // Identifier/Member/Subscript chain here), and returning
                    // it means returning that address, not its current value
                    // -- codegen_lvalue, not codegen_expr (which would
                    // auto-dereference it, same as any other read).
                    llvm::LLVMValueRef value = nullptr;
                    // Tracked separately from `value` so the void branch below can
                    // still be type-checked: a void function emits `ret void`
                    // (value stays null) but `return expr;` is only well-formed
                    // there when `expr` itself is void-typed, so the check needs
                    // the expression's own type, not the `ret` operand's.
                    llvm::LLVMTypeRef returned_type = llvm::LLVMVoidTypeInContext(context_);
                    if (stmt.expr) {
                        if (current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                            current_function_def_->return_type.name == "void") {
                            auto value_result = codegen_expr(*stmt.expr);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            if (llvm::LLVMValueRef evaluated = std::move(value_result).value())
                                returned_type = llvm::LLVMTypeOf(evaluated);
                        } else if (current_function_def_ != nullptr && is_interface_reference_type(current_function_def_->return_type)) {
                            auto value_result = codegen_interface_value_for_target(*stmt.expr, current_function_def_->return_type);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            value = std::move(value_result).value();
                        } else if (current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Reference) {
                            auto lvalue_result = codegen_lvalue(*stmt.expr);
                            if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                            value = std::move(lvalue_result).value().ptr;
                        } else if (current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                                              find_class_def(current_function_def_->return_type.name) != nullptr &&
                                              is_implicit_move_return_source(*stmt.expr, current_function_def_->return_type)) {
                            Expr implicit_move;
                            implicit_move.kind = ExprKind::Move;
                            implicit_move.loc = stmt.expr->loc;
                            implicit_move.lhs = deep_clone_expr(*stmt.expr);
                            auto value_result = codegen_expr(implicit_move);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            value = std::move(value_result).value();
                        } else if (current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                                              find_class_def(current_function_def_->return_type.name) != nullptr) {
                            // /*allow_implicit_converting_ctor=*/true: mirrors
                            // movecheck's own dataflow.cppm return-statement check
                            // (return_converting_ctor, via
                            // find_single_argument_converting_constructor_signature),
                            // which already accepts returning some other type T that
                            // the return type has a single-argument converting
                            // constructor from (e.g. `return
                            // std::unexpected(E{...});` returning into a
                            // `std::expected<T, E>`-typed function) -- without this,
                            // codegen would emit stmt.expr's own (mismatched) type
                            // directly as the `ret` operand instead of constructing
                            // the actual return type, tripping LLVM's IR verifier.
                            auto value_result = codegen_class_value_for_boundary(*stmt.expr, current_function_def_->return_type,
                                                                                 /*allow_implicit_converting_ctor=*/true);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            value = std::move(value_result).value();
                        } else if (current_function_def_ != nullptr) {
                            auto value_result = codegen_value_for_target(*stmt.expr, current_function_def_->return_type);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            value = std::move(value_result).value();
                        } else {
                            auto value_result = codegen_expr(*stmt.expr);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            value = std::move(value_result).value();
                        }
                    }
                    if (value != nullptr) returned_type = llvm::LLVMTypeOf(value);
                    if (auto r = check_return_type(returned_type, stmt); !r.has_value()) return std::unexpected(std::move(r).error());
                    if (auto r = emit_function_exit_cleanup(); !r.has_value()) return std::unexpected(std::move(r).error());
                    if (value != nullptr) {
                        llvm::LLVMBuildRet(builder_, value);
                    } else {
                        llvm::LLVMBuildRetVoid(builder_);
                    }
                    return {};
                }();

            case StmtKind::ExprStmt:
                if (stmt.expr && stmt.expr->kind == ExprKind::Delete) {
                    if (auto r = codegen_delete_expr(*stmt.expr); !r.has_value()) return std::unexpected(std::move(r).error());
                    return {};
                }
                if (stmt.expr && stmt.expr->kind == ExprKind::Destroy) {
                    if (auto r = codegen_destroy_expr(*stmt.expr); !r.has_value()) return std::unexpected(std::move(r).error());
                    return {};
                }
                if (auto r = codegen_expr(*stmt.expr); !r.has_value()) return std::unexpected(std::move(r).error());
                return {};

            case StmtKind::If:
                return [&, this]() -> std::expected<void, CodegenError> {
                    // `stmt.condition` is a `bool` expression, stored/passed
                    // as i8 (see to_llvm_type) -- CreateCondBr needs a 1-bit
                    // condition, so narrow it right here (see bool_to_i1).
                    auto cond_result = codegen_contextual_bool_i1(*stmt.condition);
                    if (!cond_result.has_value()) return std::unexpected(std::move(cond_result).error());
                    llvm::LLVMValueRef cond = std::move(cond_result).value();
                    llvm::LLVMBasicBlockRef then_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "if.then");
                    llvm::LLVMBasicBlockRef else_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "if.else");
                    llvm::LLVMBasicBlockRef merge_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "if.end");

                    llvm::LLVMBuildCondBr(builder_, cond, then_block, else_block);

                    // then/else each get their own scope so a unique_ptr
                    // declared in one branch (with or without braces -- a
                    // bare `if (c) unique_ptr<T> x = ...;` is valid grammar,
                    // same as real C++) is dropped at the end of *that*
                    // branch, not left dangling in the flat locals_ map.
                    llvm::LLVMPositionBuilderAtEnd(builder_, then_block);
                    push_scope();
                    if (auto r = codegen_stmt(*stmt.then_branch, current_function); !r.has_value()) return std::unexpected(std::move(r).error());
                    pop_scope();
                    if (llvm::LLVMGetBasicBlockTerminator(llvm::LLVMGetInsertBlock(builder_)) == nullptr) {
                        llvm::LLVMBuildBr(builder_, merge_block);
                    }

                    llvm::LLVMPositionBuilderAtEnd(builder_, else_block);
                    push_scope();
                    if (stmt.else_branch) {
                        if (auto r = codegen_stmt(*stmt.else_branch, current_function); !r.has_value()) return std::unexpected(std::move(r).error());
                    }
                    pop_scope();
                    if (llvm::LLVMGetBasicBlockTerminator(llvm::LLVMGetInsertBlock(builder_)) == nullptr) {
                        llvm::LLVMBuildBr(builder_, merge_block);
                    }

                    llvm::LLVMPositionBuilderAtEnd(builder_, merge_block);
                    if (llvm::LLVMGetFirstUse(llvm::LLVMBasicBlockAsValue(merge_block)) != nullptr) {
                        return {};
                    }
                    // Both branches terminated (e.g. returned), so this merge
                    // point is unreachable; give it a terminator anyway since
                    // every basic block must end with one, and let the caller
                    // see the *original* branches' terminators, not this dead
                    // block, when checking "does this path return?".
                    llvm::LLVMBuildUnreachable(builder_);
                    return {};
                }();

            case StmtKind::While:
                return [&, this]() -> std::expected<void, CodegenError> {
                    llvm::LLVMBasicBlockRef cond_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "while.cond");
                    llvm::LLVMBasicBlockRef body_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "while.body");
                    llvm::LLVMBasicBlockRef end_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "while.end");

                    llvm::LLVMBuildBr(builder_, cond_block);

                    llvm::LLVMPositionBuilderAtEnd(builder_, cond_block);
                    // A `while (true)` condition -- including `for (;;)`'s
                    // desugared `true` placeholder condition (see
                    // desugar_classic_for) -- is known at parse time to
                    // always hold. Branch straight to the body unconditionally
                    // instead of through a CondBr in that case: *any* CondBr
                    // targeting end_block, even one whose condition is this
                    // always-true literal, creates a structural "use" of
                    // end_block in the raw (pre-optimization) IR, which would
                    // defeat the "is end_block reachable" check below (see its
                    // comment) -- a truly infinite loop with no `break` would
                    // otherwise look reachable purely because of its own dead
                    // condition-check edge, wrongly demanding a `return` after
                    // it (the false positive this whole branch avoids).
                    bool condition_always_true = stmt.condition->kind == ExprKind::BoolLiteral && stmt.condition->bool_value;
                    if (condition_always_true) {
                        llvm::LLVMBuildBr(builder_, body_block);
                    } else {
                        // Same bool_to_i1 narrowing as the If case above.
                        auto cond_result = codegen_contextual_bool_i1(*stmt.condition);
                        if (!cond_result.has_value()) return std::unexpected(std::move(cond_result).error());
                        llvm::LLVMValueRef cond = std::move(cond_result).value();
                        llvm::LLVMBuildCondBr(builder_, cond, body_block, end_block);
                    }

                    // The body's scope is popped (and its unique_ptr locals
                    // dropped) at the end of *every* iteration, right before
                    // jumping back to re-check the condition -- so a
                    // unique_ptr re-declared each iteration doesn't leak the
                    // previous iteration's allocation.
                    llvm::LLVMPositionBuilderAtEnd(builder_, body_block);
                    push_scope();
                    control_flow_stack_.push_back(ControlFlowFrame{cond_block, end_block, scope_stack_.size()});
                    if (auto r = codegen_stmt(*stmt.then_branch, current_function); !r.has_value()) return std::unexpected(std::move(r).error());
                    pop_scope();
                    control_flow_stack_.pop_back();
                    if (llvm::LLVMGetBasicBlockTerminator(llvm::LLVMGetInsertBlock(builder_)) == nullptr) {
                        llvm::LLVMBuildBr(builder_, cond_block);
                    }

                    llvm::LLVMPositionBuilderAtEnd(builder_, end_block);
                    if (llvm::LLVMGetFirstUse(llvm::LLVMBasicBlockAsValue(end_block)) == nullptr) {
                        llvm::LLVMBuildUnreachable(builder_);
                    }
                    return {};
                }();

            case StmtKind::Switch:
                return [&, this]() -> std::expected<void, CodegenError> {
                    auto condition_result = codegen_expr(*stmt.condition);
                    if (!condition_result.has_value()) return std::unexpected(std::move(condition_result).error());
                    llvm::LLVMValueRef condition = std::move(condition_result).value();
                    llvm::LLVMBasicBlockRef end_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "switch.end");
                    std::vector<llvm::LLVMBasicBlockRef> case_blocks;
                    case_blocks.reserve(stmt.switch_cases.size());
                    for ([[maybe_unused]] const SwitchCase& switch_case : stmt.switch_cases) {
                        case_blocks.push_back(llvm::LLVMAppendBasicBlockInContext(context_, current_function, "switch.case"));
                    }
                    llvm::LLVMBasicBlockRef default_block = end_block;
                    std::vector<std::pair<llvm::LLVMValueRef, llvm::LLVMBasicBlockRef>> value_cases;
                    for (std::size_t i = 0; i < stmt.switch_cases.size(); i++) {
                        if (stmt.switch_cases[i].value) {
                            auto case_value_result = codegen_expr(*stmt.switch_cases[i].value);
                            if (!case_value_result.has_value()) return std::unexpected(std::move(case_value_result).error());
                            value_cases.push_back({std::move(case_value_result).value(), case_blocks[i]});
                        } else {
                            default_block = case_blocks[i];
                        }
                    }

                    llvm::LLVMBasicBlockRef current_test_block = llvm::LLVMGetInsertBlock(builder_);
                    for (std::size_t i = 0; i < value_cases.size(); i++) {
                        llvm::LLVMBasicBlockRef false_block =
                            (i + 1 == value_cases.size()) ? default_block
                                                          : llvm::LLVMAppendBasicBlockInContext(context_, current_function,
                                                                                                "switch.test");
                        llvm::LLVMPositionBuilderAtEnd(builder_, current_test_block);
                        llvm::LLVMValueRef matches =
                            llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, condition, value_cases[i].first, "switch.match");
                        llvm::LLVMBuildCondBr(builder_, matches, value_cases[i].second, false_block);
                        current_test_block = false_block;
                    }
                    if (value_cases.empty()) {
                        llvm::LLVMBuildBr(builder_, default_block);
                    } else if (current_test_block == default_block &&
                               llvm::LLVMGetBasicBlockTerminator(default_block) == nullptr &&
                               default_block == end_block) {
                        llvm::LLVMPositionBuilderAtEnd(builder_, default_block);
                    }

                    for (std::size_t i = 0; i < stmt.switch_cases.size(); i++) {
                        llvm::LLVMPositionBuilderAtEnd(builder_, case_blocks[i]);
                        push_scope();
                        control_flow_stack_.push_back(ControlFlowFrame{std::nullopt, end_block, scope_stack_.size()});
                        for (const StmtPtr& child : stmt.switch_cases[i].statements) {
                            if (llvm::LLVMGetBasicBlockTerminator(llvm::LLVMGetInsertBlock(builder_)) != nullptr) break;
                            if (auto r = codegen_stmt(*child, current_function); !r.has_value()) return std::unexpected(std::move(r).error());
                        }
                        bool falls_into_next_case =
                            stmt.switch_cases[i].statements.empty() ||
                            (!stmt.switch_cases[i].statements.empty() &&
                             stmt.switch_cases[i].statements.back()->kind == StmtKind::Fallthrough);
                        pop_scope();
                        control_flow_stack_.pop_back();
                        if (llvm::LLVMGetBasicBlockTerminator(llvm::LLVMGetInsertBlock(builder_)) == nullptr) {
                            llvm::LLVMBasicBlockRef target =
                                (falls_into_next_case && i + 1 < case_blocks.size()) ? case_blocks[i + 1] : end_block;
                            llvm::LLVMBuildBr(builder_, target);
                        }
                    }

                    llvm::LLVMPositionBuilderAtEnd(builder_, end_block);
                    // Ask LLVM whether anything actually branches here, rather
                    // than trying to predict it from the shape of the source:
                    // a `break;` reaching this block can sit arbitrarily deep
                    // inside a case (`case X: { ...; break; }`, or inside an
                    // `if` within the case), so no syntactic peek at a case's
                    // last statement can see all of them. Same idiom as the
                    // `if` merge block and the `while` end block above.
                    if (llvm::LLVMGetFirstUse(llvm::LLVMBasicBlockAsValue(end_block)) == nullptr) {
                        llvm::LLVMBuildUnreachable(builder_);
                    }
                    return {};
                }();

            case StmtKind::Break:
                if (!control_flow_stack_.empty()) {
                    emit_scope_cleanup_to_depth(control_flow_stack_.back().scope_depth);
                    llvm::LLVMBuildBr(builder_, control_flow_stack_.back().end_block);
                }
                return {};

            case StmtKind::Continue:
                for (auto it = control_flow_stack_.rbegin(); it != control_flow_stack_.rend(); ++it) {
                    if (!it->continue_block.has_value()) continue;
                    emit_scope_cleanup_to_depth(it->scope_depth);
                    llvm::LLVMBuildBr(builder_, *it->continue_block);
                    return {};
                }
                return {};

            case StmtKind::Fallthrough:
                return {};
        }
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::emit_function_exit_cleanup()
{
        // Block-scoped locals first (deepest scope first, reverse
        // declaration order within each), matching pop_scope()'s own
        // single-scope case -- see emit_scope_cleanup_to_depth's comment.
        emit_scope_cleanup_to_depth(0);

        // Then this function's own parameters, in reverse *parameter*
        // order: a parameter list is conceptually constructed left-to-
        // right as the function is entered (before any of the body's own
        // locals), so it's torn down right-to-left, same "reverse of
        // construction order" rule as everywhere else in this codegen.
        // Parameters live in locals_ like any other local slot, but
        // (unlike block-scoped locals) are never pushed onto
        // scope_stack_ (see the parameter-binding loop in
        // define_function), so emit_scope_cleanup_to_depth above can't
        // see them; they need this separate pass.
        if (current_function_def_ != nullptr) {
            const std::vector<Param>& params = current_function_def_->params;
            for (auto it = params.rbegin(); it != params.rend(); ++it) {
                if (it->resolved_local == 0) continue;
                auto slot_it = locals_.find(param_local(*it));
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.is_static_storage) continue;
                codegen_destroy_storage_unless_moved(slot_it->second.type, slot_it->second.alloca,
                                                     slot_it->second.moved_flag);
            }
        }

        // Finally, if this function *is* a destructor, the object's own
        // members -- last, because the user-written body above may read
        // them right up to the moment it returns. The `= default`
        // destructor reaches the same routine from
        // define_defaulted_function; routing both through
        // emit_class_member_teardown is what keeps a hand-written `~T() {
        // }` and `~T() = default;` destroying the same subobjects.
        // Interface destructors are skipped: their `this` is a raw
        // pointer to some unknown concrete object, not storage laid out
        // by the interface, and an interface declares no members of its
        // own.
        if (current_function_def_ != nullptr && is_destructor_function(*current_function_def_) &&
            !interface_destructor_uses_raw_this(*current_function_def_)) {
            auto object_ptr_result = load_this_object_ptr();
            if (!object_ptr_result.has_value()) return std::unexpected(std::move(object_ptr_result).error());
            if (auto r = emit_class_member_teardown(current_function_def_->member_owner_class,
                                                    std::move(object_ptr_result).value());
                !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
        }
        return {};
    }


    void Codegen::push_scope()
{ scope_stack_.emplace_back(); }


    void Codegen::pop_scope()
{
        std::vector<LocalId> declared = std::move(scope_stack_.back());
        scope_stack_.pop_back();

        bool already_terminated = llvm::LLVMGetBasicBlockTerminator(llvm::LLVMGetInsertBlock(builder_)) != nullptr;
        if (!already_terminated) {
            for (auto it = declared.rbegin(); it != declared.rend(); ++it) {
                auto slot_it = locals_.find(*it);
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.is_static_storage) continue;
                codegen_destroy_storage_unless_moved(slot_it->second.type, slot_it->second.alloca,
                                                     slot_it->second.moved_flag);
            }
        }
        // By declaration, not by name: an inner scope that shadows an
        // outer local has its own LocalId, so dropping it here leaves the
        // outer declaration's slot -- and therefore its destructor at its
        // own scope's end -- untouched.
        for (LocalId id : declared) {
            locals_.erase(id);
        }
    }


    void Codegen::emit_scope_cleanup_to_depth(std::size_t target_depth)
{
        for (std::size_t depth = scope_stack_.size(); depth > target_depth; depth--) {
            const std::vector<LocalId>& declared = scope_stack_[depth - 1];
            for (auto it = declared.rbegin(); it != declared.rend(); ++it) {
                auto slot_it = locals_.find(*it);
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.is_static_storage) continue;
                codegen_destroy_storage_unless_moved(slot_it->second.type, slot_it->second.alloca,
                                                     slot_it->second.moved_flag);
            }
        }
    }

} // namespace scpp
