module;

module scpp.compiler.codegen:functions;

import std;
import llvm;
import :api;

namespace scpp {

    [[nodiscard]] std::expected<void, CodegenError> Codegen::declare_function(const Function& fn)
{
        if (fn.return_type.kind == TypeKind::Reference) {
            if (auto r = validate_reference_return_elision(fn); !r.has_value()) return std::unexpected(std::move(r).error());
            if (auto r = validate_reference_pointee(*fn.return_type.pointee); !r.has_value()) return std::unexpected(std::move(r).error());
        }
        if (fn.is_extern_c) {
            if (auto r = validate_c_abi_compatible(fn.return_type, fn.name, "return type"); !r.has_value())
                return std::unexpected(std::move(r).error());
        }
        std::vector<llvm::LLVMTypeRef> param_types{};
        param_types.reserve(fn.params.size());
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            const Param& param = fn.params[i];
            if (param.type.kind == TypeKind::Reference) {
                if (auto r = validate_reference_pointee(*param.type.pointee); !r.has_value()) return std::unexpected(std::move(r).error());
            }
            if (fn.is_extern_c) {
                if (auto r = validate_c_abi_compatible(param.type, fn.name, "parameter '" + param.name + "'"); !r.has_value())
                    return std::unexpected(std::move(r).error());
            }
            if (auto r = validate_type_is_inhabitable(param.type, "function '" + fn.name + "' parameter '" + param.name + "'");
                !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
            auto param_type_result = llvm_param_type_for_function(fn, param, i);
            if (!param_type_result.has_value()) return std::unexpected(std::move(param_type_result).error());
            param_types.push_back(std::move(param_type_result).value());
        }
        auto return_llvm_type_result = to_llvm_type(fn.return_type);
        if (!return_llvm_type_result.has_value()) return std::unexpected(std::move(return_llvm_type_result).error());
        llvm::LLVMTypeRef fn_type = nullptr;
        [[scpp::unsafe]] {
            fn_type = llvm::LLVMFunctionType(std::move(return_llvm_type_result).value(), param_types.data(),
                                               static_cast<unsigned int>(param_types.size()), fn.has_varargs ? 1 : 0);
        }
        // ch11 §11.9: a module-private (non-exported) function *defined*
        // in this same translation unit never needs to be visible
        // outside it -- llvm::LLVM internal linkage (the same mechanism as C's
        // `static`) guarantees zero risk of colliding with an unrelated
        // module's own same-named private helper, with no mangling
        // needed at all. A bodyless declaration (extern "C", bare
        // `extern` awaiting a separate implementation unit, or a
        // function recovered from an *imported* module -- see the
        // parser's merge_imported_module, which always clears the
        // cloned Function's body) always keeps external linkage: llvm::LLVM
        // requires a definition for internal linkage, and there's
        // nothing here to hide regardless. Every other case (a
        // non-module file's own function, or an exported one, handled
        // via overload_names_'s mangled name already) is unaffected --
        // external linkage, exactly as before this chapter.
        llvm::LLVMLinkage linkage = llvm::LLVMExternalLinkage;
        bool has_definition = fn.body != nullptr || fn.is_defaulted || fn.is_deleted || !fn.forwards_to.empty();
        bool is_mod_impl = false;
        bool has_part = false;
        bool has_mod = false;
        bool is_own_module = fn.owning_module.empty();
        bool is_from_other_partition = false;
        [[scpp::unsafe]] {
            if (program_ != nullptr) {
                is_mod_impl = program_->is_module_impl;
                has_part = !program_->partition_name.empty();
                has_mod = !program_->module_name.empty();
                if (!fn.owning_module.empty()) {
                    is_own_module = (fn.owning_module == program_->module_name);
                }
                if (has_part && fn.loc.has_source_path()) {
                    is_from_other_partition = (fn.loc.source_path_text() != program_->source_path);
                }
            }
        }
        bool is_synthesized = fn.name.find('.') != std::string::npos || fn.name.starts_with("__lambda");
        bool will_define_body = has_definition;
        if (!fn.owning_module.empty() && !is_own_module && fn.is_exported) {
            will_define_body = false;
        }
        if (is_from_other_partition && !is_synthesized) {
            will_define_body = false;
        }

        if (!will_define_body) {
            linkage = llvm::LLVMExternalLinkage;
        } else if ((is_mod_impl || has_part) && is_own_module && !is_synthesized) {
            linkage = llvm::LLVMExternalLinkage;
        } else if (!fn.is_exported && !fn.is_extern_c &&
            (!fn.owning_module.empty() || has_mod || fn.is_compile_time_dependency)) {
            linkage = llvm::LLVMInternalLinkage;
        }
        llvm::LLVMValueRef llvm_fn = nullptr;
        [[scpp::unsafe]] {
            llvm_fn = llvm::LLVMAddFunction(module_, overload_names_.at(&fn).c_str(), fn_type);
            llvm::LLVMSetLinkage(llvm_fn, linkage);
        }
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::define_function(const Function& fn)
{
        llvm::LLVMValueRef llvm_fn = nullptr;
        [[scpp::unsafe]] {
            llvm_fn = llvm::LLVMGetNamedFunction(module_, overload_names_.at(&fn).c_str());
        }
        if (llvm_fn == nullptr) {
            return std::unexpected(CodegenError("function '" + fn.name + "' was not declared before definition",
                current_loc_));
        }

        current_function_def_ = &fn;
        // Mirrors movecheck's entry_state.unsafe_depth (ch01 §1.2/§1.3):
        // every function is checked by default and starts outside any
        // unsafe context, *except* one whose own declaration carries the
        // function-level `[[scpp::unsafe]]` marker (fn.is_unsafe) --
        // its entire body is an unsafe context throughout, exactly as if
        // wrapped in one `[[scpp::unsafe]] { }` block, so overflow/
        // bounds-check codegen is skipped throughout it too (see
        // codegen_binary_op/codegen_span_subscript below). Otherwise,
        // unsafe_depth_ only increases via an explicit, lexically nested
        // `[[scpp::unsafe]] { }` block within that function's own body
        // (the old "native function = implicitly unsafe everywhere"
        // concept is fully retired).
        unsafe_depth_ = fn.is_unsafe ? 1 : 0;
        if (auto r = attach_debug_subprogram(llvm_fn, fn); !r.has_value()) return std::unexpected(std::move(r).error());
        [[scpp::unsafe]] {
            llvm::LLVMBasicBlockRef entry = llvm::LLVMAppendBasicBlockInContext(context_, llvm_fn, "entry");
            llvm::LLVMPositionBuilderAtEnd(builder_, entry);
            current_loc_ = fn.loc;
            llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        }

        locals_.clear();
        reference_bound_places_.clear();
        scope_stack_.clear();
        scope_temporaries_.clear();
        full_expression_temporaries_.clear();
        full_expression_start_blocks_.clear();
        std::size_t index = 0;
        unsigned int n = 0;
        [[scpp::unsafe]] {
            n = llvm::LLVMCountParams(llvm_fn);
        }
        for (unsigned int i = 0; i < n; ++i) {
            llvm::LLVMValueRef arg = nullptr;
            const Param& param = fn.params[index++];
            [[scpp::unsafe]] {
                arg = llvm::LLVMGetParam(llvm_fn, i);
                llvm::LLVMSetValueName2(arg, param.name.c_str(), static_cast<unsigned long>(param.name.size()));
            }
            llvm::LLVMValueRef slot = nullptr;
            if (index == 1 && interface_destructor_uses_raw_this(fn)) {
                auto param_llvm_type_result = to_llvm_type(param.type);
                if (!param_llvm_type_result.has_value()) return std::unexpected(std::move(param_llvm_type_result).error());
                llvm::LLVMValueRef fat_this = nullptr;
                [[scpp::unsafe]] {
                    slot = llvm::LLVMBuildAlloca(builder_, std::move(param_llvm_type_result).value(), param.name.c_str());
                    if (std::optional<unsigned int> align = alignment_for_type(param.type); align.has_value()) llvm::LLVMSetAlignment(slot, *align);
                    fat_this = build_interface_value(
                        arg, llvm::LLVMConstPointerNull(llvm::LLVMPointerTypeInContext(context_, 0)));
                }
                create_store(fat_this, slot, alignment_for_type(param.type));
            } else if (param.type.kind == TypeKind::Reference && param.type.is_rvalue_ref && param.type.pointee != nullptr) {
                auto param_llvm_type_result = to_llvm_type(param.type);
                if (!param_llvm_type_result.has_value()) return std::unexpected(std::move(param_llvm_type_result).error());
                [[scpp::unsafe]] {
                    slot = llvm::LLVMBuildAlloca(builder_, std::move(param_llvm_type_result).value(), param.name.c_str());
                    if (std::optional<unsigned int> align = alignment_for_type(param.type); align.has_value()) llvm::LLVMSetAlignment(slot, *align);
                    llvm::LLVMBuildStore(builder_, arg, slot);
                }
            } else {
                [[scpp::unsafe]] {
                    slot = llvm::LLVMBuildAlloca(builder_, llvm::LLVMTypeOf(arg), param.name.c_str());
                    if (std::optional<unsigned int> align = alignment_for_type(param.type); align.has_value()) llvm::LLVMSetAlignment(slot, *align);
                    llvm::LLVMBuildStore(builder_, arg, slot);
                }
            }
            if (!has_param_local(param)) {
                return std::unexpected(CodegenError(
                    "internal error: parameter '" + param.name + "' was never resolved to a local", fn.loc));
            }
            // [dcl.fct]/5 deletes a parameter's top-level `const` from
            // the *function type*, so it lives on Param::is_const rather
            // than on Param::type -- and dropping it here made the
            // parameter object read back as writable everywhere codegen
            // asks (is_read_only_place, and through it overload
            // resolution's [over.ics.ref] viability rule): `f(v)` with a
            // `const int v` parameter selected `f(int&)` and wrote 99
            // into it.
            locals_[param_local(param)] = Codegen::LocalSlot{slot, param.type, param.is_const};
            if (auto r = maybe_emit_parameter_debug_decl(param, slot, static_cast<unsigned int>(index)); !r.has_value())
                return std::unexpected(std::move(r).error());
            if (param.type.kind == TypeKind::Named && find_class_def(param.type.name) != nullptr) {
                locals_[param_local(param)].set_whole_moved_flag(create_moved_flag_if_has_destructor(param.type.name));
            }
        }

        if (auto r = emit_constructor_member_initializers(fn); !r.has_value()) return std::unexpected(std::move(r).error());
        if (auto r = codegen_stmt(*fn.body, llvm_fn); !r.has_value()) return std::unexpected(std::move(r).error());

        // Falling off the end of a `void` function/constructor/destructor is
        // valid, exactly like C++; synthesize the implicit `return;`.
        bool has_term = false;
        [[scpp::unsafe]] {
            has_term = llvm::LLVMGetBasicBlockTerminator(llvm::LLVMGetInsertBlock(builder_)) != nullptr;
        }
        if (!has_term) {
            if (fn.return_type.kind == TypeKind::Named && fn.return_type.name == "void") {
                // The synthesized `return;` is a return like any other and
                // runs the same exit cleanup an explicit one does. Without
                // this call, everything the epilogue owns -- by-value class
                // parameters, and a destructor's own members -- silently
                // survived on exactly the path that omits `return;`.
                if (auto r = emit_function_exit_cleanup(); !r.has_value()) return std::unexpected(std::move(r).error());
                [[scpp::unsafe]] {
                    llvm::LLVMBuildRetVoid(builder_);
                    llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
                }
                current_debug_scope_ = nullptr;
                current_subprogram_ = nullptr;
                return {};
            }
            return std::unexpected(CodegenError("function '" + fn.name + "' does not return on all paths",
                current_loc_));
        }
        [[scpp::unsafe]] {
            llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        }
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
        return {};
    }


    // [dcl.fct.def.delete]/2 makes *naming* a deleted function ill-formed,
    // and movecheck rejects every such name before codegen runs -- so no
    // call to this body can exist in a well-formed program. It is emitted
    // anyway because a deleted *virtual* member still occupies a vtable
    // slot, and that slot needs an address: exactly what the Itanium ABI
    // spends `__cxa_deleted_virtual` on. Without it the vtable holds an
    // undefined reference and the program fails at *link* time, naming a
    // mangled symbol instead of the source construct.
    [[nodiscard]] std::expected<void, CodegenError> Codegen::define_deleted_function(const Function& fn)
{
        llvm::LLVMValueRef llvm_fn = nullptr;
        [[scpp::unsafe]] {
            llvm_fn = llvm::LLVMGetNamedFunction(module_, overload_names_.at(&fn).c_str());
        }
        if (llvm_fn == nullptr) {
            return std::unexpected(CodegenError("function '" + fn.name + "' was not declared before definition", fn.loc));
        }
        current_function_def_ = &fn;
        unsafe_depth_ = 0;
        if (auto r = attach_debug_subprogram(llvm_fn, fn); !r.has_value()) return std::unexpected(std::move(r).error());
        [[scpp::unsafe]] {
            llvm::LLVMBasicBlockRef entry = llvm::LLVMAppendBasicBlockInContext(context_, llvm_fn, "entry");
            llvm::LLVMPositionBuilderAtEnd(builder_, entry);
            current_loc_ = fn.loc;
            llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        }
        locals_.clear();
        reference_bound_places_.clear();
        scope_stack_.clear();
        scope_temporaries_.clear();
        full_expression_temporaries_.clear();
        full_expression_start_blocks_.clear();
        std::vector<llvm::LLVMValueRef> no_args{};
        build_call(get_or_declare_abort(), std::move(no_args));
        [[scpp::unsafe]] {
            llvm::LLVMBuildUnreachable(builder_);
        }
        current_function_def_ = nullptr;
        return {};
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::compare_field_for_equality(
        llvm::LLVMValueRef lhs_ptr, llvm::LLVMValueRef rhs_ptr, const Type& type,
        const std::string& field_name, const std::string& class_name, const SourceLocation& loc) {
        if (type.kind == TypeKind::Array && type.element != nullptr) {
            auto array_type_result = to_llvm_type(type);
            if (!array_type_result.has_value()) return std::unexpected(std::move(array_type_result).error());
            llvm::LLVMTypeRef array_type = std::move(array_type_result).value();
            llvm::LLVMValueRef equal = nullptr;
            llvm::LLVMTypeRef i32 = nullptr;
            [[scpp::unsafe]] {
                i32 = llvm::LLVMInt32TypeInContext(context_);
                equal = llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 1UL, 0);
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(type.array_size); ++i) {
                llvm::LLVMValueRef lhs_elem = nullptr;
                llvm::LLVMValueRef rhs_elem = nullptr;
                [[scpp::unsafe]] {
                    llvm::LLVMValueRef indices[2] = {llvm::LLVMConstInt(i32, 0UL, 0), llvm::LLVMConstInt(i32, static_cast<unsigned long>(i), 0)};
                    lhs_elem =
                        llvm::LLVMBuildGEP2(builder_, array_type, lhs_ptr, indices, 2U, "eq.elem.lhs");
                    rhs_elem =
                        llvm::LLVMBuildGEP2(builder_, array_type, rhs_ptr, indices, 2U, "eq.elem.rhs");
                }
                std::expected<llvm::LLVMValueRef, CodegenError> elem_equal_result =
                    compare_field_for_equality(lhs_elem, rhs_elem, *type.element, field_name, class_name, loc);
                if (!elem_equal_result.has_value()) return std::unexpected(std::move(elem_equal_result).error());
                [[scpp::unsafe]] {
                    equal = llvm::LLVMBuildAnd(builder_, equal, std::move(elem_equal_result).value(), "eq.elem.and");
                }
            }
            return equal;
        }
        if (type.kind == TypeKind::Named && structs_.contains(type.name)) {
            const Function* callee_def = nullptr;
            if (program_ != nullptr) {
                [[scpp::unsafe]] {
                    for (std::size_t i = 0; i < program_->functions.size(); ++i) {
                        const Function* candidate = &program_->functions[i];
                        if (candidate->name != type.name + "_operator_equal") continue;
                        if (!is_equality_operator_function(*candidate)) continue;
                        if (candidate->return_type.kind != TypeKind::Named || candidate->return_type.name != "bool") continue;
                        if (!is_special_member_const_lvalue_self_param(candidate->params[1].type, type.name)) continue;
                        callee_def = candidate;
                        break;
                    }
                }
            }
            if (callee_def == nullptr) {
                return std::unexpected(CodegenError("defaulted equality operator of '" + class_name +
                                       "' requires an equality-comparable field '" + field_name +
                                       "' of type '" + type.name + "'",
                                   loc));
            }
            llvm::LLVMValueRef callee = nullptr;
            [[scpp::unsafe]] {
                callee = llvm::LLVMGetNamedFunction(module_, overload_names_.at(callee_def).c_str());
            }
            if (callee == nullptr) {
                return std::unexpected(CodegenError("defaulted equality operator of '" + class_name +
                                       "' could not find generated equality function for field type '" + type.name + "'",
                                   loc));
            }
            llvm::LLVMValueRef call = build_call(callee, make_call_args(lhs_ptr, rhs_ptr));
            return bool_to_i1(call);
        }

        auto llvm_type_result = to_llvm_type(type);
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
        llvm::LLVMValueRef lhs = create_load(llvm_type, lhs_ptr, std::nullopt, "eq.lhs");
        llvm::LLVMValueRef rhs = create_load(llvm_type, rhs_ptr, std::nullopt, "eq.rhs");
        [[scpp::unsafe]] {
            if (type.kind == TypeKind::Named && is_float_scalar_type_name(type.name)) {
                return llvm::LLVMBuildFCmp(builder_, llvm::LLVMRealOEQ, lhs, rhs, "eqtmp");
            }
            return llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, lhs, rhs, "eqtmp");
        }
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::define_defaulted_function(const Function& fn)
{
        if (!fn.is_defaulted) {
            return std::unexpected(CodegenError("internal error: asked to define a non-defaulted function without a body", current_loc_));
        }
        bool is_defaulted_default_constructor = is_default_constructor_function(fn);
        bool is_defaulted_copy_constructor = is_copy_constructor_function(fn);
        bool is_defaulted_move_constructor = is_move_constructor_function(fn);
        bool is_defaulted_copy_assignment = is_copy_assignment_function(fn);
        bool is_defaulted_move_assignment = is_move_assignment_function(fn);
        bool is_defaulted_destructor = is_destructor_function(fn);
        bool is_defaulted_equality = is_defaulted_equality_operator_function(fn);
        if (!is_defaulted_default_constructor && !is_defaulted_copy_constructor && !is_defaulted_move_constructor &&
            !is_defaulted_copy_assignment && !is_defaulted_move_assignment && !is_defaulted_destructor &&
            !is_defaulted_equality) {
            return std::unexpected(CodegenError(
                "defaulted function '" + fn.name +
                    "' is not a supported destructor, constructor, assignment operator, or equality operator",
                fn.loc));
        }

        llvm::LLVMValueRef llvm_fn = nullptr;
        [[scpp::unsafe]] {
            llvm_fn = llvm::LLVMGetNamedFunction(module_, overload_names_.at(&fn).c_str());
        }
        if (llvm_fn == nullptr) {
            return std::unexpected(CodegenError("function '" + fn.name + "' was not declared before definition", fn.loc));
        }

        current_function_def_ = &fn;
        unsafe_depth_ = fn.is_unsafe ? 1 : 0;
        if (auto r = attach_debug_subprogram(llvm_fn, fn); !r.has_value()) return std::unexpected(std::move(r).error());
        [[scpp::unsafe]] {
            llvm::LLVMBasicBlockRef entry = llvm::LLVMAppendBasicBlockInContext(context_, llvm_fn, "entry");
            llvm::LLVMPositionBuilderAtEnd(builder_, entry);
            current_loc_ = fn.loc;
            llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        }

        locals_.clear();
        reference_bound_places_.clear();
        scope_stack_.clear();
        scope_temporaries_.clear();
        full_expression_temporaries_.clear();
        full_expression_start_blocks_.clear();
        std::size_t index = 0;
        unsigned int n = 0;
        [[scpp::unsafe]] {
            n = llvm::LLVMCountParams(llvm_fn);
        }
        for (unsigned int i = 0; i < n; ++i) {
            llvm::LLVMValueRef arg = nullptr;
            const Param& param = fn.params[index++];
            [[scpp::unsafe]] {
                arg = llvm::LLVMGetParam(llvm_fn, i);
                llvm::LLVMSetValueName2(arg, param.name.c_str(), static_cast<unsigned long>(param.name.size()));
            }
            llvm::LLVMValueRef slot = nullptr;
            if (index == 1 && interface_destructor_uses_raw_this(fn)) {
                auto param_llvm_type_result = to_llvm_type(param.type);
                if (!param_llvm_type_result.has_value()) return std::unexpected(std::move(param_llvm_type_result).error());
                llvm::LLVMValueRef fat_this = nullptr;
                [[scpp::unsafe]] {
                    slot = llvm::LLVMBuildAlloca(builder_, std::move(param_llvm_type_result).value(), param.name.c_str());
                    if (std::optional<unsigned int> align = alignment_for_type(param.type); align.has_value()) llvm::LLVMSetAlignment(slot, *align);
                    fat_this = build_interface_value(
                        arg, llvm::LLVMConstPointerNull(llvm::LLVMPointerTypeInContext(context_, 0)));
                }
                create_store(fat_this, slot, alignment_for_type(param.type));
            } else if (param.type.kind == TypeKind::Reference && param.type.is_rvalue_ref && param.type.pointee != nullptr) {
                auto param_llvm_type_result = to_llvm_type(param.type);
                if (!param_llvm_type_result.has_value()) return std::unexpected(std::move(param_llvm_type_result).error());
                [[scpp::unsafe]] {
                    slot = llvm::LLVMBuildAlloca(builder_, std::move(param_llvm_type_result).value(), param.name.c_str());
                    if (std::optional<unsigned int> align = alignment_for_type(param.type); align.has_value()) llvm::LLVMSetAlignment(slot, *align);
                    llvm::LLVMBuildStore(builder_, arg, slot);
                }
            } else {
                [[scpp::unsafe]] {
                    slot = llvm::LLVMBuildAlloca(builder_, llvm::LLVMTypeOf(arg), param.name.c_str());
                    if (std::optional<unsigned int> align = alignment_for_type(param.type); align.has_value()) llvm::LLVMSetAlignment(slot, *align);
                    llvm::LLVMBuildStore(builder_, arg, slot);
                }
            }
            if (!has_param_local(param)) {
                return std::unexpected(CodegenError(
                    "internal error: parameter '" + param.name + "' was never resolved to a local", fn.loc));
            }
            locals_[param_local(param)] = Codegen::LocalSlot{slot, param.type, param.is_const};
            if (auto r = maybe_emit_parameter_debug_decl(param, slot, static_cast<unsigned int>(index)); !r.has_value())
                return std::unexpected(std::move(r).error());
            if (param.type.kind == TypeKind::Named && find_class_def(param.type.name) != nullptr) {
                locals_[param_local(param)].set_whole_moved_flag(create_moved_flag_if_has_destructor(param.type.name));
            }
        }

        const Type& this_type = fn.params[0].type;
        if (this_type.kind != TypeKind::Reference || this_type.pointee == nullptr || this_type.pointee->kind != TypeKind::Named) {
            return std::unexpected(CodegenError("defaulted function '" + fn.name + "' has an invalid this parameter", fn.loc));
        }
        const std::string& class_name = this_type.pointee->name;
        auto info_it = structs_.find(class_name);
        if (info_it == structs_.end()) {
            return std::unexpected(CodegenError("defaulted function '" + fn.name + "' names unknown class '" + class_name + "'", fn.loc));
        }

        auto this_llvm_type_result = to_llvm_type(fn.params[0].type);
        if (!this_llvm_type_result.has_value()) return std::unexpected(std::move(this_llvm_type_result).error());
        llvm::LLVMValueRef this_ptr =
            create_load(std::move(this_llvm_type_result).value(), locals_.at(param_local(fn.params[0])).alloca, std::nullopt, "thisptr");
        const auto& info = info_it->second;

        if (is_defaulted_default_constructor) {
            if (auto r = emit_default_initializers_for_record_storage(this_ptr, class_name, /*initialize_virtual_interface_bases=*/true);
                !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
            [[scpp::unsafe]] {
                llvm::LLVMBuildRetVoid(builder_);
                llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
            }
            current_debug_scope_ = nullptr;
            current_subprogram_ = nullptr;
            return {};
        }

        if (is_defaulted_copy_constructor || is_defaulted_move_constructor || is_defaulted_copy_assignment ||
            is_defaulted_move_assignment) {
            const Param& other_param = fn.params[1];
            auto other_llvm_type_result = to_llvm_type(other_param.type);
            if (!other_llvm_type_result.has_value()) return std::unexpected(std::move(other_llvm_type_result).error());
            llvm::LLVMValueRef other_ptr =
                create_load(std::move(other_llvm_type_result).value(), locals_.at(param_local(other_param)).alloca, std::nullopt, "otherptr");
            if (is_defaulted_copy_constructor) {
                if (auto r = codegen_memberwise_copy_construct(this_ptr, other_ptr, class_name); !r.has_value())
                    return std::unexpected(std::move(r).error());
                [[scpp::unsafe]] {
                    llvm::LLVMBuildRetVoid(builder_);
                }
            } else if (is_defaulted_move_constructor) {
                auto object_llvm_type_result = to_llvm_type(*this_type.pointee);
                if (!object_llvm_type_result.has_value()) return std::unexpected(std::move(object_llvm_type_result).error());
                llvm::LLVMValueRef moved_value = create_load(std::move(object_llvm_type_result).value(), other_ptr, std::nullopt, "movetmp");
                create_store(moved_value, this_ptr, std::nullopt);
                if (auto r = zero_initialize_storage(other_ptr, *this_type.pointee, std::nullopt); !r.has_value())
                    return std::unexpected(std::move(r).error());
                [[scpp::unsafe]] {
                    llvm::LLVMBuildRetVoid(builder_);
                }
            } else if (is_defaulted_copy_assignment) {
                if (auto r = codegen_memberwise_copy_assign(this_ptr, other_ptr, class_name); !r.has_value())
                    return std::unexpected(std::move(r).error());
                [[scpp::unsafe]] {
                    llvm::LLVMBuildRet(builder_, this_ptr);
                }
            } else {
                codegen_destroy_old_class_state_for_move_assign(this_ptr, class_name);
                auto object_llvm_type_result = to_llvm_type(*this_type.pointee);
                if (!object_llvm_type_result.has_value()) return std::unexpected(std::move(object_llvm_type_result).error());
                llvm::LLVMValueRef moved_value = create_load(std::move(object_llvm_type_result).value(), other_ptr, std::nullopt, "movetmp");
                create_store(moved_value, this_ptr, std::nullopt);
                if (auto r = zero_initialize_storage(other_ptr, *this_type.pointee, std::nullopt); !r.has_value())
                    return std::unexpected(std::move(r).error());
                [[scpp::unsafe]] {
                    llvm::LLVMBuildRet(builder_, this_ptr);
                }
            }
            [[scpp::unsafe]] {
                llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
            }
            current_debug_scope_ = nullptr;
            current_subprogram_ = nullptr;
            return {};
        }

        if (is_defaulted_destructor) {
            if (auto r = emit_class_member_teardown(class_name, this_ptr); !r.has_value())
                return std::unexpected(std::move(r).error());
            [[scpp::unsafe]] {
                llvm::LLVMBuildRetVoid(builder_);
                llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
            }
            current_debug_scope_ = nullptr;
            current_subprogram_ = nullptr;
            return {};
        }

        auto other_llvm_type_result = to_llvm_type(fn.params[1].type);
        if (!other_llvm_type_result.has_value()) return std::unexpected(std::move(other_llvm_type_result).error());
        llvm::LLVMValueRef other_ptr = create_load(std::move(other_llvm_type_result).value(),
                                                            locals_.at(param_local(fn.params[1])).alloca, std::nullopt, "otherptr");
        llvm::LLVMValueRef equal = nullptr;
        [[scpp::unsafe]] {
            equal = llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 1UL, 0);
        }
        for (std::size_t i = 0; i < info.field_types.size(); ++i) {
            llvm::LLVMValueRef lhs_field = nullptr;
            llvm::LLVMValueRef rhs_field = nullptr;
            [[scpp::unsafe]] {
                lhs_field = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, this_ptr,
                                                                         static_cast<unsigned int>(info.physical_field_index(i)), info.field_names[i].c_str());
                rhs_field = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, other_ptr,
                                                                         static_cast<unsigned int>(info.physical_field_index(i)), info.field_names[i].c_str());
            }
            auto field_equal_result = compare_field_for_equality(lhs_field, rhs_field, info.field_types[i], info.field_names[i], class_name, fn.loc);
            if (!field_equal_result.has_value()) return std::unexpected(std::move(field_equal_result).error());
            [[scpp::unsafe]] {
                equal = llvm::LLVMBuildAnd(builder_, equal, std::move(field_equal_result).value(), "eq.and");
            }
        }
        [[scpp::unsafe]] {
            if (is_inequality_operator_function(fn)) {
                equal = llvm::LLVMBuildNot(builder_, equal, "neqtmp");
            }
            llvm::LLVMBuildRet(builder_, i1_to_bool(equal));
            llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        }
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::define_forwarding_function(const Function& fn)
{
        llvm::LLVMValueRef llvm_fn = nullptr;
        [[scpp::unsafe]] {
            llvm_fn = llvm::LLVMGetNamedFunction(module_, overload_names_.at(&fn).c_str());
        }
        if (llvm_fn == nullptr) {
            return std::unexpected(CodegenError("function '" + fn.name + "' was not declared before definition",
                current_loc_));
        }
        // Finds the exact base method this stub forwards to: `name`
        // alone isn't necessarily unique (ch05 §5.10 method
        // overloading), but this stub's own params[1:] were copied
        // verbatim from that exact overload at synthesis time, so
        // matching on both name and every non-`this` parameter's type
        // is unambiguous.
        const Function* target = nullptr;
        if (program_ != nullptr) {
            [[scpp::unsafe]] {
                for (std::size_t k = 0; k < program_->functions.size(); ++k) {
                    const Function* candidate = &program_->functions[k];
                    if (candidate->name != fn.forwards_to || candidate->params.size() != fn.params.size()) continue;
                    bool params_match = true;
                    for (std::size_t i = 1; i < fn.params.size() && params_match; i++) {
                        params_match = types_equal(candidate->params[i].type, fn.params[i].type);
                    }
                    if (params_match) {
                        target = candidate;
                        break;
                    }
                }
            }
        }
        if (target == nullptr) {
            return std::unexpected(CodegenError("forwarding stub '" + fn.name + "' names an unknown target '" + fn.forwards_to + "'",
                current_loc_));
        }
        llvm::LLVMValueRef target_llvm = nullptr;
        [[scpp::unsafe]] {
            target_llvm = llvm::LLVMGetNamedFunction(module_, overload_names_.at(target).c_str());
        }

        if (auto r = attach_debug_subprogram(llvm_fn, fn); !r.has_value()) return std::unexpected(std::move(r).error());
        [[scpp::unsafe]] {
            llvm::LLVMBasicBlockRef entry = llvm::LLVMAppendBasicBlockInContext(context_, llvm_fn, "entry");
            llvm::LLVMPositionBuilderAtEnd(builder_, entry);
            current_loc_ = fn.loc;
            llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        }
        std::vector<llvm::LLVMValueRef> args{};
        unsigned int arg_count = 0;
        [[scpp::unsafe]] {
            arg_count = llvm::LLVMCountParams(llvm_fn);
        }
        args.reserve(static_cast<std::size_t>(arg_count));
        for (unsigned int i = 0; i < arg_count; ++i) {
            [[scpp::unsafe]] {
                args.push_back(llvm::LLVMGetParam(llvm_fn, i));
            }
        }
        llvm::LLVMValueRef call_result = nullptr;
        if (!fn.params.empty() && is_interface_reference_type(fn.params.front().type)) {
            auto slot_index_result = interface_method_slot_index(fn.member_owner_class, fn);
            if (!slot_index_result.has_value()) return std::unexpected(std::move(slot_index_result).error());
            std::optional<std::size_t> slot_index = std::move(slot_index_result).value();
            if (!slot_index.has_value()) {
                return std::unexpected(CodegenError("missing interface dispatch slot for forwarding stub '" + fn.name + "'", current_loc_));
            }
            llvm::LLVMValueRef receiver_value = args.front();
            llvm::LLVMValueRef dispatch_ptr = extract_interface_dispatch_ptr(receiver_value);
            auto table_type_result = interface_dispatch_table_type(fn.member_owner_class);
            if (!table_type_result.has_value()) return std::unexpected(std::move(table_type_result).error());
            llvm::LLVMTypeRef table_type = std::move(table_type_result).value();
            llvm::LLVMValueRef target_ptr = nullptr;
            [[scpp::unsafe]] {
                llvm::LLVMValueRef table_ptr = llvm::LLVMBuildBitCast(builder_, dispatch_ptr, llvm::LLVMPointerTypeInContext(context_, 0),
                                                          "ifacetable");
                llvm::LLVMTypeRef i32_ty = llvm::LLVMInt32TypeInContext(context_);
                llvm::LLVMValueRef slot_indices[2] = {llvm::LLVMConstInt(i32_ty, 0UL, /*SignExtend=*/0),
                                               llvm::LLVMConstInt(i32_ty, static_cast<unsigned long>(*slot_index), /*SignExtend=*/0)};
                llvm::LLVMValueRef slot_ptr = llvm::LLVMBuildGEP2(builder_, table_type, table_ptr, slot_indices, 2U, "ifaceslot");
                target_ptr =
                    create_load(llvm::LLVMPointerTypeInContext(context_, 0), slot_ptr, std::nullopt, "ifacemethod");
            }
            std::vector<llvm::LLVMValueRef> dispatch_args{};
            dispatch_args.reserve(args.size());
            dispatch_args.push_back(extract_interface_object_ptr(receiver_value));
            for (std::size_t i = 1; i < args.size(); ++i) dispatch_args.push_back(args[i]);
            auto dispatch_fn_type_result = [&, this]() -> std::expected<llvm::LLVMTypeRef, CodegenError> {
                [[scpp::unsafe]] {
                    return interface_dispatch_function_type(*target);
                }
            }();
            if (!dispatch_fn_type_result.has_value()) return std::unexpected(std::move(dispatch_fn_type_result).error());
            call_result = build_call(std::move(dispatch_fn_type_result).value(), target_ptr, dispatch_args);
        } else if (!fn.params.empty() && target != nullptr) {
            bool target_takes_iface = false;
            std::string concrete_class_name{};
            std::string target_interface_name{};
            [[scpp::unsafe]] {
                if (!target->params.empty() && is_interface_reference_type(target->params.front().type)) {
                    target_takes_iface = true;
                    concrete_class_name = std::string(fn.params.front().type.pointee->name);
                    target_interface_name = std::string(target->params.front().type.pointee->name);
                }
            }
            if (target_takes_iface) {
                auto dispatch_table_result = get_or_create_interface_dispatch_table(concrete_class_name, target_interface_name);
                if (!dispatch_table_result.has_value()) return std::unexpected(std::move(dispatch_table_result).error());
                llvm::LLVMValueRef fat_receiver = build_interface_value(args.front(), std::move(dispatch_table_result).value());
                std::vector<llvm::LLVMValueRef> direct_args{};
                direct_args.reserve(args.size());
                direct_args.push_back(fat_receiver);
                for (std::size_t i = 1; i < args.size(); ++i) direct_args.push_back(args[i]);
                call_result = build_call(target_llvm, direct_args);
            } else {
                call_result = build_call(target_llvm, args);
            }
        } else {
            call_result = build_call(target_llvm, args);
        }
        [[scpp::unsafe]] {
            if (is_bare_void(fn.return_type)) {
                llvm::LLVMBuildRetVoid(builder_);
            } else {
                llvm::LLVMBuildRet(builder_, call_result);
            }
            llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        }
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
        return {};
    }

} // namespace scpp
