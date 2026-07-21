module;

module scpp.compiler.codegen:statements;

import std;
import llvm;
import :api;

namespace scpp {

    void Codegen::codegen_stmt(const Stmt& stmt, LLVMValueRef current_function)
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
                    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder_)) != nullptr) break;
                    codegen_stmt(*s, current_function);
                }
                if (stmt.is_unsafe) unsafe_depth_--;
                pop_scope();
                return;

            case StmtKind::VarDecl: {
                std::optional<unsigned> declared_alignment = alignment_for_type(stmt.type);
                if (stmt.resolved_alignment != 0) {
                    unsigned explicit_align = stmt.resolved_alignment;
                    if (!declared_alignment.has_value() || explicit_align > *declared_alignment) {
                        declared_alignment = explicit_align;
                    }
                }
                if (stmt.type.kind == TypeKind::Reference) {
                    // Real C++ references must be bound at declaration
                    // (there's no such thing as a later-bound or
                    // "null" reference) -- unlike every other type, which
                    // zero-initializes when no initializer is given.
                    if (!stmt.init) {
                        throw CodegenError("reference '" + stmt.var_name +
                                            "' must be initialized (bound to a variable) at declaration",
                            current_loc_);
                    }
                    if (is_interface_reference_type(stmt.type)) {
                        LLVMValueRef slot =
                            create_entry_block_alloca(to_llvm_type(stmt.type), stmt.var_name, declared_alignment);
                        create_store(codegen_interface_value_for_target(*stmt.init, stmt.type), slot, alignment_for_type(stmt.type));
                        locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                        locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                        maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(stmt.var_name);
                        }
                        return;
                    }
                    validate_reference_pointee(*stmt.type.pointee);
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
                    LLVMValueRef referent_addr =
                        !stmt.type.is_mutable_ref && produces_rvalue_of_type(*stmt.init, *stmt.type.pointee)
                            ? codegen_materialize_rvalue_reference_source(*stmt.init)
                            : codegen_lvalue(*stmt.init).ptr;
                    LLVMValueRef slot =
                        create_entry_block_alloca(LLVMPointerTypeInContext(context_, 0), stmt.var_name, declared_alignment);
                    LLVMBuildStore(builder_, referent_addr, slot);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                    locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                    maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    return;
                }

                if (stmt.type.kind == TypeKind::Span) {
                    // Like a reference, a std::span<T> must be bound to a
                    // place at declaration -- v0.1 only supports
                    // constructing one from a fixed-size array (spec
                    // ch06/M6; std::vector doesn't exist yet).
                    if (!stmt.init) {
                        throw CodegenError("span '" + stmt.var_name +
                                            "' must be initialized (bound to an array) at declaration",
                            current_loc_);
                    }
                    LLVMTypeRef span_type = to_llvm_type(stmt.type);
                    LLVMValueRef span_value = codegen_span_value_for_target(*stmt.init, stmt.type);
                    LLVMValueRef slot = create_entry_block_alloca(span_type, stmt.var_name, declared_alignment);
                    LLVMBuildStore(builder_, span_value, slot);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                    locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                    maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    return;
                }

                if (is_bare_void(stmt.type)) {
                    throw CodegenError("variable '" + stmt.var_name +
                                        "' cannot have type 'void' (only a return type or a pointer's "
                                        "pointee -- 'void*' -- may be 'void')",
                        current_loc_);
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
                    LLVMValueRef closure_ptr =
                        create_entry_block_alloca(to_llvm_type(stmt.type), stmt.var_name, declared_alignment);
                    codegen_construct_lambda(*stmt.init, closure_ptr);
                    locals_[stmt.var_name] = LocalSlot{closure_ptr, stmt.type};
                    locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                    maybe_emit_local_debug_decl(stmt.var_name, stmt.type, closure_ptr, stmt.loc);
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    return;
                }

                LLVMTypeRef llvm_type = to_llvm_type(stmt.type);
                LLVMValueRef slot = create_entry_block_alloca(llvm_type, stmt.var_name, declared_alignment);
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
                    zero_initialize_storage(slot, stmt.type, declared_alignment);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                    locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                    if (stmt.type.kind == TypeKind::Named) {
                        locals_[stmt.var_name].moved_flag = create_moved_flag_if_has_destructor(stmt.type.name);
                    }
                    maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    if (stmt.type.kind != TypeKind::Named || !structs_.contains(stmt.type.name)) {
                        initialize_storage_from_brace_args(LValue{slot, stmt.type, declared_alignment}, stmt.ctor_args);
                        return;
                    }
                    if (try_initialize_class_storage_from_same_type_source(LValue{slot, stmt.type, declared_alignment},
                                                                           stmt.ctor_args)) {
                        return;
                    }
                    std::string ctor_name = stmt.type.name + "_new";
                    // ch05 §5.10: a class may declare multiple
                    // constructors (all synthesized as "ClassName_new"),
                    // resolved by exact argument-type match exactly like
                    // any other overloaded name.
                    const Function* ctor_def = resolve_overload_by_type(ctor_name, stmt.ctor_args, /*param_offset=*/1);
                    if (ctor_def == nullptr) {
                        const ClassDef* class_def = find_class_def(stmt.type.name);
                        if (stmt.ctor_args.empty() && class_def == nullptr) {
                            return;
                        }
                        if (stmt.ctor_args.empty() && class_def != nullptr && !class_has_any_constructor(stmt.type.name)) {
                            emit_default_initializers_for_class_storage(slot, *class_def, /*initialize_virtual_interface_bases=*/true);
                            return;
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
                        throw CodegenError("class '" + stmt.type.name + "' has no constructor matching this call",
                            current_loc_);
                    }
                    if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                        LLVMValueRef value = codegen_constructed_class_value(stmt.type.name, stmt.ctor_args, ctor_def);
                        create_store(value, slot, declared_alignment);
                        return;
                    }
                    LLVMValueRef ctor = LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
                    if (ctor == nullptr) {
                        throw CodegenError("class '" + stmt.type.name + "' has no constructor matching this call",
                            current_loc_);
                    }
                    if (const ClassDef* class_def = find_class_def(stmt.type.name)) {
                        emit_complete_object_interface_initializers(*class_def, ctor_def, slot);
                    }
                    std::vector<LLVMValueRef> args = codegen_call_args(stmt.ctor_args, ctor_def, /*param_offset=*/1);
                    args.insert(args.begin(), slot);
                    build_call(ctor, args);
                    return;
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
                        LValue src = codegen_lvalue(*stmt.init);
                        if (const Function* user_ctor = find_user_declared_copy_ctor_ast(stmt.type.name)) {
                            LLVMValueRef ctor = LLVMGetNamedFunction(module_, overload_names_.at(user_ctor).c_str());
                            build_call(ctor, {slot, src.ptr});
                        } else {
                            codegen_memberwise_copy_construct(slot, src.ptr, stmt.type.name);
                        }
                        locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                        locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                        locals_[stmt.var_name].moved_flag = create_moved_flag_if_has_destructor(stmt.type.name);
                        maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(stmt.var_name);
                        }
                        return;
                    }
                    LLVMValueRef init_value = codegen_value_for_target(*stmt.init, stmt.type);
                    // Refresh to `stmt`'s own position: codegen_expr just
                    // recursed through `stmt.init` (possibly a compound
                    // expression like `a + b`), leaving current_loc_ at
                    // whichever sub-expression it last visited rather
                    // than the statement check_store_type is actually
                    // about.
                    refresh_debug_location(stmt.loc);
                    check_store_type(init_value, llvm_type, "variable '" + stmt.var_name + "'");
                    create_store(init_value, slot, declared_alignment);
                } else {
                    // scpp has no concept of an uninitialized variable: a
                    // local declared without an initializer is always
                    // zero-initialized (0 / false / null / all-zero
                    // fields), for every type -- scalars and raw pointers
                    // included, not just struct/array/unique_ptr.
                    zero_initialize_storage(slot, stmt.type, declared_alignment);
                }
                locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                if (stmt.type.kind == TypeKind::Named) {
                    locals_[stmt.var_name].moved_flag = create_moved_flag_if_has_destructor(stmt.type.name);
                }
                maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                if (!scope_stack_.empty()) {
                    scope_stack_.back().push_back(stmt.var_name);
                }
                return;
            }

            case StmtKind::Return: {
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
                LLVMValueRef value = nullptr;
                if (stmt.expr) {
                    if (current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                        current_function_def_->return_type.name == "void") {
                        codegen_expr(*stmt.expr);
                    } else {
                        value = current_function_def_ != nullptr && is_interface_reference_type(current_function_def_->return_type)
                                    ? codegen_interface_value_for_target(*stmt.expr, current_function_def_->return_type)
                                : current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Reference
                                    ? codegen_lvalue(*stmt.expr).ptr
                                    : current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                                          find_class_def(current_function_def_->return_type.name) != nullptr &&
                                          is_implicit_move_return_source(*stmt.expr, current_function_def_->return_type)
                                          ? [&]() {
                                                Expr implicit_move;
                                                implicit_move.kind = ExprKind::Move;
                                                implicit_move.loc = stmt.expr->loc;
                                                implicit_move.lhs = clone_expr(*stmt.expr);
                                                return codegen_expr(implicit_move);
                                            }()
                                    : current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                                          find_class_def(current_function_def_->return_type.name) != nullptr
                                          ? codegen_class_value_for_boundary(*stmt.expr, current_function_def_->return_type)
                                    : current_function_def_ != nullptr
                                          ? codegen_value_for_target(*stmt.expr, current_function_def_->return_type)
                                          : codegen_expr(*stmt.expr);
                    }
                }
                free_unique_ptr_locals();
                if (value != nullptr) {
                    LLVMBuildRet(builder_, value);
                } else {
                    LLVMBuildRetVoid(builder_);
                }
                return;
            }

            case StmtKind::ExprStmt:
                if (stmt.expr && stmt.expr->kind == ExprKind::Delete) {
                    codegen_delete_expr(*stmt.expr);
                    return;
                }
                if (stmt.expr && stmt.expr->kind == ExprKind::Destroy) {
                    codegen_destroy_expr(*stmt.expr);
                    return;
                }
                codegen_expr(*stmt.expr);
                return;

            case StmtKind::If: {
                // `stmt.condition` is a `bool` expression, stored/passed
                // as i8 (see to_llvm_type) -- CreateCondBr needs a 1-bit
                // condition, so narrow it right here (see bool_to_i1).
                LLVMValueRef cond = codegen_contextual_bool_i1(*stmt.condition);
                LLVMBasicBlockRef then_block = LLVMAppendBasicBlockInContext(context_, current_function, "if.then");
                LLVMBasicBlockRef else_block = LLVMAppendBasicBlockInContext(context_, current_function, "if.else");
                LLVMBasicBlockRef merge_block = LLVMAppendBasicBlockInContext(context_, current_function, "if.end");

                LLVMBuildCondBr(builder_, cond, then_block, else_block);

                // then/else each get their own scope so a unique_ptr
                // declared in one branch (with or without braces -- a
                // bare `if (c) unique_ptr<T> x = ...;` is valid grammar,
                // same as real C++) is dropped at the end of *that*
                // branch, not left dangling in the flat locals_ map.
                LLVMPositionBuilderAtEnd(builder_, then_block);
                push_scope();
                codegen_stmt(*stmt.then_branch, current_function);
                pop_scope();
                if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder_)) == nullptr) {
                    LLVMBuildBr(builder_, merge_block);
                }

                LLVMPositionBuilderAtEnd(builder_, else_block);
                push_scope();
                if (stmt.else_branch) {
                    codegen_stmt(*stmt.else_branch, current_function);
                }
                pop_scope();
                if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder_)) == nullptr) {
                    LLVMBuildBr(builder_, merge_block);
                }

                LLVMPositionBuilderAtEnd(builder_, merge_block);
                if (LLVMGetFirstUse(LLVMBasicBlockAsValue(merge_block)) != nullptr) {
                    return;
                }
                // Both branches terminated (e.g. returned), so this merge
                // point is unreachable; give it a terminator anyway since
                // every basic block must end with one, and let the caller
                // see the *original* branches' terminators, not this dead
                // block, when checking "does this path return?".
                LLVMBuildUnreachable(builder_);
                return;
            }

            case StmtKind::While: {
                LLVMBasicBlockRef cond_block = LLVMAppendBasicBlockInContext(context_, current_function, "while.cond");
                LLVMBasicBlockRef body_block = LLVMAppendBasicBlockInContext(context_, current_function, "while.body");
                LLVMBasicBlockRef end_block = LLVMAppendBasicBlockInContext(context_, current_function, "while.end");

                LLVMBuildBr(builder_, cond_block);

                LLVMPositionBuilderAtEnd(builder_, cond_block);
                // Same bool_to_i1 narrowing as the If case above.
                LLVMValueRef cond = codegen_contextual_bool_i1(*stmt.condition);
                LLVMBuildCondBr(builder_, cond, body_block, end_block);

                // The body's scope is popped (and its unique_ptr locals
                // dropped) at the end of *every* iteration, right before
                // jumping back to re-check the condition -- so a
                // unique_ptr re-declared each iteration doesn't leak the
                // previous iteration's allocation.
                LLVMPositionBuilderAtEnd(builder_, body_block);
                push_scope();
                loop_stack_.push_back(LoopFrame{cond_block, end_block, scope_stack_.size()});
                codegen_stmt(*stmt.then_branch, current_function);
                pop_scope();
                loop_stack_.pop_back();
                if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder_)) == nullptr) {
                    LLVMBuildBr(builder_, cond_block);
                }

                LLVMPositionBuilderAtEnd(builder_, end_block);
                return;
            }

            case StmtKind::Break:
                if (!loop_stack_.empty()) {
                    emit_scope_cleanup_to_depth(loop_stack_.back().scope_depth);
                    LLVMBuildBr(builder_, loop_stack_.back().end_block);
                }
                return;

            case StmtKind::Continue:
                if (!loop_stack_.empty()) {
                    emit_scope_cleanup_to_depth(loop_stack_.back().scope_depth);
                    LLVMBuildBr(builder_, loop_stack_.back().cond_block);
                }
                return;
        }
    }


    void Codegen::free_unique_ptr_locals()
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
                auto slot_it = locals_.find(it->name);
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.type.kind == TypeKind::Named) {
                    if (class_has_destructor_in_chain(slot_it->second.type.name)) {
                        codegen_call_destructor_chain_unless_moved(slot_it->second.type.name, slot_it->second.alloca,
                                                                   slot_it->second.moved_flag);
                    }
                }
            }
        }
    }


    void Codegen::push_scope()
{ scope_stack_.emplace_back(); }


    void Codegen::pop_scope()
{
        std::vector<std::string> names = std::move(scope_stack_.back());
        scope_stack_.pop_back();

        bool already_terminated = LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder_)) != nullptr;
        if (!already_terminated) {
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                auto slot_it = locals_.find(*it);
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.type.kind == TypeKind::Named) {
                    if (class_has_destructor_in_chain(slot_it->second.type.name)) {
                        codegen_call_destructor_chain_unless_moved(slot_it->second.type.name, slot_it->second.alloca,
                                                                   slot_it->second.moved_flag);
                    }
                }
            }
        }
        for (const std::string& name : names) {
            locals_.erase(name);
        }
    }


    void Codegen::emit_scope_cleanup_to_depth(std::size_t target_depth)
{
        for (std::size_t depth = scope_stack_.size(); depth > target_depth; depth--) {
            const std::vector<std::string>& names = scope_stack_[depth - 1];
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                auto slot_it = locals_.find(*it);
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.type.kind == TypeKind::Named) {
                    if (class_has_destructor_in_chain(slot_it->second.type.name)) {
                        codegen_call_destructor_chain_unless_moved(slot_it->second.type.name, slot_it->second.alloca,
                                                                   slot_it->second.moved_flag);
                    }
                }
            }
        }
    }

} // namespace scpp
