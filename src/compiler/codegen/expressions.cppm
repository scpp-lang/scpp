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

[[nodiscard]] bool is_string_named_type(const Type& type) {
    return type.kind == TypeKind::Named && (type.name == "std::string" || type.name == "string");
}

[[nodiscard]] bool is_const_char_pointer_type(const Type& type) {
    return type.kind == TypeKind::Pointer && type.pointee != nullptr && type.pointee->kind == TypeKind::Named &&
           type.pointee->name == "char" && !type.is_mutable_pointee;
}

[[nodiscard]] bool is_compound_assignment(BinaryOp op) {
    return op == BinaryOp::AddAssign || op == BinaryOp::SubAssign || op == BinaryOp::MulAssign || op == BinaryOp::DivAssign;
}

[[nodiscard]] BinaryOp compound_base_operator(BinaryOp op) {
    switch (op) {
        case BinaryOp::AddAssign: return BinaryOp::Add;
        case BinaryOp::SubAssign: return BinaryOp::Sub;
        case BinaryOp::MulAssign: return BinaryOp::Mul;
        case BinaryOp::DivAssign: return BinaryOp::Div;
        default: return op;
    }
}

} // namespace

    [[nodiscard]] bool Codegen::is_enum_cast_store_builtin_name(const std::string& name)
{
        return name == "scpp::__enum_cast_store" || name.rfind("scpp::__enum_cast_store.", 0) == 0;
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::store_constexpr_value_into(llvm::LLVMValueRef dest_ptr, const Type& dest_type, const ConstexprValue& value)
{
        if (is_scalar_type_name(dest_type.name)) {
            if (dest_type.kind == TypeKind::Named && dest_type.name == "bool") {
                create_store(llvm::LLVMConstInt(llvm::LLVMInt8TypeInContext(context_), value.bool_value ? 1 : 0, 0), dest_ptr,
                             std::nullopt);
                return {};
            }
            if (dest_type.kind == TypeKind::Named && dest_type.name == "char") {
                create_store(llvm::LLVMConstInt(llvm::LLVMInt8TypeInContext(context_), static_cast<std::uint64_t>(value.int_value), 0), dest_ptr,
                             std::nullopt);
                return {};
            }
            if (dest_type.kind == TypeKind::Named && dest_type.name == "double") {
                create_store(llvm::LLVMConstReal(llvm::LLVMDoubleTypeInContext(context_), value.double_value), dest_ptr,
                             std::nullopt);
                return {};
            }
            create_store(llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), static_cast<std::uint64_t>(value.int_value), 1), dest_ptr,
                         std::nullopt);
            return {};
        }
        if (dest_type.kind == TypeKind::Pointer && dest_type.pointee &&
            dest_type.pointee->kind == TypeKind::Named && dest_type.pointee->name == "char" && !dest_type.is_mutable_pointee &&
            value.kind == ConstexprValueKind::StringLiteralPointer) {
            create_store(llvm::LLVMBuildGlobalString(builder_, value.string_value.c_str(), "cexprstr"), dest_ptr, std::nullopt);
            return {};
        }
        if (dest_type.kind == TypeKind::Array && dest_type.element && value.kind == ConstexprValueKind::Array) {
            auto array_llvm_type_result = to_llvm_type(dest_type);
            if (!array_llvm_type_result.has_value()) return std::unexpected(std::move(array_llvm_type_result).error());
            llvm::LLVMTypeRef array_llvm_type = std::move(array_llvm_type_result).value();
            for (std::size_t i = 0; i < value.elements.size(); ++i) {
                llvm::LLVMTypeRef i32 = llvm::LLVMInt32TypeInContext(context_);
                llvm::LLVMValueRef indices[] = {llvm::LLVMConstInt(i32, 0, 0), llvm::LLVMConstInt(i32, static_cast<unsigned>(i), 0)};
                llvm::LLVMValueRef elem_ptr = llvm::LLVMBuildGEP2(builder_, array_llvm_type, dest_ptr, indices, 2, "");
                if (auto r = store_constexpr_value_into(elem_ptr, *dest_type.element, value.elements[i]); !r.has_value())
                    return std::unexpected(std::move(r).error());
            }
            return {};
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
                if (auto r = store_constexpr_value_into(field_ptr, info.field_types[i], *it->second); !r.has_value())
                    return std::unexpected(std::move(r).error());
            }
            return {};
        }
        return std::unexpected(CodegenError("unsupported constexpr class materialization for type '" + dest_type.name + "'", current_loc_));
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_consteval_class_value(const Expr& expr, const std::string& class_name)
{
        auto value_result = evaluate_immediate_expr(*program_, expr);
        if (!value_result.has_value()) {
            // evaluate_immediate_expr's ConstexprError bakes its own
            // "line:col: " position prefix directly into what() (unlike
            // CodegenError's own separate .loc field) -- see
            // constexpression.cppm's own comment -- so only the message is
            // carried across here, exactly like fold_immediate_calls'
            // sibling conversion in driver.cppm's
            // emit_object_file_for_program: passing current_loc_ too
            // would duplicate the position in the final printed
            // diagnostic.
            return std::unexpected(CodegenError(value_result.error().what()));
        }
        ConstexprValue value = std::move(value_result).value();
        auto llvm_type_result = to_llvm_type(named_type(class_name));
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
        std::optional<unsigned> align = alignment_for_type(named_type(class_name));
        llvm::LLVMValueRef temp = create_entry_block_alloca(llvm_type, "constevalclasstmp", align);
        if (auto r = zero_initialize_storage(temp, named_type(class_name), align); !r.has_value()) return std::unexpected(std::move(r).error());
        if (auto r = store_constexpr_value_into(temp, named_type(class_name), value); !r.has_value()) return std::unexpected(std::move(r).error());
        return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "constevalclass.value");
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_constructed_class_value(const std::string& class_name, const std::vector<ExprPtr>& args,
                                                 const Function* ctor_def, const Expr* original_expr)
{
        auto llvm_type_result = to_llvm_type(named_type(class_name));
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
        std::optional<unsigned> align = alignment_for_type(named_type(class_name));
        llvm::LLVMValueRef temp = create_entry_block_alloca(llvm_type, "classtmp", align);
        LValue target{temp, named_type(class_name), align};
        if (auto r = zero_initialize_storage(target.ptr, target.type, target.alignment); !r.has_value()) return std::unexpected(std::move(r).error());
        auto same_type_source_result = try_initialize_class_storage_from_same_type_source(target, args);
        if (!same_type_source_result.has_value()) return std::unexpected(std::move(same_type_source_result).error());
        if (std::move(same_type_source_result).value()) {
            return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "classtmp.value");
        }
        if (ctor_def != nullptr) {
            if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                ExprPtr ctor_expr;
                if (original_expr != nullptr) {
                    ctor_expr = deep_clone_expr(*original_expr);
                } else {
                    ctor_expr = std::make_unique<Expr>();
                    ctor_expr->kind = ExprKind::Call;
                    ctor_expr->loc = current_loc_;
                    ctor_expr->name = class_name;
                    ctor_expr->has_paren_init = true;
                    for (const ExprPtr& arg : args) ctor_expr->args.push_back(deep_clone_expr(*arg));
                }
                auto value_result = evaluate_immediate_expr(*program_, *ctor_expr);
                if (!value_result.has_value()) {
                    // See codegen_consteval_class_value's matching
                    // conversion above for why .loc is deliberately not
                    // forwarded here.
                    return std::unexpected(CodegenError(value_result.error().what()));
                }
                ConstexprValue value = std::move(value_result).value();
                if (auto r = store_constexpr_value_into(target.ptr, target.type, value); !r.has_value())
                    return std::unexpected(std::move(r).error());
            } else {
                llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
                if (ctor == nullptr) {
                    return std::unexpected(CodegenError("class '" + class_name + "' has no constructor matching this call", current_loc_));
                }
                auto ctor_args_result =
                    emit_constructor_arguments_and_virtual_bases(class_name, ctor_def, args, target.ptr);
                if (!ctor_args_result.has_value()) return std::unexpected(std::move(ctor_args_result).error());
                std::vector<llvm::LLVMValueRef> ctor_args = std::move(ctor_args_result).value();
                ctor_args.insert(ctor_args.begin(), target.ptr);
                build_call(ctor, ctor_args);
            }
        } else if (args.empty()) {
            const ClassDef* class_def = find_class_def(class_name);
            if (class_def != nullptr && !class_has_any_constructor(class_name)) {
                if (auto r = emit_default_initializers_for_class_storage(target.ptr, *class_def, /*initialize_virtual_interface_bases=*/true);
                    !r.has_value()) {
                    return std::unexpected(std::move(r).error());
                }
            }
        }
        return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "classtmp.value");
    }


    [[nodiscard]] std::expected<Codegen::CallResult, CodegenError> Codegen::codegen_call(const Expr& expr)
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
                        return std::unexpected(CodegenError(
                            describe_call_resolution_failure(receiver_named.name + "_" + expr.name,
                                                             receiver_named.name + "::" + expr.name, expr.args,
                                                             /*param_offset=*/1, !is_read_only_place(*expr.lhs),
                                                             expr.lhs.get()),
                            current_loc_));
                    }
                    auto receiver_value_result = codegen_expr(*expr.lhs);
                    if (!receiver_value_result.has_value()) return std::unexpected(std::move(receiver_value_result).error());
                    llvm::LLVMValueRef receiver_value = std::move(receiver_value_result).value();
                    if (!callee->is_virtual) {
                        llvm::LLVMValueRef target = llvm::LLVMGetNamedFunction(module_, overload_names_.at(callee).c_str());
                        auto args_result = codegen_call_args(expr.args, callee, /*param_offset=*/1);
                        if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                        std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                        args.insert(args.begin(), receiver_value);
                        return CallResult{build_call(target, args), callee};
                    }
                    auto slot_index_result = interface_method_slot_index(receiver_named.name, *callee);
                    if (!slot_index_result.has_value()) return std::unexpected(std::move(slot_index_result).error());
                    std::optional<std::size_t> slot_index = std::move(slot_index_result).value();
                    if (!slot_index.has_value()) {
                        return std::unexpected(CodegenError("missing interface dispatch slot for '" + callee->name + "'", current_loc_));
                    }
                    llvm::LLVMValueRef dispatch_ptr = extract_interface_dispatch_ptr(receiver_value);
                    auto table_type_result = interface_dispatch_table_type(receiver_named.name);
                    if (!table_type_result.has_value()) return std::unexpected(std::move(table_type_result).error());
                    llvm::LLVMTypeRef table_type = std::move(table_type_result).value();
                    llvm::LLVMValueRef table_ptr =
                        llvm::LLVMBuildBitCast(builder_, dispatch_ptr, llvm::LLVMPointerTypeInContext(context_, 0), "ifacetable");
                    llvm::LLVMTypeRef i32 = llvm::LLVMInt32TypeInContext(context_);
                    llvm::LLVMValueRef slot_indices[] = {llvm::LLVMConstInt(i32, 0, 0),
                                                   llvm::LLVMConstInt(i32, static_cast<unsigned>(*slot_index), 0)};
                    llvm::LLVMValueRef slot_ptr =
                        llvm::LLVMBuildGEP2(builder_, table_type, table_ptr, slot_indices, 2, "ifaceslot");
                    llvm::LLVMValueRef target_ptr =
                        create_load(llvm::LLVMPointerTypeInContext(context_, 0), slot_ptr, std::nullopt, "ifacemethod");
                    auto args_result = codegen_call_args(expr.args, callee, /*param_offset=*/1);
                    if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                    std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                    args.insert(args.begin(), extract_interface_object_ptr(receiver_value));
                    auto dispatch_fn_type_result = interface_dispatch_function_type(*callee);
                    if (!dispatch_fn_type_result.has_value()) return std::unexpected(std::move(dispatch_fn_type_result).error());
                    return CallResult{build_call(std::move(dispatch_fn_type_result).value(), target_ptr, args), callee};
                }
            }
            auto base_result = codegen_lvalue(*expr.lhs);
            if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
            LValue base = std::move(base_result).value();
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
                    auto member_llvm_type_result = to_llvm_type(member_type);
                    if (!member_llvm_type_result.has_value()) return std::unexpected(std::move(member_llvm_type_result).error());
                    llvm::LLVMValueRef callee_value =
                        create_load(std::move(member_llvm_type_result).value(), field_ptr,
                                    info.is_union ? base.alignment
                                                  : std::optional<unsigned>(info.field_alignments[*field_index_opt]),
                                    expr.name + ".fn");
                    auto args_result = codegen_call_args_for_types(expr.args, member_type.function_params);
                    if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                    std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                    std::vector<llvm::LLVMTypeRef> params;
                    params.reserve(member_type.function_params.size());
                    for (const Type& param : member_type.function_params) {
                        auto param_type_result = to_llvm_type(param);
                        if (!param_type_result.has_value()) return std::unexpected(std::move(param_type_result).error());
                        params.push_back(std::move(param_type_result).value());
                    }
                    auto return_type_result = to_llvm_type(*member_type.function_return);
                    if (!return_type_result.has_value()) return std::unexpected(std::move(return_type_result).error());
                    llvm::LLVMTypeRef fn_type =
                        llvm::LLVMFunctionType(std::move(return_type_result).value(), params.data(),
                                         static_cast<unsigned>(params.size()), /*IsVarArg=*/0);
                    return CallResult{build_call(fn_type, callee_value, args), nullptr};
                }
            }
            if (receiver_type.has_value() && receiver_type->kind == TypeKind::FunctionPointer) {
                auto callee_value_result = codegen_expr(*expr.lhs);
                if (!callee_value_result.has_value()) return std::unexpected(std::move(callee_value_result).error());
                llvm::LLVMValueRef callee_value = std::move(callee_value_result).value();
                auto args_result = codegen_call_args_for_types(expr.args, receiver_type->function_params);
                if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                std::vector<llvm::LLVMTypeRef> params;
                params.reserve(receiver_type->function_params.size());
                for (const Type& param : receiver_type->function_params) {
                    auto param_type_result = to_llvm_type(param);
                    if (!param_type_result.has_value()) return std::unexpected(std::move(param_type_result).error());
                    params.push_back(std::move(param_type_result).value());
                }
                auto return_type_result = to_llvm_type(*receiver_type->function_return);
                if (!return_type_result.has_value()) return std::unexpected(std::move(return_type_result).error());
                llvm::LLVMTypeRef fn_type = llvm::LLVMFunctionType(std::move(return_type_result).value(), params.data(),
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
                return std::unexpected(CodegenError("indirect call requires a function pointer value", current_loc_));
            }
            auto callee_value_result = codegen_expr(*callee_expr);
            if (!callee_value_result.has_value()) return std::unexpected(std::move(callee_value_result).error());
            llvm::LLVMValueRef callee_value = std::move(callee_value_result).value();
            auto args_result = codegen_call_args_for_types(expr.args, callee_type->function_params);
            if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
            std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
            std::vector<llvm::LLVMTypeRef> params;
            params.reserve(callee_type->function_params.size());
            for (const Type& param : callee_type->function_params) {
                auto param_type_result = to_llvm_type(param);
                if (!param_type_result.has_value()) return std::unexpected(std::move(param_type_result).error());
                params.push_back(std::move(param_type_result).value());
            }
            auto return_type_result = to_llvm_type(*callee_type->function_return);
            if (!return_type_result.has_value()) return std::unexpected(std::move(return_type_result).error());
            llvm::LLVMTypeRef fn_type = llvm::LLVMFunctionType(std::move(return_type_result).value(), params.data(),
                                                   static_cast<unsigned>(params.size()), /*IsVarArg=*/0);
            return CallResult{build_call(fn_type, callee_value, args), nullptr};
        }
        if (expr.lhs == nullptr) {
            if (const Function* builtin_callee = resolve_overload_by_type(expr.name, expr.args, /*param_offset=*/0);
                builtin_callee != nullptr && is_enum_cast_store_builtin_name(builtin_callee->name)) {
                return codegen_enum_cast_store_builtin(expr, *builtin_callee);
            }
            if (find_class_def(expr.name) != nullptr) {
                // `ClassName{args...}` / `ClassName(args...)` used as an
                // expression selects a constructor by exactly the same
                // rule as every other spelling of "construct a
                // ClassName": the empty-braced VarDecl form
                // (codegen_var_decl / codegen_local_var_decl), the bare
                // `return {};` ValueInit form (codegen_expr's own
                // ExprKind::ValueInit case), and the compile-time
                // evaluator (constexpression.cppm's
                // evaluate_constructor_expr) all resolve unconditionally
                // from the argument list. Resolution must NOT be gated on
                // the argument list being non-empty: `has_paren_init` is
                // only ever set on ExprKind::New nodes (parser.cppm's
                // `new T(...)` case -- the one place where a *missing*
                // initializer is a real language distinction, since bare
                // `new T` is unsafe-gated raw allocation), so gating here
                // silently skipped the user-declared default constructor
                // for every zero-argument `ClassName{}` temporary while
                // still running its destructor at scope end.
                const Function* ctor_def = resolve_constructor_overload_exact(expr.name, expr.args);
                if (ctor_def == nullptr && !expr.args.empty()) {
                    return std::unexpected(CodegenError("class '" + expr.name + "' has no constructor matching this call", current_loc_));
                }
                auto value_result = codegen_constructed_class_value(expr.name, expr.args, ctor_def, &expr);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                return CallResult{std::move(value_result).value(), nullptr};
            }
            const LocalSlot* callee_local = find_local(expr);
            if (callee_local != nullptr && callee_local->type.kind == TypeKind::FunctionPointer) {
                auto local_llvm_type_result = to_llvm_type(callee_local->type);
                if (!local_llvm_type_result.has_value()) return std::unexpected(std::move(local_llvm_type_result).error());
                llvm::LLVMValueRef callee_value = llvm::LLVMBuildLoad2(builder_, std::move(local_llvm_type_result).value(), callee_local->alloca,
                                                           (expr.name + ".fnptr").c_str());
                auto args_result = codegen_call_args_for_types(expr.args, callee_local->type.function_params);
                if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                std::vector<llvm::LLVMTypeRef> params;
                params.reserve(callee_local->type.function_params.size());
                for (const Type& param : callee_local->type.function_params) {
                    auto param_type_result = to_llvm_type(param);
                    if (!param_type_result.has_value()) return std::unexpected(std::move(param_type_result).error());
                    params.push_back(std::move(param_type_result).value());
                }
                auto return_type_result = to_llvm_type(*callee_local->type.function_return);
                if (!return_type_result.has_value()) return std::unexpected(std::move(return_type_result).error());
                llvm::LLVMTypeRef fn_type = llvm::LLVMFunctionType(std::move(return_type_result).value(), params.data(),
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
            auto receiver_result = codegen_lvalue(*expr.lhs);
            if (!receiver_result.has_value()) return std::unexpected(std::move(receiver_result).error());
            LValue receiver = std::move(receiver_result).value();
            if (receiver.type.kind != TypeKind::Named) {
                return std::unexpected(CodegenError("method call '." + expr.name + "(...)' is only supported on a class type",
                    current_loc_));
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
        if (callee_def == nullptr && expr.lhs != nullptr) {
            for (const Function& fn : program_->functions) {
                if (fn.name != callee_name || fn.is_generic_template) continue;
                std::size_t required = fn.params.size() >= param_offset ? fn.params.size() - param_offset : 0;
                if (expr.args.size() > required) continue;
                if (param_offset == 1 && !receiver_matches_method_qualifier(*expr.lhs, fn)) continue;
                bool all_match = true;
                for (std::size_t i = 0; all_match && i < expr.args.size(); i++) {
                    const Type& param_type = fn.params[param_offset + i].type;
                    if (expr.args[i]->kind == ExprKind::Identifier &&
                        param_type.kind == TypeKind::Reference && !param_type.is_mutable_ref &&
                        !param_type.is_rvalue_ref && param_type.pointee != nullptr) {
                        const LocalSlot* arg_local = find_local(*expr.args[i]);
                        all_match = arg_local != nullptr && arg_local->type.kind == TypeKind::Reference &&
                                    arg_local->type.is_rvalue_ref && arg_local->type.pointee != nullptr &&
                                    types_equal(*arg_local->type.pointee, *param_type.pointee);
                    } else if (param_type.kind == TypeKind::Reference && !param_type.is_mutable_ref &&
                               !param_type.is_rvalue_ref && param_type.pointee != nullptr) {
                        std::optional<Type> arg_type = infer_type(*expr.args[i]);
                        all_match = arg_type.has_value() &&
                                    ((arg_type->kind == TypeKind::Reference && arg_type->pointee != nullptr &&
                                      types_equal(*arg_type->pointee, *param_type.pointee)) ||
                                     types_equal(*arg_type, *param_type.pointee));
                    } else {
                        all_match = false;
                    }
                }
                if (all_match) {
                    callee_def = &fn;
                    break;
                }
            }
        }
        if (callee_def == nullptr) {
            return std::unexpected(CodegenError(
                describe_call_resolution_failure(callee_name, call_display_name(expr, receiver_static_class_name),
                                                 expr.args, param_offset, receiver_is_mutable, expr.lhs.get()),
                current_loc_));
        }
        llvm::LLVMValueRef callee = llvm::LLVMGetNamedFunction(module_, overload_names_.at(callee_def).c_str());
        if (callee == nullptr) {
            // Not a user error at all: resolution succeeded, so the
            // declaration exists, but no llvm::LLVM function was ever emitted
            // for it. Say that, rather than blaming the call.
            return std::unexpected(CodegenError("internal error: no generated code for resolved function '" +
                                                    overload_names_.at(callee_def) + "' called here",
                current_loc_));
        }
        auto args_result = codegen_call_args(expr.args, callee_def, param_offset);
        if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
        std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
        if (this_arg != nullptr) {
            if (!callee_def->params.empty() && is_interface_reference_type(callee_def->params.front().type)) {
                auto interface_value_result = codegen_interface_value_for_target(*expr.lhs, callee_def->params.front().type);
                if (!interface_value_result.has_value()) return std::unexpected(std::move(interface_value_result).error());
                args.insert(args.begin(), std::move(interface_value_result).value());
            } else {
                args.insert(args.begin(), this_arg);
                auto slot_index_result = ordinary_method_slot_index(receiver_static_class_name, *callee_def);
                if (!slot_index_result.has_value()) return std::unexpected(std::move(slot_index_result).error());
                if (std::optional<std::size_t> slot_index = std::move(slot_index_result).value(); slot_index.has_value()) {
                    const StructInfo& info = structs_.at(receiver_static_class_name);
                    llvm::LLVMValueRef vptr_slot = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, this_arg, 0, "vptr");
                    llvm::LLVMValueRef vtable_ptr = create_load(llvm::LLVMPointerTypeInContext(context_, 0), vptr_slot, std::nullopt,
                                                          "vtable");
                    auto table_type_result = ordinary_vtable_type(receiver_static_class_name);
                    if (!table_type_result.has_value()) return std::unexpected(std::move(table_type_result).error());
                    llvm::LLVMTypeRef table_type = std::move(table_type_result).value();
                    llvm::LLVMValueRef table_ptr =
                        llvm::LLVMBuildBitCast(builder_, vtable_ptr, llvm::LLVMPointerTypeInContext(context_, 0), "vtable.array");
                    llvm::LLVMTypeRef i32 = llvm::LLVMInt32TypeInContext(context_);
                    llvm::LLVMValueRef slot_indices[] = {llvm::LLVMConstInt(i32, 0, 0),
                                                   llvm::LLVMConstInt(i32, static_cast<unsigned>(*slot_index), 0)};
                    llvm::LLVMValueRef slot_ptr =
                        llvm::LLVMBuildGEP2(builder_, table_type, table_ptr, slot_indices, 2, "vtable.slot");
                    llvm::LLVMValueRef target_ptr =
                        create_load(llvm::LLVMPointerTypeInContext(context_, 0), slot_ptr, std::nullopt, "virtfn");
                    auto dispatch_fn_type_result = interface_dispatch_function_type(*callee_def);
                    if (!dispatch_fn_type_result.has_value()) return std::unexpected(std::move(dispatch_fn_type_result).error());
                    return CallResult{build_call(std::move(dispatch_fn_type_result).value(), target_ptr, args),
                                      callee_def};
                }
            }
        }
        return CallResult{build_call(callee, args), callee_def};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::initialize_reference_storage(const Codegen::LValue& target, const Expr& expr)
{
        if (target.type.kind != TypeKind::Reference || target.type.pointee == nullptr) {
            return std::unexpected(CodegenError("internal error: reference initializer target is not a reference", current_loc_));
        }
        if (is_interface_reference_type(target.type)) {
            auto interface_value_result = codegen_interface_value_for_target(expr, target.type);
            if (!interface_value_result.has_value()) return std::unexpected(std::move(interface_value_result).error());
            create_store(std::move(interface_value_result).value(), target.ptr, target.alignment);
            return {};
        }
        if (auto r = validate_reference_pointee(*target.type.pointee); !r.has_value()) return std::unexpected(std::move(r).error());
        llvm::LLVMValueRef referent_addr;
        if (const_reference_binds_materialized_temporary(expr, target.type)) {
            auto materialized_result = codegen_materialize_const_reference_source(expr, *target.type.pointee);
            if (!materialized_result.has_value()) return std::unexpected(std::move(materialized_result).error());
            referent_addr = std::move(materialized_result).value();
        } else {
            auto lvalue_result = codegen_lvalue(expr);
            if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
            referent_addr = std::move(lvalue_result).value().ptr;
        }
        create_store(referent_addr, target.ptr, target.alignment);
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::initialize_span_storage(const Codegen::LValue& target, const Expr& expr)
{
        if (target.type.kind != TypeKind::Span || target.type.pointee == nullptr) {
            return std::unexpected(CodegenError("internal error: span initializer target is not a span", current_loc_));
        }
        auto span_value_result = codegen_span_value_for_target(expr, target.type);
        if (!span_value_result.has_value()) return std::unexpected(std::move(span_value_result).error());
        create_store(std::move(span_value_result).value(), target.ptr, target.alignment);
        return {};
    }


    [[nodiscard]] std::expected<bool, CodegenError> Codegen::try_initialize_class_storage_from_same_type_source(const Codegen::LValue& target, const std::vector<ExprPtr>& args)
{
        auto same_type_moved_source_ptr = [&](const Expr& expr) -> std::expected<std::optional<llvm::LLVMValueRef>, CodegenError> {
            if (expr.kind != ExprKind::Move || expr.lhs == nullptr) return std::optional<llvm::LLVMValueRef>(std::nullopt);
            std::optional<Type> moved_source_type = infer_type(*expr.lhs);
            if (!moved_source_type.has_value()) return std::optional<llvm::LLVMValueRef>(std::nullopt);
            Type source_value_type =
                moved_source_type->kind == TypeKind::Reference && moved_source_type->pointee != nullptr
                    ? *moved_source_type->pointee
                    : *moved_source_type;
            if (!types_equal(source_value_type, target.type)) return std::optional<llvm::LLVMValueRef>(std::nullopt);
            auto lvalue_result = codegen_lvalue(*expr.lhs);
            if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
            return std::optional<llvm::LLVMValueRef>(std::move(lvalue_result).value().ptr);
        };
        auto is_moved_class_source = [&](const Expr& expr) -> std::expected<bool, CodegenError> {
            auto r = same_type_moved_source_ptr(expr);
            if (!r.has_value()) return std::unexpected(std::move(r).error());
            return std::move(r).value().has_value();
        };
        if (!is_named_record_type(target.type) || args.size() != 1) {
            return false;
        }
        auto moved_src_ptr_result = same_type_moved_source_ptr(*args[0]);
        if (!moved_src_ptr_result.has_value()) return std::unexpected(std::move(moved_src_ptr_result).error());
        if (std::optional<llvm::LLVMValueRef> moved_src_ptr = std::move(moved_src_ptr_result).value(); moved_src_ptr.has_value()) {
            auto target_llvm_type_result = to_llvm_type(target.type);
            if (!target_llvm_type_result.has_value()) return std::unexpected(std::move(target_llvm_type_result).error());
            llvm::LLVMValueRef moved_value = create_load(std::move(target_llvm_type_result).value(), *moved_src_ptr, std::nullopt, "movetmp");
            create_store(moved_value, target.ptr, target.alignment);
            if (auto r = zero_initialize_storage(*moved_src_ptr, target.type, std::nullopt); !r.has_value())
                return std::unexpected(std::move(r).error());
            if (args[0]->lhs != nullptr && args[0]->lhs->kind == ExprKind::Identifier) {
                const LocalSlot* source_local = find_local(*args[0]->lhs);
                if (source_local != nullptr && source_local->moved_flag != nullptr) {
                    llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 1, 0),
                                         source_local->moved_flag);
                }
            }
            if (class_has_ordinary_vtable(target.type.name)) {
                if (auto r = initialize_ordinary_vtable_pointer(target.type.name, target.ptr); !r.has_value())
                    return std::unexpected(std::move(r).error());
            }
            return true;
        }
        auto is_moved_source_result = is_moved_class_source(*args[0]);
        if (!is_moved_source_result.has_value()) return std::unexpected(std::move(is_moved_source_result).error());
        if (produces_rvalue_of_type(*args[0], target.type) && !std::move(is_moved_source_result).value()) {
            auto value_result = codegen_expr(*args[0]);
            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
            create_store(std::move(value_result).value(), target.ptr, target.alignment);
            if (class_has_ordinary_vtable(target.type.name)) {
                if (auto r = initialize_ordinary_vtable_pointer(target.type.name, target.ptr); !r.has_value())
                    return std::unexpected(std::move(r).error());
            }
            return true;
        }
        bool allow_hidden_helper_copy =
            current_function_def_ != nullptr && current_function_def_->is_compile_time_dependency;
        if (!is_bare_same_type_copy_source(*args[0], target.type) ||
            (!allow_hidden_helper_copy && !is_copy_constructible(target.type.name))) {
            return false;
        }
        auto src_result = codegen_lvalue(*args[0]);
        if (!src_result.has_value()) return std::unexpected(std::move(src_result).error());
        LValue src = std::move(src_result).value();
        if (auto r = codegen_copy_construct_class(target.ptr, src.ptr, target.type.name); !r.has_value())
            return std::unexpected(std::move(r).error());
        return true;
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::initialize_storage_from_expr(const Codegen::LValue& target, const Expr& expr)
{
        if (target.type.kind == TypeKind::Reference) {
            return initialize_reference_storage(target, expr);
        }
        if (target.type.kind == TypeKind::Span) {
            return initialize_span_storage(target, expr);
        }
        if (is_named_record_type(target.type)) {
            // /*allow_implicit_converting_ctor=*/true, to match every
            // other value-to-class-type boundary (codegen_value_for_target
            // already passes true, and dataflow.cppm now accepts a
            // converting constructor uniformly via
            // resolve_converting_constructor_binding). Without it a
            // source of some other type was stored into class-shaped
            // storage verbatim: a default member initializer spelled
            // with `=` -- `class C { std::string s = "hi"; };` -- reaches
            // here, and used to compile with no diagnostic at all and
            // then crash at run time when ~std::string freed a string
            // literal. The brace-spelled `std::string s{"hi"};` never
            // came through here, which is why the two spellings of the
            // same member disagreed.
            auto value_result = codegen_class_value_for_boundary(expr, target.type, /*allow_implicit_converting_ctor=*/true);
            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
            create_store(std::move(value_result).value(), target.ptr, target.alignment);
            if (class_has_ordinary_vtable(target.type.name)) {
                if (auto r = initialize_ordinary_vtable_pointer(target.type.name, target.ptr); !r.has_value())
                    return std::unexpected(std::move(r).error());
            }
            return {};
        }
        auto init_value_result = codegen_value_for_target(expr, target.type);
        if (!init_value_result.has_value()) return std::unexpected(std::move(init_value_result).error());
        llvm::LLVMValueRef init_value = std::move(init_value_result).value();
        auto target_llvm_type_result = to_llvm_type(target.type);
        if (!target_llvm_type_result.has_value()) return std::unexpected(std::move(target_llvm_type_result).error());
        if (auto r = check_store_type(init_value, std::move(target_llvm_type_result).value(), "member initializer"); !r.has_value())
            return std::unexpected(std::move(r).error());
        create_store(init_value, target.ptr, target.alignment);
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::initialize_storage_from_brace_args(const Codegen::LValue& target, const std::vector<ExprPtr>& args)
{
        if (target.type.kind == TypeKind::Reference) {
            if (args.size() != 1) {
                return std::unexpected(CodegenError("a reference member must be initialized with exactly one expression", current_loc_));
            }
            return initialize_reference_storage(target, *args[0]);
        }
        if (target.type.kind == TypeKind::Span) {
            if (args.size() != 1) {
                return std::unexpected(CodegenError("a span member must be initialized with exactly one array expression", current_loc_));
            }
            return initialize_span_storage(target, *args[0]);
        }
        if (target.type.kind == TypeKind::Named && find_class_def(target.type.name) != nullptr) {
            if (auto r = zero_initialize_storage(target.ptr, target.type, target.alignment); !r.has_value())
                return std::unexpected(std::move(r).error());
            auto same_type_source_result = try_initialize_class_storage_from_same_type_source(target, args);
            if (!same_type_source_result.has_value()) return std::unexpected(std::move(same_type_source_result).error());
            if (std::move(same_type_source_result).value()) return {};
            const Function* ctor_def = resolve_overload_by_type(target.type.name + "_new", args, /*param_offset=*/1);
            if (ctor_def == nullptr) {
                const ClassDef* class_def = find_class_def(target.type.name);
                if (args.empty() && class_def != nullptr && !class_has_any_constructor(target.type.name)) {
                    return emit_default_initializers_for_class_storage(target.ptr, *class_def, /*initialize_virtual_interface_bases=*/true);
                }
                return std::unexpected(CodegenError("class '" + target.type.name + "' has no constructor matching this call", current_loc_));
            }
            if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                auto value_result = codegen_constructed_class_value(target.type.name, args, ctor_def);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                create_store(std::move(value_result).value(), target.ptr, target.alignment);
                if (class_has_ordinary_vtable(target.type.name)) {
                    if (auto r = initialize_ordinary_vtable_pointer(target.type.name, target.ptr); !r.has_value())
                        return std::unexpected(std::move(r).error());
                }
                return {};
            }
            llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
            if (ctor == nullptr) {
                return std::unexpected(CodegenError("class '" + target.type.name + "' has no constructor matching this call", current_loc_));
            }
            auto ctor_args_result =
                emit_constructor_arguments_and_virtual_bases(target.type.name, ctor_def, args, target.ptr);
            if (!ctor_args_result.has_value()) return std::unexpected(std::move(ctor_args_result).error());
            std::vector<llvm::LLVMValueRef> ctor_args = std::move(ctor_args_result).value();
            ctor_args.insert(ctor_args.begin(), target.ptr);
            build_call(ctor, ctor_args);
            return {};
        }
        if (args.empty()) {
            // An array of class type is not "trivially" initialized by a
            // zero fill: each element is an object of the element type
            // and gets exactly the same value-initialization any other
            // object of that type would -- constructor, default member
            // initializers, vtable pointer and all. Recursing into this
            // same function per element is what makes that literally the
            // same logic rather than a second, drifting copy of it.
            if (target.type.kind == TypeKind::Array && target.type.element != nullptr &&
                type_needs_nontrivial_default_init(target.type)) {
                return emit_array_element_loop(
                    target.type, target.ptr, /*reverse=*/false,
                    [&](llvm::LLVMValueRef element_ptr, llvm::LLVMValueRef) -> std::expected<void, CodegenError> {
                        return initialize_storage_from_brace_args(
                            LValue{element_ptr, *target.type.element, alignment_for_type(*target.type.element)}, {});
                    });
            }
            return zero_initialize_storage(target.ptr, target.type, target.alignment);
        }
        if (args.size() != 1) {
            return std::unexpected(CodegenError("brace-initialization of this member requires exactly one expression", current_loc_));
        }
        return initialize_storage_from_expr(target, *args[0]);
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::initialize_storage(const Codegen::LValue& target, const Initializer& init)
{
        if (init.has_brace_args) {
            return initialize_storage_from_brace_args(target, init.brace_args);
        }
        if (init.expr) {
            return initialize_storage_from_expr(target, *init.expr);
        }
        return zero_initialize_storage(target.ptr, target.type, target.alignment);
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_class_value_for_boundary(const Expr& expr, const Type& target_type,
                                                  bool allow_implicit_converting_ctor)
{
        auto llvm_type_result = to_llvm_type(target_type);
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
        bool allow_hidden_helper_copy =
            current_function_def_ != nullptr && current_function_def_->is_compile_time_dependency;
        if (is_bare_same_type_copy_source(expr, target_type) &&
            (allow_hidden_helper_copy || is_copy_constructible(target_type.name))) {
            llvm::LLVMValueRef temp = create_entry_block_alloca(llvm_type, "classtransport");
            auto source_lvalue_result = codegen_lvalue(expr);
            if (!source_lvalue_result.has_value()) return std::unexpected(std::move(source_lvalue_result).error());
            if (auto r = codegen_copy_construct_class(temp, std::move(source_lvalue_result).value().ptr, target_type.name); !r.has_value())
                return std::unexpected(std::move(r).error());
            return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "classtransport.value");
        }
        if (expr.kind == ExprKind::Move) {
            std::optional<Type> moved_source_type = infer_type(*expr.lhs);
            if (moved_source_type.has_value()) {
                Type source_value_type =
                    moved_source_type->kind == TypeKind::Reference && moved_source_type->pointee != nullptr
                        ? *moved_source_type->pointee
                        : *moved_source_type;
                if (types_equal(source_value_type, target_type)) {
                    return codegen_expr(expr);
                }
            }
        }
        if (expr.kind == ExprKind::Lambda) {
            auto temp_result = codegen_expr(expr);
            if (!temp_result.has_value()) return std::unexpected(std::move(temp_result).error());
            return llvm::LLVMBuildLoad2(builder_, llvm_type, std::move(temp_result).value(), "classtransport.lambda");
        }
        if (produces_rvalue_of_type(expr, target_type) &&
            !(expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref)) {
            return codegen_expr(expr);
        }
        if (allow_implicit_converting_ctor) {
            if (const Function* converting_ctor = resolve_converting_constructor_by_type(target_type.name, expr);
                converting_ctor != nullptr) {
                std::vector<ExprPtr> ctor_args;
                ctor_args.push_back(deep_clone_expr(expr));
                return codegen_constructed_class_value(target_type.name, ctor_args, converting_ctor);
            }
        }
        return codegen_expr(expr);
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_interface_value_for_target(const Expr& expr, const Type& target_type)
{
        std::optional<Type> source_type = infer_type(expr);
        if (!source_type.has_value()) {
            return std::unexpected(CodegenError("cannot determine interface conversion source type", current_loc_));
        }
        if (types_equal(*source_type, target_type)) return codegen_expr(expr);
        if (target_type.kind == TypeKind::Reference && target_type.pointee != nullptr &&
            target_type.pointee->kind == TypeKind::Named) {
            if (source_type->kind == TypeKind::Named) {
                if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr) {
                    std::optional<Type> operand_type = infer_type(*expr.lhs);
                    if (operand_type.has_value() && is_interface_pointer_type(*operand_type)) return codegen_expr(expr);
                }
                auto object_lvalue_result = codegen_lvalue(expr);
                if (!object_lvalue_result.has_value()) return std::unexpected(std::move(object_lvalue_result).error());
                llvm::LLVMValueRef object_ptr = std::move(object_lvalue_result).value().ptr;
                auto table_ptr_result = get_or_create_interface_dispatch_table(source_type->name, target_type.pointee->name);
                if (!table_ptr_result.has_value()) return std::unexpected(std::move(table_ptr_result).error());
                return build_interface_value(object_ptr, std::move(table_ptr_result).value());
            }
            if (source_type->kind == TypeKind::Reference && source_type->pointee != nullptr &&
                source_type->pointee->kind == TypeKind::Named && !type_names_interface(source_type->pointee->name)) {
                auto object_lvalue_result = codegen_lvalue(expr);
                if (!object_lvalue_result.has_value()) return std::unexpected(std::move(object_lvalue_result).error());
                llvm::LLVMValueRef object_ptr = std::move(object_lvalue_result).value().ptr;
                auto table_ptr_result = get_or_create_interface_dispatch_table(source_type->pointee->name, target_type.pointee->name);
                if (!table_ptr_result.has_value()) return std::unexpected(std::move(table_ptr_result).error());
                return build_interface_value(object_ptr, std::move(table_ptr_result).value());
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
                auto object_ptr_result = codegen_expr(expr);
                if (!object_ptr_result.has_value()) return std::unexpected(std::move(object_ptr_result).error());
                llvm::LLVMValueRef object_ptr = std::move(object_ptr_result).value();
                auto table_ptr_result = get_or_create_interface_dispatch_table(source_type->pointee->name, target_type.pointee->name);
                if (!table_ptr_result.has_value()) return std::unexpected(std::move(table_ptr_result).error());
                return build_interface_value(object_ptr, std::move(table_ptr_result).value());
            }
        }
        if (source_type->kind == TypeKind::Reference && source_type->pointee != nullptr &&
            target_type.kind == TypeKind::Reference && target_type.pointee != nullptr &&
            types_equal(*source_type->pointee, *target_type.pointee)) {
            return codegen_expr(expr);
        }
        return std::unexpected(CodegenError("unsupported interface conversion at code generation time", current_loc_));
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_span_value_for_target(const Expr& expr, const Type& target_type)
{
        if (target_type.kind != TypeKind::Span || target_type.pointee == nullptr) {
            return std::unexpected(CodegenError("internal error: span conversion target is not a span", current_loc_));
        }
        if (std::optional<Type> source_type = infer_type(expr); source_type.has_value() && types_equal(*source_type, target_type)) {
            return codegen_expr(expr);
        }
        auto source_result = codegen_lvalue(expr);
        if (!source_result.has_value()) return std::unexpected(std::move(source_result).error());
        LValue source = std::move(source_result).value();
        if (source.type.kind != TypeKind::Array) {
            return std::unexpected(CodegenError("std::span<T> can currently only be constructed from a fixed-size array in this version",
                               current_loc_));
        }
        auto element_llvm_type_result = to_llvm_type(*source.type.element);
        if (!element_llvm_type_result.has_value()) return std::unexpected(std::move(element_llvm_type_result).error());
        auto pointee_llvm_type_result = to_llvm_type(*target_type.pointee);
        if (!pointee_llvm_type_result.has_value()) return std::unexpected(std::move(pointee_llvm_type_result).error());
        if (std::move(element_llvm_type_result).value() != std::move(pointee_llvm_type_result).value()) {
            return std::unexpected(CodegenError("array element type does not match the span's element type", current_loc_));
        }
        auto span_type_result = to_llvm_type(target_type);
        if (!span_type_result.has_value()) return std::unexpected(std::move(span_type_result).error());
        llvm::LLVMTypeRef span_type = std::move(span_type_result).value();
        llvm::LLVMValueRef size_value = llvm::LLVMConstInt(llvm::LLVMInt64TypeInContext(context_), static_cast<std::uint64_t>(source.type.array_size), 0);
        llvm::LLVMValueRef span_value = llvm::LLVMGetUndef(span_type);
        span_value = llvm::LLVMBuildInsertValue(builder_, span_value, source.ptr, 0, "");
        span_value = llvm::LLVMBuildInsertValue(builder_, span_value, size_value, 1, "");
        return span_value;
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_contextual_bool_value(const Expr& expr)
{
        std::optional<Type> expr_type = infer_type(expr);
        if (expr_type.has_value() && is_interface_pointer_type(*expr_type)) {
            auto interface_value_result = codegen_expr(expr);
            if (!interface_value_result.has_value()) return std::unexpected(std::move(interface_value_result).error());
            llvm::LLVMValueRef interface_value = std::move(interface_value_result).value();
            llvm::LLVMValueRef object_ptr = extract_interface_object_ptr(interface_value);
            return i1_to_bool(llvm::LLVMBuildICmp(builder_, llvm::LLVMIntNE,
                object_ptr, llvm::LLVMConstPointerNull(llvm::LLVMPointerTypeInContext(context_, 0)), "ifacenotnull"));
        }
        return codegen_expr(expr);
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_contextual_bool_i1(const Expr& expr)
{
        auto value_result = codegen_contextual_bool_value(expr);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        return bool_to_i1(std::move(value_result).value());
    }


    [[nodiscard]] std::expected<std::vector<llvm::LLVMValueRef>, CodegenError> Codegen::codegen_call_args(const std::vector<ExprPtr>& args, const Function* callee_def,
                                                  std::size_t param_offset)
{
        std::vector<llvm::LLVMValueRef> result;
        auto emit_arg = [&](const Expr& arg, std::size_t i) -> std::expected<llvm::LLVMValueRef, CodegenError> {
            Type effective_param_type;
            bool have_effective_param_type = false;
            bool collapsed_forwarding_reference_value = false;
            if (callee_def != nullptr && i + param_offset < callee_def->params.size()) {
                effective_param_type = callee_def->params[i + param_offset].type;
                if (callee_def->is_generic_template &&
                    effective_param_type.kind == TypeKind::Named && !effective_param_type.name.empty() &&
                    effective_param_type.template_args.empty()) {
                    if (std::optional<Type> inferred = infer_type(arg); inferred.has_value()) {
                        effective_param_type = *inferred;
                    }
                }
                if (!callee_def->is_generic_template && effective_param_type.kind == TypeKind::Reference &&
                    effective_param_type.is_rvalue_ref && effective_param_type.pointee != nullptr &&
                    param_offset > 0 && callee_def->member_owner_class.empty() &&
                    produces_rvalue_of_type(arg, *effective_param_type.pointee)) {
                    effective_param_type = *effective_param_type.pointee;
                    collapsed_forwarding_reference_value = true;
                }
                have_effective_param_type = true;
            }
            bool param_is_reference = have_effective_param_type && effective_param_type.kind == TypeKind::Reference;
            const Type* ref_param_type = param_is_reference ? &effective_param_type : nullptr;
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
                return codegen_interface_value_for_target(arg, *ref_param_type);
            } else if (param_is_rvalue_reference || param_is_const_reference_bound_to_rvalue) {
                // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- the move
                // checker has already verified this argument produces a
                // genuine rvalue (produces_rvalue_of_type), which may not
                // itself be an addressable place (a literal, a fresh
                // std::make_unique<T>(...)/call result, ...).
                return param_is_rvalue_reference ? codegen_materialize_rvalue_reference_source(arg)
                                                  : codegen_materialize_const_reference_source(
                                                        arg, *ref_param_type->pointee);
            } else if (param_is_reference && !collapsed_forwarding_reference_value) {
                // Bind the reference parameter to the argument's address
                // rather than passing its value, exactly like a local
                // reference's own VarDecl.
                auto lvalue_result = codegen_lvalue(arg);
                if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                return std::move(lvalue_result).value().ptr;
            } else {
                // ch06 §6: a bare literal argument adapts directly to
                // its target parameter's own declared scalar type (see
                // codegen_value_for_target) -- exactly like a VarDecl
                // initializer/plain assignment's identical treatment,
                // rather than defaulting to `int`/`double` and failing
                // the callee's own parameter-type check.
                if (have_effective_param_type) {
                    const Type& param_type = effective_param_type;
                    if (is_named_record_type(param_type)) {
                        return codegen_class_value_for_boundary(arg, param_type,
                                                                         /*allow_implicit_converting_ctor=*/true);
                    } else {
                        return codegen_value_for_target(arg, param_type);
                    }
                } else {
                    return codegen_expr(arg);
                }
            }
        };
        result.reserve(args.size());
        for (std::size_t i = 0; i < args.size(); i++) {
            auto arg_result = emit_arg(*args[i], i);
            if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
            result.push_back(std::move(arg_result).value());
        }
        if (callee_def != nullptr) {
            for (std::size_t i = args.size(); i + param_offset < callee_def->params.size(); i++) {
                const Param& param = callee_def->params[i + param_offset];
                if (param.default_expr == nullptr) break;
                ExprPtr default_arg = deep_clone_expr_with_loc(*param.default_expr, current_loc_);
                auto default_arg_result = emit_arg(*default_arg, i);
                if (!default_arg_result.has_value()) return std::unexpected(std::move(default_arg_result).error());
                result.push_back(std::move(default_arg_result).value());
            }
        }
        return result;
    }


    [[nodiscard]] std::expected<std::vector<llvm::LLVMValueRef>, CodegenError> Codegen::codegen_call_args_for_types(const std::vector<ExprPtr>& args,
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
                auto value_result = codegen_interface_value_for_target(*args[i], *ref_param_type);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                result.push_back(std::move(value_result).value());
            } else if (param_is_rvalue_reference || param_is_const_reference_bound_to_rvalue) {
                auto value_result = param_is_rvalue_reference ? codegen_materialize_rvalue_reference_source(*args[i])
                                                           : codegen_materialize_const_reference_source(
                                                                 *args[i], *ref_param_type->pointee);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                result.push_back(std::move(value_result).value());
            } else if (param_is_reference) {
                auto lvalue_result = codegen_lvalue(*args[i]);
                if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                result.push_back(std::move(lvalue_result).value().ptr);
            } else if (i < param_types.size()) {
                if (is_named_record_type(param_types[i])) {
                    auto value_result = codegen_class_value_for_boundary(*args[i], param_types[i],
                                                                     /*allow_implicit_converting_ctor=*/true);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    result.push_back(std::move(value_result).value());
                } else {
                    auto value_result = codegen_value_for_target(*args[i], param_types[i]);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    result.push_back(std::move(value_result).value());
                }
            } else {
                auto value_result = codegen_expr(*args[i]);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                result.push_back(std::move(value_result).value());
            }
        }
        return result;
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::load_value(const Codegen::LValue& lv)
{
        if (lv.type.kind == TypeKind::Array) {
            return lv.ptr;
        }
        auto llvm_type_result = to_llvm_type(lv.type);
        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
        return create_load(std::move(llvm_type_result).value(), lv.ptr, lv.alignment, "loadtmp");
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::bool_to_i1(llvm::LLVMValueRef v)
{
        if (auto r = require_bool_representation(v); !r.has_value()) return std::unexpected(std::move(r).error());
        return llvm::LLVMBuildTrunc(builder_, v, llvm::LLVMInt1TypeInContext(context_), "tobool");
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::require_bool_representation(llvm::LLVMValueRef v)
{
        if (!(llvm::LLVMGetTypeKind(llvm::LLVMTypeOf(v)) == llvm::LLVMIntegerTypeKind && llvm::LLVMGetIntTypeWidth(llvm::LLVMTypeOf(v)) == 8)) {
            return std::unexpected(CodegenError(
                "expected a 'bool' value here (e.g. an if/while condition, or an '&&'/'||' operand); "
                "scpp requires an explicit cast for any scalar-to-bool conversion, unlike real C++ "
                "(spec ch06)",
                current_loc_));
        }
        return {};
    }


    llvm::LLVMValueRef Codegen::i1_to_bool(llvm::LLVMValueRef v)
{
        return llvm::LLVMBuildZExt(builder_, v, llvm::LLVMInt8TypeInContext(context_), "boolext");
    }


    [[nodiscard]] std::expected<bool, CodegenError> Codegen::enum_value_fits_source_type(const Type& source_type, long long enum_value)
{
        if (source_type.kind != TypeKind::Named || !is_integral_scalar_type_name(source_type.name)) return false;
        auto integer_type_result = to_llvm_type(source_type);
        if (!integer_type_result.has_value()) return std::unexpected(std::move(integer_type_result).error());
        llvm::LLVMTypeRef integer_type = std::move(integer_type_result).value();
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


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::build_integral_enum_match(llvm::LLVMValueRef source, const Type& source_type, long long enum_value)
{
        llvm::LLVMTypeRef source_integer_type = llvm::LLVMTypeOf(source);
        auto fits_result = enum_value_fits_source_type(source_type, enum_value);
        if (!fits_result.has_value()) return std::unexpected(std::move(fits_result).error());
        if (llvm::LLVMGetTypeKind(source_integer_type) != llvm::LLVMIntegerTypeKind || !std::move(fits_result).value()) {
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


    [[nodiscard]] std::expected<Codegen::CallResult, CodegenError> Codegen::codegen_enum_cast_store_builtin(const Expr& expr, const Function& callee_def)
{
        if (expr.args.size() != 2 || callee_def.params.size() != 2) {
            return std::unexpected(CodegenError("internal error: malformed scpp::__enum_cast_store call", current_loc_));
        }
        const Type& source_type = callee_def.params[0].type;
        const Type& out_param_type = callee_def.params[1].type;
        if (source_type.kind != TypeKind::Named || !is_integral_scalar_type_name(source_type.name)) {
            return std::unexpected(CodegenError("scpp::enum_cast<T>(value) requires an integral source value", current_loc_));
        }
        if (out_param_type.kind != TypeKind::Reference || out_param_type.pointee == nullptr ||
            out_param_type.pointee->kind != TypeKind::Named) {
            return std::unexpected(CodegenError("scpp::enum_cast<T>(value) requires T to be an enum class", current_loc_));
        }
        const EnumDef* enum_def = find_enum_def(program_, out_param_type.pointee->name);
        if (enum_def == nullptr) {
            return std::unexpected(CodegenError("scpp::enum_cast<T>(value) requires T to be an enum class", current_loc_));
        }

        auto source_value_result = codegen_value_for_target(*expr.args[0], source_type);
        if (!source_value_result.has_value()) return std::unexpected(std::move(source_value_result).error());
        llvm::LLVMValueRef source_value = std::move(source_value_result).value();
        auto out_result = codegen_lvalue(*expr.args[1]);
        if (!out_result.has_value()) return std::unexpected(std::move(out_result).error());
        LValue out = std::move(out_result).value();
        auto enum_storage_type_result = to_llvm_type(*out_param_type.pointee);
        if (!enum_storage_type_result.has_value()) return std::unexpected(std::move(enum_storage_type_result).error());
        llvm::LLVMTypeRef enum_storage_type = std::move(enum_storage_type_result).value();
        llvm::LLVMValueRef matched = llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0);
        llvm::LLVMValueRef selected =
            enum_variant_constant(enum_storage_type, enum_def->underlying_type, 0);
        for (const EnumVariant& variant : enum_def->variants) {
            auto variant_matches_result = build_integral_enum_match(source_value, source_type, variant.value);
            if (!variant_matches_result.has_value()) return std::unexpected(std::move(variant_matches_result).error());
            llvm::LLVMValueRef variant_matches = std::move(variant_matches_result).value();
            matched = llvm::LLVMBuildOr(builder_, matched, variant_matches, "enumcastmatch");
            selected = llvm::LLVMBuildSelect(builder_,
                variant_matches, enum_variant_constant(enum_storage_type, enum_def->underlying_type, variant.value), selected,
                "enumcastselect");
        }
        create_store(selected, out.ptr, out.alignment);
        return CallResult{i1_to_bool(matched), &callee_def};
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_value_for_target(const Expr& expr, const Type& target_type)
{
        if (is_interface_representation_type(target_type)) {
            return codegen_interface_value_for_target(expr, target_type);
        }
        // A class-typed target (VarDecl initializer, plain assignment,
        // return statement, global initializer, ...) needs the same
        // "adapt a value to a class-typed boundary" treatment that
        // by-value function arguments already get in codegen_call_args/
        // codegen_call_args_for_types -- e.g. a string-literal RHS
        // assigned/initialized into a std::string-typed target must
        // route through its converting constructor rather than falling
        // through to a bare codegen_expr, which would leave the value's
        // LLVM shape mismatched against the target's.
        if (is_named_record_type(target_type)) {
            return codegen_class_value_for_boundary(expr, target_type, /*allow_implicit_converting_ctor=*/true);
        }
        if (target_type.kind == TypeKind::Pointer && expr.kind == ExprKind::Identifier && expr.name == "nullptr" &&
            !expr.explicit_global_qualification) {
            auto target_llvm_type_result = to_llvm_type(target_type);
            if (!target_llvm_type_result.has_value()) return std::unexpected(std::move(target_llvm_type_result).error());
            return llvm::LLVMConstNull(std::move(target_llvm_type_result).value());
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
                    auto target_llvm_type_result = to_llvm_type(target_type);
                    if (!target_llvm_type_result.has_value()) return std::unexpected(std::move(target_llvm_type_result).error());
                    return llvm::LLVMConstReal(std::move(target_llvm_type_result).value(), static_cast<double>(expr.int_value));
                }
                if (target_type.name != "bool" && target_type.name != "char") {
                    auto target_llvm_type_result = to_llvm_type(target_type);
                    if (!target_llvm_type_result.has_value()) return std::unexpected(std::move(target_llvm_type_result).error());
                    return llvm::LLVMConstInt(std::move(target_llvm_type_result).value(), static_cast<std::uint64_t>(expr.int_value),
                                                   /*SignExtend=*/!is_unsigned_scalar_type_name(target_type.name));
                }
            } else if (expr.kind == ExprKind::FloatLiteral && is_float_scalar_type_name(target_type.name)) {
                auto target_llvm_type_result = to_llvm_type(target_type);
                if (!target_llvm_type_result.has_value()) return std::unexpected(std::move(target_llvm_type_result).error());
                return llvm::LLVMConstReal(std::move(target_llvm_type_result).value(), expr.float_value);
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


    [[nodiscard]] std::expected<void, CodegenError> Codegen::check_store_type(llvm::LLVMValueRef value, llvm::LLVMTypeRef expected, const std::string& what)
{
        if (llvm::LLVMTypeOf(value) != expected) {
            return std::unexpected(CodegenError("type mismatch initializing/assigning " + what +
                                ": scpp has no implicit conversion between distinct scalar types (e.g. "
                                "bool/char/int are all distinct, spec ch06) -- use an explicit "
                                "'static_cast<T>(...)' if the conversion is intended",
                current_loc_));
        }
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::check_return_type(llvm::LLVMTypeRef actual, const Stmt& stmt)
{
        // The return boundary's counterpart of check_store_type above.
        // Every other boundary that hands a value to a declared type
        // already answers "does this value have exactly that type": an
        // initializer and an assignment through check_store_type, a call
        // argument through overload resolution's exact-type match (spec
        // ch05.10). `return` answered it nowhere at all, so an `int`
        // returned from a `std::int64_t` function -- or a class value
        // returned from an `int` function -- was lowered verbatim and
        // only caught by LLVM's own module verifier, which runs once
        // over the finished module and could no longer say where the
        // offending statement was.
        //
        // Compared here at the *llvm::LLVM* type, exactly as check_store_type
        // does, and for the same reason: this is the point where the
        // value being returned actually has a known type. movecheck's
        // infer_expr_type deliberately gives up on Member/Subscript
        // chains (it has no Program-level field-type information), which
        // is precisely the shape that motivated this check --
        // `return value->tag;` -- so an earlier-phase check could not
        // have caught it without guessing.
        llvm::LLVMBasicBlockRef block = llvm::LLVMGetInsertBlock(builder_);
        if (block == nullptr) return {};
        llvm::LLVMValueRef llvm_fn = llvm::LLVMGetBasicBlockParent(block);
        if (llvm_fn == nullptr) return {};
        llvm::LLVMTypeRef expected = llvm::LLVMGetReturnType(llvm::LLVMGlobalGetValueType(llvm_fn));
        llvm::LLVMTypeRef void_type = llvm::LLVMVoidTypeInContext(context_);
        if (actual == expected) return {};

        SourceLocation loc = stmt.expr != nullptr ? stmt.expr->loc : stmt.loc;
        std::string declared = current_function_def_ != nullptr
                                   ? "'" + verbatim_type_spelling(current_function_def_->return_type) + "'"
                                   : std::string("its declared return type");
        std::string in_function =
            current_function_def_ != nullptr ? " from '" + current_function_def_->name + "'" : std::string();
        if (stmt.expr == nullptr) {
            return std::unexpected(
                CodegenError("cannot 'return;' without a value" + in_function + ": it returns " + declared, loc));
        }
        if (expected == void_type) {
            return std::unexpected(CodegenError("cannot return a value" + in_function +
                                    ": it returns 'void', so only 'return;' or returning another void-typed "
                                    "expression is allowed",
                loc));
        }
        return std::unexpected(CodegenError("type mismatch returning a value" + in_function + ": it returns " +
                                declared +
                                ", and the returned expression has a different type; scpp has no implicit "
                                "conversion between distinct types (e.g. int and std::int64_t are distinct, "
                                "spec ch06) -- use an explicit 'static_cast<T>(...)' if the conversion is intended",
            loc));
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_expr(const Expr& expr)
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

            case ExprKind::ValueInit:
                // `return {};` (ast.cppm's ExprKind::ValueInit) --
                // `expr.type` was already stamped in by monomorphization
                // (Monomorphizer::walk_expr) from the enclosing
                // function's own return type. A class/struct type is
                // constructed exactly like an empty-braced `Type
                // var{};` VarDecl (resolve its own zero-argument
                // constructor, if any, else fall back to in-class field
                // initializers -- see codegen_constructed_class_value
                // and initialize_storage_from_brace_args' identical
                // class-case handling); anything else (a scalar,
                // pointer, ...) is simply zero-initialized.
                if (expr.type.kind == TypeKind::Named && find_class_def(expr.type.name) != nullptr) {
                    std::vector<ExprPtr> no_args;
                    const Function* ctor_def = resolve_overload_by_type(expr.type.name + "_new", no_args, /*param_offset=*/1);
                    return codegen_constructed_class_value(expr.type.name, no_args, ctor_def, &expr);
                }
                {
                    auto llvm_type_result = to_llvm_type(expr.type);
                    if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
                    llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
                    std::optional<unsigned> align = alignment_for_type(expr.type);
                    llvm::LLVMValueRef temp = create_entry_block_alloca(llvm_type, "valueinittmp", align);
                    if (auto r = zero_initialize_storage(temp, expr.type, align); !r.has_value()) return std::unexpected(std::move(r).error());
                    return llvm::LLVMBuildLoad2(builder_, llvm_type, temp, "valueinit.value");
                }

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
                // ch05/ch06: the conditional yields a *value*, so each arm
                // is generated against the one type both agree on (see
                // movecheck's conditional_arm_types_agree, which decides
                // the very same question): an untyped literal arm is
                // materialized at the other arm's width, and a scalar
                // lvalue arm (e.g. a `const T&`-returning call) is read
                // through as an ordinary value. Falls back to plain
                // codegen_expr whenever neither arm has an inferable type.
                auto arm_value_type = [&](const Expr& arm) -> std::optional<Type> {
                    std::optional<Type> arm_type = infer_type(arm);
                    if (!arm_type.has_value()) return std::nullopt;
                    if (arm_type->kind == TypeKind::Reference && arm_type->pointee != nullptr) return *arm_type->pointee;
                    return arm_type;
                };
                std::optional<Type> then_type = arm_value_type(*expr.rhs);
                std::optional<Type> else_type = arm_value_type(*expr.third);
                auto is_untyped_numeric_literal = [](const Expr& arm) {
                    const Expr& literal = arm.kind == ExprKind::Unary && arm.unary_op == UnaryOp::Neg && arm.lhs != nullptr
                                              ? *arm.lhs
                                              : arm;
                    return literal.kind == ExprKind::IntegerLiteral || literal.kind == ExprKind::FloatLiteral;
                };
                auto is_scalar_target = [](const std::optional<Type>& type) {
                    return type.has_value() && type->kind == TypeKind::Named && is_scalar_type_name(type->name);
                };
                std::optional<Type> common_type;
                if (then_type.has_value() && else_type.has_value() && types_equal(*then_type, *else_type)) {
                    common_type = then_type;
                } else if (is_scalar_target(then_type) && is_untyped_numeric_literal(*expr.third)) {
                    common_type = then_type;
                } else if (is_scalar_target(else_type) && is_untyped_numeric_literal(*expr.rhs)) {
                    common_type = else_type;
                }
                auto codegen_arm = [&](const Expr& arm) -> std::expected<llvm::LLVMValueRef, CodegenError> {
                    return common_type.has_value() ? codegen_value_for_target(arm, *common_type) : codegen_expr(arm);
                };

                auto cond_result = codegen_contextual_bool_i1(*expr.lhs);
                if (!cond_result.has_value()) return std::unexpected(std::move(cond_result).error());
                llvm::LLVMValueRef cond = std::move(cond_result).value();
                llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
                llvm::LLVMBasicBlockRef then_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.then");
                llvm::LLVMBasicBlockRef else_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.else");
                llvm::LLVMBasicBlockRef merge_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.end");
                llvm::LLVMBuildCondBr(builder_, cond, then_block, else_block);

                llvm::LLVMPositionBuilderAtEnd(builder_, then_block);
                auto then_value_result = codegen_arm(*expr.rhs);
                if (!then_value_result.has_value()) return std::unexpected(std::move(then_value_result).error());
                llvm::LLVMValueRef then_value = std::move(then_value_result).value();
                llvm::LLVMBuildBr(builder_, merge_block);
                llvm::LLVMBasicBlockRef then_end = llvm::LLVMGetInsertBlock(builder_);

                llvm::LLVMPositionBuilderAtEnd(builder_, else_block);
                auto else_value_result = codegen_arm(*expr.third);
                if (!else_value_result.has_value()) return std::unexpected(std::move(else_value_result).error());
                llvm::LLVMValueRef else_value = std::move(else_value_result).value();
                llvm::LLVMBuildBr(builder_, merge_block);
                llvm::LLVMBasicBlockRef else_end = llvm::LLVMGetInsertBlock(builder_);

                llvm::LLVMPositionBuilderAtEnd(builder_, merge_block);
                if (llvm::LLVMTypeOf(then_value) != llvm::LLVMTypeOf(else_value)) {
                    return std::unexpected(CodegenError("conditional operator requires both arms to have the same type", current_loc_));
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
                    return std::unexpected(CodegenError("cast operand has no inferable type", current_loc_));
                }
                if (is_interface_representation_type(*source_type) || is_interface_representation_type(expr.type)) {
                    return std::unexpected(CodegenError("casts involving interface-typed pointers or references are not supported",
                                       current_loc_));
                }
                // A reference-returning call/field (e.g. `std::string_view::
                // at`'s `const char&`) is just as castable as the plain value
                // it refers to -- unwrap via binary_operand_type (the same
                // helper every binary-operator codegen path already uses)
                // so the checks/codegen below see the referent's own type,
                // not "is a Reference" itself (movecheck's dataflow.cppm
                // Cast case unwraps identically, for the same reason).
                const Type& source_operand = binary_operand_type(*source_type);
                if (source_operand.kind == TypeKind::Pointer && expr.type.kind == TypeKind::Pointer) {
                    return codegen_value_for_target(*expr.lhs, source_operand);
                }
                if (source_operand.kind != TypeKind::Named || expr.type.kind != TypeKind::Named) {
                    return std::unexpected(CodegenError("cast is only supported between scalar types or raw pointer types in this version",
                                       current_loc_));
                }
                if (is_integral_scalar_type_name(source_operand.name) && find_enum_def(program_, expr.type.name) != nullptr) {
                    return std::unexpected(CodegenError("cannot cast an integer value to enum class '" + expr.type.name +
                                           "'; use scpp::enum_cast<" + expr.type.name + ">(value) instead",
                                       current_loc_));
                }
                bool source_is_scalar_or_enum =
                    is_scalar_type_name(source_operand.name) || find_enum_def(program_, source_operand.name) != nullptr;
                bool target_is_scalar_or_enum =
                    is_scalar_type_name(expr.type.name) || find_enum_def(program_, expr.type.name) != nullptr;
                if (!source_is_scalar_or_enum || !target_is_scalar_or_enum) {
                    return std::unexpected(CodegenError(
                        "cast is only supported between builtin scalar types or between an enum class and its "
                        "underlying integer type in this version",
                        current_loc_));
                }
                auto operand_result = codegen_value_for_target(*expr.lhs, source_operand);
                if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                return codegen_scalar_cast(std::move(operand_result).value(), source_operand, expr.type);
            }

            case ExprKind::Identifier: {
                if (find_local(expr) == nullptr) {
                    if (find_visible_global_slot(expr.name, expr.explicit_global_qualification) != nullptr) {
                        auto lv_result = codegen_lvalue(expr);
                        if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                        return load_value(std::move(lv_result).value());
                    }
                    const EnumDef* enum_def = nullptr;
                    const EnumVariant* enum_variant = find_enum_variant(program_, expr.name, &enum_def);
                    if (enum_variant != nullptr) {
                        auto llvm_type_result = to_llvm_type(named_type(enum_def->name));
                        if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
                        return llvm::LLVMConstInt(std::move(llvm_type_result).value(), static_cast<std::uint64_t>(enum_variant->value),
                                                      /*SignExtend=*/!is_unsigned_scalar_type_name(
                                                          enum_def->underlying_type.name));
                    }
                    if (std::optional<Type> fn_type = resolve_function_designator_type(expr)) {
                        if (llvm::LLVMValueRef fn = codegen_function_pointer_value_for_target(expr, *fn_type)) return fn;
                    }
                    if (expr.explicit_global_qualification) {
                        return std::unexpected(CodegenError("use of undeclared global name '" + expr.name + "'", current_loc_));
                    }
                }
                auto lv_result = codegen_lvalue(expr);
                if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                return load_value(std::move(lv_result).value());
            }

            case ExprKind::Subscript: {
                auto lv_result = codegen_lvalue(expr);
                if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                return load_value(std::move(lv_result).value());
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
                auto base_result = codegen_lvalue(*expr.lhs);
                if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
                LValue base = std::move(base_result).value();
                if (base.type.kind == TypeKind::Span && expr.name == "size") {
                    auto base_llvm_type_result = to_llvm_type(base.type);
                    if (!base_llvm_type_result.has_value()) return std::unexpected(std::move(base_llvm_type_result).error());
                    llvm::LLVMValueRef size_ptr = llvm::LLVMBuildStructGEP2(builder_, std::move(base_llvm_type_result).value(), base.ptr, 1, "sizeptr");
                    llvm::LLVMValueRef size64 = llvm::LLVMBuildLoad2(builder_, llvm::LLVMInt64TypeInContext(context_), size_ptr, "size64");
                    return llvm::LLVMBuildTrunc(builder_, size64, llvm::LLVMInt32TypeInContext(context_), "size");
                }
                auto lv_result = codegen_lvalue(expr);
                if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                return load_value(std::move(lv_result).value());
            }

            case ExprKind::Unary: {
                if (expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PreDec) {
                    auto lv_result = codegen_lvalue(expr);
                    if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                    return load_value(std::move(lv_result).value());
                }
                if (expr.unary_op == UnaryOp::PostInc || expr.unary_op == UnaryOp::PostDec) {
                    auto lv_result = codegen_lvalue(*expr.lhs);
                    if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                    LValue lv = std::move(lv_result).value();
                    auto old_value_result = load_value(lv);
                    if (!old_value_result.has_value()) return std::unexpected(std::move(old_value_result).error());
                    llvm::LLVMValueRef old_value = std::move(old_value_result).value();
                    bool is_float = lv.type.kind == TypeKind::Named && is_float_scalar_type_name(lv.type.name);
                    auto llvm_type_result = to_llvm_type(lv.type);
                    if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
                    llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
                    llvm::LLVMValueRef one = is_float ? llvm::LLVMConstReal(llvm_type, 1.0)
                                                      : llvm::LLVMConstInt(llvm_type, 1, 0);
                    llvm::LLVMValueRef new_value =
                        expr.unary_op == UnaryOp::PostInc
                            ? (is_float ? llvm::LLVMBuildFAdd(builder_, old_value, one, "postinc")
                                        : llvm::LLVMBuildAdd(builder_, old_value, one, "postinc"))
                            : (is_float ? llvm::LLVMBuildFSub(builder_, old_value, one, "postdec")
                                        : llvm::LLVMBuildSub(builder_, old_value, one, "postdec"));
                    create_store(new_value, lv.ptr, lv.alignment);
                    return old_value;
                }
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
                    auto lv_result = codegen_lvalue(expr);
                    if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                    LValue lv = std::move(lv_result).value();
                    auto llvm_type_result = to_llvm_type(lv.type);
                    if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
                    return create_load(std::move(llvm_type_result).value(), lv.ptr, lv.alignment, "loadtmp");
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
                    auto lv_result = codegen_lvalue(*expr.lhs);
                    if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                    return std::move(lv_result).value().ptr;
                }
                if (expr.unary_op == UnaryOp::Neg) {
                    auto operand_result = codegen_expr(*expr.lhs);
                    if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                    llvm::LLVMValueRef operand = std::move(operand_result).value();
                    std::optional<Type> operand_type = infer_type(*expr.lhs);
                    bool is_float = operand_type.has_value() && is_float_scalar_type_name(operand_type->name);
                    return is_float ? llvm::LLVMBuildFNeg(builder_, operand, "fnegtmp") : llvm::LLVMBuildNeg(builder_, operand, "negtmp");
                }
                auto operand_result = codegen_contextual_bool_value(*expr.lhs);
                if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                llvm::LLVMValueRef operand = std::move(operand_result).value();
                // Not (`!`) -- `operand` is a `bool` value (i8; see
                // to_llvm_type), so this goes through the i1 domain
                // rather than a raw bitwise-not directly on the i8: NOT
                // on the byte `0x01` gives `0xFE`, not the canonical
                // false=`0x00` the ch06 invariant requires (every other
                // bool-producing operation -- comparisons, `&&`/`||` --
                // is careful to only ever produce 0 or 1; this must be
                // too, or a later `== false` on the result would wrongly
                // disagree with `!` itself).
                auto i1_result = bool_to_i1(operand);
                if (!i1_result.has_value()) return std::unexpected(std::move(i1_result).error());
                return i1_to_bool(llvm::LLVMBuildNot(builder_, std::move(i1_result).value(), "nottmp"));
            }

            case ExprKind::Binary:
                return codegen_binary(expr);

            case ExprKind::Call: {
                if (is_for_range_size_builtin(expr)) {
                    std::optional<Type> range_type = infer_type(*expr.args[0]);
                    if (!range_type.has_value()) {
                        return std::unexpected(CodegenError("cannot determine range-for operand type", current_loc_));
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
                        size_expr->lhs = deep_clone_expr(*expr.args[0]);
                        size_expr->name = "size";
                        return codegen_expr(*size_expr);
                    }
                    if (unwrapped.kind == TypeKind::Named &&
                        (unwrapped.name == "std::vector" || unwrapped.name == "vector" ||
                         unwrapped.name.starts_with("std::vector.") || unwrapped.name.starts_with("vector."))) {
                        // std::vector<T>::size() returns size_t (i64), but
                        // `$for_range_size` is contractually an `int` (see
                        // its infer_type/infer_expr_type handling in
                        // semantics.cppm/calls.cppm) -- truncate to i32 to
                        // match, exactly like std::span's `.size` property
                        // just above.
                        auto size_call = std::make_unique<Expr>();
                        size_call->kind = ExprKind::Call;
                        size_call->loc = expr.loc;
                        size_call->name = "size";
                        size_call->lhs = deep_clone_expr(*expr.args[0]);
                        auto vector_size_result = codegen_expr(*size_call);
                        if (!vector_size_result.has_value()) return std::unexpected(std::move(vector_size_result).error());
                        return llvm::LLVMBuildTrunc(builder_, std::move(vector_size_result).value(), llvm::LLVMInt32TypeInContext(context_), "vecsize");
                    }
                    return std::unexpected(CodegenError("range-for requires a fixed-size array or std::span operand", current_loc_));
                }
                if (expr.name == "print_int" || expr.name == "print_bool" || expr.name == "print_char") {
                    return codegen_builtin_print(expr);
                }
                auto result_result = codegen_call(expr);
                if (!result_result.has_value()) return std::unexpected(std::move(result_result).error());
                CallResult result = std::move(result_result).value();
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
                    auto pointee_llvm_type_result = to_llvm_type(*result.callee_def->return_type.pointee);
                    if (!pointee_llvm_type_result.has_value()) return std::unexpected(std::move(pointee_llvm_type_result).error());
                    return llvm::LLVMBuildLoad2(builder_, std::move(pointee_llvm_type_result).value(), result.value,
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
                auto lv_result = codegen_lvalue(*expr.lhs);
                if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                LValue lv = std::move(lv_result).value();
                auto llvm_type_result = to_llvm_type(lv.type);
                if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
                llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
                llvm::LLVMValueRef old_value = create_load(llvm_type, lv.ptr, lv.alignment, "movetmp");
                if (auto r = zero_initialize_storage(lv.ptr, lv.type, lv.alignment); !r.has_value()) return std::unexpected(std::move(r).error());
                if (expr.lhs->kind == ExprKind::Identifier) {
                    const LocalSlot* source_local = find_local(*expr.lhs);
                    if (source_local != nullptr && source_local->moved_flag != nullptr) {
                        llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 1, 0), source_local->moved_flag);
                    }
                }
                return old_value;
            }

            case ExprKind::New:
                return codegen_new_expr(expr);

            case ExprKind::Delete:
            case ExprKind::Destroy:
                return std::unexpected(CodegenError("'delete' and explicit destructor calls are only supported as standalone statements "
                                   "in this version",
                    current_loc_));

            case ExprKind::Fold:
            case ExprKind::PackExpansion:
                return std::unexpected(CodegenError("fold expression should have been expanded before codegen",
                    current_loc_));

            case ExprKind::Lambda:
                return codegen_construct_lambda(expr);
        }
        return std::unexpected(CodegenError("unhandled expression kind",
            current_loc_));
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_construct_lambda(const Expr& expr, llvm::LLVMValueRef existing_storage)
{
        const StructInfo& info = structs_.at(expr.name);
        llvm::LLVMValueRef closure =
            existing_storage != nullptr ? existing_storage : create_entry_block_alloca(info.llvm_type, "lambdatmp");
        if (info.has_ordinary_vtable) {
            if (auto r = initialize_ordinary_vtable_pointer(expr.name, closure); !r.has_value()) return std::unexpected(std::move(r).error());
        }
        for (std::size_t i = 0; i < expr.lambda_captures.size(); i++) {
            const LambdaCapture& capture = expr.lambda_captures[i];
            const Type& field_type = info.field_types[i];
            llvm::LLVMValueRef field_ptr =
                llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, closure, info.physical_field_index(i), capture.name.c_str());
            if (capture.by_reference) {
                Expr ident = make_capture_identifier(capture, expr.loc);
                auto ident_lv_result = codegen_lvalue(ident);
                if (!ident_lv_result.has_value()) return std::unexpected(std::move(ident_lv_result).error());
                llvm::LLVMValueRef address = std::move(ident_lv_result).value().ptr;
                create_store(address, field_ptr, std::nullopt);
                continue;
            }
            Expr ident = make_capture_identifier(capture, expr.loc);
            const Expr& source = capture.init ? *capture.init : ident;
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name) &&
                is_bare_same_type_copy_source(source, field_type) && is_copy_constructible(field_type.name)) {
                auto source_lv_result = codegen_lvalue(source);
                if (!source_lv_result.has_value()) return std::unexpected(std::move(source_lv_result).error());
                if (auto r = codegen_copy_construct_class(field_ptr, std::move(source_lv_result).value().ptr, field_type.name);
                    !r.has_value()) return std::unexpected(std::move(r).error());
                continue;
            }
            auto value_result = codegen_value_for_target(source, field_type);
            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
            llvm::LLVMValueRef value = std::move(value_result).value();
            auto field_llvm_type_result = to_llvm_type(field_type);
            if (!field_llvm_type_result.has_value()) return std::unexpected(std::move(field_llvm_type_result).error());
            if (auto r = check_store_type(value, std::move(field_llvm_type_result).value(), "capture '" + capture.name + "'");
                !r.has_value()) return std::unexpected(std::move(r).error());
            create_store(value, field_ptr, std::nullopt);
        }
        return closure;
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_new_expr(const Expr& expr)
{
        auto element_type_result = to_llvm_type(expr.type);
        if (!element_type_result.has_value()) return std::unexpected(std::move(element_type_result).error());
        llvm::LLVMTypeRef element_type = std::move(element_type_result).value();
        llvm::LLVMValueRef heap_ptr = nullptr;
        if (expr.lhs) {
            auto heap_ptr_result = codegen_expr(*expr.lhs);
            if (!heap_ptr_result.has_value()) return std::unexpected(std::move(heap_ptr_result).error());
            heap_ptr = std::move(heap_ptr_result).value();
        } else {
            llvm::LLVMValueRef malloc_fn = get_or_declare_malloc();
            std::uint64_t size_in_bytes = llvm::LLVMABISizeOfType(data_layout_ref(module_), element_type);
            llvm::LLVMValueRef size_arg = llvm::LLVMConstInt(llvm::LLVMInt64TypeInContext(context_), size_in_bytes, 0);
            heap_ptr = build_call(malloc_fn, {size_arg}, "newptr");
        }

        if (expr.type.kind == TypeKind::Named && structs_.contains(expr.type.name)) {
            LValue target{heap_ptr, expr.type, std::nullopt};
            if (auto r = zero_initialize_storage(target.ptr, target.type, target.alignment); !r.has_value()) return std::unexpected(std::move(r).error());
            if (!expr.args.empty() || expr.has_paren_init) {
                auto same_type_result = try_initialize_class_storage_from_same_type_source(target, expr.args);
                if (!same_type_result.has_value()) return std::unexpected(std::move(same_type_result).error());
                if (std::move(same_type_result).value()) return heap_ptr;
                std::string ctor_name = expr.type.name + "_new";
                const Function* ctor_def = resolve_overload_by_type(ctor_name, expr.args, /*param_offset=*/1);
                if (ctor_def == nullptr) {
                    if (expr.args.empty()) return heap_ptr;
                    return std::unexpected(CodegenError("class '" + expr.type.name + "' has no constructor matching this call",
                        current_loc_));
                }
                if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                    auto value_result = codegen_constructed_class_value(expr.type.name, expr.args, ctor_def);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    llvm::LLVMValueRef value = std::move(value_result).value();
                    llvm::LLVMBuildStore(builder_, value, heap_ptr);
                    if (class_has_ordinary_vtable(expr.type.name)) {
                        if (auto r = initialize_ordinary_vtable_pointer(expr.type.name, heap_ptr);
                            !r.has_value()) return std::unexpected(std::move(r).error());
                    }
                    return heap_ptr;
                }
                llvm::LLVMValueRef ctor = llvm::LLVMGetNamedFunction(module_, overload_names_.at(ctor_def).c_str());
                if (ctor == nullptr) {
                    if (expr.args.empty()) return heap_ptr;
                    return std::unexpected(CodegenError("class '" + expr.type.name + "' has no constructor matching this call",
                        current_loc_));
                }
                auto args_result =
                    emit_constructor_arguments_and_virtual_bases(expr.type.name, ctor_def, expr.args, target.ptr);
                if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                args.insert(args.begin(), target.ptr);
                build_call(ctor, args);
            }
            return heap_ptr;
        }

        llvm::LLVMValueRef initial_value = llvm::LLVMConstNull(element_type);
        if (!expr.args.empty()) {
            if (expr.args.size() != 1) {
                return std::unexpected(CodegenError("'new T(args...)' for a non-class type currently requires exactly one argument",
                    current_loc_));
            }
            auto initial_value_result = codegen_expr(*expr.args[0]);
            if (!initial_value_result.has_value()) return std::unexpected(std::move(initial_value_result).error());
            initial_value = std::move(initial_value_result).value();
            refresh_debug_location(expr.loc);
            if (auto r = check_store_type(initial_value, element_type, "'new " + expr.type.name + "(...)' argument");
                !r.has_value()) return std::unexpected(std::move(r).error());
        }
        llvm::LLVMBuildStore(builder_, initial_value, heap_ptr);
        return heap_ptr;
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::codegen_delete_expr(const Expr& expr)
{
        auto ptr_result = codegen_expr(*expr.lhs);
        if (!ptr_result.has_value()) return std::unexpected(std::move(ptr_result).error());
        llvm::LLVMValueRef ptr = std::move(ptr_result).value();
        std::optional<Type> operand_type = infer_type(*expr.lhs);
        if (!operand_type.has_value() || operand_type->kind != TypeKind::Pointer || operand_type->pointee == nullptr) {
            return std::unexpected(CodegenError("'delete' requires a raw pointer operand in this version", current_loc_));
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
            if (auto r = emit_interface_destructor_dispatch_call(operand_type->pointee->name, ptr);
                !r.has_value()) return std::unexpected(std::move(r).error());
            build_call(get_or_declare_free(), {object_ptr});
            llvm::LLVMBuildBr(builder_, merge_bb);
            llvm::LLVMPositionBuilderAtEnd(builder_, merge_bb);
            return {};
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
                auto table_type_result = ordinary_vtable_type(pointee.name);
                if (!table_type_result.has_value()) return std::unexpected(std::move(table_type_result).error());
                llvm::LLVMTypeRef table_type = std::move(table_type_result).value();
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
                return {};
            }
            if (class_has_destructor_in_chain(pointee.name)) {
                codegen_call_destructor_chain_unless_moved(pointee.name, ptr, nullptr);
            }
        }
        build_call(get_or_declare_free(), {ptr});
        return {};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::codegen_destroy_expr(const Expr& expr)
{
        if (!expr.destroy_through_pointer) {
            return std::unexpected(CodegenError("explicit destructor calls currently require the pointer form 'ptr->~T()'",
                               current_loc_));
        }
        auto ptr_result = codegen_expr(*expr.lhs);
        if (!ptr_result.has_value()) return std::unexpected(std::move(ptr_result).error());
        llvm::LLVMValueRef ptr = std::move(ptr_result).value();
        if (expr.destroy_through_pointer) {
            std::optional<Type> operand_type = infer_type(*expr.lhs);
            if (operand_type.has_value() && is_interface_pointer_type(*operand_type)) {
                if (auto r = emit_interface_destructor_dispatch_call(operand_type->pointee->name, ptr);
                    !r.has_value()) return std::unexpected(std::move(r).error());
                return {};
            }
        }
        if (expr.type.kind == TypeKind::Named) {
            if (class_has_destructor_in_chain(expr.type.name)) {
                codegen_call_destructor_chain_unless_moved(expr.type.name, ptr, nullptr);
            }
        }
        return {};
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


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_scalar_cast(llvm::LLVMValueRef value, const Type& source_type, const Type& target_type)
{
        auto target_llvm_result = to_llvm_type(target_type);
        if (!target_llvm_result.has_value()) return std::unexpected(std::move(target_llvm_result).error());
        llvm::LLVMTypeRef target_llvm = std::move(target_llvm_result).value();
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


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_float_arith(BinaryOp op, llvm::LLVMValueRef lhs, llvm::LLVMValueRef rhs)
{
        switch (op) {
            case BinaryOp::Add: return llvm::LLVMBuildFAdd(builder_, lhs, rhs, "faddtmp");
            case BinaryOp::Sub: return llvm::LLVMBuildFSub(builder_, lhs, rhs, "fsubtmp");
            case BinaryOp::Mul: return llvm::LLVMBuildFMul(builder_, lhs, rhs, "fmultmp");
            default: return std::unexpected(CodegenError("unhandled floating-point arithmetic operator",
                current_loc_));
        }
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_checked_arith(BinaryOp op, llvm::LLVMValueRef lhs, llvm::LLVMValueRef rhs, bool is_unsigned,
                                        bool is_checked)
{
        const char* name = op == BinaryOp::Add ? "addtmp" : op == BinaryOp::Sub ? "subtmp" : "multmp";
        if (unsafe_depth_ > 0 || !is_checked) {
            switch (op) {
                case BinaryOp::Add: return llvm::LLVMBuildAdd(builder_, lhs, rhs, name);
                case BinaryOp::Sub: return llvm::LLVMBuildSub(builder_, lhs, rhs, name);
                case BinaryOp::Mul: return llvm::LLVMBuildMul(builder_, lhs, rhs, name);
                default: return std::unexpected(CodegenError("unhandled checked-arithmetic operator",
                    current_loc_));
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


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_pointer_offset(llvm::LLVMValueRef base_ptr, llvm::LLVMValueRef offset, const Type& pointer_type, bool negate_offset)
{
        llvm::LLVMValueRef gep_offset = negate_offset ? llvm::LLVMBuildNeg(builder_, offset, "ptroffset") : offset;
        llvm::LLVMValueRef gep_indices[] = {gep_offset};
        auto pointee_llvm_type_result = to_llvm_type(*pointer_type.pointee);
        if (!pointee_llvm_type_result.has_value()) return std::unexpected(std::move(pointee_llvm_type_result).error());
        return llvm::LLVMBuildGEP2(builder_, std::move(pointee_llvm_type_result).value(), base_ptr, gep_indices, 1, "ptrarith");
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_pointer_difference(llvm::LLVMValueRef lhs_ptr, llvm::LLVMValueRef rhs_ptr, const Type& pointer_type)
{
        auto diff_type_result = to_llvm_type(named_type("ptrdiff_t"));
        if (!diff_type_result.has_value()) return std::unexpected(std::move(diff_type_result).error());
        llvm::LLVMTypeRef diff_type = std::move(diff_type_result).value();
        llvm::LLVMValueRef lhs_int = llvm::LLVMBuildPtrToInt(builder_, lhs_ptr, diff_type, "lhsint");
        llvm::LLVMValueRef rhs_int = llvm::LLVMBuildPtrToInt(builder_, rhs_ptr, diff_type, "rhsint");
        llvm::LLVMValueRef byte_diff = llvm::LLVMBuildSub(builder_, lhs_int, rhs_int, "ptrbytes");
        auto pointee_llvm_type_result = to_llvm_type(*pointer_type.pointee);
        if (!pointee_llvm_type_result.has_value()) return std::unexpected(std::move(pointee_llvm_type_result).error());
        std::uint64_t elem_size = llvm::LLVMABISizeOfType(data_layout_ref(module_), std::move(pointee_llvm_type_result).value());
        if (elem_size == 1) return byte_diff;
        llvm::LLVMValueRef elem_size_value = llvm::LLVMConstInt(diff_type, elem_size, /*SignExtend=*/0);
        return llvm::LLVMBuildSDiv(builder_, byte_diff, elem_size_value, "ptrdifftmp");
    }


    [[nodiscard]] std::expected<Codegen::LValue, CodegenError> Codegen::codegen_lvalue(const Expr& expr)
{
        // Same refresh discipline as codegen_expr above.
        refresh_debug_location(expr.loc);
        switch (expr.kind) {
            case ExprKind::Identifier: {
                const LocalSlot* local = find_local(expr);
                if (local == nullptr) {
                    if (const GlobalSlot* global = find_visible_global_slot(expr.name, expr.explicit_global_qualification)) {
                        unsigned raw_alignment = llvm::LLVMGetAlignment(global->global);
                        std::optional<unsigned> explicit_alignment =
                            raw_alignment != 0 ? std::optional<unsigned>(raw_alignment) : std::nullopt;
                        return LValue{global->global, global->type,
                                      explicit_alignment.has_value() ? explicit_alignment : alignment_for_type(global->type)};
                    }
                    return std::unexpected(CodegenError("use of undeclared variable '" + expr.name + "'",
                        current_loc_));
                }
                if (local->type.kind == TypeKind::Reference) {
                    if (is_interface_reference_type(local->type)) {
                        return LValue{local->alloca, local->type, alignment_for_type(local->type)};
                    }
                    // A reference-typed local's own alloca just holds the
                    // address it's bound to (see the VarDecl case below,
                    // and how a Reference parameter arrives already as
                    // that address): auto-dereference once so every
                    // caller (reads, writes-through, and Member/Subscript
                    // base resolution) transparently operates on the
                    // referent, exactly like a real C++ reference.
                    llvm::LLVMValueRef referent_ptr =
                        create_load(llvm::LLVMPointerTypeInContext(context_, 0), local->alloca, std::nullopt, "deref");
                    return LValue{referent_ptr, *local->type.pointee, alignment_for_type(*local->type.pointee)};
                }
                return LValue{local->alloca, local->type, alignment_for_type(local->type)};
            }

            case ExprKind::Member: {
                auto base_result = codegen_lvalue(*expr.lhs);
                if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
                LValue base = std::move(base_result).value();
                if (base.type.kind != TypeKind::Named || !structs_.contains(base.type.name)) {
                    return std::unexpected(CodegenError("member access '." + expr.name + "' on a non-struct type",
                        current_loc_));
                }
                const StructInfo& info = structs_.at(base.type.name);
                std::optional<std::size_t> field_index_opt = info.find_field_index(expr.name);
                if (!field_index_opt.has_value()) {
                    return std::unexpected(CodegenError(std::string(info.is_union ? "union '" : "struct '") + base.type.name +
                                           "' has no field '" + expr.name + "'",
                        current_loc_));
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
                auto base_result = codegen_lvalue(*expr.lhs);
                if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
                LValue base = std::move(base_result).value();
                if (base.type.kind == TypeKind::Named &&
                    (base.type.name == "std::vector" || base.type.name == "vector" ||
                     base.type.name.starts_with("std::vector.") || base.type.name.starts_with("vector."))) {
                    auto struct_it = structs_.find(base.type.name);
                    if (struct_it == structs_.end()) {
                        return std::unexpected(CodegenError("unknown vector layout", current_loc_));
                    }
                    const StructInfo& info = struct_it->second;
                    std::optional<std::size_t> data_index_opt = info.find_field_index("data_");
                    std::optional<std::size_t> size_index_opt = info.find_field_index("size_");
                    if (!data_index_opt.has_value() || !size_index_opt.has_value()) {
                        return std::unexpected(CodegenError("vector layout missing required fields", current_loc_));
                    }
                    const Type& data_field_type = info.field_types[*data_index_opt];
                    if (data_field_type.kind != TypeKind::Pointer || !data_field_type.pointee) {
                        return std::unexpected(CodegenError("vector data_ field is not a pointer", current_loc_));
                    }
                    const Type& element_type = *data_field_type.pointee;
                    llvm::LLVMValueRef data_ptr = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, base.ptr,
                                                                            info.physical_field_index(*data_index_opt),
                                                                            "vec.dataptr");
                    llvm::LLVMValueRef size_ptr = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, base.ptr,
                                                                            info.physical_field_index(*size_index_opt),
                                                                            "vec.sizeptr");
                    llvm::LLVMValueRef data = llvm::LLVMBuildLoad2(builder_, llvm::LLVMPointerTypeInContext(context_, 0),
                                                                   data_ptr, "vec.data");
                    llvm::LLVMValueRef size32 = llvm::LLVMBuildLoad2(builder_, llvm::LLVMInt32TypeInContext(context_),
                                                                     size_ptr, "vec.size32");
                    llvm::LLVMValueRef size = llvm::LLVMBuildSExt(builder_, size32, llvm::LLVMInt64TypeInContext(context_),
                                                                  "vec.size");
                    auto index_result = codegen_expr(*expr.rhs);
                    if (!index_result.has_value()) return std::unexpected(std::move(index_result).error());
                    llvm::LLVMValueRef index = std::move(index_result).value();
                    emit_span_bounds_check(index, size);
                    llvm::LLVMValueRef gep_indices_vec[] = {index};
                    auto element_llvm_type_result = to_llvm_type(element_type);
                    if (!element_llvm_type_result.has_value()) return std::unexpected(std::move(element_llvm_type_result).error());
                    llvm::LLVMValueRef elem_ptr =
                        llvm::LLVMBuildGEP2(builder_, std::move(element_llvm_type_result).value(), data, gep_indices_vec, 1, "vecelem");
                    return LValue{elem_ptr, element_type, alignment_for_type(element_type)};
                }
                if (base.type.kind == TypeKind::Span) {
                    auto span_type_result = to_llvm_type(base.type);
                    if (!span_type_result.has_value()) return std::unexpected(std::move(span_type_result).error());
                    llvm::LLVMTypeRef span_type = std::move(span_type_result).value();
                    llvm::LLVMValueRef size_ptr = llvm::LLVMBuildStructGEP2(builder_, span_type, base.ptr, 1, "sizeptr");
                    llvm::LLVMValueRef size = llvm::LLVMBuildLoad2(builder_, llvm::LLVMInt64TypeInContext(context_), size_ptr, "size");
                    llvm::LLVMValueRef data_ptr = llvm::LLVMBuildStructGEP2(builder_, span_type, base.ptr, 0, "dataptr");
                    llvm::LLVMValueRef data = llvm::LLVMBuildLoad2(builder_, llvm::LLVMPointerTypeInContext(context_, 0), data_ptr, "data");
                    auto index_result = codegen_expr(*expr.rhs);
                    if (!index_result.has_value()) return std::unexpected(std::move(index_result).error());
                    llvm::LLVMValueRef index = std::move(index_result).value();
                    // Runtime bounds check (spec ch08: checked by default,
                    // bounds checks inserted unconditionally) -- unlike a
                    // fixed-size array's subscript below, a span's length is
                    // only known at runtime, so (even for a compile-time-
                    // constant index) there's no way to reject an
                    // out-of-bounds index at compile time; it's always this
                    // same runtime check instead.
                    emit_span_bounds_check(index, size);
                    llvm::LLVMValueRef gep_indices_span[] = {index};
                    auto pointee_llvm_type_result = to_llvm_type(*base.type.pointee);
                    if (!pointee_llvm_type_result.has_value()) return std::unexpected(std::move(pointee_llvm_type_result).error());
                    llvm::LLVMValueRef elem_ptr =
                        llvm::LLVMBuildGEP2(builder_, std::move(pointee_llvm_type_result).value(), data, gep_indices_span, 1, "elemtmp");
                    return LValue{elem_ptr, *base.type.pointee, alignment_for_type(*base.type.pointee)};
                }
                if (base.type.kind == TypeKind::Pointer) {
                    llvm::LLVMValueRef data = llvm::LLVMBuildLoad2(builder_, llvm::LLVMPointerTypeInContext(context_, 0), base.ptr, "data");
                    auto index_result = codegen_expr(*expr.rhs);
                    if (!index_result.has_value()) return std::unexpected(std::move(index_result).error());
                    llvm::LLVMValueRef index = std::move(index_result).value();
                    llvm::LLVMValueRef gep_indices_ptr[] = {index};
                    auto pointee_llvm_type_result = to_llvm_type(*base.type.pointee);
                    if (!pointee_llvm_type_result.has_value()) return std::unexpected(std::move(pointee_llvm_type_result).error());
                    llvm::LLVMValueRef elem_ptr =
                        llvm::LLVMBuildGEP2(builder_, std::move(pointee_llvm_type_result).value(), data, gep_indices_ptr, 1, "elemtmp");
                    return LValue{elem_ptr, *base.type.pointee, alignment_for_type(*base.type.pointee)};
                }
                if (base.type.kind != TypeKind::Array) {
                    return std::unexpected(CodegenError("subscript on a non-array type",
                        current_loc_));
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
                    return std::unexpected(CodegenError("array subscript " + std::to_string(*constant_index) +
                                            " is out of bounds for array of size " +
                                            std::to_string(base.type.array_size),
                        current_loc_));
                }
                auto index_result = codegen_expr(*expr.rhs);
                if (!index_result.has_value()) return std::unexpected(std::move(index_result).error());
                llvm::LLVMValueRef index = std::move(index_result).value();
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
                auto base_llvm_type_result = to_llvm_type(base.type);
                if (!base_llvm_type_result.has_value()) return std::unexpected(std::move(base_llvm_type_result).error());
                llvm::LLVMValueRef elem_ptr =
                    llvm::LLVMBuildGEP2(builder_, std::move(base_llvm_type_result).value(), base.ptr, gep_indices_arr, 2, "elemtmp");
                return LValue{elem_ptr, *base.type.element, alignment_for_type(*base.type.element)};
            }

            case ExprKind::Call: {
                // Reachable whenever a call to a reference-returning
                // function is itself used as a reference-binding source
                // (`T& r = f(x);`), a reference argument (`g(f(x))`), or
                // forwarded in a `return` -- see
                // resolve_borrow_source_root in movecheck.cppm. It's also
                // reachable whenever *any* call (reference-returning or
                // not) is itself used as a method-call receiver -- see
                // the Lambda case just below's own comment: codegen_call
                // calls codegen_lvalue on its own `expr.lhs` uniformly,
                // regardless of receiver shape (e.g.
                // `some_by_value_call(x).method()`).
                // codegen_call's raw result is already the referent's
                // address in the reference-returning case -- no load
                // needed, unlike codegen_expr's own Call case. Validity
                // (must actually be reference-returning) is checked
                // *after* codegen_call returns rather than before, unlike
                // the pre-method-call version of this code -- codegen_call
                // must run first regardless, to resolve a possible
                // method-call receiver exactly once; an invalid program
                // reaching this far would already have been rejected by
                // movecheck, so emitting a few extra instructions first
                // (discarded below in the reference case, reused as-is
                // in the by-value case) is harmless either way.
                auto result_result = codegen_call(expr);
                if (!result_result.has_value()) return std::unexpected(std::move(result_result).error());
                CallResult result = std::move(result_result).value();
                if (result.callee_def != nullptr && result.callee_def->return_type.kind == TypeKind::Reference) {
                    if (is_interface_reference_type(result.callee_def->return_type)) {
                        auto slot_type_result = to_llvm_type(result.callee_def->return_type);
                        if (!slot_type_result.has_value()) return std::unexpected(std::move(slot_type_result).error());
                        llvm::LLVMValueRef slot =
                            create_entry_block_alloca(std::move(slot_type_result).value(), "ifacereftmp");
                        create_store(result.value, slot, alignment_for_type(result.callee_def->return_type));
                        return LValue{slot, result.callee_def->return_type, alignment_for_type(result.callee_def->return_type)};
                    }
                    return LValue{result.value, *result.callee_def->return_type.pointee,
                                  alignment_for_type(*result.callee_def->return_type.pointee)};
                }
                // A by-value-returning call (e.g. a method call's own
                // receiver, `builtin_scalar_keyword_type_name(kind).empty()`)
                // is a fresh prvalue with no existing place to alias --
                // materialize it into a temporary and return that
                // temporary's address, exactly like
                // codegen_materialize_rvalue_reference_source's identical
                // pattern.
                std::optional<Type> result_type = infer_type(expr);
                if (!result_type.has_value()) {
                    return std::unexpected(CodegenError("expression is not assignable",
                        current_loc_));
                }
                llvm::LLVMValueRef temp = create_entry_block_alloca(llvm::LLVMTypeOf(result.value), "calllvaluetmp");
                llvm::LLVMBuildStore(builder_, result.value, temp);
                return LValue{temp, *result_type, alignment_for_type(*result_type)};
            }

            case ExprKind::Lambda: {
                // ch05 §5.12: an IIFE's receiver (`[](...){...}(args)`,
                // parser.cppm's own Lambda-followed-by-`(` case) needs
                // the constructed closure's *address* to invoke its
                // "call" method on -- exactly like an ordinary method
                // call's receiver (codegen_call's own `expr.lhs != nullptr`
                // branch calls codegen_lvalue on it uniformly, regardless
                // of receiver shape).
                auto ptr_result = codegen_construct_lambda(expr);
                if (!ptr_result.has_value()) return std::unexpected(std::move(ptr_result).error());
                llvm::LLVMValueRef ptr = std::move(ptr_result).value();
                return LValue{ptr, named_type(expr.name),
                              alignment_for_type(named_type(expr.name))};
            }

            case ExprKind::Conditional: {
                auto cond_result = codegen_contextual_bool_i1(*expr.lhs);
                if (!cond_result.has_value()) return std::unexpected(std::move(cond_result).error());
                llvm::LLVMValueRef cond = std::move(cond_result).value();
                llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
                llvm::LLVMBasicBlockRef then_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.lvalue.then");
                llvm::LLVMBasicBlockRef else_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.lvalue.else");
                llvm::LLVMBasicBlockRef merge_block = llvm::LLVMAppendBasicBlockInContext(context_, current_function, "cond.lvalue.end");
                llvm::LLVMBuildCondBr(builder_, cond, then_block, else_block);

                llvm::LLVMPositionBuilderAtEnd(builder_, then_block);
                auto then_lvalue_result = codegen_lvalue(*expr.rhs);
                if (!then_lvalue_result.has_value()) return std::unexpected(std::move(then_lvalue_result).error());
                LValue then_lvalue = std::move(then_lvalue_result).value();
                llvm::LLVMBuildBr(builder_, merge_block);
                llvm::LLVMBasicBlockRef then_end = llvm::LLVMGetInsertBlock(builder_);

                llvm::LLVMPositionBuilderAtEnd(builder_, else_block);
                auto else_lvalue_result = codegen_lvalue(*expr.third);
                if (!else_lvalue_result.has_value()) return std::unexpected(std::move(else_lvalue_result).error());
                LValue else_lvalue = std::move(else_lvalue_result).value();
                llvm::LLVMBuildBr(builder_, merge_block);
                llvm::LLVMBasicBlockRef else_end = llvm::LLVMGetInsertBlock(builder_);

                llvm::LLVMPositionBuilderAtEnd(builder_, merge_block);
                if (!types_equal(then_lvalue.type, else_lvalue.type) || then_lvalue.alignment != else_lvalue.alignment ||
                    llvm::LLVMTypeOf(then_lvalue.ptr) != llvm::LLVMTypeOf(else_lvalue.ptr)) {
                    return std::unexpected(CodegenError("expression is not assignable", current_loc_));
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
                    return std::unexpected(CodegenError("expression is not assignable", current_loc_));
                }
                auto value_result = codegen_expr(expr);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                llvm::LLVMValueRef value = std::move(value_result).value();
                auto expr_llvm_type_result = to_llvm_type(expr.type);
                if (!expr_llvm_type_result.has_value()) return std::unexpected(std::move(expr_llvm_type_result).error());
                llvm::LLVMValueRef slot = create_entry_block_alloca(std::move(expr_llvm_type_result).value(), "castptrtmp");
                create_store(value, slot, alignment_for_type(expr.type));
                return LValue{slot, expr.type, alignment_for_type(expr.type)};
            }

            case ExprKind::Unary: {
                if (expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PreDec) {
                    auto lv_result = codegen_lvalue(*expr.lhs);
                    if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
                    LValue lv = std::move(lv_result).value();
                    auto old_value_result = load_value(lv);
                    if (!old_value_result.has_value()) return std::unexpected(std::move(old_value_result).error());
                    llvm::LLVMValueRef old_value = std::move(old_value_result).value();
                    bool is_float = lv.type.kind == TypeKind::Named && is_float_scalar_type_name(lv.type.name);
                    auto llvm_type_result = to_llvm_type(lv.type);
                    if (!llvm_type_result.has_value()) return std::unexpected(std::move(llvm_type_result).error());
                    llvm::LLVMTypeRef llvm_type = std::move(llvm_type_result).value();
                    llvm::LLVMValueRef one = is_float ? llvm::LLVMConstReal(llvm_type, 1.0)
                                                      : llvm::LLVMConstInt(llvm_type, 1, 0);
                    llvm::LLVMValueRef new_value =
                        expr.unary_op == UnaryOp::PreInc
                            ? (is_float ? llvm::LLVMBuildFAdd(builder_, old_value, one, "preinc")
                                        : llvm::LLVMBuildAdd(builder_, old_value, one, "preinc"))
                            : (is_float ? llvm::LLVMBuildFSub(builder_, old_value, one, "predec")
                                        : llvm::LLVMBuildSub(builder_, old_value, one, "predec"));
                    create_store(new_value, lv.ptr, lv.alignment);
                    return lv;
                }
                // Only `*p` (Deref) and prefix ++/-- are addressable; the
                // other unary forms produce a plain value with no backing
                // storage.
                if (expr.unary_op != UnaryOp::Deref) {
                    return std::unexpected(CodegenError("expression is not assignable",
                        current_loc_));
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
                    return std::unexpected(CodegenError("expression is not assignable", current_loc_));
                }
                const Type& operand_underlying =
                    operand_type->kind == TypeKind::Reference && operand_type->pointee ? *operand_type->pointee : *operand_type;
                if (operand_underlying.kind == TypeKind::Named) {
                    auto operand_result = codegen_lvalue(*expr.lhs);
                    if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                    LValue operand = std::move(operand_result).value();
                    std::vector<ExprPtr> no_args;
                    bool receiver_is_mutable = !is_read_only_place(*expr.lhs);
                    if (const Function* callee_def =
                            resolve_overload_by_type(operand.type.name + "_operator_deref", no_args, 1,
                                                     receiver_is_mutable, expr.lhs.get())) {
                        llvm::LLVMValueRef callee = llvm::LLVMGetNamedFunction(module_, overload_names_.at(callee_def).c_str());
                        if (callee == nullptr) {
                            return std::unexpected(CodegenError("internal error: no generated code for '" +
                                                                    operand.type.name + "::operator*' used here",
                                current_loc_));
                        }
                        llvm::LLVMValueRef referent_ptr = build_call(callee, {operand.ptr});
                        if (callee_def->return_type.kind != TypeKind::Reference) {
                            return std::unexpected(CodegenError("operator* on class '" + operand.type.name +
                                                   "' must return a reference to be assignable",
                                current_loc_));
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
                    return std::unexpected(CodegenError("dereference ('*') is only supported for a raw pointer or a class with operator*",
                        current_loc_));
                }
                if (is_interface_pointer_type(*operand_type)) {
                    return std::unexpected(CodegenError("dereferencing an interface pointer does not yield an assignable storage location",
                        current_loc_));
                }
                bool operand_has_storage =
                    expr.lhs->kind == ExprKind::Identifier || expr.lhs->kind == ExprKind::Member ||
                    expr.lhs->kind == ExprKind::Subscript;
                llvm::LLVMValueRef pointee_ptr = nullptr;
                if (operand_has_storage) {
                    auto operand_result = codegen_lvalue(*expr.lhs);
                    if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                    LValue operand = std::move(operand_result).value();
                    pointee_ptr =
                       create_load(llvm::LLVMPointerTypeInContext(context_, 0), operand.ptr, operand.alignment, "deref");
                } else {
                    auto pointee_ptr_result = codegen_expr(*expr.lhs);
                    if (!pointee_ptr_result.has_value()) return std::unexpected(std::move(pointee_ptr_result).error());
                    pointee_ptr = std::move(pointee_ptr_result).value();
                }
                return LValue{pointee_ptr, *operand_type->pointee, alignment_for_type(*operand_type->pointee)};
            }

            default:
                return std::unexpected(CodegenError("expression is not assignable",
                    current_loc_));
        }
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_builtin_print(const Expr& expr)
{
        if (expr.args.size() != 1) {
            return std::unexpected(CodegenError(expr.name + " expects exactly 1 argument",
                current_loc_));
        }
        llvm::LLVMValueRef printf_fn = get_or_declare_printf();
        auto arg_result = codegen_expr(*expr.args[0]);
        if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
        llvm::LLVMValueRef arg = std::move(arg_result).value();

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
            auto arg_i1_result = bool_to_i1(arg);
            if (!arg_i1_result.has_value()) return std::unexpected(std::move(arg_i1_result).error());
            printf_arg = llvm::LLVMBuildSelect(builder_, std::move(arg_i1_result).value(), true_str, false_str, "booltmp");
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


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_binary(const Expr& expr)
{
        if (expr.binary_op == BinaryOp::Assign) {
            auto lv_result = codegen_lvalue(*expr.lhs);
            if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
            LValue lv = std::move(lv_result).value();
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
                auto src_lv_result = codegen_lvalue(*expr.rhs);
                if (!src_lv_result.has_value()) return std::unexpected(std::move(src_lv_result).error());
                llvm::LLVMValueRef src_ptr = std::move(src_lv_result).value().ptr;
                if (const Function* user_assign = find_user_declared_copy_assign_ast(lv.type.name)) {
                    llvm::LLVMValueRef op = llvm::LLVMGetNamedFunction(module_, overload_names_.at(user_assign).c_str());
                    build_call(op, {lv.ptr, src_ptr});
                } else {
                    auto memberwise_result = codegen_memberwise_copy_assign(lv.ptr, src_ptr, lv.type.name);
                    if (!memberwise_result.has_value()) return std::unexpected(std::move(memberwise_result).error());
                }
                if (expr.lhs->kind == ExprKind::Identifier) {
                    // See the move-assignment path's identical
                    // comment below for why this reset is needed
                    // (covers reassigning a previously-moved-out
                    // variable via a copy this time).
                    const LocalSlot* target_local = find_local(*expr.lhs);
                    if (target_local != nullptr && target_local->moved_flag != nullptr) {
                        llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0),
                                             target_local->moved_flag);
                    }
                }
                return lv.ptr;
            }
            auto value_result = codegen_value_for_target(*expr.rhs, lv.type);
            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
            llvm::LLVMValueRef value = std::move(value_result).value();
            // Refresh to `expr`'s own position -- see the VarDecl case's
            // identical comment in codegen_stmt.
            refresh_debug_location(expr.loc);
            auto lv_llvm_type_result = to_llvm_type(lv.type);
            if (!lv_llvm_type_result.has_value()) return std::unexpected(std::move(lv_llvm_type_result).error());
            auto check_store_result = check_store_type(value, std::move(lv_llvm_type_result).value(), "'" + expr.lhs->name + "'");
            if (!check_store_result.has_value()) return std::unexpected(std::move(check_store_result).error());
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
                    const LocalSlot* target_local = find_local(*expr.lhs);
                    if (target_local != nullptr) target_moved_flag = target_local->moved_flag;
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
                const LocalSlot* target_local = find_local(*expr.lhs);
                if (target_local != nullptr && target_local->moved_flag != nullptr) {
                    llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0), target_local->moved_flag);
                }
            }
            return value;
        }
        if (is_compound_assignment(expr.binary_op)) {
            auto lv_result = codegen_lvalue(*expr.lhs);
            if (!lv_result.has_value()) return std::unexpected(std::move(lv_result).error());
            LValue lv = std::move(lv_result).value();
            std::optional<Type> lhs_type = infer_type(*expr.lhs);
            std::optional<Type> rhs_type = infer_type(*expr.rhs);
            const Type& operand_type = lv.type.kind == TypeKind::Reference && lv.type.pointee ? *lv.type.pointee : lv.type;
            if (expr.binary_op == BinaryOp::AddAssign && lhs_type.has_value() && rhs_type.has_value()) {
                const Type& lhs_operand = binary_operand_type(*lhs_type);
                const Type& rhs_operand = binary_operand_type(*rhs_type);
                if (is_string_named_type(lhs_operand) &&
                    (is_string_named_type(rhs_operand) || is_const_char_pointer_type(rhs_operand))) {
                    std::vector<ExprPtr> append_args;
                    append_args.push_back(deep_clone_expr(*expr.rhs));
                    const Function* callee =
                        resolve_overload_by_type(lhs_operand.name + "_append", append_args, /*param_offset=*/1,
                                                 !is_read_only_place(*expr.lhs), expr.lhs.get());
                    if (callee == nullptr) {
                        return std::unexpected(CodegenError("class '" + lhs_operand.name + "' has no append overload matching this '+='", current_loc_));
                    }
                    llvm::LLVMValueRef target = llvm::LLVMGetNamedFunction(module_, overload_names_.at(callee).c_str());
                    if (target == nullptr) {
                        return std::unexpected(CodegenError("internal error: no generated code for '" +
                                                                lhs_operand.name + "::append' used here",
                            current_loc_));
                    }
                    auto args_result = codegen_call_args(append_args, callee, /*param_offset=*/1);
                    if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                    std::vector<llvm::LLVMValueRef> args = std::move(args_result).value();
                    args.insert(args.begin(), lv.ptr);
                    build_call(target, args);
                    auto lv_llvm_type_result = to_llvm_type(lv.type);
                    if (!lv_llvm_type_result.has_value()) return std::unexpected(std::move(lv_llvm_type_result).error());
                    return create_load(std::move(lv_llvm_type_result).value(), lv.ptr, lv.alignment, "compoundassign.tmp");
                }
            }

            auto lv_llvm_type_result = to_llvm_type(lv.type);
            if (!lv_llvm_type_result.has_value()) return std::unexpected(std::move(lv_llvm_type_result).error());
            llvm::LLVMValueRef lhs = create_load(std::move(lv_llvm_type_result).value(), lv.ptr, lv.alignment, "compoundassign.lhs");
            auto rhs_result = codegen_value_for_target(*expr.rhs, lv.type);
            if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
            llvm::LLVMValueRef rhs = std::move(rhs_result).value();
            std::optional<Type> pointer_result_type =
                lhs_type.has_value() && rhs_type.has_value()
                    ? pointer_arithmetic_result_type(compound_base_operator(expr.binary_op), *lhs_type, *rhs_type)
                    : std::nullopt;
            if (pointer_result_type.has_value()) {
                if (expr.binary_op != BinaryOp::AddAssign && expr.binary_op != BinaryOp::SubAssign) {
                    return std::unexpected(CodegenError("pointer compound assignment only supports '+=' and '-='", current_loc_));
                }
                auto value_result = codegen_pointer_offset(lhs, rhs, operand_type, expr.binary_op == BinaryOp::SubAssign);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                llvm::LLVMValueRef value = std::move(value_result).value();
                create_store(value, lv.ptr, lv.alignment);
                return value;
            }
            bool is_float = operand_type.kind == TypeKind::Named && is_float_scalar_type_name(operand_type.name);
            bool is_unsigned = operand_type.kind == TypeKind::Named && is_unsigned_scalar_type_name(operand_type.name);
            bool is_checked = operand_type.kind == TypeKind::Named && is_checked_arithmetic_scalar_type_name(operand_type.name);
            BinaryOp arithmetic_op = compound_base_operator(expr.binary_op);
            llvm::LLVMValueRef value = nullptr;
            switch (arithmetic_op) {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                    if (is_float) {
                        auto value_result = codegen_float_arith(arithmetic_op, lhs, rhs);
                        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                        value = std::move(value_result).value();
                    } else {
                        auto value_result = codegen_checked_arith(arithmetic_op, lhs, rhs, is_unsigned, is_checked);
                        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                        value = std::move(value_result).value();
                    }
                    break;
                case BinaryOp::Div:
                    value = is_float ? llvm::LLVMBuildFDiv(builder_, lhs, rhs, "fdivtmp")
                                     : codegen_checked_div(lhs, rhs, is_unsigned, is_checked);
                    break;
                default:
                    return std::unexpected(CodegenError("unhandled compound assignment operator", current_loc_));
            }
            create_store(value, lv.ptr, lv.alignment);
            if (lv.type.kind == TypeKind::Named && expr.lhs->kind == ExprKind::Identifier) {
                const LocalSlot* target_local = find_local(*expr.lhs);
                if (target_local != nullptr && target_local->moved_flag != nullptr) {
                    llvm::LLVMBuildStore(builder_, llvm::LLVMConstInt(llvm::LLVMInt1TypeInContext(context_), 0, 0),
                                         target_local->moved_flag);
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
        if (expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne) {
            // `rawPtr == nullptr` (or `!=`) -- unlike a smart pointer
            // (which defines a real overloaded operator== taking a
            // literal T*, handled below via resolve_equality_receiver),
            // a raw pointer has no such overload to dispatch through.
            // `nullptr` itself has no fixed type at all (see
            // codegen_value_for_target's own identical recognition,
            // used for argument-passing/initialization), so it can
            // never contribute a usable lhs_type/rhs_type the way an
            // ordinary scalar literal would (lhs_is_literal/
            // rhs_is_literal above only ever recognize IntegerLiteral/
            // FloatLiteral). Recognized directly here, building the
            // icmp against a genuine null pointer constant of the
            // *other* side's own pointer type -- mirroring
            // codegen_value_for_target exactly -- rather than falling
            // through to the generic operand codegen further below,
            // which would otherwise try to evaluate "nullptr" as an
            // ordinary named value and fail.
            auto is_nullptr_identifier = [](const Expr& operand) {
                return operand.kind == ExprKind::Identifier && operand.name == "nullptr" &&
                       !operand.explicit_global_qualification;
            };
            bool lhs_is_nullptr = is_nullptr_identifier(*expr.lhs);
            bool rhs_is_nullptr = is_nullptr_identifier(*expr.rhs);
            if (lhs_is_nullptr != rhs_is_nullptr) {
                const Expr& pointer_expr = lhs_is_nullptr ? *expr.rhs : *expr.lhs;
                const std::optional<Type>& pointer_expr_type = lhs_is_nullptr ? rhs_type : lhs_type;
                if (pointer_expr_type.has_value() && pointer_expr_type->kind == TypeKind::Pointer) {
                    auto pointer_value_result = codegen_expr(pointer_expr);
                    if (!pointer_value_result.has_value()) return std::unexpected(std::move(pointer_value_result).error());
                    llvm::LLVMValueRef pointer_value = std::move(pointer_value_result).value();
                    auto null_llvm_type_result = to_llvm_type(*pointer_expr_type);
                    if (!null_llvm_type_result.has_value()) return std::unexpected(std::move(null_llvm_type_result).error());
                    llvm::LLVMValueRef null_value = llvm::LLVMConstNull(std::move(null_llvm_type_result).value());
                    return i1_to_bool(expr.binary_op == BinaryOp::Eq
                                          ? llvm::LLVMBuildICmp(builder_, llvm::LLVMIntEQ, pointer_value, null_value,
                                                                "nulleqtmp")
                                          : llvm::LLVMBuildICmp(builder_, llvm::LLVMIntNE, pointer_value, null_value,
                                                                "nullnetmp"));
                }
            }
            const Type* lhs_named =
                lhs_type.has_value() ? &(lhs_type->kind == TypeKind::Reference && lhs_type->pointee ? *lhs_type->pointee
                                                                                                     : *lhs_type)
                                     : nullptr;
            const Type* rhs_named =
                rhs_type.has_value() ? &(rhs_type->kind == TypeKind::Reference && rhs_type->pointee ? *rhs_type->pointee
                                                                                                     : *rhs_type)
                                     : nullptr;
            std::string operator_name = equality_operator_method_name(expr.binary_op);
            auto resolve_equality_receiver =
                [&](const Expr& receiver_expr, const Expr& arg_expr,
                    const Type* receiver_named) -> std::expected<std::optional<llvm::LLVMValueRef>, CodegenError> {
                if (receiver_named == nullptr || receiver_named->kind != TypeKind::Named) return std::nullopt;
                std::vector<ExprPtr> overload_args;
                overload_args.push_back(deep_clone_expr_with_loc(arg_expr, expr.loc));
                if (resolve_overload_by_type(receiver_named->name + "_" + operator_name, overload_args, /*param_offset=*/1,
                                             !is_read_only_place(receiver_expr), &receiver_expr) != nullptr) {
                    ExprPtr overload_call =
                        make_overloaded_equality_call_expr(receiver_expr, arg_expr, expr.binary_op, expr.loc);
                    auto call_result = codegen_call(*overload_call);
                    if (!call_result.has_value()) return std::unexpected(std::move(call_result).error());
                    return std::move(call_result).value().value;
                }
                return std::nullopt;
            };
            auto lhs_receiver_result = resolve_equality_receiver(*expr.lhs, *expr.rhs, lhs_named);
            if (!lhs_receiver_result.has_value()) return std::unexpected(std::move(lhs_receiver_result).error());
            if (std::optional<llvm::LLVMValueRef> value = std::move(lhs_receiver_result).value()) {
                return *value;
            }
            auto rhs_receiver_result = resolve_equality_receiver(*expr.rhs, *expr.lhs, rhs_named);
            if (!rhs_receiver_result.has_value()) return std::unexpected(std::move(rhs_receiver_result).error());
            if (std::optional<llvm::LLVMValueRef> value = std::move(rhs_receiver_result).value()) {
                return *value;
            }
            bool lhs_is_record = lhs_named != nullptr && lhs_named->kind == TypeKind::Named && is_named_record_type(*lhs_named);
            bool rhs_is_record = rhs_named != nullptr && rhs_named->kind == TypeKind::Named && is_named_record_type(*rhs_named);
            if (lhs_is_record || rhs_is_record) {
                std::string receiver_name = lhs_is_record ? lhs_named->name : rhs_named->name;
                std::string receiver_side = lhs_is_record ? "left" : "right";
                return std::unexpected(CodegenError("operator '" + std::string(expr.binary_op == BinaryOp::Eq ? "==" : "!=") +
                                       "' requires a matching overloaded member operator on " + receiver_side +
                                       " operand type '" + receiver_name + "'",
                                   current_loc_));
            }
        }
        if ((expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne) && lhs_type.has_value() && rhs_type.has_value() &&
            is_interface_pointer_type(binary_operand_type(*lhs_type)) && is_interface_pointer_type(binary_operand_type(*rhs_type))) {
            auto lhs_expr_result = codegen_expr(*expr.lhs);
            if (!lhs_expr_result.has_value()) return std::unexpected(std::move(lhs_expr_result).error());
            llvm::LLVMValueRef lhs_object = extract_interface_object_ptr(std::move(lhs_expr_result).value());
            auto rhs_expr_result = codegen_expr(*expr.rhs);
            if (!rhs_expr_result.has_value()) return std::unexpected(std::move(rhs_expr_result).error());
            llvm::LLVMValueRef rhs_object = extract_interface_object_ptr(std::move(rhs_expr_result).value());
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
            return std::unexpected(CodegenError("pointer arithmetic requires 'pointer +/- integer' or 'pointer - pointer' with matching "
                               "non-void pointer types",
                current_loc_));
        }
        bool needs_strict_scalar_match = expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne ||
                                         expr.binary_op == BinaryOp::Lt || expr.binary_op == BinaryOp::Gt ||
                                         expr.binary_op == BinaryOp::Le || expr.binary_op == BinaryOp::Ge;
        if (needs_strict_scalar_match && lhs_type.has_value() && rhs_type.has_value()) {
            const Type& lhs_operand_type = binary_operand_type(*lhs_type);
            const Type& rhs_operand_type = binary_operand_type(*rhs_type);
            if (!types_equal(lhs_operand_type, rhs_operand_type) && !lhs_is_literal && !rhs_is_literal) {
                return std::unexpected(CodegenError("binary operator requires operands of the same type; scpp has no implicit conversion "
                                   "between distinct scalar types",
                                   current_loc_));
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
                return std::unexpected(CodegenError("binary operator requires operands of the same type; scpp has no implicit conversion "
                                   "between distinct scalar types",
                                   current_loc_));
            }
        }
        auto lhs_result = context_type.has_value() ? codegen_value_for_target(*expr.lhs, *context_type)
                                                      : codegen_expr(*expr.lhs);
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        llvm::LLVMValueRef lhs = std::move(lhs_result).value();
        auto rhs_result = context_type.has_value() ? codegen_value_for_target(*expr.rhs, *context_type)
                                                      : codegen_expr(*expr.rhs);
        if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
        llvm::LLVMValueRef rhs = std::move(rhs_result).value();

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
            default: return std::unexpected(CodegenError("unhandled binary operator",
                current_loc_));
        }
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::codegen_short_circuit(const Expr& expr)
{
        llvm::LLVMValueRef current_function = llvm::LLVMGetBasicBlockParent(llvm::LLVMGetInsertBlock(builder_));
        bool is_and = expr.binary_op == BinaryOp::And;

        // `lhs`/`rhs` stay in the i8 bool representation throughout (so
        // the merging PHI below can use either directly, matching how
        // every other bool value is stored/passed/returned) -- only the
        // branch conditions themselves need the narrower bool_to_i1 form.
        auto lhs_result = codegen_contextual_bool_value(*expr.lhs);
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        llvm::LLVMValueRef lhs = std::move(lhs_result).value();
        llvm::LLVMBasicBlockRef rhs_block =
            llvm::LLVMAppendBasicBlockInContext(context_, current_function, is_and ? "and.rhs" : "or.rhs");
        llvm::LLVMBasicBlockRef merge_block =
            llvm::LLVMAppendBasicBlockInContext(context_, current_function, is_and ? "and.end" : "or.end");
        llvm::LLVMBasicBlockRef lhs_block = llvm::LLVMGetInsertBlock(builder_);

        auto lhs_i1_result = bool_to_i1(lhs);
        if (!lhs_i1_result.has_value()) return std::unexpected(std::move(lhs_i1_result).error());
        llvm::LLVMValueRef lhs_i1 = std::move(lhs_i1_result).value();
        if (is_and) {
            llvm::LLVMBuildCondBr(builder_, lhs_i1, rhs_block, merge_block);
        } else {
            llvm::LLVMBuildCondBr(builder_, lhs_i1, merge_block, rhs_block);
        }

        llvm::LLVMPositionBuilderAtEnd(builder_, rhs_block);
        auto rhs_result = codegen_contextual_bool_value(*expr.rhs);
        if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
        llvm::LLVMValueRef rhs = std::move(rhs_result).value();
        auto require_bool_result = require_bool_representation(rhs);
        if (!require_bool_result.has_value()) return std::unexpected(std::move(require_bool_result).error());
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
