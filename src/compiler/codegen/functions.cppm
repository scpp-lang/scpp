module;

module scpp.compiler.codegen:functions;

import std;
import llvm.core;
import :api;

namespace scpp {

    void Codegen::declare_function(const Function& fn)
{
        if (fn.return_type.kind == TypeKind::Reference) {
            validate_reference_return_elision(fn);
            validate_reference_pointee(*fn.return_type.pointee);
        }
        if (fn.is_extern_c) {
            validate_c_abi_compatible(fn.return_type, fn.name, "return type");
        }
        std::vector<LLVMTypeRef> param_types;
        param_types.reserve(fn.params.size());
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            const Param& param = fn.params[i];
            if (param.type.kind == TypeKind::Reference) {
                validate_reference_pointee(*param.type.pointee);
            }
            if (fn.is_extern_c) {
                validate_c_abi_compatible(param.type, fn.name, "parameter '" + param.name + "'");
            }
            if (is_bare_void(param.type)) {
                throw CodegenError("function '" + fn.name + "': parameter '" + param.name +
                                    "' cannot have type 'void' (only a return type or a pointer's pointee "
                                    "-- 'void*' -- may be 'void')",
                    current_loc_);
            }
            param_types.push_back(llvm_param_type_for_function(fn, param, i));
        }
        LLVMTypeRef fn_type = LLVMFunctionType(to_llvm_type(fn.return_type), param_types.data(),
                                               static_cast<unsigned>(param_types.size()), fn.has_varargs);
        // ch11 §11.9: a module-private (non-exported) function *defined*
        // in this same translation unit never needs to be visible
        // outside it -- LLVM internal linkage (the same mechanism as C's
        // `static`) guarantees zero risk of colliding with an unrelated
        // module's own same-named private helper, with no mangling
        // needed at all. A bodyless declaration (extern "C", bare
        // `extern` awaiting a separate implementation unit, or a
        // function recovered from an *imported* module -- see the
        // parser's merge_imported_module, which always clears the
        // cloned Function's body) always keeps external linkage: LLVM
        // requires a definition for internal linkage, and there's
        // nothing here to hide regardless. Every other case (a
        // non-module file's own function, or an exported one, handled
        // via overload_names_'s mangled name already) is unaffected --
        // external linkage, exactly as before this chapter.
        LLVMLinkage linkage = LLVMExternalLinkage;
        // ch05 §5.14: a forwarding stub (Function::forwards_to) gets a
        // real, defined body too (define_forwarding_function), just
        // never an scpp-level AST one -- eligible for the same internal
        // linkage as an ordinary defined function.
        bool has_definition = fn.body != nullptr || fn.is_defaulted || !fn.forwards_to.empty();
        if (has_definition && !fn.is_exported && !fn.is_extern_c &&
            (!fn.owning_module.empty() || !program_->module_name.empty() || fn.is_compile_time_dependency)) {
            linkage = LLVMInternalLinkage;
        }
        LLVMValueRef llvm_fn = LLVMAddFunction(module_, overload_names_.at(&fn).c_str(), fn_type);
        LLVMSetLinkage(llvm_fn, linkage);
    }


    void Codegen::define_function(const Function& fn)
{
        LLVMValueRef llvm_fn = LLVMGetNamedFunction(module_, overload_names_.at(&fn).c_str());
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition",
                current_loc_);
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
        attach_debug_subprogram(llvm_fn, fn);
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context_, llvm_fn, "entry");
        LLVMPositionBuilderAtEnd(builder_, entry);
        current_loc_ = fn.loc;
        LLVMSetCurrentDebugLocation2(builder_, nullptr);

        locals_.clear();
        scope_stack_.clear();
        std::size_t index = 0;
        for (unsigned i = 0, n = LLVMCountParams(llvm_fn); i < n; ++i) {
            LLVMValueRef arg = LLVMGetParam(llvm_fn, i);
            const Param& param = fn.params[index++];
            LLVMSetValueName2(arg, param.name.c_str(), param.name.size());
            LLVMValueRef slot = nullptr;
            if (index == 1 && interface_destructor_uses_raw_this(fn)) {
                slot = LLVMBuildAlloca(builder_, to_llvm_type(param.type), param.name.c_str());
                if (std::optional<unsigned> align = alignment_for_type(param.type)) LLVMSetAlignment(slot, *align);
                LLVMValueRef fat_this = build_interface_value(
                    arg, LLVMConstPointerNull(LLVMPointerTypeInContext(context_, 0)));
                create_store(fat_this, slot, alignment_for_type(param.type));
            } else {
                slot = LLVMBuildAlloca(builder_, LLVMTypeOf(arg), param.name.c_str());
                if (std::optional<unsigned> align = alignment_for_type(param.type)) LLVMSetAlignment(slot, *align);
                LLVMBuildStore(builder_, arg, slot);
            }
            locals_[param.name] = LocalSlot{slot, param.type};
            maybe_emit_parameter_debug_decl(param, slot, static_cast<unsigned>(index));
            if (param.type.kind == TypeKind::Named && find_class_def(param.type.name) != nullptr) {
                locals_[param.name].moved_flag = create_moved_flag_if_has_destructor(param.type.name);
            }
        }

        emit_constructor_member_initializers(fn);
        codegen_stmt(*fn.body, llvm_fn);

        // Falling off the end of a `void` function/constructor/destructor is
        // valid, exactly like C++; synthesize the implicit `return;`.
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(builder_)) == nullptr) {
            if (fn.return_type.kind == TypeKind::Named && fn.return_type.name == "void") {
                LLVMBuildRetVoid(builder_);
                LLVMSetCurrentDebugLocation2(builder_, nullptr);
                current_debug_scope_ = nullptr;
                current_subprogram_ = nullptr;
                return;
            }
            throw CodegenError("function '" + fn.name + "' does not return on all paths",
                current_loc_);
        }
        LLVMSetCurrentDebugLocation2(builder_, nullptr);
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }


    void Codegen::define_defaulted_function(const Function& fn)
{
        if (!fn.is_defaulted) {
            throw CodegenError("internal error: asked to define a non-defaulted function without a body", current_loc_);
        }
        if (!fn.name.ends_with("_delete") || fn.params.size() != 1) {
            throw CodegenError("only defaulted destructors are code-generated in this version", fn.loc);
        }

        LLVMValueRef llvm_fn = LLVMGetNamedFunction(module_, overload_names_.at(&fn).c_str());
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition", fn.loc);
        }

        current_function_def_ = &fn;
        unsafe_depth_ = fn.is_unsafe ? 1 : 0;
        attach_debug_subprogram(llvm_fn, fn);
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context_, llvm_fn, "entry");
        LLVMPositionBuilderAtEnd(builder_, entry);
        current_loc_ = fn.loc;
        LLVMSetCurrentDebugLocation2(builder_, nullptr);

        locals_.clear();
        scope_stack_.clear();
        std::size_t index = 0;
        for (unsigned i = 0, n = LLVMCountParams(llvm_fn); i < n; ++i) {
            LLVMValueRef arg = LLVMGetParam(llvm_fn, i);
            const Param& param = fn.params[index++];
            LLVMSetValueName2(arg, param.name.c_str(), param.name.size());
            LLVMValueRef slot = nullptr;
            if (index == 1 && interface_destructor_uses_raw_this(fn)) {
                slot = LLVMBuildAlloca(builder_, to_llvm_type(param.type), param.name.c_str());
                if (std::optional<unsigned> align = alignment_for_type(param.type)) LLVMSetAlignment(slot, *align);
                LLVMValueRef fat_this = build_interface_value(
                    arg, LLVMConstPointerNull(LLVMPointerTypeInContext(context_, 0)));
                create_store(fat_this, slot, alignment_for_type(param.type));
            } else {
                slot = LLVMBuildAlloca(builder_, LLVMTypeOf(arg), param.name.c_str());
                if (std::optional<unsigned> align = alignment_for_type(param.type)) LLVMSetAlignment(slot, *align);
                LLVMBuildStore(builder_, arg, slot);
            }
            locals_[param.name] = LocalSlot{slot, param.type};
            maybe_emit_parameter_debug_decl(param, slot, static_cast<unsigned>(index));
            if (param.type.kind == TypeKind::Named && find_class_def(param.type.name) != nullptr) {
                locals_[param.name].moved_flag = create_moved_flag_if_has_destructor(param.type.name);
            }
        }

        const Type& this_type = fn.params[0].type;
        if (this_type.kind != TypeKind::Reference || this_type.pointee == nullptr || this_type.pointee->kind != TypeKind::Named) {
            throw CodegenError("defaulted destructor '" + fn.name + "' has an invalid this parameter", fn.loc);
        }
        const std::string& class_name = this_type.pointee->name;
        auto info_it = structs_.find(class_name);
        if (info_it == structs_.end()) {
            throw CodegenError("defaulted destructor '" + fn.name + "' names unknown class '" + class_name + "'", fn.loc);
        }

        LLVMTypeRef this_llvm_type = to_llvm_type(fn.params[0].type);
        LLVMValueRef this_ptr = LLVMBuildLoad2(builder_, this_llvm_type, locals_.at("this").alloca, "thisptr");
        const StructInfo& info = info_it->second;
        for (std::size_t i = info.field_types.size(); i > 0; --i) {
            const Type& field_type = info.field_types[i - 1];
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                LLVMValueRef field_ptr = LLVMBuildStructGEP2(builder_, info.llvm_type, this_ptr,
                                                             info.physical_field_index(i - 1),
                                                             info.field_names[i - 1].c_str());
                codegen_destroy_old_class_state_for_move_assign(field_ptr, field_type.name);
            }
        }

        LLVMBuildRetVoid(builder_);
        LLVMSetCurrentDebugLocation2(builder_, nullptr);
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }


    void Codegen::define_forwarding_function(const Function& fn)
{
        LLVMValueRef llvm_fn = LLVMGetNamedFunction(module_, overload_names_.at(&fn).c_str());
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition",
                current_loc_);
        }
        // Finds the exact base method this stub forwards to: `name`
        // alone isn't necessarily unique (ch05 §5.10 method
        // overloading), but this stub's own params[1:] were copied
        // verbatim from that exact overload at synthesis time, so
        // matching on both name and every non-`this` parameter's type
        // is unambiguous.
        const Function* target = nullptr;
        for (const Function& candidate : program_->functions) {
            if (candidate.name != fn.forwards_to || candidate.params.size() != fn.params.size()) continue;
            bool params_match = true;
            for (std::size_t i = 1; i < fn.params.size() && params_match; i++) {
                params_match = types_equal(candidate.params[i].type, fn.params[i].type);
            }
            if (params_match) {
                target = &candidate;
                break;
            }
        }
        if (target == nullptr) {
            throw CodegenError("forwarding stub '" + fn.name + "' names an unknown target '" + fn.forwards_to + "'",
                current_loc_);
        }
        LLVMValueRef target_llvm = LLVMGetNamedFunction(module_, overload_names_.at(target).c_str());

        attach_debug_subprogram(llvm_fn, fn);
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context_, llvm_fn, "entry");
        LLVMPositionBuilderAtEnd(builder_, entry);
        current_loc_ = fn.loc;
        LLVMSetCurrentDebugLocation2(builder_, nullptr);
        std::vector<LLVMValueRef> args;
        unsigned arg_count = LLVMCountParams(llvm_fn);
        args.reserve(arg_count);
        for (unsigned i = 0; i < arg_count; ++i) args.push_back(LLVMGetParam(llvm_fn, i));
        LLVMValueRef call_result = nullptr;
        if (!fn.params.empty() && is_interface_reference_type(fn.params.front().type)) {
            std::optional<std::size_t> slot_index = interface_method_slot_index(fn.member_owner_class, fn);
            if (!slot_index.has_value()) {
                throw CodegenError("missing interface dispatch slot for forwarding stub '" + fn.name + "'", current_loc_);
            }
            LLVMValueRef receiver_value = args.front();
            LLVMValueRef dispatch_ptr = extract_interface_dispatch_ptr(receiver_value);
            LLVMTypeRef table_type = interface_dispatch_table_type(fn.member_owner_class);
            LLVMValueRef table_ptr = LLVMBuildBitCast(builder_, dispatch_ptr, LLVMPointerTypeInContext(context_, 0),
                                                      "ifacetable");
            LLVMTypeRef i32_ty = LLVMInt32TypeInContext(context_);
            LLVMValueRef slot_indices[] = {LLVMConstInt(i32_ty, 0, /*SignExtend=*/0),
                                           LLVMConstInt(i32_ty, static_cast<unsigned>(*slot_index), /*SignExtend=*/0)};
            LLVMValueRef slot_ptr = LLVMBuildGEP2(builder_, table_type, table_ptr, slot_indices, 2, "ifaceslot");
            LLVMValueRef target_ptr =
                create_load(LLVMPointerTypeInContext(context_, 0), slot_ptr, std::nullopt, "ifacemethod");
            std::vector<LLVMValueRef> dispatch_args;
            dispatch_args.reserve(args.size());
            dispatch_args.push_back(extract_interface_object_ptr(receiver_value));
            for (std::size_t i = 1; i < args.size(); ++i) dispatch_args.push_back(args[i]);
            call_result = build_call(interface_dispatch_function_type(*target), target_ptr, dispatch_args);
        } else if (!fn.params.empty() && !target->params.empty() && is_interface_reference_type(target->params.front().type)) {
            const std::string& concrete_class_name = fn.params.front().type.pointee->name;
            const std::string& target_interface_name = target->params.front().type.pointee->name;
            LLVMValueRef fat_receiver =
                build_interface_value(args.front(), get_or_create_interface_dispatch_table(concrete_class_name,
                                                                                           target_interface_name));
            std::vector<LLVMValueRef> direct_args;
            direct_args.reserve(args.size());
            direct_args.push_back(fat_receiver);
            for (std::size_t i = 1; i < args.size(); ++i) direct_args.push_back(args[i]);
            call_result = build_call(target_llvm, direct_args);
        } else {
            call_result = build_call(target_llvm, args);
        }
        if (is_bare_void(fn.return_type)) {
            LLVMBuildRetVoid(builder_);
        } else {
            LLVMBuildRet(builder_, call_result);
        }
        LLVMSetCurrentDebugLocation2(builder_, nullptr);
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }

} // namespace scpp
