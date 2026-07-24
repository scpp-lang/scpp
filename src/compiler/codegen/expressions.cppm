module;

// Official llvm::LLVM-C (llvm-c/*.h) is itself already a stable, extern "C"
// interface -- every llvm::LLVM operation this file needs (IRBuilder-style
// instruction construction, constants, types, intrinsics, DataLayout
// numeric queries) goes through module `llvm`'s own `:target` partition's
// functions directly below (Core.h's own functions come from its `:core`
// partition instead) rather than any native llvm::LLVM C++ header, all reached
// via the single `import llvm;` below (module `llvm` re-exports every
// partition, see libs/llvm/llvm.cpp). See libs/README.md for why this
// project binds straight to llvm::LLVM-C wherever it already covers what's
// needed -- a rigorous, function-by-function empirical audit found
// llvm::LLVM-C fully covers every llvm::LLVM operation this project's codegen needs,
// so there is no custom wrapper of any kind anywhere in this project.

module scpp.compiler.codegen:expressions;

import std;
import llvm;
import :api;

namespace scpp {

namespace {

llvm::LLVMTargetDataRef data_layout_ref(llvm::LLVMModuleRef module) { return llvm::LLVMGetModuleDataLayout(module); }

// Every scalar type scpp's codegen ever casts between is either a plain
// (non-vector) integer type or `float`/`double` (32/64-bit; see
// is_float_scalar_type_name) -- so, unlike llvm::Type::getScalarSizeInBits
// (which also has to handle vector types), this only ever needs to
// distinguish those three cases via llvm::LLVMGetTypeKind.
unsigned scalar_bit_width(llvm::LLVMTypeRef ty)
{
    llvm::LLVMTypeKind kind = llvm::LLVMGetTypeKind(ty);
    if (kind == llvm::LLVMIntegerTypeKind) return llvm::LLVMGetIntTypeWidth(ty);
    if (kind == llvm::LLVMFloatTypeKind) return 32;
    if (kind == llvm::LLVMDoubleTypeKind) return 64;
    return 0;
}

} // namespace

    [[nodiscard]] bool Codegen::is_enum_cast_store_builtin_name(const std::string& name)
{
        return name == "scpp::__enum_cast_store" || name.rfind("scpp::__enum_cast_store.", 0) == 0;
    }


    void Codegen::store_constexpr_value_into(llvm::LLVMValueRef dest_ptr, const Type& dest_type, const ConstexprValue& value)
{
        if (is_scalar_type_name(dest_type.name)) {
            if (dest_type.kind == TypeKind::Named && dest_type.name == "bool") {
                create_store(llvm::LLVMConstInt(llvm::LLVMInt8TypeInContext(context_), value.bool_value ? 1 : 0, 0), dest_ptr,
                             std::nullopt);
                return;
            }
            if (dest_type.kind == TypeKind::Named && dest_type.name == "char") {
                create_store(llvm::LLVMConstInt(llvm::LLVMInt8TypeInContext(context_), static_cast<std::uint64_t>(value.int_value), 0), dest_ptr,
                             std::nullopt);
                return;
            }
            if (dest_type.kind == TypeKind::Named && dest_type.name == "double") {
                create_store(llvm::LLVMConstReal(llvm::LLVMDoubleTypeInContext(context_), value.double_value), dest_ptr,
                             std::nullopt);
                return;
            }
            create_store(llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), static_cast<std::uint64_t>(value.int_value), 1), dest_ptr,
                         std::nullopt);
            return;
        }
        if (dest_type.kind == TypeKind::Pointer && dest_type.pointee &&
            dest_type.pointee->kind == TypeKind::Named && dest_type.pointee->name == "char" && !dest_type.is_mutable_pointee &&
            value.kind == ConstexprValueKind::StringLiteralPointer) {
            create_store(llvm::LLVMBuildGlobalString(builder_, value.string_value.c_str(), "cexprstr"), dest_ptr, std::nullopt);
            return;
        }
        if (dest_type.kind == TypeKind::Array && dest_type.element && value.kind == ConstexprValueKind::Array) {
            for (std::size_t i = 0; i < value.elements.size(); ++i) {
                llvm::LLVMTypeRef i32 = llvm::LLVMInt32TypeInContext(context_);
                llvm::LLVMValueRef indices[] = {llvm::LLVMConstInt(i32, 0, 0), llvm::LLVMConstInt(i32, static_cast<unsigned>(i), 0)};
                llvm::LLVMValueRef elem_ptr = llvm::LLVMBuildGEP2(builder_, to_llvm_type(dest_type), dest_ptr, indices, 2, "");
                store_constexpr_value_into(elem_ptr, *dest_type.element, value.elements[i]);
            }
            return;
        }
        if (dest_type.kind == TypeKind::Named && find_class_def(dest_type.name) != nullptr &&
            value.kind == ConstexprValueKind::Object) {
            const StructInfo& info = structs_.at(dest_type.name);
            for (std::size_t i = 0; i < info.field_names.size(); ++i) {
                auto it = std::find_if(value.object_fields.begin(), value.object_fields.end(),
                                       [&](const auto& field) { return field.first == info.field_names[i]; });
                if (it == value.object_fields.end()) continue;
                llvm::LLVMValueRef field_ptr =
                    llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, dest_ptr, info.physical_field_index(i), info.field_names[i].c_str());
                store_constexpr_value_into(field_ptr, info.field_types[i], *it->second);
            }
            return;
        }
        throw CodegenError("unsupported constexpr class materialization for type '" + dest_type.name + "'", current_loc_);
    }


    llvm::LLVMValueRef Codegen::codegen_consteval_class_value(const Expr& expr, const std::string& class_name)
{
        ConstexprValue value = evaluate_immediate_expr(*program_, expr);
        llvm::LLVMTypeRef llvm_type = to_llvm_type(named_type(class_name));
        std::optional<unsigned> align = alignment_for_type(named_type(class_name));
        llvm::LLVMValueRef temp = create_entry_block_alloca(llvm_type, "constevalclasstmp", align);
        zero_initialize_storage(temp, named_type(class_name), align);
        store_constexpr_value_into(temp, named_type(class_name), value);
        return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "constevalclass.value");
    }


    llvm::LLVMValueRef Codegen::codegen_constructed_class_value(const std::string& class_name, const std::vector<ExprPtr>& args,
                                                 const Function* ctor_def, const Expr* original_expr)
{
        llvm::LLVMTypeRef llvm_type = to_llvm_type(named_type(class_name));
        std::optional<unsigned> align = alignment_for_type(named_type(class_name));
        llvm::LLVMValueRef temp = create_entry_block_alloca(llvm_type, "classtmp", align);
        LValue target{temp, named_type(class_name), align};
        zero_initialize_storage(target.ptr, target.type, target.alignment);
        if (try_initialize_class_storage_from_same_type_source(target, args)) {
            return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "classtmp.value");
        }
        if (ctor_def != nullptr) {
            if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                ExprPtr ctor_expr;
                if (original_expr != nullptr) {
                    ctor_expr = clone_expr(*original_expr);
                } else {
                    ctor_expr = std::make_unique<Expr>();
                    ctor_expr->kind = ExprKind::Call;
                    ctor_expr->loc = current_loc_;
                    ctor_expr->name = class_name;
                    ctor_expr->has_paren_init = true;
                    for (const ExprPtr& arg : args) ctor_expr->args.push_back(clone_expr(*arg));
                }
                ConstexprValue value = evaluate_immediate_expr(*program_, *ctor_expr);
                store_constexpr_value_into(target.ptr, target.type, value);
            } else {
                llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
                if (ctor == nullptr) {
                    throw CodegenError("class '" + class_name + "' has no constructor matching this call", current_loc_);
                }
                if (const ClassDef* class_def = find_class_def(class_name)) {
                    emit_complete_object_interface_initializers(*class_def, ctor_def, target.ptr);
                }
                std::vector<llvm::LLVMValueRef> ctor_args = codegen_call_args(args, ctor_def, /*param_offset=*/1);
                ctor_args.insert(ctor_args.begin(), target.ptr);
                build_call(ctor, ctor_args);
            }
        } else if (args.empty()) {
            const ClassDef* class_def = find_class_def(class_name);
            if (class_def != nullptr && !class_has_any_constructor(class_name)) {
                emit_default_initializers_for_class_storage(target.ptr, *class_def, /*initialize_virtual_interface_bases=*/true);
            }
        }
        return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "classtmp.value");
    }


    Codegen::CallResult Codegen::codegen_call(const Expr& expr)
{
        if (expr.lhs != nullptr && !expr.name.empty() && expr.lhs->kind != ExprKind::Lambda) {
            std::optional<Type> receiver_type = infer_type(*expr.lhs);
            if (receiver_type.has_value()) {
                const Type& receiver_named =
                    receiver_type->kind == TypeKind::Reference && receiver_type->pointee ? *receiver_type->pointee : *receiver_type;
                if (receiver_named.kind == TypeKind::Named && type_names_interface(receiver_named.name)) {
                    const Function* callee =
                        resolve_overload_by_type(receiver_named.name + "_" + expr.name, expr.args, /*param_offset=*/1,
                                                 !is_read_only_place(*expr.lhs), expr.lhs.get());
                    if (callee == nullptr) {
                        throw CodegenError("call to unknown function '" + receiver_named.name + "_" + expr.name + "'",
                                           current_loc_);
                    }
                    llvm::LLVMValueRef receiver_value = codegen_expr(*expr.lhs);
                    if (!callee->is_virtual) {
                        llvm::LLVMValueRef target = llvm::LLVMGetNamedFunction(module_, overload_names_.at(callee).c_str());
                        std::vector<llvm::LLVMValueRef> args = codegen_call_args(expr.args, callee, /*param_offset=*/1);
                        args.insert(args.begin(), receiver_value);
                        return CallResult{build_call(target, args), callee};
                    }
                    std::optional<std::size_t> slot_index = interface_method_slot_index(receiver_named.name, *callee);
                    if (!slot_index.has_value()) {
                        throw CodegenError("missing interface dispatch slot for '" + callee->name + "'", current_loc_);
                    }
                    llvm::LLVMValueRef dispatch_ptr = extract_interface_dispatch_ptr(receiver_value);
                    llvm::LLVMTypeRef table_type = interface_dispatch_table_type(receiver_named.name);
                    llvm::LLVMValueRef table_ptr =
                        llvm::LLVMBuildBitCast(builder_, dispatch_ptr, llvm::LLVMPointerTypeInContext(context_, 0), "ifacetable");
                    llvm::LLVMTypeRef i32 = llvm::LLVMInt32TypeInContext(context_);
                    llvm::LLVMValueRef slot_indices[] = {llvm::LLVMConstInt(i32, 0, 0),
                                                   llvm::LLVMConstInt(i32, static_cast<unsigned>(*slot_index), 0)};
                    llvm::LLVMValueRef slot_ptr =
                        llvm::LLVMBuildGEP2(builder_, table_type, table_ptr, slot_indices, 2, "ifaceslot");
                    llvm::LLVMValueRef target_ptr =
                        create_load(llvm::LLVMPointerTypeInContext(context_, 0), slot_ptr, std::nullopt, "ifacemethod");
                    std::vector<llvm::LLVMValueRef> args = codegen_call_args(expr.args, callee, /*param_offset=*/1);
                    args.insert(args.begin(), extract_interface_object_ptr(receiver_value));
                    return CallResult{build_call(interface_dispatch_function_type(*callee), target_ptr, args), callee};
                }
            }
            LValue base = codegen_lvalue(*expr.lhs);
            if (base.type.kind == TypeKind::Named && structs_.contains(base.type.name)) {
                const StructInfo& info = structs_.at(base.type.name);
                std::optional<std::size_t> field_index_opt = info.find_field_index(expr.name);
                if (field_index_opt.has_value() &&
                    info.field_types[*field_index_opt].kind == TypeKind::FunctionPointer) {
                    const Type& member_type = info.field_types[*field_index_opt];
                    llvm::LLVMValueRef field_ptr = info.is_union
                                                 ? llvm::LLVMBuildBitCast(builder_, base.ptr,
                                                                     llvm::LLVMPointerTypeInContext(context_, 0),
                                                                     (expr.name + ".fnptr").c_str())
                                                 : llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, base.ptr,
                                                                       info.physical_field_index(*field_index_opt),
                                                                       (expr.name + ".fnptr").c_str());
                    llvm::LLVMValueRef callee_value =
                        create_load(to_llvm_type(member_type), field_ptr,
                                    info.is_union ? base.alignment
                                                  : std::optional<unsigned>(info.field_alignments[*field_index_opt]),
                                    expr.name + ".fn");
                    std::vector<llvm::LLVMValueRef> args =
                        codegen_call_args_for_types(expr.args, member_type.function_params);
                    std::vector<llvm::LLVMTypeRef> params;
                    params.reserve(member_type.function_params.size());
                    for (const Type& param : member_type.function_params) {
                        params.push_back(to_llvm_type(param));
                    }
                    llvm::LLVMTypeRef fn_type =
                        llvm::LLVMFunctionType(to_llvm_type(*member_type.function_return), params.data(),
                                         static_cast<unsigned>(params.size()), /*IsVarArg=*/0);
                    return CallResult{build_call(fn_type, callee_value, args), nullptr};
                }
            }
            if (receiver_type.has_value() && receiver_type->kind == TypeKind::FunctionPointer) {
                llvm::LLVMValueRef callee_value = codegen_expr(*expr.lhs);
                std::vector<llvm::LLVMValueRef> args = codegen_call_args_for_types(expr.args, receiver_type->function_params);
                std::vector<llvm::LLVMTypeRef> params;
                params.reserve(receiver_type->function_params.size());
                for (const Type& param : receiver_type->function_params) {
                    params.push_back(to_llvm_type(param));
                }
                llvm::LLVMTypeRef fn_type = llvm::LLVMFunctionType(to_llvm_type(*receiver_type->function_return), params.data(),
                                                       static_cast<unsigned>(params.size()), /*IsVarArg=*/0);
                return CallResult{build_call(fn_type, callee_value, args), nullptr};
            }
        }
        if (expr.lhs != nullptr && expr.name.empty()) {
            const Expr* callee_expr = expr.lhs.get();
            if (callee_expr->kind == ExprKind::Unary && callee_expr->unary_op == UnaryOp::Deref && callee_expr->lhs) {
                callee_expr = callee_expr->lhs.get();
            }
            std::optional<Type> callee_type = infer_type(*callee_expr);
            if (!callee_type.has_value() || callee_type->kind != TypeKind::FunctionPointer) {
                throw CodegenError("indirect call requires a function pointer value", current_loc_);
            }
            llvm::LLVMValueRef callee_value = codegen_expr(*callee_expr);
            std::vector<llvm::LLVMValueRef> args = codegen_call_args_for_types(expr.args, callee_type->function_params);
            std::vector<llvm::LLVMTypeRef> params;
            params.reserve(callee_type->function_params.size());
            for (const Type& param : callee_type->function_params) {
                params.push_back(to_llvm_type(param));
            }
            llvm::LLVMTypeRef fn_type = llvm::LLVMFunctionType(to_llvm_type(*callee_type->function_return), params.data(),
                                                   static_cast<unsigned>(params.size()), /*IsVarArg=*/0);
            return CallResult{build_call(fn_type, callee_value, args), nullptr};
        }
        if (expr.lhs == nullptr) {
            if (const Function* builtin_callee = resolve_overload_by_type(expr.name, expr.args, /*param_offset=*/0);
                builtin_callee != nullptr && is_enum_cast_store_builtin_name(builtin_callee->name)) {
                return codegen_enum_cast_store_builtin(expr, *builtin_callee);
            }
            if (find_class_def(expr.name) != nullptr) {
                const Function* ctor_def = nullptr;
                if (!expr.args.empty() || expr.has_paren_init) {
                    std::string ctor_name = expr.name + "_new";
                    ctor_def = resolve_overload_by_type(ctor_name, expr.args, /*param_offset=*/1);
                    if (ctor_def == nullptr) {
                        if (expr.args.empty()) {
                            return CallResult{codegen_constructed_class_value(expr.name, expr.args, nullptr, &expr), nullptr};
                        }
                        throw CodegenError("class '" + expr.name + "' has no constructor matching this call", current_loc_);
                    }
                }
                return CallResult{codegen_constructed_class_value(expr.name, expr.args, ctor_def, &expr), nullptr};
            }
            auto local_it = expr.explicit_global_qualification ? locals_.end() : locals_.find(expr.name);
            if (local_it != locals_.end() && local_it->second.type.kind == TypeKind::FunctionPointer) {
                llvm::LLVMValueRef callee_value = llvm::LLVMBuildLoad2(builder_, to_llvm_type(local_it->second.type), local_it->second.alloca,
                                                           (expr.name + ".fnptr").c_str());
                std::vector<llvm::LLVMValueRef> args = codegen_call_args_for_types(expr.args, local_it->second.type.function_params);
                std::vector<llvm::LLVMTypeRef> params;
                params.reserve(local_it->second.type.function_params.size());
                for (const Type& param : local_it->second.type.function_params) {
                    params.push_back(to_llvm_type(param));
                }
                llvm::LLVMTypeRef fn_type = llvm::LLVMFunctionType(to_llvm_type(*local_it->second.type.function_return), params.data(),
                                                       static_cast<unsigned>(params.size()), /*IsVarArg=*/0);
                return CallResult{build_call(fn_type, callee_value, args), nullptr};
            }
        }
        std::string callee_name = expr.name;
        llvm::LLVMValueRef this_arg = nullptr;
        std::size_t param_offset = 0;
        bool receiver_is_mutable = true;
        std::string receiver_static_class_name;
        if (expr.lhs != nullptr) {
            LValue receiver = codegen_lvalue(*expr.lhs);
            if (receiver.type.kind != TypeKind::Named) {
                throw CodegenError("method call '." + expr.name + "(...)' is only supported on a class type",
                    current_loc_);
            }
            receiver_static_class_name = receiver.type.name;
            callee_name = receiver.type.name + "_" + expr.name;
            this_arg = receiver.ptr;
            param_offset = 1;
            receiver_is_mutable = !is_read_only_place(*expr.lhs);
        }

        // ch05 §5.10: resolve the specific overload this call targets
        // (movecheck has already independently confirmed exactly one
        // overload matches, so this is expected to agree with it -- see
        // resolve_overload_by_type's own comment) *before* generating
        // this call's own arguments below: codegen_call_args needs
        // `callee_def` already in hand to decide value-vs-address per
        // parameter.
        const Function* callee_def =
            resolve_overload_by_type(callee_name, expr.args, param_offset, receiver_is_mutable, expr.lhs.get());
        if (callee_def == nullptr) {
            throw CodegenError("call to unknown function '" + callee_name + "' (resolve)",
                current_loc_);
        }
        llvm::LLVMValueRef callee = llvm::LLVMGetNamedFunction(module_, overload_names_.at(callee_def).c_str());
        if (callee == nullptr) {
            throw CodegenError("call to unknown function '" + callee_name + "' (llvm)",
                current_loc_);
        }
        std::vector<llvm::LLVMValueRef> args = codegen_call_args(expr.args, callee_def, param_offset);
        if (this_arg != nullptr) {
            if (!callee_def->params.empty() && is_interface_reference_type(callee_def->params.front().type)) {
                args.insert(args.begin(), codegen_interface_value_for_target(*expr.lhs, callee_def->params.front().type));
            } else {
                args.insert(args.begin(), this_arg);
                if (std::optional<std::size_t> slot_index =
                        ordinary_method_slot_index(receiver_static_class_name, *callee_def);
                    slot_index.has_value()) {
                    const StructInfo& info = structs_.at(receiver_static_class_name);
                    llvm::LLVMValueRef vptr_slot = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, this_arg, 0, "vptr");
                    llvm::LLVMValueRef vtable_ptr = create_load(llvm::LLVMPointerTypeInContext(context_, 0), vptr_slot, std::nullopt,
                                                          "vtable");
                    llvm::LLVMTypeRef table_type = ordinary_vtable_type(receiver_static_class_name);
                    llvm::LLVMValueRef table_ptr =
                        llvm::LLVMBuildBitCast(builder_, vtable_ptr, llvm::LLVMPointerTypeInContext(context_, 0), "vtable.array");
                    llvm::LLVMTypeRef i32 = llvm::LLVMInt32TypeInContext(context_);
                    llvm::LLVMValueRef slot_indices[] = {llvm::LLVMConstInt(i32, 0, 0),
                                                   llvm::LLVMConstInt(i32, static_cast<unsigned>(*slot_index), 0)};
                    llvm::LLVMValueRef slot_ptr =
                        llvm::LLVMBuildGEP2(builder_, table_type, table_ptr, slot_indices, 2, "vtable.slot");
                    llvm::LLVMValueRef target_ptr =
                        create_load(llvm::LLVMPointerTypeInContext(context_, 0), slot_ptr, std::nullopt, "virtfn");
                    return CallResult{build_call(interface_dispatch_function_type(*callee_def), target_ptr, args),
                                      callee_def};
                }
            }
        }
        return CallResult{build_call(callee, args), callee_def};
    }


    void Codegen::initialize_reference_storage(const Codegen::LValue& target, const Expr& expr)
{
        if (target.type.kind != TypeKind::Reference || target.type.pointee == nullptr) {
            throw CodegenError("internal error: reference initializer target is not a reference", current_loc_);
        }
        if (is_interface_reference_type(target.type)) {
            create_store(codegen_interface_value_for_target(expr, target.type), target.ptr, target.alignment);
            return;
        }
        validate_reference_pointee(*target.type.pointee);
        llvm::LLVMValueRef referent_addr =
            const_reference_binds_materialized_temporary(expr, target.type)
                ? codegen_materialize_const_reference_source(expr, *target.type.pointee)
                : codegen_lvalue(expr).ptr;
        create_store(referent_addr, target.ptr, target.alignment);
    }


    void Codegen::initialize_span_storage(const Codegen::LValue& target, const Expr& expr)
{
        if (target.type.kind != TypeKind::Span || target.type.pointee == nullptr) {
            throw CodegenError("internal error: span initializer target is not a span", current_loc_);
        }
        llvm::LLVMValueRef span_value = codegen_span_value_for_target(expr, target.type);
        create_store(span_value, target.ptr, target.alignment);
    }


    bool Codegen::try_initialize_class_storage_from_same_type_source(const Codegen::LValue& target, const std::vector<ExprPtr>& args)
{
        if (!is_named_record_type(target.type) || args.size() != 1) {
            return false;
        }
        if (produces_rvalue_of_type(*args[0], target.type)) {
            create_store(codegen_expr(*args[0]), target.ptr, target.alignment);
            if (class_has_ordinary_vtable(target.type.name)) {
                initialize_ordinary_vtable_pointer(target.type.name, target.ptr);
            }
            return true;
        }
        bool allow_hidden_helper_copy =
            current_function_def_ != nullptr && current_function_def_->is_compile_time_dependency;
        if (!is_bare_same_type_copy_source(*args[0], target.type) ||
            (!allow_hidden_helper_copy && !is_copy_constructible(target.type.name))) {
            return false;
        }
        LValue src = codegen_lvalue(*args[0]);
        codegen_copy_construct_class(target.ptr, src.ptr, target.type.name);
        return true;
    }


    void Codegen::initialize_storage_from_expr(const Codegen::LValue& target, const Expr& expr)
{
        if (target.type.kind == TypeKind::Reference) {
            initialize_reference_storage(target, expr);
            return;
        }
        if (target.type.kind == TypeKind::Span) {
            initialize_span_storage(target, expr);
            return;
        }
        if (is_named_record_type(target.type)) {
            llvm::LLVMValueRef value = codegen_class_value_for_boundary(expr, target.type);
            create_store(value, target.ptr, target.alignment);
            if (class_has_ordinary_vtable(target.type.name)) {
                initialize_ordinary_vtable_pointer(target.type.name, target.ptr);
            }
            return;
        }
        llvm::LLVMValueRef init_value = codegen_value_for_target(expr, target.type);
        check_store_type(init_value, to_llvm_type(target.type), "member initializer");
        create_store(init_value, target.ptr, target.alignment);
    }


    void Codegen::initialize_storage_from_brace_args(const Codegen::LValue& target, const std::vector<ExprPtr>& args)
{
        if (target.type.kind == TypeKind::Reference) {
            if (args.size() != 1) {
                throw CodegenError("a reference member must be initialized with exactly one expression", current_loc_);
            }
            initialize_reference_storage(target, *args[0]);
            return;
        }
        if (target.type.kind == TypeKind::Span) {
            if (args.size() != 1) {
                throw CodegenError("a span member must be initialized with exactly one array expression", current_loc_);
            }
            initialize_span_storage(target, *args[0]);
            return;
        }
        if (target.type.kind == TypeKind::Named && find_class_def(target.type.name) != nullptr) {
            zero_initialize_storage(target.ptr, target.type, target.alignment);
            if (try_initialize_class_storage_from_same_type_source(target, args)) return;
            const Function* ctor_def = resolve_overload_by_type(target.type.name + "_new", args, /*param_offset=*/1);
            if (ctor_def == nullptr) {
                const ClassDef* class_def = find_class_def(target.type.name);
                if (args.empty() && class_def != nullptr && !class_has_any_constructor(target.type.name)) {
                    emit_default_initializers_for_class_storage(target.ptr, *class_def, /*initialize_virtual_interface_bases=*/true);
                    return;
                }
                throw CodegenError("class '" + target.type.name + "' has no constructor matching this call", current_loc_);
            }
            if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                llvm::LLVMValueRef value = codegen_constructed_class_value(target.type.name, args, ctor_def);
                create_store(value, target.ptr, target.alignment);
                if (class_has_ordinary_vtable(target.type.name)) {
                    initialize_ordinary_vtable_pointer(target.type.name, target.ptr);
                }
                return;
            }
            llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
            if (ctor == nullptr) {
                throw CodegenError("class '" + target.type.name + "' has no constructor matching this call", current_loc_);
            }
            if (const ClassDef* class_def = find_class_def(target.type.name)) {
                emit_complete_object_interface_initializers(*class_def, ctor_def, target.ptr);
            }
            std::vector<llvm::LLVMValueRef> ctor_args = codegen_call_args(args, ctor_def, /*param_offset=*/1);
            ctor_args.insert(ctor_args.begin(), target.ptr);
            build_call(ctor, ctor_args);
            return;
        }
        if (args.empty()) {
            zero_initialize_storage(target.ptr, target.type, target.alignment);
            return;
        }
        if (args.size() != 1) {
            throw CodegenError("brace-initialization of this member requires exactly one expression", current_loc_);
        }
        initialize_storage_from_expr(target, *args[0]);
    }


    void Codegen::initialize_storage(const Codegen::LValue& target, const Initializer& init)
{
        if (init.has_brace_args) {
            initialize_storage_from_brace_args(target, init.brace_args);
            return;
        }
        if (init.expr) {
            initialize_storage_from_expr(target, *init.expr);
            return;
        }
        zero_initialize_storage(target.ptr, target.type, target.alignment);
    }


    llvm::LLVMValueRef Codegen::codegen_class_value_for_boundary(const Expr& expr, const Type& target_type,
                                                  bool allow_implicit_converting_ctor)
{
        llvm::LLVMTypeRef llvm_type = to_llvm_type(target_type);
        bool allow_hidden_helper_copy =
            current_function_def_ != nullptr && current_function_def_->is_compile_time_dependency;
        if (is_bare_same_type_copy_source(expr, target_type) &&
            (allow_hidden_helper_copy || is_copy_constructible(target_type.name))) {
            llvm::LLVMValueRef temp = create_entry_block_alloca(llvm_type, "classtransport");
            codegen_copy_construct_class(temp, codegen_lvalue(expr).ptr, target_type.name);
            return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "classtransport.value");
        }
        if (expr.kind == ExprKind::Lambda) {
            llvm::LLVMValueRef temp = codegen_expr(expr);
            return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "classtransport.lambda");
        }
        if (produces_rvalue_of_type(expr, target_type)) {
            return codegen_expr(expr);
        }
        if (allow_implicit_converting_ctor) {
            if (const Function* converting_ctor = resolve_converting_constructor_by_type(target_type.name, expr);
                converting_ctor != nullptr) {
                std::vector<ExprPtr> ctor_args;
                ctor_args.push_back(clone_expr(expr));
                return codegen_constructed_class_value(target_type.name, ctor_args, converting_ctor);
            }
        }
        return codegen_expr(expr);
    }


    llvm::LLVMValueRef Codegen::codegen_interface_value_for_target(const Expr& expr, const Type& target_type)
{
        std::optional<Type> source_type = infer_type(expr);
        if (!source_type.has_value()) {
            throw CodegenError("cannot determine interface conversion source type", current_loc_);
        }
        if (types_equal(*source_type, target_type)) return codegen_expr(expr);
        if (target_type.kind == TypeKind::Reference && target_type.pointee != nullptr &&
            target_type.pointee->kind == TypeKind::Named) {
            if (source_type->kind == TypeKind::Named) {
                if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr) {
                    std::optional<Type> operand_type = infer_type(*expr.lhs);
                    if (operand_type.has_value() && is_interface_pointer_type(*operand_type)) return codegen_expr(expr);
                }
                llvm::LLVMValueRef object_ptr = codegen_lvalue(expr).ptr;
                llvm::LLVMValueRef table_ptr =
                    get_or_create_interface_dispatch_table(source_type->name, target_type.pointee->name);
                return build_interface_value(object_ptr, table_ptr);
            }
            if (source_type->kind == TypeKind::Reference && source_type->pointee != nullptr &&
                source_type->pointee->kind == TypeKind::Named && !type_names_interface(source_type->pointee->name)) {
                llvm::LLVMValueRef object_ptr = codegen_lvalue(expr).ptr;
                llvm::LLVMValueRef table_ptr =
                    get_or_create_interface_dispatch_table(source_type->pointee->name, target_type.pointee->name);
                return build_interface_value(object_ptr, table_ptr);
            }
        }
        if (target_type.kind == TypeKind::Pointer && target_type.pointee != nullptr &&
            target_type.pointee->kind == TypeKind::Named) {
            if (source_type->kind == TypeKind::Pointer && source_type->pointee != nullptr &&
                source_type->pointee->kind == TypeKind::Named &&
                type_names_interface(target_type.pointee->name) &&
                type_names_interface(source_type->pointee->name) &&
                source_type->pointee->name == target_type.pointee->name &&
                (!target_type.is_mutable_pointee || source_type->is_mutable_pointee)) {
                return codegen_expr(expr);
            }
            if (source_type->kind == TypeKind::Pointer && source_type->pointee != nullptr &&
                source_type->pointee->kind == TypeKind::Named && !type_names_interface(source_type->pointee->name)) {
                llvm::LLVMValueRef object_ptr = codegen_expr(expr);
                llvm::LLVMValueRef table_ptr =
                    get_or_create_interface_dispatch_table(source_type->pointee->name, target_type.pointee->name);
                return build_interface_value(object_ptr, table_ptr);
            }
        }
        if (source_type->kind == TypeKind::Reference && source_type->pointee != nullptr &&
            target_type.kind == TypeKind::Reference && target_type.pointee != nullptr &&
            types_equal(*source_type->pointee, *target_type.pointee)) {
            return codegen_expr(expr);
        }
        throw CodegenError("unsupported interface conversion at code generation time", current_loc_);
    }


    llvm::LLVMValueRef Codegen::codegen_span_value_for_target(const Expr& expr, const Type& target_type)
{
        if (target_type.kind != TypeKind::Span || target_type.pointee == nullptr) {
            throw CodegenError("internal error: span conversion target is not a span", current_loc_);
        }
        if (std::optional<Type> source_type = infer_type(expr); source_type.has_value() && types_equal(*source_type, target_type)) {
            return codegen_expr(expr);
        }
        LValue source = codegen_lvalue(expr);
        if (source.type.kind != TypeKind::Array) {
            throw CodegenError("std::span<T> can currently only be constructed from a fixed-size array in this version",
                               current_loc_);
        }
        if (to_llvm_type(*source.type.element) != to_llvm_type(*target_type.pointee)) {
            throw CodegenError("array element type does not match the span's element type", current_loc_);
        }
        llvm::LLVMTypeRef span_type = to_llvm_type(target_type);
        llvm::LLVMValueRef size_value = llvm::LLVMConstInt(llvm::LLVMInt64TypeInContext(context_), static_cast<std::uint64_t>(source.type.array_size), 0);
        llvm::LLVMValueRef span_value = llvm::LLVMGetUndef(span_type);
        span_value = llvm::LLVMBuildInsertValue(builder_, span_value, source.ptr, 0, "");
        span_value = llvm::LLVMBuildInsertValue(builder_, span_value, size_value, 1, "");
        return span_value;
    }


    llvm::LLVMValueRef Codegen::codegen_contextual_bool_value(const Expr& expr)
{
        std::optional<Type> expr_type = infer_type(expr);
        if (expr_type.has_value() && is_interface_pointer_type(*expr_type)) {
            llvm::LLVMValueRef interface_value = codegen_expr(expr);
            llvm::LLVMValueRef object_ptr = extract_interface_object_ptr(interface_value);
            return i1_to_bool(llvm::LLVMBuildICmp(builder_, llvm::LLVMIntNE,
                object_ptr, llvm::LLVMConstPointerNull(llvm::LLVMPointerTypeInContext(context_, 0)), "ifacenotnull"));
        }
        return codegen_expr(expr);
    }


    llvm::LLVMValueRef Codegen::codegen_contextual_bool_i1(const Expr& expr)
{
        return bool_to_i1(codegen_contextual_bool_value(expr));
    }


    std::vector<llvm::LLVMValueRef> Codegen::codegen_call_args(const std::vector<ExprPtr>& args, const Function* callee_def,
                                                  std::size_t param_offset)
{
        std::vector<llvm::LLVMValueRef> result;
        auto emit_arg = [&](const Expr& arg, std::size_t i) {
            bool param_is_reference = callee_def != nullptr && i + param_offset < callee_def->params.size() &&
                                       callee_def->params[i + param_offset].type.kind == TypeKind::Reference;
            const Type* ref_param_type =
                param_is_reference ? &callee_def->params[i + param_offset].type : nullptr;
            bool param_is_interface_reference = param_is_reference && is_interface_reference_type(*ref_param_type);
            bool param_is_rvalue_reference = param_is_reference && ref_param_type->is_rvalue_ref;
            // ch05 §5.x: a *const* (non-rvalue, non-mutable) reference
            // parameter may also bind directly to a fresh rvalue argument
            // -- movecheck's own argument_matches_parameter/
            // apply_reference_argument already gate this identically
            // (produces_rvalue_of_type), only ever for a const reference
            // (real C++ itself forbids binding a *mutable* lvalue
            // reference to a temporary).
            bool param_is_const_reference_bound_to_rvalue =
                param_is_reference && const_reference_binds_materialized_temporary(arg, *ref_param_type);
            if (param_is_interface_reference) {
                result.push_back(codegen_interface_value_for_target(arg, *ref_param_type));
            } else if (param_is_rvalue_reference || param_is_const_reference_bound_to_rvalue) {
                // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- the move
                // checker has already verified this argument produces a
                // genuine rvalue (produces_rvalue_of_type), which may not
                // itself be an addressable place (a literal, a fresh
                // std::make_unique<T>(...)/call result, ...).
                result.push_back(param_is_rvalue_reference ? codegen_materialize_rvalue_reference_source(arg)
                                                           : codegen_materialize_const_reference_source(
                                                                 arg, *ref_param_type->pointee));
            } else if (param_is_reference) {
                // Bind the reference parameter to the argument's address
                // rather than passing its value, exactly like a local
                // reference's own VarDecl.
                result.push_back(codegen_lvalue(arg).ptr);
            } else {
                // ch06 §6: a bare literal argument adapts directly to
                // its target parameter's own declared scalar type (see
                // codegen_value_for_target) -- exactly like a VarDecl
                // initializer/plain assignment's identical treatment,
                // rather than defaulting to `int`/`double` and failing
                // the callee's own parameter-type check.
                if (callee_def != nullptr && i + param_offset < callee_def->params.size()) {
                    const Type& param_type = callee_def->params[i + param_offset].type;
                    if (is_named_record_type(param_type)) {
                        result.push_back(codegen_class_value_for_boundary(arg, param_type,
                                                                         /*allow_implicit_converting_ctor=*/true));
                    } else {
                        result.push_back(codegen_value_for_target(arg, param_type));
                    }
                } else {
                    result.push_back(codegen_expr(arg));
                }
            }
        };
        result.reserve(args.size());
        for (std::size_t i = 0; i < args.size(); i++) emit_arg(*args[i], i);
        if (callee_def != nullptr) {
            for (std::size_t i = args.size(); i + param_offset < callee_def->params.size(); i++) {
                const Param& param = callee_def->params[i + param_offset];
                if (param.default_expr == nullptr) break;
                ExprPtr default_arg = deep_clone_expr_with_loc(*param.default_expr, current_loc_);
                emit_arg(*default_arg, i);
            }
        }
        return result;
    }


    std::vector<llvm::LLVMValueRef> Codegen::codegen_call_args_for_types(const std::vector<ExprPtr>& args,
                                                          const std::vector<Type>& param_types)
{
        std::vector<llvm::LLVMValueRef> result;
        result.reserve(args.size());
        for (std::size_t i = 0; i < args.size(); i++) {
            bool param_is_reference = i < param_types.size() && param_types[i].kind == TypeKind::Reference;
            const Type* ref_param_type = param_is_reference ? &param_types[i] : nullptr;
            bool param_is_interface_reference = param_is_reference && is_interface_reference_type(*ref_param_type);
            bool param_is_rvalue_reference = param_is_reference && ref_param_type->is_rvalue_ref;
            bool param_is_const_reference_bound_to_rvalue =
                param_is_reference && const_reference_binds_materialized_temporary(*args[i], *ref_param_type);
            if (param_is_interface_reference) {
                result.push_back(codegen_interface_value_for_target(*args[i], *ref_param_type));
            } else if (param_is_rvalue_reference || param_is_const_reference_bound_to_rvalue) {
                result.push_back(param_is_rvalue_reference ? codegen_materialize_rvalue_reference_source(*args[i])
                                                           : codegen_materialize_const_reference_source(
                                                                 *args[i], *ref_param_type->pointee));
            } else if (param_is_reference) {
                result.push_back(codegen_lvalue(*args[i]).ptr);
            } else if (i < param_types.size()) {
                if (is_named_record_type(param_types[i])) {
                    result.push_back(codegen_class_value_for_boundary(*args[i], param_types[i],
                                                                     /*allow_implicit_converting_ctor=*/true));
                } else {
                    result.push_back(codegen_value_for_target(*args[i], param_types[i]));
                }
            } else {
                result.push_back(codegen_expr(*args[i]));
            }
        }
        return result;
    }


    llvm::LLVMValueRef Codegen::load_value(const Codegen::LValue& lv)
{
        if (lv.type.kind == TypeKind::Array) {
            return lv.ptr;
        }
        return create_load(to_llvm_type(lv.type), lv.ptr, lv.alignment, "loadtmp");
    }


    llvm::LLVMValueRef Codegen::bool_to_i1(llvm::LLVMValueRef v)
{
        if (!(llvm::LLVMGetTypeKind(llvm::LLVMTypeOf(v)) == llvm::LLVMIntegerTypeKind && llvm::LLVMGetIntTypeWidth(llvm::LLVMTypeOf(v)) == 8)) {
            throw CodegenError(
                "expected a 'bool' value here (e.g. an if/while condition, or an '&&'/'||' operand); "
                "scpp requires an explicit cast for any scalar-to-bool conversion, unlike real C++ "
                "(spec ch06)",
                current_loc_);
        }
        return llvm::LLVMBuildTrunc(builder_, v, llvm::LLVMInt1TypeInContext(context_), "tobool");
    }


    llvm::LLVMValueRef Codegen::i1_to_bool(llvm::LLVMValueRef v)
{
        return llvm::LLVMBuildZExt(builder_, v, llvm::LLVMInt8TypeInContext(context_), "boolext");
    }


    [[nodiscard]] bool Codegen::enum_value_fits_source_type(const Type& source_type, long long enum_value)
{
        if (source_type.kind != TypeKind::Named || !is_integral_scalar_type_name(source_type.name)) return false;
        llvm::LLVMTypeRef integer_type = to_llvm_type(source_type);
        if (llvm::LLVMGetTypeKind(integer_type) != llvm::LLVMIntegerTypeKind) return false;
        unsigned bits = llvm::LLVMGetIntTypeWidth(integer_type);
        bool source_is_unsigned = is_unsigned_for_cast(source_type.name);
        if (source_is_unsigned) {
            if (enum_value < 0) return false;
            if (bits >= 64) return true;
            std::uint64_t max_value = (std::uint64_t{1} << bits) - 1;
            return static_cast<std::uint64_t>(enum_value) <= max_value;
        }
        if (bits >= 64) return true;
        long long min_value = -(std::int64_t{1} << (bits - 1));
        long long max_value = (std::int64_t{1} << (bits - 1)) - 1;
        return enum_value >= min_value && enum_value <= max_value;
    }


    llvm::LLVMValueRef Codegen::build_integral_enum_match(llvm::LLVMValueRef source, const Type& source_type, long long enum_value)
{
        llvm::LLVMTypeRef source_integer_type = llvm::LLVMTypeOf(source);
        if (llvm::LLVMGetTypeKind(source_integer_type) != llvm::LLVMIntegerTypeKind || !enum_value_fits_source_type(source_type, enum_value)) {
            return llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0);
        }
        if (is_unsigned_for_cast(source_type.name)) {
            return llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ,
                source, llvm::LLVMConstInt(source_integer_type, static_cast<std::uint64_t>(enum_value), 0),
                "enumcastcmp");
        }
        return llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, source, llvm::LLVMConstInt(source_integer_type, static_cast<std::uint64_t>(enum_value), 1),
                                      "enumcastcmp");
    }


    llvm::LLVMValueRef Codegen::enum_variant_constant(llvm::LLVMTypeRef enum_storage_type, const Type& underlying_type, long long enum_value)
{
        if (is_unsigned_for_cast(underlying_type.name)) {
            return llvm::LLVMConstInt(enum_storage_type, static_cast<std::uint64_t>(enum_value), 0);
        }
        return llvm::LLVMConstInt(enum_storage_type, static_cast<std::uint64_t>(enum_value), 1);
    }


    Codegen::CallResult Codegen::codegen_enum_cast_store_builtin(const Expr& expr, const Function& callee_def)
{
        if (expr.args.size() != 2 || callee_def.params.size() != 2) {
            throw CodegenError("internal error: malformed scpp::__enum_cast_store call", current_loc_);
        }
        const Type& source_type = callee_def.params[0].type;
        const Type& out_param_type = callee_def.params[1].type;
        if (source_type.kind != TypeKind::Named || !is_integral_scalar_type_name(source_type.name)) {
            throw CodegenError("scpp::enum_cast<T>(value) requires an integral source value", current_loc_);
        }
        if (out_param_type.kind != TypeKind::Reference || out_param_type.pointee == nullptr ||
            out_param_type.pointee->kind != TypeKind::Named) {
            throw CodegenError("scpp::enum_cast<T>(value) requires T to be an enum class", current_loc_);
        }
        const EnumDef* enum_def = find_enum_def(program_, out_param_type.pointee->name);
        if (enum_def == nullptr) {
            throw CodegenError("scpp::enum_cast<T>(value) requires T to be an enum class", current_loc_);
        }

        llvm::LLVMValueRef source_value = codegen_value_for_target(*expr.args[0], source_type);
        LValue out = codegen_lvalue(*expr.args[1]);
        llvm::LLVMTypeRef enum_storage_type = to_llvm_type(*out_param_type.pointee);
        llvm::LLVMValueRef matched = llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0);
        llvm::LLVMValueRef selected =
            enum_variant_constant(enum_storage_type, enum_def->underlying_type, 0);
        for (const EnumVariant& variant : enum_def->variants) {
            llvm::LLVMValueRef variant_matches = build_integral_enum_match(source_value, source_type, variant.value);
            matched = llvm::LLVMBuildOr(builder_, matched, variant_matches, "enumcastmatch");
            selected = llvm::LLVMBuildSelect(builder_,
                variant_matches, enum_variant_constant(enum_storage_type, enum_def->underlying_type, variant.value), selected,
                "enumcastselect");
        }
        create_store(selected, out.ptr, out.alignment);
        return CallResult{i1_to_bool(matched), &callee_def};
    }


    llvm::LLVMValueRef Codegen::codegen_value_for_target(const Expr& expr, const Type& target_type)
{
        if (is_interface_representation_type(target_type)) {
            return codegen_interface_value_for_target(expr, target_type);
        }
        // `-100`/`-1.5` (a negated literal, ExprKind::Unary/Neg over a
        // bare literal) is just as untyped as the bare literal itself --
        // real C++ itself treats a unary-minus-literal as a single
        // token for exactly this reason (a negative literal, not "minus
        // applied to a positive one"). Recurses once, with the negation
        // folded into the literal's own value, rather than falling
        // through to plain codegen_expr (which would infer a fixed
        // int/double type for the un-negated literal, then apply `-` in
        // that type, defeating the point).
        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Neg) {
            if (expr.lhs->kind == ExprKind::IntegerLiteral) {
                Expr negated;
                negated.kind = ExprKind::IntegerLiteral;
                negated.loc = expr.loc;
                negated.int_value = -expr.lhs->int_value;
                return codegen_value_for_target(negated, target_type);
            }
            if (expr.lhs->kind == ExprKind::FloatLiteral) {
                Expr negated;
                negated.kind = ExprKind::FloatLiteral;
                negated.loc = expr.loc;
                negated.float_value = -expr.lhs->float_value;
                return codegen_value_for_target(negated, target_type);
            }
        }
        if (target_type.kind == TypeKind::Named) {
            if (expr.kind == ExprKind::IntegerLiteral) {
                if (is_float_scalar_type_name(target_type.name)) {
                    return llvm::LLVMConstReal(to_llvm_type(target_type), static_cast<double>(expr.int_value));
                }
                if (target_type.name != "bool" && target_type.name != "char") {
                    return llvm::LLVMConstInt(to_llvm_type(target_type), static_cast<std::uint64_t>(expr.int_value),
                                                   /*SignExtend=*/!is_unsigned_scalar_type_name(target_type.name));
                }
            } else if (expr.kind == ExprKind::FloatLiteral && is_float_scalar_type_name(target_type.name)) {
                return llvm::LLVMConstReal(to_llvm_type(target_type), expr.float_value);
            }
        }
        if (target_type.kind == TypeKind::FunctionPointer) {
            if (llvm::LLVMValueRef fn = codegen_function_pointer_value_for_target(expr, target_type)) return fn;
        }
        if (target_type.kind == TypeKind::Span) {
            return codegen_span_value_for_target(expr, target_type);
        }
        return codegen_expr(expr);
    }


    void Codegen::check_store_type(llvm::LLVMValueRef value, llvm::LLVMTypeRef expected, const std::string& what)
{
        if (llvm::LLVMTypeOf(value) != expected) {
            throw CodegenError("type mismatch initializing/assigning " + what +
                                ": scpp has no implicit conversion between distinct scalar types (e.g. "
                                "bool/char/int are all distinct, spec ch06) -- an explicit cast would be "
                                "required, but cast expressions aren't implemented in this version yet",
                current_loc_);
        }
    }


    llvm::LLVMValueRef Codegen::codegen_expr(const Expr& expr)
{
        // Refreshed on every call (including each recursive call for a
        // child sub-expression), same reasoning as codegen_stmt above --
        // so a CodegenError thrown while examining `expr` itself (before
        // or after recursing into any children) reports `expr`'s own
        // position, not whichever child was most recently visited.
        refresh_debug_location(expr.loc);
        switch (expr.kind) {
            case ExprKind::IntegerLiteral:
                return llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), static_cast<std::uint64_t>(expr.int_value), /*SignExtend=*/1);

            case ExprKind::FloatLiteral:
                // Defaults to `double` (ch06 §6, real C++'s own
                // no-suffix default) -- adapted to a narrower/other float
                // type by context wherever the target type is known
                // instead (VarDecl/Assign/call argument/return -- see
                // codegen_value_for_target), exactly like an
                // IntegerLiteral's own default-to-`int` treatment.
                return llvm::LLVMConstReal(llvm::LLVMDoubleTypeInContext(context_), expr.float_value);

            case ExprKind::BoolLiteral:
            case ExprKind::TypeTrait:
                // `bool` is stored as a full byte (i8; see to_llvm_type
                // and its false=0/true=1 invariant, ch06) -- a literal's
                // value is already exactly 0 or 1, so no i1_to_bool
                // widening is needed here (unlike a comparison/logical
                // result, which starts out as a genuine i1).
                return llvm::LLVMConstInt(llvm::LLVMInt8TypeInContext(context_), expr.bool_value ? 1 : 0, 0);

            case ExprKind::CharLiteral:
                // `char` is its own distinct 1-byte type (ch06) -- not an
                // alias for any fixed-width integer type, so it takes no
                // stance on signedness at all (no implicit arithmetic or
                // cross-type comparison exists for it to matter for);
                // `expr.int_value` already holds the decoded ordinal
                // value 0-255 (see parser's decode_char_literal), which
                // fits identically in the 8 bits either way.
                return llvm::LLVMConstInt(llvm::LLVMInt8TypeInContext(context_), static_cast<std::uint64_t>(expr.int_value), /*SignExtend=*/0);

            case ExprKind::Alignof:
                return codegen_alignof_value(expr);

            case ExprKind::Sizeof:
                return codegen_sizeof_value(expr);

            case ExprKind::StringLiteral:
                // A read-only global byte array (null-terminated, like a
                // real C string literal), decaying directly to a pointer
                // to its first byte -- there is no backing local
                // variable/place for a literal, so (unlike an array-typed
                // identifier's load_value decay) this needs no separate
                // lvalue-then-decay step; CreateGlobalString itself
                // returns the pointer. Reuses the exact mechanism already
                // used for print_bool's "true"/"false" constants.
                return llvm::LLVMBuildGlobalString(builder_, expr.name.c_str(), "str");

            case ExprKind::Conditional: {
                llvm::LLVMValueRef cond = codegen_contextual_bool_i1(*expr.lhs);
                llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
                llvm::LLVMBasicBlockRef then_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.then");
                llvm::LLVMBasicBlockRef else_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.else");
                llvm::LLVMBasicBlockRef merge_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.end");
                llvm::LLVMBuildCondBr(builder_, cond, then_block, else_block);

                llvm::LLVMPositionBuilderAtEnd(builder_, then_block);
                llvm::LLVMValueRef then_value = codegen_expr(*expr.rhs);
                llvm::LLVMBuildBr(builder_, merge_block);
                llvm::LLVMBasicBlockRef then_end = llvm::LLVMGetInsertBlock(builder_);

                llvm::LLVMPositionBuilderAtEnd(builder_, else_block);
                llvm::LLVMValueRef else_value = codegen_expr(*expr.third);
                llvm::LLVMBuildBr(builder_, merge_block);
                llvm::LLVMBasicBlockRef else_end = llvm::LLVMGetInsertBlock(builder_);

                llvm::LLVMPositionBuilderAtEnd(builder_, merge_block);
                if (llvm::LLVMTypeOf(then_value) != llvm::LLVMTypeOf(else_value)) {
                    throw CodegenError("conditional operator requires both arms to have the same type", current_loc_);
                }
                llvm::LLVMValueRef phi = llvm::LLVMBuildPhi(builder_, llvm::LLVMTypeOf(then_value), "condtmp");
                llvm::LLVMValueRef incoming_values[] = {then_value, else_value};
                llvm::LLVMBasicBlockRef incoming_blocks[] = {then_end, else_end};
                llvm::LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
                return phi;
            }

            case ExprKind::Cast: {
                // ch06 §6 / spec §5.1(5.2): `static_cast<T>(expr)`/`(T)expr`
                // converts either between scalar types, or between raw
                // pointer types (movecheck already enforces the latter's
                // unsafe-context requirement). With llvm::LLVM opaque pointers,
                // every raw pointer lowers to the same `ptr` type, so a
                // pointer-to-pointer cast is a codegen no-op.
                std::optional<Type> source_type = infer_type(*expr.lhs);
                if (!source_type.has_value()) {
                    throw CodegenError("cast operand has no inferable type", current_loc_);
                }
                if (is_interface_representation_type(*source_type) || is_interface_representation_type(expr.type)) {
                    throw CodegenError("casts involving interface-typed pointers or references are not supported",
                                       current_loc_);
                }
                if (source_type->kind == TypeKind::Pointer && expr.type.kind == TypeKind::Pointer) {
                    return codegen_value_for_target(*expr.lhs, *source_type);
                }
                if (source_type->kind != TypeKind::Named || expr.type.kind != TypeKind::Named) {
                    throw CodegenError("cast is only supported between scalar types or raw pointer types in this version",
                                       current_loc_);
                }
                if (is_integral_scalar_type_name(source_type->name) && find_enum_def(program_, expr.type.name) != nullptr) {
                    throw CodegenError("cannot cast an integer value to enum class '" + expr.type.name +
                                           "'; use scpp::enum_cast<" + expr.type.name + ">(value) instead",
                                       current_loc_);
                }
                bool source_is_scalar_or_enum =
                    is_scalar_type_name(source_type->name) || find_enum_def(program_, source_type->name) != nullptr;
                bool target_is_scalar_or_enum =
                    is_scalar_type_name(expr.type.name) || find_enum_def(program_, expr.type.name) != nullptr;
                if (!source_is_scalar_or_enum || !target_is_scalar_or_enum) {
                    throw CodegenError(
                        "cast is only supported between builtin scalar types or between an enum class and its "
                        "underlying integer type in this version",
                        current_loc_);
                }
                llvm::LLVMValueRef operand = codegen_value_for_target(*expr.lhs, *source_type);
                return codegen_scalar_cast(operand, *source_type, expr.type);
            }

            case ExprKind::Identifier: {
                if (expr.explicit_global_qualification || !locals_.contains(expr.name)) {
                    if (find_visible_global_slot(expr.name, expr.explicit_global_qualification) != nullptr) {
                        LValue lv = codegen_lvalue(expr);
                        return load_value(lv);
                    }
                    const EnumDef* enum_def = nullptr;
                    const EnumVariant* enum_variant = find_enum_variant(program_, expr.name, &enum_def);
                    if (enum_variant != nullptr) {
                        return llvm::LLVMConstInt(to_llvm_type(named_type(enum_def->name)), static_cast<std::uint64_t>(enum_variant->value),
                                                      /*SignExtend=*/!is_unsigned_scalar_type_name(
                                                          enum_def->underlying_type.name));
                    }
                    if (std::optional<Type> fn_type = resolve_function_designator_type(expr)) {
                        if (llvm::LLVMValueRef fn = codegen_function_pointer_value_for_target(expr, *fn_type)) return fn;
                    }
                    if (expr.explicit_global_qualification) {
                        throw CodegenError("use of undeclared global name '" + expr.name + "'", current_loc_);
                    }
                }
                LValue lv = codegen_lvalue(expr);
                return load_value(lv);
            }

            case ExprKind::Subscript: {
                LValue lv = codegen_lvalue(expr);
                return load_value(lv);
            }

            case ExprKind::Member: {
                // `s.size` on a std::span<T> is a computed, read-only
                // property (there's no backing storage to take the
                // address of at the *scpp* type level -- it's an i64
                // internally but exposed as a plain `int`, see
                // to_llvm_type's Span case) -- codegen_lvalue's own
                // Member case rejects it outright for that reason, so it
                // has to be handled here instead, before falling back to
                // the ordinary lvalue-then-load pattern used for a real
                // struct field.
                LValue base = codegen_lvalue(*expr.lhs);
                if (base.type.kind == TypeKind::Span && expr.name == "size") {
                    llvm::LLVMValueRef size_ptr = llvm::LLVMBuildStructGEP2(builder_, to_llvm_type(base.type), base.ptr, 1, "sizeptr");
                    llvm::LLVMValueRef size64 = llvm::LLVMBuildLoad2(builder_, llvm::LLVMInt64TypeInContext(context_), size_ptr, "size64");
                    return llvm::LLVMBuildTrunc(builder_, size64, llvm::LLVMInt32TypeInContext(context_), "size");
                }
                LValue lv = codegen_lvalue(expr);
                return load_value(lv);
            }

            case ExprKind::Unary: {
                if (expr.unary_op == UnaryOp::Deref) {
                    if (std::optional<Type> operand_type = infer_type(*expr.lhs);
                        operand_type.has_value() && is_interface_pointer_type(*operand_type)) {
                        return codegen_expr(*expr.lhs);
                    }
                    if (std::optional<Type> operand_type = infer_type(*expr.lhs);
                        operand_type.has_value() && operand_type->kind == TypeKind::FunctionPointer) {
                        return codegen_expr(*expr.lhs);
                    }
                    // Same lvalue-then-load pattern as Identifier/Member/
                    // Subscript above: codegen_lvalue resolves *what*
                    // `*p` addresses (see its own Unary case), this just
                    // reads the value stored there.
                    LValue lv = codegen_lvalue(expr);
                    return create_load(to_llvm_type(lv.type), lv.ptr, lv.alignment, "loadtmp");
                }
                if (expr.unary_op == UnaryOp::AddressOf) {
                    if (std::optional<Type> operand_type = infer_type(*expr.lhs); operand_type.has_value()) {
                        if (is_interface_reference_type(*operand_type)) {
                            return codegen_expr(*expr.lhs);
                        }
                        if (expr.lhs->kind == ExprKind::Unary && expr.lhs->unary_op == UnaryOp::Deref && expr.lhs->lhs != nullptr) {
                            std::optional<Type> inner = infer_type(*expr.lhs->lhs);
                            if (inner.has_value() && is_interface_pointer_type(*inner)) {
                                return codegen_expr(*expr.lhs->lhs);
                            }
                        }
                    }
                    if (std::optional<Type> fn_type = resolve_function_designator_type(expr)) {
                        if (llvm::LLVMValueRef fn = codegen_function_pointer_value_for_target(expr, *fn_type)) return fn;
                    }
                    // `&expr` (ch05 §5.7) -- the mirror image of Deref
                    // just above: codegen_lvalue already resolves
                    // expr.lhs's address (its `.ptr`); returning that
                    // pointer directly as this expression's value --
                    // instead of loading through it -- is the entire
                    // codegen difference between reading a `T&`/
                    // `const T&` (which loads) and creating a raw `T*`
                    // (which doesn't). No new address-computation logic
                    // needed; movecheck (apply_address_of) has already
                    // verified expr.lhs resolves to a real place.
                    return codegen_lvalue(*expr.lhs).ptr;
                }
                if (expr.unary_op == UnaryOp::Neg) {
                    llvm::LLVMValueRef operand = codegen_expr(*expr.lhs);
                    std::optional<Type> operand_type = infer_type(*expr.lhs);
                    bool is_float = operand_type.has_value() && is_float_scalar_type_name(operand_type->name);
                    return is_float ? llvm::LLVMBuildFNeg(builder_, operand, "fnegtmp") : llvm::LLVMBuildNeg(builder_, operand, "negtmp");
                }
                llvm::LLVMValueRef operand = codegen_contextual_bool_value(*expr.lhs);
                // Not (`!`) -- `operand` is a `bool` value (i8; see
                // to_llvm_type), so this goes through the i1 domain
                // rather than a raw bitwise-not directly on the i8: NOT
                // on the byte `0x01` gives `0xFE`, not the canonical
                // false=`0x00` the ch06 invariant requires (every other
                // bool-producing operation -- comparisons, `&&`/`||` --
                // is careful to only ever produce 0 or 1; this must be
                // too, or a later `== false` on the result would wrongly
                // disagree with `!` itself).
                return i1_to_bool(llvm::LLVMBuildNot(builder_, bool_to_i1(operand), "nottmp"));
            }

            case ExprKind::Binary:
                return codegen_binary(expr);

            case ExprKind::Call: {
                if (is_for_range_size_builtin(expr)) {
                    std::optional<Type> range_type = infer_type(*expr.args[0]);
                    if (!range_type.has_value()) {
                        throw CodegenError("cannot determine range-for operand type", current_loc_);
                    }
                    const Type& unwrapped = range_type->kind == TypeKind::Reference && range_type->pointee != nullptr
                                                ? *range_type->pointee
                                                : *range_type;
                    if (unwrapped.kind == TypeKind::Array) {
                        return llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), static_cast<std::uint64_t>(unwrapped.array_size), 1);
                    }
                    if (unwrapped.kind == TypeKind::Span) {
                        auto size_expr = std::make_unique<Expr>();
                        size_expr->kind = ExprKind::Member;
                        size_expr->loc = expr.loc;
                        size_expr->lhs = clone_expr(*expr.args[0]);
                        size_expr->name = "size";
                        return codegen_expr(*size_expr);
                    }
                    throw CodegenError("range-for requires a fixed-size array or std::span operand", current_loc_);
                }
                if (expr.name == "print_int" || expr.name == "print_bool" || expr.name == "print_char") {
                    return codegen_builtin_print(expr);
                }
                CallResult result = codegen_call(expr);
                if (result.callee_def != nullptr && is_interface_reference_type(result.callee_def->return_type)) {
                    return result.value;
                }
                if (result.callee_def != nullptr && result.callee_def->return_type.kind == TypeKind::Reference) {
                    // The callee returns a reference -- an address,
                    // lowered identically to a pointer (see
                    // to_llvm_type) -- so using the call's result as a
                    // *value* here means auto-dereferencing it, exactly
                    // like a reference local's own read (see
                    // codegen_lvalue's Identifier case).
                    return llvm::LLVMBuildLoad2(builder_, to_llvm_type(*result.callee_def->return_type.pointee), result.value,
                                                 "derefcalltmp");
                }
                return result.value;
            }

            case ExprKind::Move: {
                // The move checker has already verified `expr.lhs` is a
                // plain, currently-Initialized unique_ptr or class-typed
                // variable. At the IR level a move is: read the old
                // value, then null out the source slot -- so even code
                // that (incorrectly) bypassed the move checker would
                // observe a null pointer rather than an aliased/
                // duplicated one. For a class-typed source with a
                // destructor, also set its own moved_flag (spec §6.3/
                // §6.4: the destructor is never invoked for a moved-out
                // object) -- see codegen_call_destructor_unless_moved.
                LValue lv = codegen_lvalue(*expr.lhs);
                llvm::LLVMTypeRef llvm_type = to_llvm_type(lv.type);
                llvm::LLVMValueRef old_value = create_load(llvm_type, lv.ptr, lv.alignment, "movetmp");
                zero_initialize_storage(lv.ptr, lv.type, lv.alignment);
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto local_it = locals_.find(expr.lhs->name);
                    if (local_it != locals_.end() && local_it->second.moved_flag != nullptr) {
                        llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 1, 0), local_it->second.moved_flag);
                    }
                }
                return old_value;
            }

            case ExprKind::New:
                return codegen_new_expr(expr);

            case ExprKind::Delete:
            case ExprKind::Destroy:
                throw CodegenError("'delete' and explicit destructor calls are only supported as standalone statements "
                                   "in this version",
                    current_loc_);

            case ExprKind::Fold:
            case ExprKind::PackExpansion:
                throw CodegenError("fold expression should have been expanded before codegen",
                    current_loc_);

            case ExprKind::Lambda:
                return codegen_construct_lambda(expr);
        }
        throw CodegenError("unhandled expression kind",
            current_loc_);
    }


    llvm::LLVMValueRef Codegen::codegen_construct_lambda(const Expr& expr, llvm::LLVMValueRef existing_storage)
{
        const StructInfo& info = structs_.at(expr.name);
        llvm::LLVMValueRef closure =
            existing_storage != nullptr ? existing_storage : create_entry_block_alloca(info.llvm_type, "lambdatmp");
        if (info.has_ordinary_vtable) initialize_ordinary_vtable_pointer(expr.name, closure);
        for (std::size_t i = 0; i < expr.lambda_captures.size(); i++) {
            const LambdaCapture& capture = expr.lambda_captures[i];
            const Type& field_type = info.field_types[i];
            llvm::LLVMValueRef field_ptr =
                llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, closure, info.physical_field_index(i), capture.name.c_str());
            if (capture.by_reference) {
                Expr ident;
                ident.kind = ExprKind::Identifier;
                ident.loc = expr.loc;
                ident.name = capture.name;
                llvm::LLVMValueRef address = codegen_lvalue(ident).ptr;
                create_store(address, field_ptr, std::nullopt);
                continue;
            }
            Expr ident;
            ident.kind = ExprKind::Identifier;
            ident.loc = expr.loc;
            ident.name = capture.name;
            const Expr& source = capture.init ? *capture.init : ident;
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name) &&
                is_bare_same_type_copy_source(source, field_type) && is_copy_constructible(field_type.name)) {
                codegen_copy_construct_class(field_ptr, codegen_lvalue(source).ptr, field_type.name);
                continue;
            }
            llvm::LLVMValueRef value = codegen_value_for_target(source, field_type);
            check_store_type(value, to_llvm_type(field_type), "capture '" + capture.name + "'");
            create_store(value, field_ptr, std::nullopt);
        }
        return closure;
    }


    llvm::LLVMValueRef Codegen::codegen_new_expr(const Expr& expr)
{
        llvm::LLVMTypeRef element_type = to_llvm_type(expr.type);
        llvm::LLVMValueRef heap_ptr = nullptr;
        if (expr.lhs) {
            heap_ptr = codegen_expr(*expr.lhs);
        } else {
            llvm::LLVMValueRef malloc_fn = get_or_declare_malloc();
            std::uint64_t size_in_bytes = llvm::LLVMABISizeOfType(data_layout_ref(module_), element_type);
            llvm::LLVMValueRef size_arg = llvm::LLVMConstInt(llvm::LLVMInt64TypeInContext(context_), size_in_bytes, 0);
            heap_ptr = build_call(malloc_fn, {size_arg}, "newptr");
        }

        if (expr.type.kind == TypeKind::Named && structs_.contains(expr.type.name)) {
            LValue target{heap_ptr, expr.type, std::nullopt};
            zero_initialize_storage(target.ptr, target.type, target.alignment);
            if (!expr.args.empty() || expr.has_paren_init) {
                if (try_initialize_class_storage_from_same_type_source(target, expr.args)) return heap_ptr;
                std::string ctor_name = expr.type.name + "_new";
                const Function* ctor_def = resolve_overload_by_type(ctor_name, expr.args, /*param_offset=*/1);
                if (ctor_def == nullptr) {
                    if (expr.args.empty()) return heap_ptr;
                    throw CodegenError("class '" + expr.type.name + "' has no constructor matching this call",
                        current_loc_);
                }
                if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                    llvm::LLVMValueRef value = codegen_constructed_class_value(expr.type.name, expr.args, ctor_def);
                    llvm::LLVMBuildStore(builder_, value, heap_ptr);
                    if (class_has_ordinary_vtable(expr.type.name)) {
                        initialize_ordinary_vtable_pointer(expr.type.name, heap_ptr);
                    }
                    return heap_ptr;
                }
                llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
                if (ctor == nullptr) {
                    if (expr.args.empty()) return heap_ptr;
                    throw CodegenError("class '" + expr.type.name + "' has no constructor matching this call",
                        current_loc_);
                }
                if (const ClassDef* class_def = find_class_def(expr.type.name)) {
                    emit_complete_object_interface_initializers(*class_def, ctor_def, target.ptr);
                }
                std::vector<llvm::LLVMValueRef> args = codegen_call_args(expr.args, ctor_def, /*param_offset=*/1);
                args.insert(args.begin(), target.ptr);
                build_call(ctor, args);
            }
            return heap_ptr;
        }

        llvm::LLVMValueRef initial_value = llvm::LLVMConstNull(element_type);
        if (!expr.args.empty()) {
            if (expr.args.size() != 1) {
                throw CodegenError("'new T(args...)' for a non-class type currently requires exactly one argument",
                    current_loc_);
            }
            initial_value = codegen_expr(*expr.args[0]);
            refresh_debug_location(expr.loc);
            check_store_type(initial_value, element_type, "'new " + expr.type.name + "(...)' argument");
        }
        llvm::LLVMBuildStore(builder_, initial_value, heap_ptr);
        return heap_ptr;
    }


    void Codegen::codegen_delete_expr(const Expr& expr)
{
        llvm::LLVMValueRef ptr = codegen_expr(*expr.lhs);
        std::optional<Type> operand_type = infer_type(*expr.lhs);
        if (!operand_type.has_value() || operand_type->kind != TypeKind::Pointer || operand_type->pointee == nullptr) {
            throw CodegenError("'delete' requires a raw pointer operand in this version", current_loc_);
        }
        if (is_interface_pointer_type(*operand_type)) {
            llvm::LLVMValueRef object_ptr = extract_interface_object_ptr(ptr);
            llvm::LLVMValueRef is_null = llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ,
                object_ptr, llvm::LLVMConstPointerNull(llvm::LLVMPointerTypeInContext(context_, 0)), "iface.isnull");
            llvm::LLVMValueRef current_fn = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
            llvm::LLVMBasicBlockRef delete_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "iface.delete");
            llvm::LLVMBasicBlockRef merge_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "iface.delete.skip");
            llvm::LLVMBuildCondBr(builder_, is_null, merge_bb, delete_bb);
            llvm::LLVMPositionBuilderAtEnd(builder_, delete_bb);
            emit_interface_destructor_dispatch_call(operand_type->pointee->name, ptr);
            build_call(get_or_declare_free(), {object_ptr});
            llvm::LLVMBuildBr(builder_, merge_bb);
            llvm::LLVMPositionBuilderAtEnd(builder_, merge_bb);
            return;
        }
        const Type& pointee = *operand_type->pointee;
        if (pointee.kind == TypeKind::Named) {
            if (class_has_ordinary_vtable(pointee.name)) {
                llvm::LLVMValueRef is_null = llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ,
                    ptr, llvm::LLVMConstPointerNull(llvm::LLVMPointerTypeInContext(context_, 0)), "delete.isnull");
                llvm::LLVMValueRef current_fn = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
                llvm::LLVMBasicBlockRef delete_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "delete.body");
                llvm::LLVMBasicBlockRef merge_bb = llvm::LLVMAppendBasicBlockInContext(context_, current_fn, "delete.skip");
                llvm::LLVMBuildCondBr(builder_, is_null, merge_bb, delete_bb);
                llvm::LLVMPositionBuilderAtEnd(builder_, delete_bb);
                const StructInfo& info = structs_.at(pointee.name);
                llvm::LLVMValueRef vptr_slot = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, ptr, 0, "vptr");
                llvm::LLVMValueRef vtable_ptr = create_load(llvm::LLVMPointerTypeInContext(context_, 0), vptr_slot, std::nullopt,
                                                      "vtable");
                llvm::LLVMTypeRef table_type = ordinary_vtable_type(pointee.name);
                llvm::LLVMValueRef table_ptr =
                    llvm::LLVMBuildBitCast(builder_, vtable_ptr, llvm::LLVMPointerTypeInContext(context_, 0), "vtable.array");
                llvm::LLVMValueRef gep_indices[] = {llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), 0, 0),
                                               llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), 0, 0)};
                llvm::LLVMValueRef slot_ptr =
                    llvm::LLVMBuildGEP2(builder_, table_type, table_ptr, gep_indices, 2, "vtable.dtor.slot");
                llvm::LLVMValueRef dtor_ptr =
                    create_load(llvm::LLVMPointerTypeInContext(context_, 0), slot_ptr, std::nullopt, "dtorfn");
                llvm::LLVMTypeRef dtor_param_types[] = {llvm::LLVMPointerTypeInContext(context_, 0)};
                llvm::LLVMTypeRef dtor_type =
                    llvm::LLVMFunctionType(llvm::LLVMVoidTypeInContext(context_), dtor_param_types, 1, 0);
                build_call(dtor_type, dtor_ptr, {ptr});
                build_call(get_or_declare_free(), {ptr});
                llvm::LLVMBuildBr(builder_, merge_bb);
                llvm::LLVMPositionBuilderAtEnd(builder_, merge_bb);
                return;
            }
            if (class_has_destructor_in_chain(pointee.name)) {
                codegen_call_destructor_chain_unless_moved(pointee.name, ptr, nullptr);
            }
        }
        build_call(get_or_declare_free(), {ptr});
    }


    void Codegen::codegen_destroy_expr(const Expr& expr)
{
        if (!expr.destroy_through_pointer) {
            throw CodegenError("explicit destructor calls currently require the pointer form 'ptr->~T()'",
                               current_loc_);
        }
        llvm::LLVMValueRef ptr = codegen_expr(*expr.lhs);
        if (expr.destroy_through_pointer) {
            std::optional<Type> operand_type = infer_type(*expr.lhs);
            if (operand_type.has_value() && is_interface_pointer_type(*operand_type)) {
                emit_interface_destructor_dispatch_call(operand_type->pointee->name, ptr);
                return;
            }
        }
        if (expr.type.kind == TypeKind::Named) {
            if (class_has_destructor_in_chain(expr.type.name)) {
                codegen_call_destructor_chain_unless_moved(expr.type.name, ptr, nullptr);
            }
        }
    }


    llvm::LLVMValueRef Codegen::get_or_declare_malloc()
{
        if (llvm::LLVMValueRef existing = llvm::LLVMGetNamedFunction(module_, "malloc")) {
            return existing;
        }
        llvm::LLVMTypeRef ptr_type = llvm::LLVMPointerTypeInContext(context_, 0);
        llvm::LLVMTypeRef malloc_param_types[] = {llvm::LLVMInt64TypeInContext(context_)};
        llvm::LLVMTypeRef malloc_type =
            llvm::LLVMFunctionType(ptr_type, malloc_param_types, 1, /*IsVarArg=*/0);
        return llvm::LLVMAddFunction(module_, "malloc", malloc_type);
    }


    llvm::LLVMValueRef Codegen::get_or_declare_free()
{
        if (llvm::LLVMValueRef existing = llvm::LLVMGetNamedFunction(module_, "free")) {
            return existing;
        }
        llvm::LLVMTypeRef ptr_type = llvm::LLVMPointerTypeInContext(context_, 0);
        llvm::LLVMTypeRef free_param_types[] = {ptr_type};
        llvm::LLVMTypeRef free_type =
            llvm::LLVMFunctionType(llvm::LLVMVoidTypeInContext(context_), free_param_types, 1, /*IsVarArg=*/0);
        return llvm::LLVMAddFunction(module_, "free", free_type);
    }


    llvm::LLVMValueRef Codegen::get_or_declare_abort()
{
        if (llvm::LLVMValueRef existing = llvm::LLVMGetNamedFunction(module_, "abort")) {
            return existing;
        }
        llvm::LLVMTypeRef abort_type = llvm::LLVMFunctionType(llvm::LLVMVoidTypeInContext(context_), nullptr, 0, /*IsVarArg=*/0);
        llvm::LLVMValueRef fn = llvm::LLVMAddFunction(module_, "abort", abort_type);
        // libc's abort() never returns -- telling llvm::LLVM this lets it treat
        // the code right after a call to it as unreachable, same as real
        // Clang does.
        unsigned noreturn_kind = llvm::LLVMGetEnumAttributeKindForName("noreturn", 8);
        llvm::LLVMAddAttributeAtIndex(fn, llvm::LLVMAttributeFunctionIndex, llvm::LLVMCreateEnumAttribute(context_, noreturn_kind, 0));
        return fn;
    }


    void Codegen::emit_span_bounds_check(llvm::LLVMValueRef index, llvm::LLVMValueRef size)
{
        if (unsafe_depth_ > 0) return;

        llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
        llvm::LLVMBasicBlockRef fail_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "bounds.fail");
        llvm::LLVMBasicBlockRef ok_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "bounds.ok");

        llvm::LLVMValueRef index64 = llvm::LLVMBuildSExt(builder_, index, llvm::LLVMInt64TypeInContext(context_), "idx64");
        llvm::LLVMValueRef too_low =
            llvm::LLVMBuildICmp(builder_, llvm::LLVMIntSLT, index64, llvm::LLVMConstInt(llvm::LLVMInt64TypeInContext(context_), 0, 0), "toolow");
        llvm::LLVMValueRef too_high = llvm::LLVMBuildICmp(builder_, llvm::LLVMIntSGE, index64, size, "toohigh");
        llvm::LLVMValueRef out_of_bounds = llvm::LLVMBuildOr(builder_, too_low, too_high, "oob");
        llvm::LLVMBuildCondBr(builder_, out_of_bounds, fail_block, ok_block);

        llvm::LLVMPositionBuilderAtEnd(builder_, fail_block);
        build_call(get_or_declare_abort(), {});
        llvm::LLVMBuildUnreachable(builder_);

        llvm::LLVMPositionBuilderAtEnd(builder_, ok_block);
    }


    [[nodiscard]] std::optional<long long> Codegen::try_eval_constant_index(const Expr& expr) const
{
        if (expr.kind == ExprKind::IntegerLiteral) return expr.int_value;
        // `-1` (a negated literal, ExprKind::Unary/Neg over a bare
        // literal) is just as much a single compile-time-constant token
        // as the bare literal itself -- same reasoning as
        // codegen_value_for_target's identical recognition of a negated
        // literal.
        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Neg && expr.lhs->kind == ExprKind::IntegerLiteral) {
            return -expr.lhs->int_value;
        }
        return std::nullopt;
    }


    void Codegen::emit_array_bounds_check(llvm::LLVMValueRef index, long long bound)
{
        emit_span_bounds_check(index, llvm::LLVMConstInt(llvm::LLVMInt64TypeInContext(context_), static_cast<std::uint64_t>(bound), /*SignExtend=*/1));
    }


    [[nodiscard]] bool Codegen::is_float_scalar_type_name(const std::string& name)
{
        return name == "float" || name == "double" || name == "float32_t" || name == "float64_t";
    }


    [[nodiscard]] bool Codegen::is_integral_scalar_type_name(const std::string& name)
{
        return name == "char" || name == "int" || name == "long" || name == "unsigned int" ||
               name == "unsigned long" || name == "int8_t" || name == "int16_t" || name == "int32_t" ||
               name == "int64_t" || name == "uint8_t" || name == "uint16_t" || name == "uint32_t" ||
               name == "uint64_t" || name == "size_t" || name == "ptrdiff_t";
    }


    [[nodiscard]] bool Codegen::is_unsigned_scalar_type_name(const std::string& name)
{
        return name == "unsigned int" || name == "unsigned long" || name == "uint8_t" || name == "uint16_t" ||
               name == "uint32_t" || name == "uint64_t" || name == "size_t";
    }


    [[nodiscard]] bool Codegen::is_checked_arithmetic_scalar_type_name(const std::string& name)
{
        return name != "bool" && name != "char";
    }


    [[nodiscard]] bool Codegen::is_unsigned_for_cast(const std::string& name)
{
        return name == "bool" || name == "char" || is_unsigned_scalar_type_name(name);
    }


    [[nodiscard]] std::string Codegen::scalar_name_for_cast(const Type& type) const
{
        if (type.kind != TypeKind::Named) return {};
        if (const EnumDef* def = find_enum_def(program_, type.name)) return def->underlying_type.name;
        return type.name;
    }


    llvm::LLVMValueRef Codegen::codegen_scalar_cast(llvm::LLVMValueRef value, const Type& source_type, const Type& target_type)
{
        llvm::LLVMTypeRef target_llvm = to_llvm_type(target_type);
        if (llvm::LLVMTypeOf(value) == target_llvm) return value;
        std::string source_name = scalar_name_for_cast(source_type);
        std::string target_name = scalar_name_for_cast(target_type);
        bool source_is_float = is_float_scalar_type_name(source_name);
        bool target_is_float = is_float_scalar_type_name(target_name);
        if (source_is_float && target_is_float) {
            return scalar_bit_width(llvm::LLVMTypeOf(value)) < scalar_bit_width(target_llvm)
                       ? llvm::LLVMBuildFPExt(builder_, value, target_llvm, "fpexttmp")
                       : llvm::LLVMBuildFPTrunc(builder_, value, target_llvm, "fptrunctmp");
        }
        if (source_is_float) {
            return is_unsigned_for_cast(target_name) ? llvm::LLVMBuildFPToUI(builder_, value, target_llvm, "fptouitmp")
                                                     : llvm::LLVMBuildFPToSI(builder_, value, target_llvm, "fptositmp");
        }
        if (target_is_float) {
            return is_unsigned_for_cast(source_name) ? llvm::LLVMBuildUIToFP(builder_, value, target_llvm, "uitofptmp")
                                                     : llvm::LLVMBuildSIToFP(builder_, value, target_llvm, "sitofptmp");
        }
        // int -> int: same width already returned `value` unchanged
        // above (e.g. int8_t <-> uint8_t <-> char <-> bool).
        if (scalar_bit_width(llvm::LLVMTypeOf(value)) < scalar_bit_width(target_llvm)) {
            return is_unsigned_for_cast(source_name) ? llvm::LLVMBuildZExt(builder_, value, target_llvm, "zexttmp")
                                                     : llvm::LLVMBuildSExt(builder_, value, target_llvm, "sexttmp");
        }
        return llvm::LLVMBuildTrunc(builder_, value, target_llvm, "trunctmp");
    }


    llvm::LLVMValueRef Codegen::codegen_float_arith(BinaryOp op, llvm::LLVMValueRef lhs, llvm::LLVMValueRef rhs)
{
        switch (op) {
            case BinaryOp::Add: return llvm::LLVMBuildFAdd(builder_, lhs, rhs, "faddtmp");
            case BinaryOp::Sub: return llvm::LLVMBuildFSub(builder_, lhs, rhs, "fsubtmp");
            case BinaryOp::Mul: return llvm::LLVMBuildFMul(builder_, lhs, rhs, "fmultmp");
            default: throw CodegenError("unhandled floating-point arithmetic operator",
                current_loc_);
        }
    }


    llvm::LLVMValueRef Codegen::codegen_checked_arith(BinaryOp op, llvm::LLVMValueRef lhs, llvm::LLVMValueRef rhs, bool is_unsigned,
                                        bool is_checked)
{
        const char* name = op == BinaryOp::Add ? "addtmp" : op == BinaryOp::Sub ? "subtmp" : "multmp";
        if (unsafe_depth_ > 0 || !is_checked) {
            switch (op) {
                case BinaryOp::Add: return llvm::LLVMBuildAdd(builder_, lhs, rhs, name);
                case BinaryOp::Sub: return llvm::LLVMBuildSub(builder_, lhs, rhs, name);
                case BinaryOp::Mul: return llvm::LLVMBuildMul(builder_, lhs, rhs, name);
                default: throw CodegenError("unhandled checked-arithmetic operator",
                    current_loc_);
            }
        }

        const char* intrinsic_name =
            op == BinaryOp::Add
                ? (is_unsigned ? "llvm.uadd.with.overflow" : "llvm.sadd.with.overflow")
            : op == BinaryOp::Sub
                ? (is_unsigned ? "llvm.usub.with.overflow" : "llvm.ssub.with.overflow")
                : (is_unsigned ? "llvm.umul.with.overflow" : "llvm.smul.with.overflow");
        unsigned intrinsic_id = llvm::LLVMLookupIntrinsicID(intrinsic_name, std::strlen(intrinsic_name));
        llvm::LLVMTypeRef overload_types[] = {llvm::LLVMTypeOf(lhs)};
        llvm::LLVMValueRef intrinsic =
            llvm::LLVMGetIntrinsicDeclaration(module_, intrinsic_id, overload_types, 1);
        llvm::LLVMValueRef pair = build_call(intrinsic, {lhs, rhs}, name);
        llvm::LLVMValueRef result = llvm::LLVMBuildExtractValue(builder_, pair, 0, name);
        llvm::LLVMValueRef overflowed = llvm::LLVMBuildExtractValue(builder_, pair, 1, "overflow");

        llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
        llvm::LLVMBasicBlockRef fail_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "overflow.fail");
        llvm::LLVMBasicBlockRef ok_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "overflow.ok");
        llvm::LLVMBuildCondBr(builder_, overflowed, fail_block, ok_block);

        llvm::LLVMPositionBuilderAtEnd(builder_, fail_block);
        build_call(get_or_declare_abort(), {});
        llvm::LLVMBuildUnreachable(builder_);

        llvm::LLVMPositionBuilderAtEnd(builder_, ok_block);
        return result;
    }


    llvm::LLVMValueRef Codegen::codegen_checked_div(llvm::LLVMValueRef lhs, llvm::LLVMValueRef rhs, bool is_unsigned, bool is_checked)
{
        if (!is_checked) {
            return is_unsigned ? llvm::LLVMBuildUDiv(builder_, lhs, rhs, "divtmp") : llvm::LLVMBuildSDiv(builder_, lhs, rhs, "divtmp");
        }

        llvm::LLVMTypeRef int_ty = llvm::LLVMTypeOf(lhs);
        llvm::LLVMValueRef zero = llvm::LLVMConstInt(int_ty, 0, 0);
        llvm::LLVMValueRef divides_by_zero = llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, rhs, zero, "divzero");
        llvm::LLVMValueRef traps = divides_by_zero;
        if (!is_unsigned) {
            unsigned bit_width = llvm::LLVMGetIntTypeWidth(int_ty);
            llvm::LLVMValueRef int_min = llvm::LLVMConstInt(int_ty, std::uint64_t{1} << (bit_width - 1), 0);
            llvm::LLVMValueRef neg_one = llvm::LLVMConstInt(int_ty, static_cast<std::uint64_t>(-1), 1);
            llvm::LLVMValueRef overflows = llvm::LLVMBuildAnd(builder_, llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, lhs, int_min, "isintmin"),
                                                           llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, rhs, neg_one, "isnegone"),
                                                           "divoverflow");
            traps = llvm::LLVMBuildOr(builder_, divides_by_zero, overflows, "divtraps");
        }

        llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
        llvm::LLVMBasicBlockRef fail_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "div.fail");
        llvm::LLVMBasicBlockRef ok_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "div.ok");
        llvm::LLVMBuildCondBr(builder_, traps, fail_block, ok_block);

        llvm::LLVMPositionBuilderAtEnd(builder_, fail_block);
        build_call(get_or_declare_abort(), {});
        llvm::LLVMBuildUnreachable(builder_);

        llvm::LLVMPositionBuilderAtEnd(builder_, ok_block);
        return is_unsigned ? llvm::LLVMBuildUDiv(builder_, lhs, rhs, "divtmp") : llvm::LLVMBuildSDiv(builder_, lhs, rhs, "divtmp");
    }


    llvm::LLVMValueRef Codegen::codegen_pointer_offset(llvm::LLVMValueRef base_ptr, llvm::LLVMValueRef offset, const Type& pointer_type, bool negate_offset)
{
        llvm::LLVMValueRef gep_offset = negate_offset ? llvm::LLVMBuildNeg(builder_, offset, "ptroffset") : offset;
        llvm::LLVMValueRef gep_indices[] = {gep_offset};
        return llvm::LLVMBuildGEP2(builder_, to_llvm_type(*pointer_type.pointee), base_ptr, gep_indices, 1, "ptrarith");
    }


    llvm::LLVMValueRef Codegen::codegen_pointer_difference(llvm::LLVMValueRef lhs_ptr, llvm::LLVMValueRef rhs_ptr, const Type& pointer_type)
{
        llvm::LLVMTypeRef diff_type = to_llvm_type(named_type("ptrdiff_t"));
        llvm::LLVMValueRef lhs_int = llvm::LLVMBuildPtrToInt(builder_, lhs_ptr, diff_type, "lhsint");
        llvm::LLVMValueRef rhs_int = llvm::LLVMBuildPtrToInt(builder_, rhs_ptr, diff_type, "rhsint");
        llvm::LLVMValueRef byte_diff = llvm::LLVMBuildSub(builder_, lhs_int, rhs_int, "ptrbytes");
        std::uint64_t elem_size = llvm::LLVMABISizeOfType(data_layout_ref(module_), to_llvm_type(*pointer_type.pointee));
        if (elem_size == 1) return byte_diff;
        llvm::LLVMValueRef elem_size_value = llvm::LLVMConstInt(diff_type, elem_size, /*SignExtend=*/0);
        return llvm::LLVMBuildSDiv(builder_, byte_diff, elem_size_value, "ptrdifftmp");
    }


    Codegen::LValue Codegen::codegen_lvalue(const Expr& expr)
{
        // Same refresh discipline as codegen_expr above.
        refresh_debug_location(expr.loc);
        switch (expr.kind) {
            case ExprKind::Identifier: {
                auto it = locals_.find(expr.name);
                if (it == locals_.end()) {
                    if (const GlobalSlot* global = find_visible_global_slot(expr.name, expr.explicit_global_qualification)) {
                        unsigned raw_alignment = llvm::LLVMGetAlignment(global->global);
                        std::optional<unsigned> explicit_alignment =
                            raw_alignment != 0 ? std::optional<unsigned>(raw_alignment) : std::nullopt;
                        return LValue{global->global, global->type,
                                      explicit_alignment.has_value() ? explicit_alignment : alignment_for_type(global->type)};
                    }
                    throw CodegenError("use of undeclared variable '" + expr.name + "'",
                        current_loc_);
                }
                if (it->second.type.kind == TypeKind::Reference) {
                    if (is_interface_reference_type(it->second.type)) {
                        return LValue{it->second.alloca, it->second.type, alignment_for_type(it->second.type)};
                    }
                    // A reference-typed local's own alloca just holds the
                    // address it's bound to (see the VarDecl case below,
                    // and how a Reference parameter arrives already as
                    // that address): auto-dereference once so every
                    // caller (reads, writes-through, and Member/Subscript
                    // base resolution) transparently operates on the
                    // referent, exactly like a real C++ reference.
                    llvm::LLVMValueRef referent_ptr =
                        create_load(llvm::LLVMPointerTypeInContext(context_, 0), it->second.alloca, std::nullopt, "deref");
                    return LValue{referent_ptr, *it->second.type.pointee, alignment_for_type(*it->second.type.pointee)};
                }
                return LValue{it->second.alloca, it->second.type, alignment_for_type(it->second.type)};
            }

            case ExprKind::Member: {
                LValue base = codegen_lvalue(*expr.lhs);
                if (base.type.kind != TypeKind::Named || !structs_.contains(base.type.name)) {
                    throw CodegenError("member access '." + expr.name + "' on a non-struct type",
                        current_loc_);
                }
                const StructInfo& info = structs_.at(base.type.name);
                std::optional<std::size_t> field_index_opt = info.find_field_index(expr.name);
                if (!field_index_opt.has_value()) {
                    throw CodegenError(std::string(info.is_union ? "union '" : "struct '") + base.type.name +
                                           "' has no field '" + expr.name + "'",
                        current_loc_);
                }
                std::size_t field_index = *field_index_opt;
                const Type& field_type = info.field_types[field_index];
                std::optional<unsigned> field_alignment =
                    info.is_union ? (base.alignment.has_value() ? base.alignment : alignment_for_type(base.type))
                                  : std::optional<unsigned>(info.field_alignments[field_index]);
                llvm::LLVMValueRef field_ptr = info.is_union
                                             ? llvm::LLVMBuildBitCast(builder_, base.ptr, llvm::LLVMPointerTypeInContext(context_, 0),
                                                                       (expr.name + ".unionfield").c_str())
                                             : llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, base.ptr,
                                                                         info.physical_field_index(field_index),
                                                                         expr.name.c_str());
                if (field_type.kind == TypeKind::Reference) {
                    if (is_interface_reference_type(field_type)) {
                        return LValue{field_ptr, field_type, field_alignment};
                    }
                    // ch05 §5.12: a Reference-typed field (e.g. a
                    // closure's own by-reference capture) stores just
                    // the address it's bound to, exactly like a
                    // Reference-typed local's own alloca (see the
                    // Identifier case above) -- auto-dereference once so
                    // every caller (reads, writes-through, and further
                    // Member/Subscript base resolution) transparently
                    // operates on the referent, not the field's own
                    // storage slot.
                    llvm::LLVMValueRef referent_ptr =
                        create_load(llvm::LLVMPointerTypeInContext(context_, 0), field_ptr, field_alignment, "fieldderef");
                    return LValue{referent_ptr, *field_type.pointee, alignment_for_type(*field_type.pointee)};
                }
                return LValue{field_ptr, field_type, field_alignment};
            }

            case ExprKind::Subscript: {
                LValue base = codegen_lvalue(*expr.lhs);
                if (base.type.kind == TypeKind::Span) {
                    llvm::LLVMTypeRef span_type = to_llvm_type(base.type);
                    llvm::LLVMValueRef size_ptr = llvm::LLVMBuildStructGEP2(builder_, span_type, base.ptr, 1, "sizeptr");
                    llvm::LLVMValueRef size = llvm::LLVMBuildLoad2(builder_, llvm::LLVMInt64TypeInContext(context_), size_ptr, "size");
                    llvm::LLVMValueRef data_ptr = llvm::LLVMBuildStructGEP2(builder_, span_type, base.ptr, 0, "dataptr");
                    llvm::LLVMValueRef data = llvm::LLVMBuildLoad2(builder_, llvm::LLVMPointerTypeInContext(context_, 0), data_ptr, "data");
                    llvm::LLVMValueRef index = codegen_expr(*expr.rhs);
                    // Runtime bounds check (spec ch08: checked by default,
                    // bounds checks inserted unconditionally) -- unlike a
                    // fixed-size array's subscript below, a span's length is
                    // only known at runtime, so (even for a compile-time-
                    // constant index) there's no way to reject an
                    // out-of-bounds index at compile time; it's always this
                    // same runtime check instead.
                    emit_span_bounds_check(index, size);
                    llvm::LLVMValueRef gep_indices_span[] = {index};
                    llvm::LLVMValueRef elem_ptr =
                        llvm::LLVMBuildGEP2(builder_, to_llvm_type(*base.type.pointee), data, gep_indices_span, 1, "elemtmp");
                    return LValue{elem_ptr, *base.type.pointee, alignment_for_type(*base.type.pointee)};
                }
                if (base.type.kind == TypeKind::Pointer) {
                    llvm::LLVMValueRef data = llvm::LLVMBuildLoad2(builder_, llvm::LLVMPointerTypeInContext(context_, 0), base.ptr, "data");
                    llvm::LLVMValueRef index = codegen_expr(*expr.rhs);
                    llvm::LLVMValueRef gep_indices_ptr[] = {index};
                    llvm::LLVMValueRef elem_ptr =
                        llvm::LLVMBuildGEP2(builder_, to_llvm_type(*base.type.pointee), data, gep_indices_ptr, 1, "elemtmp");
                    return LValue{elem_ptr, *base.type.pointee, alignment_for_type(*base.type.pointee)};
                }
                if (base.type.kind != TypeKind::Array) {
                    throw CodegenError("subscript on a non-array type",
                        current_loc_);
                }
                // A fixed-size array's bound `N` is always statically known
                // (ch05 §9.4), so a compile-time-constant index (e.g. a bare
                // integer literal) that's out of bounds is rejected right
                // here at compile time instead of merely at runtime --
                // strictly better than a runtime abort when both operands
                // are already known now, and unlike the span case above,
                // never skipped inside `unsafe { }` (this is a detected-at-
                // compile-time ill-formed program, not a scpp-inserted
                // runtime check being opted out of).
                std::optional<long long> constant_index = try_eval_constant_index(*expr.rhs);
                if (constant_index.has_value() &&
                    (*constant_index < 0 || *constant_index >= base.type.array_size)) {
                    throw CodegenError("array subscript " + std::to_string(*constant_index) +
                                            " is out of bounds for array of size " +
                                            std::to_string(base.type.array_size),
                        current_loc_);
                }
                llvm::LLVMValueRef index = codegen_expr(*expr.rhs);
                // Otherwise (a runtime-variable index), the same runtime
                // bounds check as a span's subscript above, just against a
                // compile-time-constant bound instead of a runtime-loaded
                // size -- respects `unsafe { }` exactly like span's check
                // (see emit_array_bounds_check).
                if (!constant_index.has_value()) {
                    emit_array_bounds_check(index, base.type.array_size);
                }
                llvm::LLVMValueRef zero = llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), 0, 0);
                llvm::LLVMValueRef gep_indices_arr[] = {zero, index};
                llvm::LLVMValueRef elem_ptr =
                    llvm::LLVMBuildGEP2(builder_, to_llvm_type(base.type), base.ptr, gep_indices_arr, 2, "elemtmp");
                return LValue{elem_ptr, *base.type.element, alignment_for_type(*base.type.element)};
            }

            case ExprKind::Call: {
                // Reachable whenever a call to a reference-returning
                // function is itself used as a reference-binding source
                // (`T& r = f(x);`), a reference argument (`g(f(x))`), or
                // forwarded in a `return` -- see
                // resolve_borrow_source_root in movecheck.cppm.
                // codegen_call's raw result is already the referent's
                // address in that case -- no load needed, unlike
                // codegen_expr's own Call case. Validity (must actually
                // be reference-returning) is checked *after* codegen_call
                // returns rather than before, unlike the pre-method-call
                // version of this code -- codegen_call must run first
                // regardless, to resolve a possible method-call receiver
                // exactly once; an invalid program reaching this far
                // would already have been rejected by movecheck, so
                // emitting (and then discarding, via the throw below) a
                // few extra instructions first is harmless.
                CallResult result = codegen_call(expr);
                if (result.callee_def == nullptr || result.callee_def->return_type.kind != TypeKind::Reference) {
                    throw CodegenError("expression is not assignable",
                        current_loc_);
                }
                if (is_interface_reference_type(result.callee_def->return_type)) {
                    llvm::LLVMValueRef slot =
                        create_entry_block_alloca(to_llvm_type(result.callee_def->return_type), "ifacereftmp");
                    create_store(result.value, slot, alignment_for_type(result.callee_def->return_type));
                    return LValue{slot, result.callee_def->return_type, alignment_for_type(result.callee_def->return_type)};
                }
                return LValue{result.value, *result.callee_def->return_type.pointee,
                              alignment_for_type(*result.callee_def->return_type.pointee)};
            }

            case ExprKind::Lambda: {
                // ch05 §5.12: an IIFE's receiver (`[](...){...}(args)`,
                // parser.cppm's own Lambda-followed-by-`(` case) needs
                // the constructed closure's *address* to invoke its
                // "call" method on -- exactly like an ordinary method
                // call's receiver (codegen_call's own `expr.lhs != nullptr`
                // branch calls codegen_lvalue on it uniformly, regardless
                // of receiver shape).
                llvm::LLVMValueRef ptr = codegen_construct_lambda(expr);
                return LValue{ptr, named_type(expr.name),
                              alignment_for_type(named_type(expr.name))};
            }

            case ExprKind::Conditional: {
                llvm::LLVMValueRef cond = codegen_contextual_bool_i1(*expr.lhs);
                llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
                llvm::LLVMBasicBlockRef then_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.lvalue.then");
                llvm::LLVMBasicBlockRef else_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.lvalue.else");
                llvm::LLVMBasicBlockRef merge_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.lvalue.end");
                llvm::LLVMBuildCondBr(builder_, cond, then_block, else_block);

                llvm::LLVMPositionBuilderAtEnd(builder_, then_block);
                LValue then_lvalue = codegen_lvalue(*expr.rhs);
                llvm::LLVMBuildBr(builder_, merge_block);
                llvm::LLVMBasicBlockRef then_end = llvm::LLVMGetInsertBlock(builder_);

                llvm::LLVMPositionBuilderAtEnd(builder_, else_block);
                LValue else_lvalue = codegen_lvalue(*expr.third);
                llvm::LLVMBuildBr(builder_, merge_block);
                llvm::LLVMBasicBlockRef else_end = llvm::LLVMGetInsertBlock(builder_);

                llvm::LLVMPositionBuilderAtEnd(builder_, merge_block);
                if (!types_equal(then_lvalue.type, else_lvalue.type) || then_lvalue.alignment != else_lvalue.alignment ||
                    llvm::LLVMTypeOf(then_lvalue.ptr) != llvm::LLVMTypeOf(else_lvalue.ptr)) {
                    throw CodegenError("expression is not assignable", current_loc_);
                }
                llvm::LLVMValueRef phi = llvm::LLVMBuildPhi(builder_, llvm::LLVMTypeOf(then_lvalue.ptr), "cond.lvalue");
                llvm::LLVMValueRef incoming_values[] = {then_lvalue.ptr, else_lvalue.ptr};
                llvm::LLVMBasicBlockRef incoming_blocks[] = {then_end, else_end};
                llvm::LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
                return LValue{phi, then_lvalue.type, then_lvalue.alignment};
            }

            case ExprKind::Move:
                return codegen_lvalue(*expr.lhs);

            case ExprKind::Cast: {
                if (expr.type.kind != TypeKind::Pointer) {
                    throw CodegenError("expression is not assignable", current_loc_);
                }
                llvm::LLVMValueRef value = codegen_expr(expr);
                llvm::LLVMValueRef slot = create_entry_block_alloca(to_llvm_type(expr.type), "castptrtmp");
                create_store(value, slot, alignment_for_type(expr.type));
                return LValue{slot, expr.type, alignment_for_type(expr.type)};
            }

            case ExprKind::Unary: {
                // Only `*p` (Deref) is addressable; Neg/Not produce a
                // plain value with no backing storage.
                if (expr.unary_op != UnaryOp::Deref) {
                    throw CodegenError("expression is not assignable",
                        current_loc_);
                }
                if (expr.lhs->kind == ExprKind::Identifier && expr.lhs->name == "this") {
                    // parser/movecheck model `this` as a reference-typed
                    // pseudo-parameter, but ch05 §5.9 keeps the real-C++
                    // `(*this).x` spelling valid at expression level. That
                    // makes `*this` just an explicit spelling of the same
                    // referent codegen_lvalue(Identifier "this") already
                    // resolves.
                    return codegen_lvalue(*expr.lhs);
                }
                std::optional<Type> operand_type = infer_type(*expr.lhs);
                if (!operand_type.has_value()) {
                    throw CodegenError("expression is not assignable", current_loc_);
                }
                const Type& operand_underlying =
                    operand_type->kind == TypeKind::Reference && operand_type->pointee ? *operand_type->pointee : *operand_type;
                if (operand_underlying.kind == TypeKind::Named) {
                    LValue operand = codegen_lvalue(*expr.lhs);
                    std::vector<ExprPtr> no_args;
                    bool receiver_is_mutable = !is_read_only_place(*expr.lhs);
                    if (const Function* callee_def =
                            resolve_overload_by_type(operand.type.name + "_operator_deref", no_args, 1,
                                                     receiver_is_mutable, expr.lhs.get())) {
                        llvm::LLVMValueRef callee = llvm::LLVMGetNamedFunction(module_, overload_names_.at(callee_def).c_str());
                        if (callee == nullptr) {
                            throw CodegenError("call to unknown function '" + operand.type.name + "_operator_deref'",
                                current_loc_);
                        }
                        llvm::LLVMValueRef referent_ptr = build_call(callee, {operand.ptr});
                        if (callee_def->return_type.kind != TypeKind::Reference) {
                            throw CodegenError("operator* on class '" + operand.type.name +
                                                   "' must return a reference to be assignable",
                                current_loc_);
                        }
                        return LValue{referent_ptr, *callee_def->return_type.pointee,
                                      alignment_for_type(*callee_def->return_type.pointee)};
                    }
                }
                if (operand_type->kind != TypeKind::Pointer) {
                    // Whether a raw pointer dereference is licensed here
                    // (ch01 §1.3: only inside `unsafe {}`) is the move
                    // checker's job (scpp.movecheck), not codegen's --
                    // by the time a program reaches codegen it's already
                    // been accepted, so this is purely an "operand has no
                    // sensible address to load" guard. A reference
                    // operand can't reach here at all (codegen_lvalue's
                    // own Identifier case already auto-dereferences a
                    // reference-typed local, so `*r` where `r` is `T&`
                    // would already have `r` resolved to its referent by
                    // the time this runs).
                    throw CodegenError("dereference ('*') is only supported for a raw pointer or a class with operator*",
                        current_loc_);
                }
                if (is_interface_pointer_type(*operand_type)) {
                    throw CodegenError("dereferencing an interface pointer does not yield an assignable storage location",
                        current_loc_);
                }
                bool operand_has_storage =
                    expr.lhs->kind == ExprKind::Identifier || expr.lhs->kind == ExprKind::Member ||
                    expr.lhs->kind == ExprKind::Subscript;
                llvm::LLVMValueRef pointee_ptr = nullptr;
                if (operand_has_storage) {
                    LValue operand = codegen_lvalue(*expr.lhs);
                    pointee_ptr =
                       create_load(llvm::LLVMPointerTypeInContext(context_, 0), operand.ptr, operand.alignment, "deref");
                } else {
                    pointee_ptr = codegen_expr(*expr.lhs);
                }
                return LValue{pointee_ptr, *operand_type->pointee, alignment_for_type(*operand_type->pointee)};
            }

            default:
                throw CodegenError("expression is not assignable",
                    current_loc_);
        }
    }


    llvm::LLVMValueRef Codegen::codegen_builtin_print(const Expr& expr)
{
        if (expr.args.size() != 1) {
            throw CodegenError(expr.name + " expects exactly 1 argument",
                current_loc_);
        }
        llvm::LLVMValueRef printf_fn = get_or_declare_printf();
        llvm::LLVMValueRef arg = codegen_expr(*expr.args[0]);

        llvm::LLVMValueRef format;
        llvm::LLVMValueRef printf_arg;
        if (expr.name == "print_int") {
            format = llvm::LLVMBuildGlobalString(builder_, "%d\n", "fmt_int");
            printf_arg = arg;
        } else if (expr.name == "print_char") {
            format = llvm::LLVMBuildGlobalString(builder_, "%c\n", "fmt_char");
            // C's variadic calling convention always promotes a `char`
            // argument to `int` (the same "default argument promotion"
            // real C/C++ applies to any variadic call) -- printf's `%c`
            // reads a full `int`-sized argument regardless of the
            // narrower declared parameter type, so the raw i8 value must
            // be sign-extended before being passed through `...` here.
            printf_arg = llvm::LLVMBuildSExt(builder_, arg, llvm::LLVMInt32TypeInContext(context_), "charpromo");
        } else {
            format = llvm::LLVMBuildGlobalString(builder_, "%s\n", "fmt_bool");
            llvm::LLVMValueRef true_str = llvm::LLVMBuildGlobalString(builder_, "true", "str_true");
            llvm::LLVMValueRef false_str = llvm::LLVMBuildGlobalString(builder_, "false", "str_false");
            // `arg` is the i8 bool representation (see to_llvm_type);
            // CreateSelect needs a 1-bit condition.
            printf_arg = llvm::LLVMBuildSelect(builder_, bool_to_i1(arg), true_str, false_str, "booltmp");
        }
        return build_call(printf_fn, {format, printf_arg});
    }


    llvm::LLVMValueRef Codegen::get_or_declare_printf()
{
        if (llvm::LLVMValueRef existing = llvm::LLVMGetNamedFunction(module_, "printf")) {
            return existing;
        }
        llvm::LLVMTypeRef char_ptr_type = llvm::LLVMPointerTypeInContext(context_, 0);
        llvm::LLVMTypeRef printf_param_types[] = {char_ptr_type};
        llvm::LLVMTypeRef printf_type =
            llvm::LLVMFunctionType(llvm::LLVMInt32TypeInContext(context_), printf_param_types, 1, /*IsVarArg=*/1);
        return llvm::LLVMAddFunction(module_, "printf", printf_type);
    }


    llvm::LLVMValueRef Codegen::codegen_binary(const Expr& expr)
{
        if (expr.binary_op == BinaryOp::Assign) {
            LValue lv = codegen_lvalue(*expr.lhs);
            // spec §6.5: `y = x;` -- copy assignment (movecheck has
            // already verified `x` is the exact same class type and
            // that the class is copy-assignable) -- checked *before*
            // the generic value-evaluation path below, since this needs
            // to dispatch to a real function call (the user-declared
            // operator=, so its own side effects -- e.g. incrementing a
            // reference count -- actually run) or a recursive
            // memberwise copy-assign, neither of which is "evaluate the
            // RHS as one flat value, then store it" the way every other
            // assignment kind (including move assignment, which reuses
            // that same generic path below) is.
            if (lv.type.kind == TypeKind::Named && structs_.contains(lv.type.name) &&
                is_bare_same_type_copy_source(*expr.rhs, lv.type)) {
                llvm::LLVMValueRef src_ptr = codegen_lvalue(*expr.rhs).ptr;
                if (const Function* user_assign = find_user_declared_copy_assign_ast(lv.type.name)) {
                    llvm::LLVMValueRef op = llvm::LLVMGetNamedFunction(module_, overload_names_.at(user_assign).c_str());
                    build_call(op, {lv.ptr, src_ptr});
                } else {
                    codegen_memberwise_copy_assign(lv.ptr, src_ptr, lv.type.name);
                }
                if (expr.lhs->kind == ExprKind::Identifier) {
                    // See the move-assignment path's identical
                    // comment below for why this reset is needed
                    // (covers reassigning a previously-moved-out
                    // variable via a copy this time).
                    auto target_it = locals_.find(expr.lhs->name);
                    if (target_it != locals_.end() && target_it->second.moved_flag != nullptr) {
                        llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0),
                                             target_it->second.moved_flag);
                    }
                }
                return lv.ptr;
            }
            llvm::LLVMValueRef value = codegen_value_for_target(*expr.rhs, lv.type);
            // Refresh to `expr`'s own position -- see the VarDecl case's
            // identical comment in codegen_stmt.
            refresh_debug_location(expr.loc);
            check_store_type(value, to_llvm_type(lv.type), "'" + expr.lhs->name + "'");
            if (lv.type.kind == TypeKind::Named && structs_.contains(lv.type.name)) {
                // spec §6.4(3)/(5): `y = std::move(x);` -- the compiler-
                // synthesized move assignment operator (movecheck has
                // already verified `expr.rhs` is exactly this shape --
                // ordinary class reassignment is rejected before this
                // point is ever reached, see check_moves). Tear down
                // whatever `y` already owns before the `CreateStore`
                // below overwrites it wholesale with the source's own
                // bytes -- `value` above already came from `codegen_expr`
                // on the Move expression, which already nulled (and, for
                // a local class with a destructor, marked moved-out) the
                // source's slot, exactly like move-construction's own
                // identical reasoning (codegen_stmt's VarDecl case).
                llvm::LLVMValueRef target_moved_flag = nullptr;
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto target_it = locals_.find(expr.lhs->name);
                    if (target_it != locals_.end()) target_moved_flag = target_it->second.moved_flag;
                }
                codegen_destroy_old_class_state_for_move_assign(lv.ptr, lv.type.name, target_moved_flag);
            }
            create_store(value, lv.ptr, lv.alignment);
            if (lv.type.kind == TypeKind::Named && expr.lhs->kind == ExprKind::Identifier) {
                // spec §6.2(4)/§6.4: an assignment always leaves its own
                // target in the initialized state, holding the newly
                // assigned value -- including the (real, discovered-and-
                // fixed) self-move-assignment case `a = std::move(a);`,
                // where evaluating the RHS above transiently sets `a`'s
                // *own* moved_flag true as a side effect of `a` being the
                // Move's own source (see codegen_expr's Move case) before
                // this same statement's target (also `a`) is overwritten
                // right back with its own (unaliased-copy-preserved)
                // original value. Without this reset, `a`'s destructor
                // would be wrongly skipped at its own later scope-exit,
                // even though it again fully owns a valid value. Also
                // covers reassigning a *previously* moved-out variable
                // (its moved_flag would otherwise still read true from
                // that earlier move, despite this assignment giving it a
                // brand new value).
                auto target_it = locals_.find(expr.lhs->name);
                if (target_it != locals_.end() && target_it->second.moved_flag != nullptr) {
                    llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0), target_it->second.moved_flag);
                }
            }
            return value;
        }

        // `&&`/`||` short-circuit like ordinary C++; everything else is a
        // plain eager binary op on the operand values.
        if (expr.binary_op == BinaryOp::And || expr.binary_op == BinaryOp::Or) {
            return codegen_short_circuit(expr);
        }

        // ch06 §6: an operand that's a bare literal has no fixed type of
        // its own (see codegen_value_for_target) -- infer a "context
        // type" from whichever side is *not* a literal (if either is)
        // before evaluating either operand, so e.g. `int64_t x = c + 1;`
        // generates `1` directly as an i64 constant rather than the
        // default i32 (which would otherwise mismatch `c` and fail
        // llvm::LLVM's own module verifier at the arithmetic instruction
        // itself, a much less clear diagnostic than check_store_type's
        // own). Movecheck has already rejected any two-distinct-real-
        // scalar-type mismatch, so this can never itself paper over a
        // genuine type error -- only ever resolves an otherwise-untyped
        // literal.
        bool lhs_is_literal = expr.lhs->kind == ExprKind::IntegerLiteral || expr.lhs->kind == ExprKind::FloatLiteral;
        bool rhs_is_literal = expr.rhs->kind == ExprKind::IntegerLiteral || expr.rhs->kind == ExprKind::FloatLiteral;
        std::optional<Type> lhs_type = infer_type(*expr.lhs);
        std::optional<Type> rhs_type = infer_type(*expr.rhs);
        if ((expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne) && lhs_type.has_value() && rhs_type.has_value() &&
            is_interface_pointer_type(binary_operand_type(*lhs_type)) && is_interface_pointer_type(binary_operand_type(*rhs_type))) {
            llvm::LLVMValueRef lhs_object = extract_interface_object_ptr(codegen_expr(*expr.lhs));
            llvm::LLVMValueRef rhs_object = extract_interface_object_ptr(codegen_expr(*expr.rhs));
            return i1_to_bool(expr.binary_op == BinaryOp::Eq ? llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, lhs_object, rhs_object, "eqtmp")
                                                             : llvm::LLVMBuildICmp(builder_, llvm::LLVMIntNE, lhs_object, rhs_object, "netmp"));
        }
        std::optional<Type> pointer_result_type =
            lhs_type.has_value() && rhs_type.has_value() ? pointer_arithmetic_result_type(expr.binary_op, *lhs_type, *rhs_type)
                                                         : std::nullopt;
        bool arithmetic_op = expr.binary_op == BinaryOp::Add || expr.binary_op == BinaryOp::Sub || expr.binary_op == BinaryOp::Mul ||
                             expr.binary_op == BinaryOp::Div;
        bool pointer_operand_present =
            lhs_type.has_value() && rhs_type.has_value() &&
            (binary_operand_type(*lhs_type).kind == TypeKind::Pointer || binary_operand_type(*rhs_type).kind == TypeKind::Pointer);
        if (arithmetic_op && pointer_operand_present && !pointer_result_type.has_value()) {
            throw CodegenError("pointer arithmetic requires 'pointer +/- integer' or 'pointer - pointer' with matching "
                               "non-void pointer types",
                current_loc_);
        }
        bool needs_strict_scalar_match = expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne ||
                                         expr.binary_op == BinaryOp::Lt || expr.binary_op == BinaryOp::Gt ||
                                         expr.binary_op == BinaryOp::Le || expr.binary_op == BinaryOp::Ge;
        if (needs_strict_scalar_match && lhs_type.has_value() && rhs_type.has_value()) {
            const Type& lhs_operand_type = binary_operand_type(*lhs_type);
            const Type& rhs_operand_type = binary_operand_type(*rhs_type);
            if (!types_equal(lhs_operand_type, rhs_operand_type) && !lhs_is_literal && !rhs_is_literal) {
                throw CodegenError("binary operator requires operands of the same type; scpp has no implicit conversion "
                                   "between distinct scalar types",
                                   current_loc_);
            }
        }
        std::optional<Type> context_type;
        if (!pointer_result_type.has_value() && lhs_is_literal && !rhs_is_literal) {
            context_type = lhs_type.has_value() && rhs_type.has_value() ? binary_operand_type(*rhs_type) : infer_type(*expr.rhs);
        } else if (!pointer_result_type.has_value() && rhs_is_literal && !lhs_is_literal) {
            context_type = lhs_type.has_value() && rhs_type.has_value() ? binary_operand_type(*lhs_type) : infer_type(*expr.lhs);
        }
        if (needs_strict_scalar_match && lhs_type.has_value() && rhs_type.has_value() &&
            !types_equal(binary_operand_type(*lhs_type), binary_operand_type(*rhs_type)) &&
            context_type.has_value()) {
            const Type& literal_target = *context_type;
            bool lhs_matches = !lhs_is_literal || ((expr.lhs->kind == ExprKind::FloatLiteral && is_float_scalar_type_name(literal_target.name)) ||
                                                   (expr.lhs->kind == ExprKind::IntegerLiteral &&
                                                    literal_target.kind == TypeKind::Named &&
                                                    literal_target.name != "bool" && literal_target.name != "char"));
            bool rhs_matches = !rhs_is_literal || ((expr.rhs->kind == ExprKind::FloatLiteral && is_float_scalar_type_name(literal_target.name)) ||
                                                   (expr.rhs->kind == ExprKind::IntegerLiteral &&
                                                    literal_target.kind == TypeKind::Named &&
                                                    literal_target.name != "bool" && literal_target.name != "char"));
            if (!(lhs_matches && rhs_matches)) {
                throw CodegenError("binary operator requires operands of the same type; scpp has no implicit conversion "
                                   "between distinct scalar types",
                                   current_loc_);
            }
        }
        llvm::LLVMValueRef lhs = context_type.has_value() ? codegen_value_for_target(*expr.lhs, *context_type)
                                                      : codegen_expr(*expr.lhs);
        llvm::LLVMValueRef rhs = context_type.has_value() ? codegen_value_for_target(*expr.rhs, *context_type)
                                                      : codegen_expr(*expr.rhs);

        // ch06 §6: the operand type (preferring the resolved context
        // type above, when there was a literal to resolve; otherwise the
        // LHS -- movecheck has already rejected any two-distinct-real-
        // scalar-type mismatch, so both operands always share one type
        // by the time this runs) decides signed-vs-unsigned-vs-floating-
        // point codegen for every arithmetic/ordering operator below;
        // `Eq`/`Ne` alone are signedness-independent (an icmp/fcmp
        // equality predicate is the same regardless) but still need
        // fcmp for a float operand.
        std::optional<Type> operand_type = context_type.has_value() ? context_type : lhs_type;
        if (operand_type.has_value()) operand_type = binary_operand_type(*operand_type);
        bool is_float = operand_type.has_value() && is_float_scalar_type_name(operand_type->name);
        bool is_unsigned = operand_type.has_value() && is_unsigned_scalar_type_name(operand_type->name);
        bool is_checked = operand_type.has_value() && is_checked_arithmetic_scalar_type_name(operand_type->name);

        switch (expr.binary_op) {
            case BinaryOp::Add:
            case BinaryOp::Sub:
                if (pointer_result_type.has_value()) {
                    const Type& lhs_operand_type = binary_operand_type(*lhs_type);
                    const Type& rhs_operand_type = binary_operand_type(*rhs_type);
                    if (lhs_operand_type.kind == TypeKind::Pointer) {
                        if (rhs_operand_type.kind == TypeKind::Pointer) {
                            return codegen_pointer_difference(lhs, rhs, lhs_operand_type);
                        }
                        return codegen_pointer_offset(lhs, rhs, lhs_operand_type, expr.binary_op == BinaryOp::Sub);
                    }
                    return codegen_pointer_offset(rhs, lhs, binary_operand_type(*rhs_type), /*negate_offset=*/false);
                }
                [[fallthrough]];
            case BinaryOp::Mul:
                if (is_float) return codegen_float_arith(expr.binary_op, lhs, rhs);
                return codegen_checked_arith(expr.binary_op, lhs, rhs, is_unsigned, is_checked);
            case BinaryOp::Div:
                if (is_float) return llvm::LLVMBuildFDiv(builder_, lhs, rhs, "fdivtmp");
                return codegen_checked_div(lhs, rhs, is_unsigned, is_checked);
            // Comparisons always produce a genuine i1 from icmp/fcmp, but
            // a scpp `bool` result needs to be widened to the i8 every
            // other bool value uses (see i1_to_bool/to_llvm_type) before
            // it can be stored, passed, or returned like any other value.
            case BinaryOp::Eq:
                return i1_to_bool(is_float ? llvm::LLVMBuildFCmp(builder_, llvm::LLVMRealOEQ, lhs, rhs, "eqtmp")
                                            : llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, lhs, rhs, "eqtmp"));
            case BinaryOp::Ne:
                return i1_to_bool(is_float ? llvm::LLVMBuildFCmp(builder_, llvm::LLVMRealONE, lhs, rhs, "netmp")
                                            : llvm::LLVMBuildICmp(builder_, llvm::LLVMIntNE, lhs, rhs, "netmp"));
            case BinaryOp::Lt:
                return i1_to_bool(is_float ? llvm::LLVMBuildFCmp(builder_, llvm::LLVMRealOLT, lhs, rhs, "lttmp")
                                   : is_unsigned ? llvm::LLVMBuildICmp(builder_, llvm::LLVMIntULT, lhs, rhs, "lttmp")
                                                  : llvm::LLVMBuildICmp(builder_, llvm::LLVMIntSLT, lhs, rhs, "lttmp"));
            case BinaryOp::Gt:
                return i1_to_bool(is_float ? llvm::LLVMBuildFCmp(builder_, llvm::LLVMRealOGT, lhs, rhs, "gttmp")
                                   : is_unsigned ? llvm::LLVMBuildICmp(builder_, llvm::LLVMIntUGT, lhs, rhs, "gttmp")
                                                  : llvm::LLVMBuildICmp(builder_, llvm::LLVMIntSGT, lhs, rhs, "gttmp"));
            case BinaryOp::Le:
                return i1_to_bool(is_float ? llvm::LLVMBuildFCmp(builder_, llvm::LLVMRealOLE, lhs, rhs, "letmp")
                                   : is_unsigned ? llvm::LLVMBuildICmp(builder_, llvm::LLVMIntULE, lhs, rhs, "letmp")
                                                  : llvm::LLVMBuildICmp(builder_, llvm::LLVMIntSLE, lhs, rhs, "letmp"));
            case BinaryOp::Ge:
                return i1_to_bool(is_float ? llvm::LLVMBuildFCmp(builder_, llvm::LLVMRealOGE, lhs, rhs, "getmp")
                                   : is_unsigned ? llvm::LLVMBuildICmp(builder_, llvm::LLVMIntUGE, lhs, rhs, "getmp")
                                                  : llvm::LLVMBuildICmp(builder_, llvm::LLVMIntSGE, lhs, rhs, "getmp"));
            default: throw CodegenError("unhandled binary operator",
                current_loc_);
        }
    }


    llvm::LLVMValueRef Codegen::codegen_short_circuit(const Expr& expr)
{
        llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
        bool is_and = expr.binary_op == BinaryOp::And;

        // `lhs`/`rhs` stay in the i8 bool representation throughout (so
        // the merging PHI below can use either directly, matching how
        // every other bool value is stored/passed/returned) -- only the
        // branch conditions themselves need the narrower bool_to_i1 form.
        llvm::LLVMValueRef lhs = codegen_contextual_bool_value(*expr.lhs);
        llvm::LLVMBasicBlockRef rhs_block =
            llvm::LLVMAppendBasicBlockInContext(context_, current_function, is_and ? "and.rhs" : "or.rhs");
        llvm::LLVMBasicBlockRef merge_block =
            llvm::LLVMAppendBasicBlockInContext(context_, current_function, is_and ? "and.end" : "or.end");
        llvm::LLVMBasicBlockRef lhs_block = llvm::LLVMGetInsertBlock(builder_);

        if (is_and) {
            llvm::LLVMBuildCondBr(builder_, bool_to_i1(lhs), rhs_block, merge_block);
        } else {
            llvm::LLVMBuildCondBr(builder_, bool_to_i1(lhs), merge_block, rhs_block);
        }

        llvm::LLVMPositionBuilderAtEnd(builder_, rhs_block);
        llvm::LLVMValueRef rhs = codegen_contextual_bool_value(*expr.rhs);
        llvm::LLVMBasicBlockRef rhs_end_block = llvm::LLVMGetInsertBlock(builder_);
        llvm::LLVMBuildBr(builder_, merge_block);

        llvm::LLVMPositionBuilderAtEnd(builder_, merge_block);
        llvm::LLVMValueRef phi = llvm::LLVMBuildPhi(builder_, llvm::LLVMInt8TypeInContext(context_), "logictmp");
        llvm::LLVMValueRef incoming_values[] = {lhs, rhs};
        llvm::LLVMBasicBlockRef incoming_blocks[] = {lhs_block, rhs_end_block};
        llvm::LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
        return phi;
    }

} // namespace scpp
