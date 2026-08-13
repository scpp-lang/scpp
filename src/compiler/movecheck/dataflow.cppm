module;

module scpp.compiler.movecheck:dataflow;

import std;
import scpp.ast;
import :errors;
import scpp.mir;
import :state;
import :types;
import :signatures;
import :calls;
import :borrows;
import :interfaces;
import :threadsafety;
import :lambdas;

namespace scpp {

[[nodiscard]] bool binary_expr_has_compatible_types(const Expr& expr, const Body& body,
                                                    const Signatures& signatures);
[[nodiscard]] bool binary_expr_has_valid_arithmetic_types(const Expr& expr, const Body& body,
                                                          const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> check_binary_expr_operand_types(const Expr& expr, const Body& body, const Signatures& signatures,
                                     const SourceLocation& loc);
[[nodiscard]] std::optional<Type> resolve_member_field_type(const Expr& member_expr, const Body& body,
                                                            const DataflowState& state, const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> validate_deref_expr(const Expr& expr, const DataflowState& state, const Body& body,
                         const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> apply_deref(const Expr& expr, const DataflowState& state, const Body& body, const Signatures& signatures,
                 bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
                const Signatures& signatures, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_call_arguments(const Expr& expr, DataflowState& state, const Body& body,
                          const Signatures& signatures, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> apply_reference_argument(const Expr& arg, const Type& param_type, DataflowState& state,
                              BorrowMap& in_call_borrows, const Body& body,
                              const Signatures& signatures, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_constructor_arguments(const Type& constructed_type, const std::vector<ExprPtr>& ctor_args,
                                 DataflowState& state, const Body& body, const Signatures& signatures,
                                 bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> validate_increment_decrement_expr(const Expr& expr, DataflowState& state, const Body& body,
                                       const Signatures& signatures, bool report_errors);
[[nodiscard]] bool write_is_licensed_by_mutable_reborrow_lender(const Expr& target, const DataflowState& state,
                                                                const Body& body, const Signatures& signatures);
[[nodiscard]] bool is_lvalue_copy_source_shape(const Expr& expr);
[[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type,
                                                 const Body& body, const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> apply_statement(const MirStatement& stmt, DataflowState& state, const Body& body,
                     const Signatures& signatures, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_terminator(const Terminator& term, DataflowState& state, const Function& fn, const Body& body,
                      const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> check_function(const Function& fn, const Program& program, const Signatures& signatures,
                    const std::unordered_set<std::string>& class_names,
                    const ClassFieldTypes& class_field_types,
                    const ClassFieldAccess& class_field_access,
                    const std::unordered_set<std::string>& classes_with_copy_ctor,
                    const std::unordered_set<std::string>& classes_with_copy_assign,
                    const std::unordered_set<std::string>& witness_class_names);
[[nodiscard]] std::expected<void, DataflowError> check_moves_impl(const Program& program);

[[nodiscard]] bool is_wrapper_constructor_call_compatible_with_lifetime_return(const Expr& expr, const Type& target_type,
                                                                               const Body& body,
                                                                               const Signatures& signatures) {
    if (expr.kind != ExprKind::Call || expr.lhs != nullptr) return false;
    if ((expr.name == "std::reference_wrapper" || expr.name == "reference_wrapper") && expr.args.size() == 1) {
        std::optional<Type> arg_type = infer_expr_type(*expr.args[0], body, signatures);
        if (!arg_type.has_value() || !is_reference(*arg_type) || arg_type->pointee == nullptr || target_type.template_args.size() != 1) {
            return false;
        }
        const Type& wrapped = target_type.template_args[0];
        const Type& expected_referent = wrapped.template_args.size() == 1 ? wrapped.template_args[0] : wrapped;
        return types_equal(*arg_type->pointee, expected_referent) ||
               (body.program != nullptr &&
                types_compatible_with_base_conversion(*arg_type->pointee, expected_referent, *body.program,
                                                      enclosing_class_name(body)));
    }
    if ((expr.name == "std::optional" || expr.name == "optional") && target_type.template_args.size() == 1) {
        if (expr.args.empty()) return true;
        if (expr.args.size() != 1) return false;
        return is_wrapper_constructor_call_compatible_with_lifetime_return(*expr.args[0], target_type.template_args[0], body,
                                                                           signatures);
    }
    return false;
}

[[nodiscard]] const GlobalVar* find_visible_global_for_name(const std::string& name, bool explicit_global_qualification,
                                                            const Body& body) {
    if (body.program == nullptr) {
        return find_visible_global(OptionalProgramRef{}, body.function_namespace_path, name, explicit_global_qualification);
    }
    std::reference_wrapper<const Program> program_ref{*body.program};
    return find_visible_global(OptionalProgramRef{program_ref}, body.function_namespace_path, name,
                               explicit_global_qualification);
}

[[nodiscard]] std::optional<Type> find_visible_global_type(const std::string& name, bool explicit_global_qualification,
                                                           const Body& body) {
    const GlobalVar* global = find_visible_global_for_name(name, explicit_global_qualification, body);
    if (global == nullptr || global->decl == nullptr) return std::nullopt;
    return global->decl->type;
}

[[nodiscard]] bool is_visible_global_const(const std::string& name, bool explicit_global_qualification, const Body& body) {
    const GlobalVar* global = find_visible_global_for_name(name, explicit_global_qualification, body);
    return global != nullptr && global->decl != nullptr && (global->decl->is_const || global->decl->is_constexpr);
}

[[nodiscard]] bool is_string_named_type(const Type& type) {
    return type.kind == TypeKind::Named && (type.name == "std::string" || type.name == "string");
}

[[nodiscard]] bool is_const_char_pointer_type(const Type& type) {
    return type.kind == TypeKind::Pointer && type.pointee != nullptr && type.pointee->kind == TypeKind::Named &&
           type.pointee->name == "char" && !type.is_mutable_pointee;
}

[[nodiscard]] bool is_nullptr_literal_expr(const Expr& expr) {
    return expr.kind == ExprKind::NullptrLiteral;
}

[[nodiscard]] bool is_supported_compound_assignment(BinaryOp op) {
    return op == BinaryOp::AddAssign || op == BinaryOp::SubAssign || op == BinaryOp::MulAssign || op == BinaryOp::DivAssign;
}

[[nodiscard]] bool return_roots_are_proven_to_outlive_call(const RootSet& returned_roots, LocalId expected_root) {
    if (returned_roots.empty()) return false;
    for (LocalId root : returned_roots) {
        if (root == expected_root) continue;
        if (is_program_lifetime_root(root)) continue;
        return false;
    }
    return true;
}

[[nodiscard]] bool roots_are_program_lifetime_only(const RootSet& roots) {
    if (roots.empty()) return false;
    for (LocalId root : roots) {
        if (!is_program_lifetime_root(root)) return false;
    }
    return true;
}

[[nodiscard]] bool expr_is_definitely_null_pointer(const Expr& expr, const DataflowState& state, const Body& body) {
    if (is_nullptr_literal_expr(expr)) return true;
    if (expr.kind != ExprKind::Identifier || expr.explicit_global_qualification) return false;
    std::optional<LocalId> local = body.local_of(expr);
    if (!local.has_value() || body.type_of(*local).kind != TypeKind::Pointer) return false;
    auto source_it = state.local_lifetime_sources.find(*local);
    return source_it != state.local_lifetime_sources.end() && source_it->second.empty();
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

[[nodiscard]] std::string_view compound_operator_spelling(BinaryOp op) {
    switch (op) {
        case BinaryOp::AddAssign: return "+=";
        case BinaryOp::SubAssign: return "-=";
        case BinaryOp::MulAssign: return "*=";
        case BinaryOp::DivAssign: return "/=";
        default: return "?=";
    }
}

[[nodiscard]] bool is_increment_decrement_numeric_type(const Type& type) {
    return type.kind == TypeKind::Named && type.name != "bool" &&
           (is_integral_scalar_type_name(type.name) || type.name == "float" || type.name == "double" ||
            type.name == "float32_t" || type.name == "float64_t");
}

[[nodiscard]] std::expected<void, DataflowError> validate_increment_decrement_expr(const Expr& expr, DataflowState& state, const Body& body,
                                       const Signatures& signatures, bool report_errors) {
    if (auto _r = apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (!report_errors) return {};
    std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures);
    if (!operand_type.has_value()) return {};
    const Type& effective = operand_type->kind == TypeKind::Reference && operand_type->pointee != nullptr
                                ? *operand_type->pointee
                                : *operand_type;
    if (!is_increment_decrement_numeric_type(effective)) {
        return std::unexpected(DataflowError("operand of '" +
                                std::string(expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PostInc ? "++" : "--") +
                                "' must be a builtin numeric lvalue",
                            expr.loc));
    }
    auto write_roots_result = resolve_borrow_source_root(*expr.lhs, state, body, signatures, /*report_errors=*/false);
    if (!write_roots_result.has_value()) return std::unexpected(std::move(write_roots_result).error());
    RootSet write_roots = std::move(write_roots_result).value();
    if (write_roots.empty()) {
        return std::unexpected(DataflowError("operand of '" +
                                std::string(expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PostInc ? "++" : "--") +
                                "' must be an assignable place",
                            expr.loc));
    }
    if (assignment_target_is_read_only(*expr.lhs, body, signatures)) {
        return std::unexpected(DataflowError("cannot apply '" +
                                std::string(expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PostInc ? "++" : "--") +
                                "' to this place: it is reached through a read-only (const) reference",
                            expr.loc));
    }
    if (std::optional<LocalId> lender = resolve_reborrow_lender(*expr.lhs, body, signatures); lender.has_value()) {
        if (auto _r = validate_reborrow_lender_write(*lender, state, body, report_errors); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    if (!write_is_licensed_by_mutable_reborrow_lender(*expr.lhs, state, body, signatures)) {
        for (LocalId root : write_roots) {
            auto borrow_it = state.borrows.find(root);
            if (borrow_it != state.borrows.end() &&
                (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                return std::unexpected(DataflowError("cannot apply '" +
                                        std::string(expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PostInc ? "++" : "--") +
                                        "' to this place: " + format_root(body, root) + " is currently borrowed",
                                    expr.loc));
            }
        }
    }
    if (std::optional<LocalId> target = body.local_of(*expr.lhs); target.has_value()) {
        state.locals[*target] = LocalState::Initialized;
    }
    return {};
}

[[nodiscard]] bool binary_expr_has_compatible_types(const Expr& expr, const Body& body, const Signatures& signatures) {
    std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
    std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
    if (!lhs_type.has_value() || !rhs_type.has_value()) return true;
    const Type& lhs_operand = binary_operand_type(*lhs_type);
    const Type& rhs_operand = binary_operand_type(*rhs_type);
    if (types_equal(lhs_operand, rhs_operand)) return true;
    if (literal_compatible_with_type(*expr.lhs, rhs_operand) || literal_compatible_with_type(*expr.rhs, lhs_operand)) {
        return true;
    }
    return false;
}

[[nodiscard]] bool binary_expr_has_valid_arithmetic_types(const Expr& expr, const Body& body, const Signatures& signatures) {
    std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
    std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
    if (!lhs_type.has_value() || !rhs_type.has_value()) return true;
    const Type& lhs_operand = binary_operand_type(*lhs_type);
    const Type& rhs_operand = binary_operand_type(*rhs_type);
    bool pointer_operand_present = lhs_operand.kind == TypeKind::Pointer || rhs_operand.kind == TypeKind::Pointer;
    if (pointer_operand_present) {
        return pointer_arithmetic_result_type(expr.binary_op, *lhs_type, *rhs_type).has_value();
    }
    // spec §6: arithmetic operands are held to the same no-implicit-
    // conversion rule as everything else. This used to `return true`
    // unconditionally the moment neither side was a pointer, so
    // arithmetic was the one operator class never name-checked at all --
    // `int + int32_t`, `int + unsigned int` and `size_t + ptrdiff_t` all
    // went straight through, while the comparison operators just below
    // rejected the very same operand pairs. Only the *mismatch* was
    // invisible: `int + long`, whose operands lower differently, still
    // failed, but downstream in LLVM's module verifier rather than here.
    //
    // Restricted to two operands that are both actually scalars.
    // Arithmetic on a class type is dispatched through an operator
    // overload, and a still-generic parameter carries a placeholder type
    // (`$auto` for a forwarding reference, or a bare type-parameter
    // name) that only becomes real at monomorphization -- neither is a
    // scalar-conversion question, and both are answered by machinery
    // that runs elsewhere, so neither may be judged here.
    //
    // Deferring the rest to binary_expr_has_compatible_types keeps
    // arithmetic and comparison answering out of one function, so the
    // two can no longer drift apart. It carries the literal exemption
    // with it, which arithmetic needs just as much: `x + 1` must stay
    // legal for every integral `x`, not only for an `int` one.
    bool both_operands_are_scalars = lhs_operand.kind == TypeKind::Named && rhs_operand.kind == TypeKind::Named &&
                                     is_scalar_type_name(lhs_operand.name) && is_scalar_type_name(rhs_operand.name);
    if (!both_operands_are_scalars) return true;
    return binary_expr_has_compatible_types(expr, body, signatures);
}

[[nodiscard]] bool write_is_licensed_by_mutable_reborrow_lender(const Expr& target, const DataflowState& state,
                                                                const Body& body, const Signatures& signatures) {
    std::optional<LocalId> lender = resolve_reborrow_lender(target, body, signatures);
    if (!lender.has_value()) return false;
    const Type& lender_type = body.type_of(*lender);
    if (!is_reborrowable_local_type(lender_type) || !lender_type.is_mutable_ref) {
        return false;
    }
    return lookup(state.locals, *lender) == LocalState::Initialized;
}

[[nodiscard]] std::expected<void, DataflowError> validate_compound_assignment_expr(const Expr& expr, DataflowState& state, const Body& body,
                                       const Signatures& signatures, bool report_errors) {
    if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (auto _r = apply_expr(*expr.rhs, false, state, body, signatures, report_errors); !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (!report_errors) return {};
    std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
    std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
    if (!lhs_type.has_value() || !rhs_type.has_value()) return {};
    const Type& lhs_operand = binary_operand_type(*lhs_type);
    const Type& rhs_operand = binary_operand_type(*rhs_type);
    bool string_add_assign =
        expr.binary_op == BinaryOp::AddAssign && is_string_named_type(lhs_operand) &&
        (is_const_char_pointer_type(rhs_operand) || is_string_named_type(rhs_operand));
    if (!string_add_assign) {
        Expr arithmetic_check{};
        arithmetic_check.binary_op = compound_base_operator(expr.binary_op);
        arithmetic_check.lhs = deep_clone_expr(*expr.lhs);
        arithmetic_check.rhs = deep_clone_expr(*expr.rhs);
        if (auto _r = check_binary_expr_operand_types(arithmetic_check, body, signatures, expr.loc); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    if (assignment_target_is_read_only(*expr.lhs, body, signatures)) {
        return std::unexpected(DataflowError("cannot assign to this place: it is reached through a read-only (const) reference",
                            state.current_loc));
    }
    if (std::optional<LocalId> lender = resolve_reborrow_lender(*expr.lhs, body, signatures); lender.has_value()) {
        if (auto _r = validate_reborrow_lender_write(*lender, state, body, report_errors); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    bool write_through_mutable_reborrow = write_is_licensed_by_mutable_reborrow_lender(*expr.lhs, state, body, signatures);
    RootSet write_roots;
    if (std::optional<LocalId> root = direct_write_root(*expr.lhs, body)) {
        write_roots = single_root(*root);
    } else {
        auto write_roots_result = resolve_borrow_source_root(*expr.lhs, state, body, signatures, /*report_errors=*/false);
        if (!write_roots_result.has_value()) return std::unexpected(std::move(write_roots_result).error());
        write_roots = std::move(write_roots_result).value();
    }
    if (write_roots.empty()) {
        return std::unexpected(DataflowError("left operand of '" + std::string(compound_operator_spelling(expr.binary_op)) +
                                "' must be an assignable place",
                            expr.loc));
    }
    if (!write_through_mutable_reborrow) {
        for (LocalId root : write_roots) {
            auto borrow_it = state.borrows.find(root);
            if (borrow_it != state.borrows.end() &&
                (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                return std::unexpected(DataflowError("cannot assign to this place: " + format_root(body, root) +
                                                         " is currently borrowed", state.current_loc));
            }
        }
    }
    if (std::optional<LocalId> target = body.local_of(*expr.lhs); target.has_value()) {
        state.locals[*target] = LocalState::Initialized;
    }
    return {};
}

[[nodiscard]] std::expected<void, DataflowError> check_binary_expr_operand_types(const Expr& expr, const Body& body, const Signatures& signatures,
                                     const SourceLocation& loc) {
    if (expr.binary_op == BinaryOp::Assign) return {};
    if (expr.binary_op == BinaryOp::And || expr.binary_op == BinaryOp::Or) return {};
    std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
    std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
    bool lhs_is_enum = lhs_type.has_value() && is_enum_type(binary_operand_type(*lhs_type), body.program);
    bool rhs_is_enum = rhs_type.has_value() && is_enum_type(binary_operand_type(*rhs_type), body.program);
    if ((lhs_is_enum || rhs_is_enum) && expr.binary_op != BinaryOp::Eq && expr.binary_op != BinaryOp::Ne) {
        return std::unexpected(DataflowError("enum class values only support '==' and '!=' in this version", loc));
    }
    bool arithmetic_op = expr.binary_op == BinaryOp::Add || expr.binary_op == BinaryOp::Sub || expr.binary_op == BinaryOp::Mul ||
                         expr.binary_op == BinaryOp::Div;
    if (arithmetic_op) {
        if (binary_expr_has_valid_arithmetic_types(expr, body, signatures)) return {};
        if (!lhs_type.has_value() || !rhs_type.has_value()) return {};
        const Type& lhs_operand = binary_operand_type(*lhs_type);
        const Type& rhs_operand = binary_operand_type(*rhs_type);
        if (lhs_operand.kind == TypeKind::Pointer || rhs_operand.kind == TypeKind::Pointer) {
            return std::unexpected(DataflowError("pointer arithmetic requires 'pointer +/- integer' or 'pointer - pointer' with matching "
                                "non-void pointer types",
                loc));
        }
        return std::unexpected(DataflowError("binary operator requires operands of the same type; scpp has no implicit conversion between '" +
                                lhs_operand.name + "' and '" + rhs_operand.name + "' (ch06)",
            loc));
    }
    if (expr.binary_op != BinaryOp::Eq && expr.binary_op != BinaryOp::Ne && expr.binary_op != BinaryOp::Lt &&
        expr.binary_op != BinaryOp::Gt && expr.binary_op != BinaryOp::Le && expr.binary_op != BinaryOp::Ge) {
        return {};
    }
    if (binary_expr_has_compatible_types(expr, body, signatures)) return {};
    if (!lhs_type.has_value() || !rhs_type.has_value()) return {};
    const Type& lhs_operand = binary_operand_type(*lhs_type);
    const Type& rhs_operand = binary_operand_type(*rhs_type);
    return std::unexpected(DataflowError("binary operator requires operands of the same type; scpp has no implicit conversion between '" +
                            lhs_operand.name + "' and '" + rhs_operand.name + "' (ch06)",
                        loc));
}

// Resolves a `base.field` Member expression's own declared field type --
// `base` must be either a plain Identifier naming a struct/class-typed
// local or parameter (covers `this.field`, ch05 §5.12's rewritten
// captured-name access, as well as an ordinary `obj.field`) or the
// equivalent explicit `*this` spelling (`(*this).field`, ch05 §5.9).
// Anything else (a nested `a.b.c`, `arr[i].field`, ...) returns nullopt,
// left unsupported for now -- see DataflowState::class_field_types' own
// comment for why this lookup is possible at all despite movecheck's
// otherwise Body-only (no Program access) architecture.
[[nodiscard]] std::optional<Type> resolve_member_field_type(const Expr& member_expr, const Body& body,
                                                            const DataflowState& state, const Signatures& signatures) {
    if (member_expr.kind != ExprKind::Member) return std::nullopt;
    if (state.class_field_types == nullptr) return std::nullopt;
    std::optional<Type> base_type = infer_expr_type(*member_expr.lhs, body, signatures);
    if (!base_type.has_value()) return std::nullopt;
    const Type& effective = base_type->kind == TypeKind::Reference && base_type->pointee ? *base_type->pointee : *base_type;
    if (effective.kind != TypeKind::Named) return std::nullopt;
    const std::string& type_name = effective.name;
    auto class_it = state.class_field_types->find(type_name);
    if (class_it == state.class_field_types->end()) return std::nullopt;
    auto field_it = class_it->second.find(member_expr.name);
    if (field_it == class_it->second.end()) return std::nullopt;
    return field_it->second;
}

// Validates that `operand` (a plain Identifier, e.g. `p`, or a
// `base.field` Member, e.g. `this.p` -- ch05 §5.12's rewritten
// captured-name access) currently names/resolves to a readable
// pointer-like value that `*p`/`p->x` (UnaryOp::Deref) is licensed to
// dereference at this stage: a raw pointer `T*` (only while
// `state.unsafe_depth > 0`, ch01 §1.3/ch02/ch05.5), a function pointer
// being parenthesized for a call (`(*fp)(...)`), or `*this`. Class
// overloads of `operator*` are rewritten to ordinary calls earlier in the
// pipeline, so they no longer reach this raw Deref validator. A
// `base.field` Member operand has no independent move/borrow-state of its
// own to check (movecheck tracks move/borrow state per plain local, not
// per struct/class field -- there is no way to move *out of* a field in
// this version at all, matching the documented pre-existing gap), so it
// is implicitly always considered "Initialized, unborrowed" -- only its
// *type* (and, for a raw pointer, the enclosing unsafe context) is
// checked.
[[nodiscard]] std::expected<void, DataflowError> validate_deref_expr(const Expr& expr, const DataflowState& state, const Body& body,
                         const Signatures& signatures) {
    const Expr& operand = *expr.lhs;
    std::string describe = operand.kind == ExprKind::Member ? operand.lhs->name + "." + operand.name
                                                            : (operand.name.empty() ? "<expression>" : operand.name);
    std::optional<Type> resolved =
        operand.kind == ExprKind::Member ? resolve_member_field_type(operand, body, state, signatures)
                                         : [&]() -> std::optional<Type> {
            if (const Type* local_type = body.type_if_local(operand); local_type != nullptr) return *local_type;
            return find_visible_global_type(operand.name, operand.explicit_global_qualification, body);
        }();
    if (!resolved.has_value() && operand.kind != ExprKind::Identifier && operand.kind != ExprKind::Member) {
        resolved = infer_expr_type(operand, body, signatures);
    }
    const Type* underlying =
        resolved.has_value() && resolved->kind == TypeKind::Reference && resolved->pointee ? &*resolved->pointee
                                                                                            : (resolved ? &*resolved : nullptr);
    bool is_raw_ptr = resolved.has_value() && resolved->kind == TypeKind::Pointer;
    bool is_fn_ptr = resolved.has_value() && is_function_pointer(*resolved);
    bool is_class_deref =
        underlying != nullptr && underlying->kind == TypeKind::Named &&
        signatures.contains(underlying->name + "_operator_deref");
    bool is_this_ref = resolved.has_value() && operand.kind == ExprKind::Identifier && operand.name == "this" &&
                       resolved->kind == TypeKind::Reference;
    if (!is_raw_ptr && !is_fn_ptr && !is_class_deref && !is_this_ref) {
        return std::unexpected(DataflowError("cannot dereference ('*') '" + describe +
                             "': only a raw pointer (inside '[[scpp::unsafe]] { }'), a function pointer "
                             "being called, a class with operator*, or '*this' is supported here",
            state.current_loc));
    }
    if (is_raw_ptr && state.unsafe_depth == 0 && !(expr.implicit_arrow_deref && expr.implicit_arrow_chain_safe)) {
        return std::unexpected(DataflowError("cannot dereference raw pointer '" + describe +
                             "': requires '[[scpp::unsafe]] { }' (spec ch01 §1.3/ch02)",
            state.current_loc));
    }
    if (operand.kind == ExprKind::Member || operand.kind == ExprKind::Call || expr.implicit_arrow_deref) {
        // No separate per-field state for a Member (see above), and no
        // "local variable" move/borrow state at all for a Call's freshly-
        // returned pointer/reference -- its callee name lives in the same
        // Expr::name field an Identifier's variable name would, but it
        // isn't a tracked local, so looking it up in state.locals below
        // would (incorrectly) report it as "out of scope" instead of just
        // relying on the type-only checks already done above.
        return {};
    }
    std::optional<LocalId> operand_local = body.local_of(operand);
    if (!operand_local.has_value() &&
        find_visible_global_for_name(operand.name, operand.explicit_global_qualification, body) != nullptr) {
        return {};
    }
    LocalState current = operand_local.has_value() ? lookup(state.locals, *operand_local) : LocalState::Bottom;
    if (current != LocalState::Initialized) {
        return std::unexpected(DataflowError(describe_bad_state(operand.name, current),
            state.current_loc));
    }
    return {};
}

// Handles a raw-pointer/function-pointer/`*this` Deref expression used as
// a plain read (not as a borrow source -- see resolve_borrow_source_root's
// own Deref case for that). Class overloads of `operator*` are rewritten
// to ordinary calls earlier in the pipeline, so they bypass this helper
// entirely. A raw pointer has no ownership/move state of its own to
// disturb. `*this` is likewise just an explicit spelling of the receiver
// object itself (ch05 §5.9), so it behaves exactly like reading `this`.
[[nodiscard]] std::expected<void, DataflowError> apply_deref(const Expr& expr, const DataflowState& state, const Body& body, const Signatures& signatures,
                 bool report_errors) {
    if (is_explicit_star_this(expr)) {
        if (!report_errors) return {};
        return validate_deref_expr(expr, state, body, signatures);
    }
    if (expr.implicit_arrow_deref) {
        if (!report_errors) return {};
        return validate_deref_expr(expr, state, body, signatures);
    }
    bool is_plain_identifier = expr.lhs->kind == ExprKind::Identifier;
    // ch05 §5.12: `*this.p`/`*p`, where a captured raw/function pointer was
    // rewritten to a `this.p` Member access by the closure's own
    // field-access rewrite (rewrite_captured_identifiers_as_field_access)
    // -- see validate_deref_operand's own comment for why a Member operand
    // has no separate move/borrow state to check beyond its type.
    bool is_member_of_identifier =
        expr.lhs->kind == ExprKind::Member &&
        (expr.lhs->lhs->kind == ExprKind::Identifier || is_explicit_star_this(*expr.lhs->lhs));
    if (!is_plain_identifier && !is_member_of_identifier) {
        if (report_errors) {
            return std::unexpected(DataflowError("dereference ('*') currently only supports a plain local raw/function pointer "
                                 "variable, '*this', or a captured field of one ('this.field') (not a subscript "
                                 "or other expression)",
                state.current_loc));
        }
        return {};
    }
    if (!report_errors) return {}; // purely diagnostic: doesn't move p or change any tracked state
    if (auto _r = validate_deref_expr(expr, state, body, signatures); !_r.has_value()) return std::unexpected(std::move(_r).error());
    if (!is_plain_identifier) return {}; // no separate borrow-tracking key for a field -- see the comment above
    std::optional<LocalId> pointer_local = body.local_of(*expr.lhs);
    if (!pointer_local.has_value()) return {};
    const std::string& name = expr.lhs->name;
    auto borrow_it = state.borrows.find(*pointer_local);
    if (borrow_it != state.borrows.end() && borrow_it->second.mutable_borrow) {
        // `expr.lhs->loc` (the identifier `name` itself), not
        // `state.current_loc` (the enclosing `*`/Deref's own position,
        // one token earlier) -- both checks above are about `name`
        // specifically, so pointing at it directly is more precise.
        return std::unexpected(DataflowError("cannot use '" + name + "' while it is mutably borrowed",
            expr.lhs->loc));
    }
    return {};
}

// Resolves `name` to the root place its borrow-tracking should apply to.
// If `name` is itself a currently-bound reference, `ref_targets` already
// stores *its* fully-resolved root directly (every entry is written
// pre-flattened, see apply_reference_binding), so a single lookup -- not
// a manual walk -- is enough to follow a chain of reference-to-reference
// bindings (`const int& s = r;` where `r` is itself `int& r = a;`) back
// to the one real place (`a`) that must be checked/recorded for
// exclusivity. Falls back to `name` itself for an ordinary place or a
// reference *parameter* (opaque from inside this function -- there's no
// caller-side place to resolve to, so its own name is treated as its own
// root; see the Call/apply_reference_argument handling for how the
// caller-side place is checked instead, at each call site).
[[nodiscard]] std::expected<void, DataflowError> apply_reference_argument(const Expr& arg, const Type& param_type, DataflowState& state,
                               BorrowMap& in_call_borrows, const Body& body, const Signatures& signatures,
                               bool report_errors) {
    // ch05 §5.x: a *const* reference parameter bound directly to a fresh
    // rvalue argument (a literal, std::move/std::make_unique, a lambda
    // literal, or a call not itself returning by reference) binds to a
    // freshly-materialized temporary -- exactly like real C++'s own
    // temporary lifetime extension (mirrors argument_matches_parameter's
    // identical acceptance of this shape during overload resolution).
    // Never reached for a *mutable* `T&` (real C++ itself forbids binding
    // a non-const lvalue reference to a temporary). A fresh temporary
    // aliases nothing else in the entire program, so there is nothing
    // further to check here at all: just evaluate `arg` for its own side
    // effects (e.g. std::move's move-out bookkeeping) and return, skipping
    // resolve_borrow_source_root/every borrow-conflict check below
    // entirely (there is no "root" at all for a temporary).
    if (const_reference_binds_materialized_temporary(arg, param_type, body, signatures)) {
        return apply_expr(arg, /*is_move_target_context=*/arg.kind == ExprKind::Move, state, body, signatures, report_errors);
    }

    // resolve_borrow_source_root may have real (move-tracking) side
    // effects on `state` via nested apply_expr calls (e.g. a subscript
    // index) that must apply on *every* pass, not just the reporting
    // one -- unlike the rest of this function, which is purely
    // diagnostic (a call argument's borrow never outlives the call, so
    // there's nothing else here for a later statement's fixed-point
    // computation to observe).
    auto roots_result = resolve_borrow_source_root(arg, state, body, signatures, report_errors);
    if (!roots_result.has_value()) return std::unexpected(std::move(roots_result).error());
    RootSet roots = std::move(roots_result).value();
    if (!report_errors) return {};

    if (body.program != nullptr && param_type.pointee != nullptr && param_type.pointee->kind == TypeKind::Named) {
        const ClassDef* param_interface = find_class_def(*body.program, param_type.pointee->name);
        if (param_interface != nullptr && param_interface->is_interface) {
        std::optional<Type> source_type = infer_expr_type(arg, body, signatures);
        if (source_type.has_value() &&
            !types_equal(*source_type, param_type) &&
            !types_compatible_with_base_conversion(*source_type, param_type, *body.program, enclosing_class_name(body))) {
            return std::unexpected(DataflowError("cannot bind reference parameter from an incompatible source type", state.current_loc));
        }
        }
    }

    bool is_mutable = param_type.is_mutable_ref;

    // Passing an *already-bound* local reference variable directly (`f(r)`
    // where `r` is itself `T& r = ...;`/`const T& r = ...;`) is a
    // reborrow, not a fresh independent borrow: `r` already holds the one
    // live access to `root` (nothing else can coexist with it -- any
    // other attempt to borrow `root` while `r` is alive is already
    // rejected by apply_reference_binding/this same function's
    // persistent-conflict check below), so temporarily re-lending that
    // same access to a callee can't create a new conflict. Only the
    // mutability has to be checked, which validate_reborrow_lender does:
    // a shared (`const T&`) reference can't satisfy a `T&` parameter
    // (that would manufacture a mutable alias out of a shared one), but
    // a mutable reference may always be lent out as either mutable or
    // shared, and a shared one as shared.
    std::optional<LocalId> lender = resolve_reborrow_lender(arg, body, signatures);
    if (reborrow_is_tracked_against_lender(lender, body)) {
        if (auto _r = validate_reborrow_lender(*lender, is_mutable, state, body, report_errors); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    } else {
        // The general case: `arg` doesn't reach a locally-bound
        // reference/span lender at all (that case is handled above), so
        // it may instead be a `.field`/`[index]` projection rooted at an
        // owned local, or a plain *parameter* (never entered into
        // `ref_targets`, since a
        // parameter is never processed through BindReference -- see
        // apply_reference_binding) whose own declared type is `const T&`/
        // `std::span<const T>`, or a chain that dereferences a `const T*`
        // -- any of which must likewise reject manufacturing a mutable
        // reference out of a read-only one (spec ch05 §5.7's "projection
        // chain's const-reachability").
        if (is_mutable && is_read_only_reachable(arg, body, signatures)) {
            return std::unexpected(DataflowError("cannot pass " + format_roots(body, roots) + " by mutable reference: it is only reachable "
                                                          "through a read-only (const) reference",
                state.current_loc));
        }
        for (LocalId root : roots) {
            auto persistent_it = state.borrows.find(root);
            bool persistent_conflict =
                persistent_it != state.borrows.end() &&
                (is_mutable ? (persistent_it->second.mutable_borrow || persistent_it->second.shared_count > 0)
                            : persistent_it->second.mutable_borrow);
            if (persistent_conflict) {
                return std::unexpected(DataflowError("cannot pass " + format_root(body, root) + " by " + std::string(is_mutable ? "mutable " : "") +
                                        "reference: it is already borrowed",
                                    state.current_loc));
            }
        }
    }

    for (LocalId root : roots) {
        auto in_call_it = in_call_borrows.find(root);
        bool in_call_conflict =
            in_call_it != in_call_borrows.end() &&
            (is_mutable ? (in_call_it->second.mutable_borrow || in_call_it->second.shared_count > 0)
                        : in_call_it->second.mutable_borrow);
        if (in_call_conflict) {
            return std::unexpected(DataflowError("cannot pass " + format_root(body, root) + " by " + std::string(is_mutable ? "mutable " : "") +
                                    "reference more than once in the same call",
                                state.current_loc));
        }

        BorrowState& borrow = in_call_borrows[root];
        if (is_mutable) {
            borrow.mutable_borrow = true;
        } else {
            borrow.shared_count++;
        }
    }
    return {};
}

// ch04 §4.2/[expr.prim.lambda]: whether `state`'s current lexical
// position has private-member access to `target_class` -- true either
// when `target_class` is this function's own class (state.current_class,
// e.g. an ordinary method's own class, or a lambda's own synthesized
// closure class for its captured fields) or, for a lambda's synthesized
// `_call` method specifically, its lexically enclosing class
// (state.lexical_access_context_class -- see Function::
// access_context_class's own doc comment, ast.cppm, and
// DataflowState::lexical_access_context_class's). Every private-access
// check in this file (member function/constructor calls, field access)
// should go through this rather than comparing state.current_class
// directly, so a lambda body's access to *either* of its two relevant
// classes is recognized uniformly.
[[nodiscard]] bool grants_private_access(const DataflowState& state, std::string_view target_class) {
    return state.current_class == target_class ||
           (!state.lexical_access_context_class.empty() && state.lexical_access_context_class == target_class);
}

// The result of asking "may `source` initialize a by-value destination of
// class type `T` through one of `T`'s single-argument converting
// constructors?" -- the question every *value-to-declared-class-type
// boundary* has to ask, and which four separate places in this file used
// to answer differently.
//
// The boundaries are: a by-value class function parameter
// (check_call_arguments), a by-value class *constructor* parameter
// (check_constructor_arguments), a `return` operand whose function
// returns a class by value (check_terminator), and a class-typed
// variable's own initializer (check_var_decl). Only the first two of
// those consulted a converting constructor at all, so the exact same
// conversion was accepted or rejected depending purely on which of the
// four syntactic positions it appeared in:
//
//     void sink(std::string s);
//     sink("hi");                    // accepted
//     std::string make() { return "hi"; }   // accepted
//     std::string s = "hi";          // REJECTED (check_var_decl)
//     Holder h{"hi"};                // REJECTED (Holder(std::string))
//
// `std::string s = "hi";` is about as ordinary a line as exists, and
// nothing in spec §6.5-§6.7 distinguishes these positions -- the two
// rejections were drift between near-duplicate checks, not a rule. All
// four now route through this one function, so a future change to what
// counts as a converting conversion cannot apply to only some of them.
//
// `ctor` is null when no converting constructor applies (the caller then
// reports its own boundary-specific "requires a fresh value" error).
// `effective_param_type` is that constructor's own parameter type with
// the `T&&`-binding-an-rvalue normalization already applied, so callers
// can dispatch the operand the same way an ordinary argument would be.
struct ConvertingConstructorBinding {
    const FunctionSignature* ctor = nullptr;
    Type effective_param_type{};
};

[[nodiscard]] std::expected<ConvertingConstructorBinding, DataflowError> resolve_converting_constructor_binding(
    const Type& destination_type, const Expr& source, const DataflowState& state, const Body& body,
    const Signatures& signatures, bool report_errors) {
    ConvertingConstructorBinding binding;
    if (!is_named_record_type_for_call_binding(destination_type, body)) return binding;
    binding.ctor = find_single_argument_converting_constructor_signature(destination_type, source, body, signatures);
    if (binding.ctor == nullptr) return binding;
    if (report_errors && binding.ctor->is_unsafe && state.unsafe_depth == 0) {
        return std::unexpected(DataflowError("cannot use '" + destination_type.name +
                             "'s converting constructor outside '[[scpp::unsafe]] { }': its own declaration is "
                             "marked '[[scpp::unsafe]]', so its soundness depends on a precondition only the "
                             "caller can guarantee (ch01 §1.2/§1.3)",
            state.current_loc));
    }
    binding.effective_param_type = binding.ctor->param_types[1];
    if (!binding.ctor->is_generic_template && is_reference(binding.effective_param_type) &&
        binding.effective_param_type.is_rvalue_ref && binding.effective_param_type.pointee != nullptr &&
        produces_rvalue_of_type(source, *binding.effective_param_type.pointee, body, signatures)) {
        binding.effective_param_type = *binding.effective_param_type.pointee;
    }
    return binding;
}

// Why a class-typed boundary cannot accept `source`, when the reason is
// more specific than "this isn't a fresh value".
//
// `std::move(E)` is only a move when `E` is an *id-expression* (spec
// §6.2(3)); move state is recorded per named object, so a member,
// element, or other projection has nowhere to record it and is rejected
// (apply_expr's own ExprKind::Move case says exactly that). But three of
// the four class-value boundaries never got that far: they tested
// `produces_rvalue_of_type` first, which quietly answers "no" for
// `std::move(obj.field)`, and reported the generic
// "...requires ... a fresh value such as std::move(x)" instead -- a
// message that advises precisely what the reader already wrote. Give the
// real reason wherever the boundary is the first to notice.
[[nodiscard]] std::optional<std::string> explain_unusable_class_value_source(const Expr& source) {
    if (source.kind != ExprKind::Move || source.lhs == nullptr) return std::nullopt;
    if (source.lhs->kind == ExprKind::Identifier) return std::nullopt;
    return std::string("std::move currently only supports a plain local variable "
                       "(not a member, subscript, or other expression)");
}

// Checks every argument of a Call expression against its callee's
// signature (if known), exactly the same way regardless of context --
// shared by apply_expr's own Call case (a call used as a plain
// statement or value sub-expression) and resolve_borrow_source_root's
// Call case below (a call to a reference-returning function used
// itself as a further reference-binding source). Also the single place
// (reached from every Call site) that enforces ch02/ch05.5's "calling an
// `extern \"C\"` function requires `unsafe {}`" rule (ch01 §1.3):
// rejected only when the callee is *known* (an unresolved/unknown callee
// name is left to codegen's own "call to unknown function" check, same
// treatment as elsewhere in this file) and is an `extern "C"`
// declaration, and the call site itself isn't currently inside an
// `unsafe { }` block (state.unsafe_depth > 0 -- see check_function's
// entry_state setup and DataflowState::unsafe_depth). Every other
// callee -- an ordinary scpp function or a bare `extern` (ch11 §11.6)
// declaration alike -- is checked by default (ch01) and needs no
// `unsafe {}` to call at all. print_int/print_bool and other
// codegen-only builtins are never in `signatures` at all, so they're
// always callable regardless of context, same as they already bypass
// every other signature-based check in this file.
[[nodiscard]] std::expected<void, DataflowError> check_call_arguments(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                           bool report_errors) {
    // A method call's receiver (`obj.method(...)`/`this->method(...)`,
    // stored in `expr.lhs`, never part of `expr.args` -- see
    // CalleeSignature's own comment) is an ordinary read of `obj` and was
    // previously never visited at all here, unlike the identical
    // receiver sub-expression on a field access (ExprKind::Member's own
    // `apply_expr(*expr.lhs, ...)` call) -- a real, discovered-and-fixed
    // gap: calling *any* method (a mutating one or a read-only `const`
    // getter alike) on a moved-out class-typed variable went entirely
    // unchecked, even though reading one of its fields directly was
    // already correctly rejected. Also covers an IIFE's lambda literal
    // receiver (`[capture](args){...}(...)`, ExprKind::Lambda -- see
    // resolve_callee_signature's own comment): this is also the only
    // place that would otherwise ever visit that literal at all when it
    // is called immediately rather than bound to a variable first, so
    // this fix incidentally makes an IIFE's own captures subject to the
    // same checking (apply_expr's Lambda case, which calls
    // apply_lambda_captures) that a stored closure already got via the
    // VarDecl case in apply_statement -- previously entirely unchecked
    // too (e.g. an IIFE could init-capture an already-moved-out
    // std::unique_ptr without error).
    std::optional<Type> direct_call_type = expr.lhs == nullptr ? infer_expr_type(expr, body, signatures) : std::nullopt;
    if (!expr.lhs && direct_call_type.has_value() && direct_call_type->kind == TypeKind::Named &&
        state.class_names != nullptr && state.class_names->contains(expr.name)) {
        return check_constructor_arguments(*direct_call_type, expr.args, state, body, signatures, report_errors);
    }
    CalleeSignature callee = resolve_callee_signature(expr, body, signatures, state.class_field_types);
    auto name_it = signatures.find(callee.key);
    const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
    if (expr.lhs) {
        bool receiver_is_reference =
            sig != nullptr && !sig->param_types.empty() && is_reference(sig->param_types[0]) && !sig->param_types[0].is_rvalue_ref;
        (void)receiver_is_reference;
        if (auto _r = apply_expr(*expr.lhs, /*is_move_target_context=*/expr.lhs->kind == ExprKind::Move, state, body, signatures,
                   report_errors); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    std::string callee_display = expr.name;
    if (callee_display.empty()) {
        if (expr.lhs && expr.lhs->kind == ExprKind::Identifier) {
            callee_display = expr.lhs->name;
        } else {
            callee_display = "<function pointer>";
        }
    }
    // ch05 §5.10: a name that exists but has no overload whose parameters
    // match this call's actual arguments is a hard error (an explicit
    // cast/a genuinely matching overload is required) -- distinct from
    // "the name doesn't exist at all", which this function has never
    // rejected itself (left to codegen's own "call to unknown function"
    // check; preserved here unchanged).
    if (report_errors && name_it != signatures.end() && sig == nullptr) {
        if (find_const_blocked_method_candidate(expr, callee, body, signatures) != nullptr) {
            return std::unexpected(DataflowError("cannot call non-const member function '" + callee_display +
                                    "' through a read-only (const) receiver",
                state.current_loc));
        }
        return std::unexpected(DataflowError(
            describe_overload_failure(expr, callee, callee_display, name_it->second, body, signatures),
            state.current_loc));
    }
    if (report_errors && sig != nullptr && sig->access == AccessSpecifier::Private &&
        !sig->member_owner_class.empty() && !grants_private_access(state, sig->member_owner_class)) {
        return std::unexpected(DataflowError("cannot call private member function '" + callee_display + "' of class '" +
                             sig->member_owner_class + "' from outside its own methods",
            state.current_loc));
    }
    if (report_errors && sig != nullptr && sig->is_extern_c_declaration_only && state.unsafe_depth == 0) {
        return std::unexpected(DataflowError("cannot call 'extern \"C\"' function '" + callee_display +
                             "' outside '[[scpp::unsafe]] { }': no scpp compiler ever sees its real "
                             "implementation to check it (spec ch01 §1.3/ch02)",
            state.current_loc));
    }
    if (report_errors && sig != nullptr && sig->is_unsafe && state.unsafe_depth == 0) {
        return std::unexpected(DataflowError("cannot call '" + callee_display +
                             "' outside '[[scpp::unsafe]] { }': its own declaration is marked "
                             "'[[scpp::unsafe]]', so its soundness depends on a precondition only the "
                             "caller can guarantee (ch01 §1.2/§1.3)",
            state.current_loc));
    }
    // Scratch borrow-map shared by every reference argument of *this*
    // call only (see apply_reference_argument) -- never merged into
    // `state`, since none of these transient borrows outlive the call.
    BorrowMap in_call_borrows;
    auto apply_one_argument = [&](const Expr& arg, std::size_t param_index) -> std::expected<void, DataflowError> {
        Type effective_param_type;
        bool have_effective_param_type = false;
        if (sig != nullptr && param_index < sig->param_types.size()) {
            effective_param_type = sig->param_types[param_index];
            if (!sig->member_owner_class.empty() && !sig->is_generic_template &&
                param_index < sig->param_is_forwarding_reference.size() &&
                sig->param_is_forwarding_reference[param_index]) {
                effective_param_type.is_rvalue_ref = false;
                effective_param_type.is_mutable_ref = !is_read_only_reachable(arg, body, signatures);
                if (effective_param_type.pointee != nullptr) {
                    effective_param_type.pointee->is_const_qualified = is_read_only_reachable(arg, body, signatures);
                }
            }
            if (param_index < sig->param_is_forwarding_reference.size() && sig->param_is_forwarding_reference[param_index] &&
                is_reference(effective_param_type) && effective_param_type.pointee != nullptr &&
                !produces_rvalue_of_type(arg, *effective_param_type.pointee, body, signatures)) {
                effective_param_type.is_rvalue_ref = false;
                effective_param_type.is_mutable_ref = !is_read_only_reachable(arg, body, signatures);
                effective_param_type.pointee->is_const_qualified = is_read_only_reachable(arg, body, signatures);
            }
            have_effective_param_type = true;
        }
        bool param_is_reference = have_effective_param_type && is_reference(effective_param_type);
        bool param_is_rvalue_reference = param_is_reference && effective_param_type.is_rvalue_ref;
        if (param_is_rvalue_reference) {
            // ch03/ch05 §5.11: once a parameter is still a genuine
            // rvalue-reference *after* any forwarding-reference collapse,
            // it is an ownership-transfer argument, not a borrow: needs a
            // genuine rvalue (see produces_rvalue_of_type), never
            // apply_reference_argument's place-borrow bookkeeping. Still
            // walked via apply_expr (exactly like a by-value/unique_ptr
            // argument below) for its own side effects -- e.g.
            // std::move(x) marking x moved-out in `state`.
            if (report_errors && !produces_rvalue_of_type(arg, *effective_param_type.pointee, body, signatures)) {
                return std::unexpected(DataflowError(
                    "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                    "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                    "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                    state.current_loc));
            }
            if (auto _r = apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        } else if (param_is_reference) {
            if (auto _r = apply_reference_argument(arg, effective_param_type, state, in_call_borrows, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        } else {
            if (report_errors && sig != nullptr && param_index < sig->param_types.size() &&
                sig->param_types[param_index].kind == TypeKind::Pointer) {
                std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
                auto unwrap_pointer_pointee = [](const Type& type) -> const Type* {
                    if (type.kind != TypeKind::Pointer || type.pointee == nullptr) return nullptr;
                    if (type.pointee->kind == TypeKind::Reference && type.pointee->pointee) return &*type.pointee->pointee;
                    return &*type.pointee;
                };
                const Type* arg_pointee = arg_type.has_value() ? unwrap_pointer_pointee(*arg_type) : nullptr;
                const Type* param_pointee = unwrap_pointer_pointee(sig->param_types[param_index]);
                bool needs_class_pointer_validation =
                    body.program != nullptr && arg_pointee != nullptr && param_pointee != nullptr &&
                    arg_pointee->kind == TypeKind::Named && param_pointee->kind == TypeKind::Named &&
                    (find_class_def(*body.program, arg_pointee->name) != nullptr ||
                     find_class_def(*body.program, param_pointee->name) != nullptr);
                // ch05 §5.14: a call through an *external* same-generic-
                // class-typed parameter (e.g. `target.__insert_new_entry
                // (fresh)`, `target: std::unordered_map<K, V>&` alongside
                // `this` inside `unordered_map<K, V>::__append_all_to` --
                // see that method's own comment in std_unordered_map.scpp)
                // never gets its callee resolved against `target`'s own,
                // properly witness-substituted per-method synthetic
                // check-class the way `this->` calls do (only `this`'s
                // type is rewritten in check_generic_type_methods_once);
                // it falls back to the *original*, un-substituted
                // template's registered signature instead (still literally
                // typed e.g. `unordered_map_entry<K, V>*`), while `fresh`'s
                // own inferred type is now the real, witness-resolved
                // instantiation (resolve_generic_type_optimistic having
                // done its job correctly on it) -- a mismatch with no
                // bearing on any real, concrete instantiation of this
                // same method (whose own external parameter *is* the
                // correctly monomorphized, matching concrete type, via
                // the ordinary get_or_create_clone/instantiate_generic_type
                // path), so tolerated here the same way as every other
                // is_synthetic_check_only-only false positive in this
                // file.
                bool caller_is_synthetic_check_only = [&] {
                    if (body.program == nullptr) return false;
                    std::string owner_name = enclosing_class_name(body);
                    if (owner_name.empty()) return false;
                    const ClassDef* owner_def = find_class_def(*body.program, owner_name);
                    return owner_def != nullptr && owner_def->is_synthetic_check_only;
                }();
                if (needs_class_pointer_validation && !caller_is_synthetic_check_only && arg_type->kind == TypeKind::Pointer &&
                    !raw_pointer_implicitly_convertible(*arg_type, sig->param_types[param_index]) &&
                    !types_compatible_with_base_conversion(*arg_type, sig->param_types[param_index], *body.program,
                                                           enclosing_class_name(body))) {
                    return std::unexpected(DataflowError("cannot pass an incompatible pointer type to parameter '" +
                                            sig->param_names[param_index] + "'",
                                        state.current_loc));
                }
            }
            bool class_value_param =
                sig != nullptr && param_index < sig->param_types.size() &&
                is_named_record_type_for_call_binding(sig->param_types[param_index], body);
            bool copyable_lvalue_source =
                class_value_param && is_copyable_class_lvalue_boundary_source(arg, sig->param_types[param_index], body, signatures);
            bool freely_copyable_value_source =
                class_value_param && is_freely_copyable_class_value_source(arg, sig->param_types[param_index], body, signatures);
            const FunctionSignature* converting_ctor = nullptr;
            Type converting_ctor_param_type{};
            if (class_value_param) {
                auto binding = resolve_converting_constructor_binding(sig->param_types[param_index], arg, state, body, signatures,
                                                                     report_errors);
                if (!binding.has_value()) return std::unexpected(std::move(binding).error());
                converting_ctor = binding->ctor;
                converting_ctor_param_type = binding->effective_param_type;
            }
            if (report_errors && class_value_param && !copyable_lvalue_source && !freely_copyable_value_source &&
                !produces_rvalue_of_type(arg, sig->param_types[param_index], body, signatures) && converting_ctor == nullptr) {
                if (std::optional<std::string> why = explain_unusable_class_value_source(arg); why.has_value()) {
                    return std::unexpected(DataflowError(*why, state.current_loc));
                }
                return std::unexpected(DataflowError("passing class '" + sig->param_types[param_index].name +
                                     "' by value requires either an implicitly copyable same-type source or "
                                     "a fresh value such as std::move(x) or a call returning by value",
                    state.current_loc));
            }
            if (converting_ctor != nullptr) {
                Type ctor_param_type = converting_ctor_param_type;
                if (is_reference(ctor_param_type) && ctor_param_type.is_rvalue_ref) {
                    if (report_errors && !produces_rvalue_of_type(arg, *ctor_param_type.pointee, body, signatures)) {
                        return std::unexpected(DataflowError(
                            "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                            "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                            "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                            state.current_loc));
                    }
                    if (auto _r = apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else if (is_reference(ctor_param_type)) {
                    if (auto _r = apply_reference_argument(arg, ctor_param_type, state, in_call_borrows, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else {
                    if (auto _r = apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
                if (report_errors && sig != nullptr) {
                    if (auto _r = enforce_thread_safety_constraints_for_argument(arg, *sig, param_index, "function", callee_display, body,
                                                                   signatures, state.current_loc);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
                return {};
            }
            if (auto _r = apply_expr(arg, /*is_move_target_context=*/!(copyable_lvalue_source || freely_copyable_value_source), state,
                       body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }

            // `&expr` (ch05 §5.7) passed directly as a call argument --
            // the primary motivating use case (an `extern "C"` out
            // parameter, e.g. `getsockopt(..., &value, &len)`). If the
            // declared parameter wants a *mutable* `T*` but `expr`'s
            // place is only reachable read-only, reject: same
            // const-widens-only-one-way rule as everywhere else in this
            // version (`const T*` never converts to `T*`, so there is no
            // way to legitimately satisfy a mutable-pointee parameter
            // here). Scoped to exactly this direct syntactic shape (not a
            // general type-checker): a raw pointer value that has already
            // passed through some other variable/call has no
            // "reachability" left to check -- only its own already-
            // enforced declared constness matters by then (see
            // assignment_target_is_read_only's Unary case).
            bool param_wants_mutable_pointer =
                sig != nullptr && param_index < sig->param_types.size() &&
                sig->param_types[param_index].kind == TypeKind::Pointer &&
                sig->param_types[param_index].is_mutable_pointee;
            if (report_errors && param_wants_mutable_pointer && arg.kind == ExprKind::Unary &&
                arg.unary_op == UnaryOp::AddressOf && is_read_only_reachable(*arg.lhs, body, signatures)) {
                return std::unexpected(DataflowError("cannot pass '&' of a read-only-reachable place as a mutable 'T*' "
                                    "argument (would need 'const T*', which this parameter doesn't accept)",
                    state.current_loc));
            }
        }
        if (report_errors && sig != nullptr) {
            if (auto _r = enforce_thread_safety_constraints_for_argument(arg, *sig, param_index, "function", callee_display, body,
                                                           signatures, state.current_loc);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        return {};
    };
    for (std::size_t i = 0; i < expr.args.size(); i++) {
        if (auto _r = apply_one_argument(*expr.args[i], i + callee.param_offset); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    if (sig != nullptr) {
        for (std::size_t param_index = expr.args.size() + callee.param_offset; param_index < sig->param_types.size();
             param_index++) {
            if (sig->param_default_exprs[param_index] == nullptr) break;
            ExprPtr default_arg = deep_clone_expr_with_loc(*sig->param_default_exprs[param_index], state.current_loc);
            if (auto _r = apply_one_argument(*default_arg, param_index); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
    }
    return {};
}

// ch04 §4.2 / spec §6.1: checks every argument of a
// `ClassName name{args};`
// constructor-call VarDecl (Stmt::ctor_args) -- mirrors
// check_call_arguments' own per-argument dataflow processing/validation
// (reference-argument borrowing, rvalue-reference genuine-rvalue
// requirement, unsafe-constructor gating) exactly, just resolved against
// `class_name + "_new"`'s own signature(s) instead of an ordinary Call
// expression's callee, since a constructor-call VarDecl has no wrapping
// Call Expr of its own to hand to resolve_callee_signature/resolve_
// overload (Stmt::ctor_args is a bare argument list). `param_offset` is
// always 1 (the implicit `this`, exactly like any other method --
// see make_this_param), and the receiver is unconditionally treated as
// fully mutable (a freshly-constructed object always accepts a mutable
// `this` -- there's no *existing* object yet for read-only-reachability
// to apply to, mirroring codegen's own resolve_overload_by_type default).
// Previously, constructor arguments were entirely invisible to the
// dataflow checker (a has_ctor_args VarDecl lowered to a bare,
// argument-blind Declare, see mir.cppm) -- e.g. `Holder h{std::move(p)};`
// never marked `p` moved-out at all. Multiple candidates matching by
// argument count alone generally leave `sig` null, exactly like
// resolve_overload's own "let a more specific, later check report it"
// pattern -- except for zero-argument/default-brace construction, which
// must be diagnosed here as "no default constructor" rather than
// slipping through to codegen and crashing LLVM module verification.
[[nodiscard]] std::expected<void, DataflowError> check_constructor_arguments(const Type& constructed_type, const std::vector<ExprPtr>& ctor_args,
                                  DataflowState& state, const Body& body, const Signatures& signatures,
                                  bool report_errors) {
    std::string class_name = constructed_type.name;
    std::string ctor_name = class_name + "_new";
    auto is_constructor_clone_name = [&](std::string_view name) {
        return name == ctor_name || (!name.empty() && name.starts_with(ctor_name + "."));
    };
    const FunctionSignature* sig = nullptr;
    std::vector<const FunctionSignature*> constructor_candidates;
    for (const auto& [name, overloads] : signatures) {
        if (!is_constructor_clone_name(name)) continue;
        for (const FunctionSignature& candidate : overloads) {
            if (candidate.member_owner_class != class_name) continue;
            constructor_candidates.push_back(&candidate);
        }
    }
    if (!constructor_candidates.empty()) {
        std::vector<const FunctionSignature*> visible_arity_matches;
        for (const FunctionSignature* candidate : constructor_candidates) {
            if (!compile_time_dependency_visible_in_body(*candidate, body)) continue;
            if (!function_signature_accepts_argument_count(*candidate, ctor_args.size(), 1)) continue;
            visible_arity_matches.push_back(candidate);
        }
        if (visible_arity_matches.size() == 1) {
            sig = visible_arity_matches[0];
        }
        std::vector<const FunctionSignature*> matches;
        if (sig == nullptr) {
            for (const FunctionSignature* candidate : constructor_candidates) {
                if (!compile_time_dependency_visible_in_body(*candidate, body)) continue;
                if (!function_signature_accepts_argument_count(*candidate, ctor_args.size(), 1)) continue;
                bool all_match = true;
                for (std::size_t i = 0; all_match && i < ctor_args.size(); i++) {
                    all_match = argument_matches_parameter_for_constructor_selection(*ctor_args[i],
                                                                                     candidate->param_types[i + 1], body,
                                                                                     signatures);
                }
                if (all_match) matches.push_back(candidate);
            }
            if (matches.size() == 1) sig = matches[0];
            // ch05 §5.10: an *exact* argument-type match is strictly more
            // specific than merely "already monomorphized", so it is tried
            // first. Trying the concrete-candidate fallback below first
            // instead made the choice depend on the order `signatures`
            // (an unordered_map) happens to enumerate its overloads in --
            // e.g. `std::move_only_function<int(int)> f{Adder(2)};` picked
            // whichever of that class's own already-instantiated
            // constructor clones came out of the hash table first, which
            // could be the `move_only_function<int(int)>` one rather than
            // the `Adder` one, and changed when an unrelated `std` module
            // partition was added.
            if (sig == nullptr && !matches.empty()) {
                auto exact_type_match = [&](const FunctionSignature* candidate) {
                    for (std::size_t i = 0; i < ctor_args.size(); i++) {
                        std::optional<Type> arg_type = infer_expr_type(*ctor_args[i], body, signatures);
                        if (!arg_type.has_value()) return false;
                        if (!types_equal(*arg_type, candidate->param_types[i + 1])) return false;
                    }
                    return true;
                };
                for (const FunctionSignature* candidate : matches) {
                    if (exact_type_match(candidate)) {
                        sig = candidate;
                        break;
                    }
                }
            }
            if (sig == nullptr) {
                for (const FunctionSignature* candidate : matches) {
                    if (!candidate->is_generic_template) {
                        sig = candidate;
                        break;
                    }
                }
            }
        }
    }
    if (sig == nullptr && report_errors && ctor_args.empty()) {
        static const std::vector<ExprPtr> no_ctor_args;
        if (body.program != nullptr && !class_has_any_constructor(class_name, *body.program)) {
            if (auto _r = ensure_implicit_default_construction_is_valid(class_name, state.current_class, body, signatures,
                                                          state.current_loc,
                                                          "implicit default construction of class '" + class_name +
                                                              "' is ill-formed");
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            return {};
        }
        sig = resolve_constructor_signature(class_name, no_ctor_args, body, signatures);
        if (sig == nullptr) {
            return std::unexpected(DataflowError("type '" + class_name +
                                    "' has no default constructor; no constructor of '" + class_name +
                                    "' matches 0 arguments",
                                state.current_loc));
        }
    }
    if (report_errors && sig != nullptr && sig->access == AccessSpecifier::Private &&
        !sig->member_owner_class.empty() && !grants_private_access(state, sig->member_owner_class)) {
        return std::unexpected(DataflowError("cannot call private constructor of class '" + class_name +
                             "' from outside its own methods",
            state.current_loc));
    }
    if (report_errors && sig != nullptr && sig->is_unsafe && state.unsafe_depth == 0) {
        return std::unexpected(DataflowError("cannot call '" + class_name +
                             "'s constructor outside '[[scpp::unsafe]] { }': its own declaration is marked "
                             "'[[scpp::unsafe]]', so its soundness depends on a precondition only the "
                             "caller can guarantee (ch01 §1.2/§1.3)",
            state.current_loc));
    }
    BorrowMap in_call_borrows;
    bool constructed_state_can_carry_lifetimes =
        report_errors && body.program != nullptr &&
        type_contains_lifetime_carrying_state(constructed_type, *body.program) &&
        !constructed_type.is_reference_wrapper_lifetime_source;
    auto apply_one_argument = [&](const Expr& arg, std::size_t param_index) -> std::expected<void, DataflowError> {
        Type effective_param_type;
        bool have_effective_param_type = false;
        const Type* destination_type = &constructed_type;
        if (sig != nullptr && param_index < sig->param_types.size()) {
            effective_param_type = sig->param_types[param_index];
            if (param_index < sig->param_is_forwarding_reference.size() && sig->param_is_forwarding_reference[param_index] &&
                is_reference(effective_param_type) && effective_param_type.pointee != nullptr &&
                !produces_rvalue_of_type(arg, *effective_param_type.pointee, body, signatures)) {
                effective_param_type.is_rvalue_ref = false;
                effective_param_type.is_mutable_ref = !is_read_only_reachable(arg, body, signatures);
            }
            have_effective_param_type = true;
            destination_type = &sig->param_types[param_index];
        }
        if (constructed_state_can_carry_lifetimes) {
            if (auto _r = reject_lifetime_group_state_embedding(arg, state, body, signatures, report_errors, "constructed object state",
                                                  destination_type);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        bool param_is_reference = have_effective_param_type && is_reference(effective_param_type);
        bool param_is_rvalue_reference = param_is_reference && effective_param_type.is_rvalue_ref;
        bool allow_temporary_reference_binding =
            param_is_reference && !effective_param_type.is_mutable_ref &&
            const_reference_binds_materialized_temporary(arg, effective_param_type, body, signatures);
        if (param_is_rvalue_reference) {
            if (report_errors &&
                !produces_rvalue_of_type(arg, *effective_param_type.pointee, body, signatures)) {
                return std::unexpected(DataflowError(
                    "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                    "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                    "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                    state.current_loc));
            }
            if (auto _r = apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        } else if (param_is_reference && allow_temporary_reference_binding) {
            if (auto _r = apply_expr(arg, /*is_move_target_context=*/arg.kind == ExprKind::Move, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        } else if (param_is_reference) {
            if (auto _r = apply_reference_argument(arg, effective_param_type, state, in_call_borrows, body, signatures,
                                      report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        } else {
            bool class_value_param =
                sig != nullptr && param_index < sig->param_types.size() &&
                is_named_record_type_for_call_binding(sig->param_types[param_index], body);
            bool copyable_lvalue_source =
                class_value_param && is_copyable_class_lvalue_boundary_source(arg, sig->param_types[param_index], body, signatures);
            bool freely_copyable_value_source =
                class_value_param && is_freely_copyable_class_value_source(arg, sig->param_types[param_index], body, signatures);
            if (arg.kind == ExprKind::Lambda) freely_copyable_value_source = class_value_param;
            const FunctionSignature* converting_ctor = nullptr;
            Type converting_ctor_param_type{};
            if (class_value_param) {
                auto binding = resolve_converting_constructor_binding(sig->param_types[param_index], arg, state, body, signatures,
                                                                     report_errors);
                if (!binding.has_value()) return std::unexpected(std::move(binding).error());
                converting_ctor = binding->ctor;
                converting_ctor_param_type = binding->effective_param_type;
            }
            if (report_errors && class_value_param && !copyable_lvalue_source && !freely_copyable_value_source &&
                !produces_rvalue_of_type(arg, sig->param_types[param_index], body, signatures) && converting_ctor == nullptr) {
                if (std::optional<std::string> why = explain_unusable_class_value_source(arg); why.has_value()) {
                    return std::unexpected(DataflowError(*why, state.current_loc));
                }
                return std::unexpected(DataflowError("passing class '" + sig->param_types[param_index].name +
                                     "' by value requires either an implicitly copyable same-type source or "
                                     "a fresh value such as std::move(x) or a call returning by value",
                    state.current_loc));
            }
            if (converting_ctor != nullptr) {
                if (is_reference(converting_ctor_param_type) && converting_ctor_param_type.is_rvalue_ref) {
                    if (report_errors &&
                        !produces_rvalue_of_type(arg, *converting_ctor_param_type.pointee, body, signatures)) {
                        return std::unexpected(DataflowError(
                            "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                            "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                            "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                            state.current_loc));
                    }
                    if (auto _r = apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else if (is_reference(converting_ctor_param_type)) {
                    if (auto _r = apply_reference_argument(arg, converting_ctor_param_type, state, in_call_borrows, body,
                                                          signatures, report_errors);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else {
                    if (auto _r = apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
            } else if (auto _r = apply_expr(arg, /*is_move_target_context=*/!(copyable_lvalue_source || freely_copyable_value_source), state,
                       body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        if (report_errors && sig != nullptr) {
            if (auto _r = enforce_thread_safety_constraints_for_argument(arg, *sig, param_index, "constructor", class_name, body,
                                                           signatures, state.current_loc);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        return {};
    };
    for (std::size_t i = 0; i < ctor_args.size(); i++) {
        if (auto _r = apply_one_argument(*ctor_args[i], i + 1); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    if (sig != nullptr) {
        for (std::size_t param_index = ctor_args.size() + 1; param_index < sig->param_types.size(); param_index++) {
            if (sig->param_default_exprs[param_index] == nullptr) break;
            ExprPtr default_arg = deep_clone_expr_with_loc(*sig->param_default_exprs[param_index], state.current_loc);
            if (auto _r = apply_one_argument(*default_arg, param_index); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
    }
    return {};
}

// Walks `expr`, updating `state` for any std::move / assignment / borrow
// side effects and, when `report_errors` is true, throwing DataflowError
// on an unsafe read. `is_move_target_context` is true exactly where
// a bare `std::move(x)` is allowed to appear: a var-decl initializer, an
// assignment RHS, a return value, a call argument, a constructor-call
// argument, or a by-value class lambda capture initializer (ch04 §4.2 --
// see check_constructor_arguments and apply_lambda_captures). ch04
// §4.2/ch05 §5.15/spec §6.4: `std::move(x)` is legitimate here for any
// class-typed variable -- move construction/assignment for `class` types
// is always the compiler-provided memberwise operation (never
// user-written, spec §6.4(1)), so there is no additional per-class
// validation to do here beyond the ordinary move-state bookkeeping every
// movable type already gets.
//
// This function is run twice per program point: once during the
// worklist's fixed-point iteration (report_errors=false, just to compute
// stable per-block states) and once more in the final reporting pass
// (report_errors=true). Both runs must apply the *same* state mutations so
// the two phases stay consistent.
[[nodiscard]] std::expected<void, DataflowError> apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
                 const Signatures& signatures, bool report_errors) {
    // Refreshed on every call (including each recursive call for a child
    // sub-expression) so that, by the time *this* node's own checks run
    // (whether before any recursive call, like the Identifier case just
    // below, or after one, like Member's access-control check further
    // down), `state.current_loc` reliably points at `expr` itself rather
    // than whatever child was most recently visited -- see
    // DataflowState::current_loc.
    state.current_loc = expr.loc;
    switch (expr.kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::NullptrLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
            return {};

        case ExprKind::Sizeof:
            if (report_errors) {
                if (auto _r = validate_sizeof_operand(expr, body, signatures, state.current_loc); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            return {};
        case ExprKind::Alignof:
            if (report_errors) {
                if (auto _r = validate_alignof_operand(expr, body, state.current_loc); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            return {};
        case ExprKind::ValueInit:
            // `return {};` (ast.cppm's ExprKind::ValueInit) value-
            // initializes `expr.type` (filled in by monomorphization from
            // the enclosing function's own return type -- see
            // Monomorphizer::walk_expr) with zero constructor arguments,
            // so it needs exactly the same "no default constructor"
            // validation as an empty-braced `Type var{};` VarDecl.
            if (report_errors) {
                if (auto _r = check_constructor_arguments(expr.type, {}, state, body, signatures, report_errors); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            return {};

        case ExprKind::Identifier: {
            if (!report_errors) return {};
            std::optional<LocalId> local = body.local_of(expr);
            if (!local.has_value()) {
                return {}; // a global, or an unknown name left to codegen's own check
            }
            LocalState current = lookup(state.locals, *local);
            if (current != LocalState::Initialized) {
                return std::unexpected(DataflowError(describe_bad_state(expr.name, current),
                    state.current_loc));
            }
            // A direct read is rejected if `expr.name` is currently
            // serving as the root of an active *mutable* borrow --
            // alias-XOR-mutability means only that borrow's own
            // reference may access it while live. Note `state.borrows`
            // is naturally keyed only by roots: a reference that itself
            // borrows something (e.g. `r` in `int& r = a;`) is never a
            // key here (its borrow is recorded, pre-flattened, against
            // `a`; see resolve_root_place), so reading `r` by its own
            // name to go *through* the very borrow it holds is never
            // blocked by this check -- only reading the aliased root
            // directly (`a`, or an opaque reference parameter that some
            // other local reference borrows from) is.
            auto borrow_it = state.borrows.find(*local);
            if (borrow_it != state.borrows.end() && borrow_it->second.mutable_borrow) {
                return std::unexpected(DataflowError("cannot use '" + expr.name + "' while it is mutably borrowed",
                    state.current_loc));
            }
            return {};
        }

        case ExprKind::Move: {
            if (expr.lhs->kind != ExprKind::Identifier) {
                if (report_errors) {
                    return std::unexpected(DataflowError("std::move currently only supports a plain local variable "
                                         "(not a member, subscript, or other expression)",
                        state.current_loc));
                }
                return {};
            }
            const std::string& name = expr.lhs->name;
            // spec §6.2(3): `std::move(E)` is a syntactic ownership-state
            // transition on any named object `E`, not just on class types.
            // The same named-object rule already covers an rvalue-
            // reference local/parameter (`Inner&& i`, ch03/ch05 §5.11):
            // `i` itself is still a name, and moving from it marks that
            // local/parameter moved-out exactly like any other local name.
            std::optional<LocalId> moved = body.local_of(*expr.lhs);
            if (!moved.has_value()) {
                if (find_visible_global_for_name(name, expr.lhs->explicit_global_qualification, body) != nullptr) {
                    return {};
                }
                if (report_errors) {
                    return std::unexpected(DataflowError("unknown variable '" + name + "'",
                        state.current_loc));
                }
                return {};
            }
            LocalState current = lookup(state.locals, *moved);
            if (report_errors && current != LocalState::Initialized) {
                return std::unexpected(DataflowError(describe_bad_state(name, current),
                    state.current_loc));
            }
            if (report_errors) {
                auto borrow_it = state.borrows.find(*moved);
                if (borrow_it != state.borrows.end() &&
                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                    return std::unexpected(DataflowError("cannot move '" + name + "' while it is borrowed",
                        state.current_loc));
                }
            }
            state.locals[*moved] = LocalState::MovedOut;
            if (report_errors && !is_move_target_context) {
                return std::unexpected(DataflowError("std::move(" + name + ") must be used to initialize, assign into, return, "
                                                            "pass, or capture a value",
                    state.current_loc));
            }
            return {};
        }

        // `static_cast<T>(expr)`/`(T)expr` (ch06 §6): visits the operand
        // for its own move/borrow bookkeeping exactly like any other
        // sub-expression (never itself a move-target-context -- a cast
        // reads its operand's value, it doesn't take ownership of it),
        // then validates the (source, target) pair is actually a legal
        // conversion in this version: scalar-to-scalar (always) or
        // raw-pointer-to-raw-pointer only inside an unsafe context
        // (spec §5.1(5.2)).
        case ExprKind::Cast: {
            if (auto _r = apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (report_errors) {
                std::optional<Type> source_type = infer_expr_type(*expr.lhs, body, signatures);
                if ((source_type.has_value() && is_interface_representation_type(*source_type, *body.program)) ||
                    is_interface_representation_type(expr.type, *body.program)) {
                    return std::unexpected(DataflowError("cannot cast interface-typed pointers or references to other scalar or raw "
                                            "pointer representations",
                                        state.current_loc));
                }
                // A reference-returning call/field (e.g. `std::string_view::
                // at`'s `const char&`) is just as castable as the plain value
                // it refers to -- unwrap via binary_operand_type (the same
                // helper every binary-operator check in this file already
                // uses) so the checks below see the referent's own type,
                // not "is a Reference" itself.
                const Type* source_operand = source_type.has_value() ? &binary_operand_type(*source_type) : nullptr;
                bool scalar_source = source_operand != nullptr && source_operand->kind == TypeKind::Named &&
                                     is_scalar_type_name(source_operand->name);
                bool scalar_target = expr.type.kind == TypeKind::Named && is_scalar_type_name(expr.type.name);
                if (scalar_source && scalar_target) return {};

                bool integral_source = source_operand != nullptr && source_operand->kind == TypeKind::Named &&
                                       is_integral_scalar_type_name(source_operand->name);
                bool target_is_enum = is_enum_type(expr.type, body.program);
                if (integral_source && target_is_enum) {
                    return std::unexpected(DataflowError("cannot cast an integer value to enum class '" + expr.type.name +
                                            "'; use scpp::enum_cast<" + expr.type.name + ">(value) instead",
                                        state.current_loc));
                }

                const Type* source_enum_underlying =
                    source_operand != nullptr && source_operand->kind == TypeKind::Named ? enum_underlying_type(*source_operand, body.program)
                                                                                    : nullptr;
                if (source_operand != nullptr && source_enum_underlying != nullptr && expr.type.kind == TypeKind::Named &&
                    types_equal(*source_enum_underlying, expr.type)) {
                    return {};
                }

                bool raw_pointer_source = source_operand != nullptr && source_operand->kind == TypeKind::Pointer;
                bool raw_pointer_target = expr.type.kind == TypeKind::Pointer;
                if (raw_pointer_source && raw_pointer_target) {
                    if (state.unsafe_depth == 0) {
                        return std::unexpected(DataflowError("cannot cast between raw pointer types outside '[[scpp::unsafe]] { }' "
                                                "(spec §5.1(5.2))",
                                            state.current_loc));
                    }
                    return {};
                }

                {
                    return std::unexpected(DataflowError(
                        "a cast is only supported between two builtin scalar types, from an enum class to its "
                        "underlying integer type, or between two raw pointer types inside '[[scpp::unsafe]] { }', in "
                        "this version",
                        state.current_loc));
                }
            }
            return {};
        }

        case ExprKind::Unary:
            if (expr.unary_op == UnaryOp::Deref) {
                return apply_deref(expr, state, body, signatures, report_errors);
            }
            if (expr.unary_op == UnaryOp::AddressOf) {
                if (auto _r = apply_address_of(expr, state, body, signatures, report_errors); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                return {};
            }
            if (expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PreDec ||
                expr.unary_op == UnaryOp::PostInc || expr.unary_op == UnaryOp::PostDec) {
                return validate_increment_decrement_expr(expr, state, body, signatures, report_errors);
            }
            return apply_expr(*expr.lhs, false, state, body, signatures, report_errors);

        case ExprKind::New:
            if (report_errors && state.unsafe_depth == 0) {
                return std::unexpected(DataflowError("cannot use 'new' outside '[[scpp::unsafe]] { }' (spec §5.1(5.4))",
                                    state.current_loc));
            }
            if (expr.lhs) {
                if (auto _r = apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (report_errors) {
                    std::optional<Type> placement_type = infer_expr_type(*expr.lhs, body, signatures);
                    if (!placement_type.has_value() || placement_type->kind != TypeKind::Pointer ||
                        placement_type->pointee == nullptr || !types_equal(*placement_type->pointee, expr.type)) {
                        return std::unexpected(DataflowError("placement 'new' requires a raw pointer to the constructed type in this "
                                                "version",
                                            state.current_loc));
                    }
                }
            }
            if (expr.type.kind == TypeKind::Named && state.class_names != nullptr && state.class_names->contains(expr.type.name)) {
                bool move_shape = expr.args.size() == 1 && expr.args[0]->kind == ExprKind::Move &&
                                  produces_rvalue_of_type(*expr.args[0], expr.type, body, signatures);
                bool freely_copyable_copy_shape =
                    expr.args.size() == 1 &&
                    is_freely_copyable_class_value_source(*expr.args[0], expr.type, body, signatures);
                if (move_shape) {
                    if (auto _r = apply_expr(*expr.args[0], /*is_move_target_context=*/true, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else if (freely_copyable_copy_shape) {
                    if (auto _r = apply_expr(*expr.args[0], /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else if (expr.args.size() == 1 &&
                           body.program != nullptr && !has_user_declared_copy_ctor(expr.type.name, *body.program) &&
                           is_copyable_class_lvalue_boundary_source(*expr.args[0], expr.type, body, signatures)) {
                    if (auto _r = apply_expr(*expr.args[0], /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else {
                    if (auto _r = check_constructor_arguments(expr.type, expr.args, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
                return {};
            }
            for (const auto& arg : expr.args) {
                if (auto _r = apply_expr(*arg, /*is_move_target_context=*/arg->kind == ExprKind::Move, state, body, signatures,
                           report_errors); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            return {};

        case ExprKind::Delete:
            if (auto _r = apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (report_errors && state.unsafe_depth == 0) {
                return std::unexpected(DataflowError("cannot use 'delete' outside '[[scpp::unsafe]] { }' (spec §5.1(5.4))",
                                    state.current_loc));
            }
            if (report_errors) {
                std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures);
                if (!operand_type.has_value() || operand_type->kind != TypeKind::Pointer) {
                    return std::unexpected(DataflowError("'delete' requires a raw pointer operand in this version",
                                        state.current_loc));
                }
            }
            return {};

        case ExprKind::Destroy:
            if (auto _r = apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (!report_errors) return {};
            if (state.unsafe_depth == 0) {
                return std::unexpected(DataflowError("cannot call an explicit destructor outside '[[scpp::unsafe]] { }'", state.current_loc));
            }
            if (!expr.destroy_through_pointer) {
                return std::unexpected(DataflowError("explicit destructor calls currently require the pointer form 'ptr->~T()'",
                                    state.current_loc));
            }
            {
                std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures);
                if (!operand_type.has_value() || operand_type->kind != TypeKind::Pointer || operand_type->pointee == nullptr ||
                    !types_equal(*operand_type->pointee, expr.type)) {
                    return std::unexpected(DataflowError("explicit destructor calls require a raw pointer to the named type in this version",
                                        state.current_loc));
                }
            }
            return {};

        case ExprKind::Fold:
            if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (expr.rhs) {
                if (auto _r = apply_expr(*expr.rhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            return {};

        case ExprKind::Conditional:
            if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (auto _r = apply_expr(*expr.rhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (auto _r = apply_expr(*expr.third, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (report_errors) {
                std::optional<Type> condition_type = infer_expr_type(*expr.lhs, body, signatures);
                if (!condition_type.has_value() || condition_type->kind != TypeKind::Named ||
                    condition_type->name != "bool") {
                    return std::unexpected(DataflowError("conditional operator requires a 'bool' condition", state.current_loc));
                }
                std::optional<Type> then_type = infer_expr_type(*expr.rhs, body, signatures);
                std::optional<Type> else_type = infer_expr_type(*expr.third, body, signatures);
                if (then_type.has_value() && else_type.has_value() &&
                    !conditional_arm_types_agree(*expr.rhs, *then_type, *expr.third, *else_type)) {
                    return std::unexpected(DataflowError("conditional operator requires both arms to have the same type",
                                        state.current_loc));
                }
            }
            return {};

        case ExprKind::Binary:
            if (expr.binary_op == BinaryOp::Assign) {
                bool target_is_movable_class = false;
                std::optional<Type> target_class_type;
                if (expr.lhs->kind == ExprKind::Identifier) {
                    const Type* target_type = body.type_if_local(*expr.lhs);
                    if (target_type != nullptr && target_type->kind == TypeKind::Named &&
                        state.class_names != nullptr && state.class_names->contains(target_type->name)) {
                        target_is_movable_class = true;
                        target_class_type = *target_type;
                    }
                } else if (expr.lhs->kind == ExprKind::Member) {
                    // ch04 §4.2/spec §6.4/§6.5: `this.field = std::move(x);`
                    // (or any `obj.field = std::move(x);`) -- a field
                    // write is always "reinitializing" regardless of
                    // prior state (see this case's own Member branch
                    // below: struct/class locals are Initialized as a
                    // whole from declaration, so a field write never
                    // needs its own separate reassignment gate the way a
                    // *whole* class-typed local does) -- the only thing
                    // worth resolving here is whether std::move is
                    // *licensed* at all for this field's own declared
                    // type, exactly like the Identifier case just above.
                    // Recognizes any class-typed field, so a constructor
                    // moving a by-value/by-move parameter directly into
                    // its own field (e.g. `Outer(Inner&& i) { this.inner =
                    // std::move(i); }`) works the same way as any other
                    // class move.
                    std::optional<Type> field_type = resolve_member_field_type(*expr.lhs, body, state, signatures);
                    if (field_type.has_value() && field_type->kind == TypeKind::Named &&
                        state.class_names != nullptr && state.class_names->contains(field_type->name)) {
                        target_is_movable_class = true;
                        target_class_type = field_type;
                    }
                }
                // spec §6.5: a bare (non-move) same-type Identifier RHS
                // assigned into a class-typed *field* target is copy
                // assignment -- licensed only when the class is copy-
                // assignable. This is the only gate a field-target copy
                // assignment gets at all (a field write is never subject
                // to the whole-local-only "first write vs. reassignment"
                // restriction the MirStatementKind::Assign case enforces
                // for a plain local target, which is also where a
                // *whole-local* copy assignment's own, separate
                // eligibility check already lives -- a whole-local
                // Assign statement lowers directly to that MIR node, and
                // never reaches this general expression-level handler at
                // all, so there is no duplicate/conflicting check here
                // for that case; this exists for the Member-target case
                // specifically, previously a real, unchecked gap -- see
                // is_bare_same_type_copy_source's own comment).
                if (report_errors && target_class_type.has_value() &&
                    is_bare_same_type_copy_source(*expr.rhs, *target_class_type, body, signatures) &&
                    (state.classes_with_copy_assign == nullptr ||
                     !state.classes_with_copy_assign->contains(target_class_type->name))) {
                    return std::unexpected(DataflowError("class '" + target_class_type->name +
                                         "' is not copy-assignable (spec §6.5(3)) -- this assignment is not "
                                         "licensed",
                        state.current_loc));
                }
                bool is_move_target = target_is_movable_class || expr.rhs->kind == ExprKind::Move;
                if (auto _r = apply_expr(*expr.rhs, is_move_target, state, body, signatures, report_errors); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (expr.lhs->kind == ExprKind::Member || expr.lhs->kind == ExprKind::Subscript) {
                    if (auto _r = reject_lifetime_group_state_embedding(*expr.rhs, state, body, signatures, report_errors,
                                                          expr.lhs->kind == ExprKind::Member ? "object state"
                                                                                            : "an array element",
                                                          nullptr);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
                if (expr.lhs->kind == ExprKind::Identifier) {
                    if (const Type* target_type = body.type_if_local(*expr.lhs); target_type != nullptr) {
                        if (auto _r = check_function_pointer_assignment(*target_type, *expr.rhs, body, signatures, state.current_loc,
                                                          expr.lhs->name, report_errors);
                            !_r.has_value()) {
                            return std::unexpected(std::move(_r).error());
                        }
                        if (report_errors) {
                            if (auto _r = check_enum_conversion_compatibility(*target_type, *expr.rhs, body, signatures,
                                                                state.current_loc);
                                !_r.has_value()) {
                                return std::unexpected(std::move(_r).error());
                            }
                        }
                    }
                } else if (expr.lhs->kind == ExprKind::Member) {
                    std::optional<Type> field_type = resolve_member_field_type(*expr.lhs, body, state, signatures);
                    if (field_type.has_value()) {
                        if (auto _r = check_function_pointer_assignment(*field_type, *expr.rhs, body, signatures, state.current_loc,
                                                          expr.lhs->name, report_errors);
                            !_r.has_value()) {
                            return std::unexpected(std::move(_r).error());
                        }
                        if (report_errors) {
                            if (auto _r = check_enum_conversion_compatibility(*field_type, *expr.rhs, body, signatures,
                                                                state.current_loc);
                                !_r.has_value()) {
                                return std::unexpected(std::move(_r).error());
                            }
                        }
                    }
                }
                if (std::optional<LocalId> target = body.local_of(*expr.lhs); target.has_value()) {
                    // The assignment target is never a "read": whatever
                    // its previous state, assigning any value returns it
                    // to Initialized (spec ch05.1).
                    state.locals[*target] = LocalState::Initialized;
                } else if (expr.lhs->kind != ExprKind::Identifier) {
                    // e.g. `p.x = 1;` or `arr[i] = 1;`: the base
                    // object/index are evaluated (as addresses / an
                    // index value), not read as "the assignment target",
                    // so still worth walking for nested reads.
                    if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    if (report_errors) {
                        if (assignment_target_is_read_only(*expr.lhs, body, signatures)) {
                            return std::unexpected(DataflowError("cannot assign to this place: it is reached through a "
                                                 "read-only (const) reference",
                                state.current_loc));
                        }
                        if (std::optional<LocalId> lender = resolve_reborrow_lender(*expr.lhs, body, signatures);
                            lender.has_value()) {
                            if (auto _r = validate_reborrow_lender_write(*lender, state, body, report_errors); !_r.has_value()) {
                                return std::unexpected(std::move(_r).error());
                            }
                        }
                        bool write_through_mutable_reborrow =
                            write_is_licensed_by_mutable_reborrow_lender(*expr.lhs, state, body, signatures);
                        RootSet write_roots;
                        if (std::optional<LocalId> root = direct_write_root(*expr.lhs, body)) {
                            write_roots = single_root(*root);
                        } else {
                            auto write_roots_result = resolve_borrow_source_root(*expr.lhs, state, body, signatures, /*report_errors=*/false);
                            if (!write_roots_result.has_value()) return std::unexpected(std::move(write_roots_result).error());
                            write_roots = std::move(write_roots_result).value();
                        }
                        if (!write_through_mutable_reborrow) {
                            for (LocalId root : write_roots) {
                                auto borrow_it = state.borrows.find(root);
                                if (borrow_it != state.borrows.end() &&
                                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                                    return std::unexpected(DataflowError("cannot assign to this place: " + format_root(body, root) +
                                                            " is currently borrowed",
                                                        state.current_loc));
                                }
                            }
                        }
                    };
                }
                return {};
            }
            if (is_supported_compound_assignment(expr.binary_op)) {
                return validate_compound_assignment_expr(expr, state, body, signatures, report_errors);
            }
            if (expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne) {
                std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
                std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
                const Type* lhs_named =
                    lhs_type.has_value() ? &(lhs_type->kind == TypeKind::Reference && lhs_type->pointee ? *lhs_type->pointee
                                                                                                         : *lhs_type)
                                         : nullptr;
                const Type* rhs_named =
                    rhs_type.has_value() ? &(rhs_type->kind == TypeKind::Reference && rhs_type->pointee ? *rhs_type->pointee
                                                                                                         : *rhs_type)
                                         : nullptr;
                auto maybe_check_equality_overload = [&](const Expr& receiver_expr, const Expr& arg_expr,
                                                         const Type* receiver_named) -> std::expected<bool, DataflowError> {
                    if (receiver_named == nullptr || receiver_named->kind != TypeKind::Named) return false;
                    std::string overload_name = receiver_named->name + "_" + equality_operator_method_name(expr.binary_op);
                    if (!signatures.contains(overload_name)) return false;
                    ExprPtr overload_call =
                        make_overloaded_equality_call_expr(receiver_expr, arg_expr, expr.binary_op, expr.loc);
                    if (auto _r = check_call_arguments(*overload_call, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    return true;
                };
                auto _lhs_overload = maybe_check_equality_overload(*expr.lhs, *expr.rhs, lhs_named);
                if (!_lhs_overload.has_value()) return std::unexpected(std::move(_lhs_overload).error());
                if (_lhs_overload.value()) return {};
                auto _rhs_overload = maybe_check_equality_overload(*expr.rhs, *expr.lhs, rhs_named);
                if (!_rhs_overload.has_value()) return std::unexpected(std::move(_rhs_overload).error());
                if (_rhs_overload.value()) return {};
                bool lhs_is_record = lhs_named != nullptr && lhs_named->kind == TypeKind::Named &&
                                     body.program != nullptr &&
                                     (find_class_def(*body.program, lhs_named->name) != nullptr ||
                                      find_struct_def(*body.program, lhs_named->name) != nullptr);
                bool rhs_is_record = rhs_named != nullptr && rhs_named->kind == TypeKind::Named &&
                                     body.program != nullptr &&
                                     (find_class_def(*body.program, rhs_named->name) != nullptr ||
                                      find_struct_def(*body.program, rhs_named->name) != nullptr);
                if (report_errors && (lhs_is_record || rhs_is_record)) {
                    std::string receiver_name = lhs_is_record ? lhs_named->name : rhs_named->name;
                    std::string receiver_side = lhs_is_record ? "left" : "right";
                    return std::unexpected(DataflowError("operator '" + std::string(expr.binary_op == BinaryOp::Eq ? "==" : "!=") +
                                            "' requires a matching overloaded member operator on " + receiver_side +
                                            " operand type '" + receiver_name + "'",
                                        state.current_loc));
                }
            }
            if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (auto _r = apply_expr(*expr.rhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (report_errors) {
                if (auto _r = check_binary_expr_operand_types(expr, body, signatures, state.current_loc); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            return {};

        case ExprKind::Call:
            if (is_for_range_size_builtin(expr)) {
                return apply_expr(*expr.args[0], false, state, body, signatures, report_errors);
            }
            return check_call_arguments(expr, state, body, signatures, report_errors);

        case ExprKind::Member: {
            if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (report_errors && body.program != nullptr) {
                std::optional<Type> base_type = infer_expr_type(*expr.lhs, body, signatures);
                if (base_type.has_value()) {
                    const Type& named = base_type->kind == TypeKind::Reference ? *base_type->pointee : *base_type;
                    if (named.kind == TypeKind::Named) {
                        for (const StructDef& def : body.program->structs) {
                            if (def.name == named.name && def.is_union && state.unsafe_depth == 0) {
                                return std::unexpected(DataflowError("accessing a union member requires [[scpp::unsafe]] "
                                                    "(FFI union storage may alias multiple representations)",
                                    state.current_loc));
                            }
                        }
                    }
                };
            }
            // ch04 §4.2: real, unrestricted C++ access control -- a
            // member variable may be `public` or `private` in any
            // combination. External access (from outside the class's
            // own methods) to a `private` field is rejected, exactly
            // like before; a `public` one is now allowed (checked
            // exactly like a struct field access -- the borrow itself
            // is still recorded against the whole root object,
            // conservatively, by the caller's own apply_place/
            // resolve_borrow_source_root machinery, unaffected by this
            // access-control gate). Scoped to a plain Identifier base
            // (`this`, or an ordinary local/parameter) for now --
            // movecheck doesn't otherwise infer the type of an arbitrary
            // nested expression, so a deeper chain (`a.b.field` where
            // `a.b` is itself class-typed) isn't covered by this check
            // yet, a known, narrow scope limitation.
            if (report_errors && expr.lhs->kind == ExprKind::Identifier && state.class_names != nullptr) {
                if (const Type* base_type = body.type_if_local(*expr.lhs); base_type != nullptr) {
                    std::string class_name = named_type_name(*base_type);
                    if (!class_name.empty() && state.class_names->contains(class_name) &&
                        !grants_private_access(state, class_name)) {
                        AccessSpecifier access = AccessSpecifier::Private;
                        if (state.class_field_access != nullptr) {
                            auto class_it = state.class_field_access->find(class_name);
                            if (class_it != state.class_field_access->end()) {
                                auto field_it = class_it->second.find(expr.name);
                                if (field_it != class_it->second.end()) access = field_it->second;
                            }
                        }
                        if (access == AccessSpecifier::Private) {
                            return std::unexpected(DataflowError("cannot access private member '" + expr.name + "' of class '" +
                                                 class_name + "' from outside its own methods (ch04 §4.2)",
                                state.current_loc));
                        }
                    }
                }
            }
            return {};
        }

        case ExprKind::Subscript:
            if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            return apply_expr(*expr.rhs, false, state, body, signatures, report_errors);

        case ExprKind::PackExpansion:
            if (report_errors) {
                return std::unexpected(DataflowError("unexpanded parameter-pack expression reached move checking",
                                    state.current_loc));
            }
            return {};

        case ExprKind::Lambda: {
            // ch05 §5.12: a resolved lambda literal constructs a fresh
            // instance of its synthesized closure class, binding each
            // capture. When the literal is used *transiently* (an
            // IIFE, a call argument, ...) it can never outlive this
            // statement (scpp has no way to name/store a closure value
            // beyond this one -- unless it's the direct initializer of
            // an `auto` variable, see apply_statement's own Assign
            // case, which calls apply_lambda_captures directly with
            // `state.borrows` itself instead of a throwaway map), so a
            // fresh, local, discarded-afterward BorrowMap here is sound
            // -- see apply_lambda_captures' own comment for the shared
            // per-capture logic.
            BorrowMap capture_borrows;
            if (auto _r = apply_lambda_captures(expr, state, capture_borrows, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            return {};
        }
    }
}

// Handles a `BindReference` MIR statement -- `T& r = place;` /
// `const T& r = place;` (emitted only by a Reference-typed VarDecl), or
// `std::span<T> s = arr;` / `std::span<const T> s = arr;` (emitted only
// by a Span-typed VarDecl) -- see mir.cppm. Checks the borrowed place is
// currently readable and not already borrowed in a conflicting way
// (ch05.2's alias-XOR-mutability), then records the new borrow -- against
// the place's *root* (see resolve_borrow_source_root), not necessarily
// `place` itself, so a chain of reference-to-reference bindings (and a
// `.field`/`[index]` projection off a plain place) is tracked precisely.
[[nodiscard]] std::expected<void, DataflowError> apply_reference_binding(const MirStatement& stmt, DataflowState& state, const Body& body,
                              const Signatures& signatures, bool report_errors) {
    if (stmt.expr == nullptr) {
        // No initializer (`int& r;` / `std::span<int> s;`): illegal,
        // since unlike every other scpp type, neither a reference nor a
        // span has a zero/default state to fall back to -- real C++ has
        // no such thing as a null or later-bound reference either, and
        // v0.1 conservatively requires the same discipline of span (see
        // apply_statement's Assign case for why it isn't rebindable
        // either).
        if (report_errors) {
            const char* kind_name = is_span(stmt.type) ? "span" : "reference";
            return std::unexpected(DataflowError(std::string(kind_name) + " '" + body.name_of(stmt.local) +
                                 "' must be initialized (bound to a variable) at declaration",
                state.current_loc));
        }
        state.locals[stmt.local] = LocalState::Initialized;
        return {};
    }

    // ch05 §5.x: `const T& r = <rvalue>;` (a literal, std::move/
    // std::make_unique, a lambda literal, or a call not itself returning
    // by reference) binds to a freshly-materialized temporary -- exactly
    // like real C++'s own temporary lifetime extension (mirrors
    // apply_reference_argument's identical handling for a call
    // argument). Scoped to `TypeKind::Reference` only (never `Span`: a
    // std::span is only ever constructed from an existing fixed-size
    // array, ch06, never a fresh rvalue) and to a *const* reference (real
    // C++ forbids binding a *mutable* lvalue reference to a temporary). A
    // fresh temporary aliases nothing else in the program, so there is
    // no "root" to track in state.borrows/state.ref_targets at all --
    // just evaluate the initializer for its own side effects and mark
    // `stmt.local` initialized.
    if (const_reference_binds_materialized_temporary(*stmt.expr, stmt.type, body, signatures)) {
        if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body, signatures,
                   report_errors); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
        state.locals[stmt.local] = LocalState::Initialized;
        return {};
    }

    if (report_errors && !is_span(stmt.type)) {
        std::optional<Type> source_type = infer_expr_type(*stmt.expr, body, signatures);
        bool reference_binding_compatible = false;
        if (source_type.has_value()) {
            reference_binding_compatible =
                types_equal(*source_type, stmt.type) ||
                types_compatible_with_base_conversion(*source_type, stmt.type, *body.program, state.current_class);
            if (!reference_binding_compatible && stmt.type.pointee != nullptr) {
                reference_binding_compatible =
                    types_equal(*source_type, *stmt.type.pointee) ||
                    types_compatible_with_base_conversion(*source_type, *stmt.type.pointee, *body.program,
                                                          state.current_class);
            }
        }
        if (!reference_binding_compatible) {
            return std::unexpected(DataflowError("cannot bind reference '" + body.name_of(stmt.local) +
                                 "' from an incompatible source type",
                                state.current_loc));
        }
    }

    auto roots_result = resolve_borrow_source_root(*stmt.expr, state, body, signatures, report_errors);
    if (!roots_result.has_value()) return std::unexpected(std::move(roots_result).error());
    RootSet roots = std::move(roots_result).value();
    if (roots.empty()) {
        // Only reachable when report_errors=false and the source
        // expression's shape isn't (yet) a supported borrow source --
        // resolve_borrow_source_root would have thrown had report_errors
        // been true, so this whole program is already doomed to be
        // rejected by the upcoming reporting pass; just leave `stmt.local`
        // itself readable so this (discarded) silent fixed-point
        // iteration has *some* defined state to continue from.
        state.locals[stmt.local] = LocalState::Initialized;
        return {};
    }

    bool is_mutable = stmt.type.is_mutable_ref;
    std::optional<LocalId> lender = resolve_reborrow_lender(*stmt.expr, body, signatures);
    bool uses_lender_suspension = reborrow_is_tracked_against_lender(lender, body);
    if (uses_lender_suspension) {
        if (auto _r = validate_reborrow_lender(*lender, is_mutable, state, body, report_errors); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }

    // Reject manufacturing a mutable `T&`/`std::span<T>` out of a place
    // that's only reachable read-only (e.g. `int& r = p.x;`/
    // `std::span<int> s = p.arr;` where `p` is `const Foo&`) -- spec
    // ch05 §5.7's "projection chain's const-reachability" check, shared
    // with apply_reference_argument's identical guard for a call
    // argument. A `const T&`/`std::span<const T>` binding is always fine
    // regardless (read-only never needs to widen).
    if (report_errors && is_mutable && is_read_only_reachable(*stmt.expr, body, signatures)) {
        const char* kind_name = is_span(stmt.type) ? "span" : "reference";
        return std::unexpected(DataflowError(std::string("cannot bind a mutable ") + kind_name + " '" + body.name_of(stmt.local) +
                             "': its source is only reachable through a read-only (const) reference",
            state.current_loc));
    }

    if (!uses_lender_suspension) {
        for (LocalId root : roots) {
            BorrowState& borrow = state.borrows[root];
            if (report_errors) {
                if (is_mutable && (borrow.mutable_borrow || borrow.shared_count > 0)) {
                    return std::unexpected(DataflowError("cannot mutably borrow " + format_root(body, root) + ": it is already borrowed",
                                        state.current_loc));
                }
                if (!is_mutable && borrow.mutable_borrow) {
                    return std::unexpected(DataflowError("cannot borrow " + format_root(body, root) + ": it is already mutably borrowed",
                                        state.current_loc));
                }
            }
            if (is_mutable) {
                borrow.mutable_borrow = true;
            } else {
                borrow.shared_count++;
            }
        }
    } else if (is_mutable) {
        state.suspended_reborrows[*lender].mutable_suspended = true;
    } else {
        state.suspended_reborrows[*lender].shared_count++;
    }
    state.ref_targets[stmt.local] =
        RefTarget{roots, uses_lender_suspension ? lender : std::optional<LocalId>{}, is_mutable};
    state.local_lifetime_sources[stmt.local] = roots;
    state.locals[stmt.local] = LocalState::Initialized;
    return {};
}

// Handles a plain `r = expr;` MIR Assign statement where `r` was
// *previously* bound as a reference (its VarDecl went through
// BindReference, not this path -- see mir.cppm). Real C++ references
// can't be rebound, so this always means "write through `r` to its
// current referent", not "rebind r": rejected outright for a `const T&`
// (read-only), otherwise just an ordinary write with no borrow-conflict
// check needed here, since `r` holding a live mutable borrow *is* the
// license to write through it (see the Identifier-case comment in
// apply_expr for the symmetric read-side reasoning).
[[nodiscard]] std::expected<void, DataflowError> apply_reference_write_through(const MirStatement& stmt, DataflowState& state, const Body& body,
                                    const Signatures& signatures, bool report_errors) {
    const Type& ref_type = body.type_of(stmt.local);
    if (report_errors) {
        if (auto _r = validate_reborrow_lender_write(stmt.local, state, body, report_errors); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
        if (!ref_type.is_mutable_ref) {
            return std::unexpected(DataflowError("cannot assign through '" + body.name_of(stmt.local) +
                                 "': it is a read-only (const) reference",
                state.current_loc));
        }
        LocalState current = lookup(state.locals, stmt.local);
        if (current != LocalState::Initialized) {
            return std::unexpected(DataflowError(describe_bad_state(body.name_of(stmt.local), current),
                state.current_loc));
        }
    }
    return apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
               signatures, report_errors);
}

// ch04 §4.2/spec §6.4: true exactly when `ctor_args` is the single-
// argument shape `std::move(x)` where `x`'s own declared type is the
// exact same class `constructed_type` names -- the shape that dispatches
// to the compiler-synthesized move constructor (spec §6.4(2)) rather
// than any of the class's own user-declared constructors (which can
// never themselves be a move constructor -- spec §6.4(1) forbids
// declaring one, enforced at parse time). A mismatched-type std::move
// argument (or any other shape) falls through to ordinary constructor
// resolution unchanged, exactly as it always has -- e.g. a real,
// user-declared `Bar(Foo&& f)` constructor taking a *different* type's
// rvalue reference is untouched by this and still resolved by
// check_constructor_arguments below.
[[nodiscard]] bool is_move_construction_shape(const std::vector<ExprPtr>& ctor_args, const Type& constructed_type,
                                               const Body& body) {
    if (ctor_args.size() != 1) return false;
    const Expr& arg = *ctor_args[0];
    if (arg.kind != ExprKind::Move || arg.lhs->kind != ExprKind::Identifier) return false;
    const Type* source_type = body.type_if_local(*arg.lhs);
    return source_type != nullptr && types_equal(*source_type, constructed_type);
}

[[nodiscard]] bool is_lvalue_copy_source_shape(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::Identifier:
            return true;
        case ExprKind::Member:
        case ExprKind::Subscript:
            return expr.lhs != nullptr && is_lvalue_copy_source_shape(*expr.lhs);
        case ExprKind::Unary: {
            if (expr.unary_op != UnaryOp::Deref || expr.lhs == nullptr) return false;
            // A user-written `*p` (a raw pointer or smart-pointer local
            // dereferenced to reach a field): the same addressable-place
            // shape resolve_borrow_source_root already recognizes for
            // borrow sources (borrows.cppm) -- a dereferenced pointer is
            // just as legitimate an lvalue copy source as a plain
            // Member/Subscript root.
            if (!expr.implicit_arrow_deref) return is_lvalue_copy_source_shape(*expr.lhs);
            // `p->x` on a smart-pointer `p`: monomorphize.cppm's
            // rewrite_arrow_receiver desugars this into exactly this
            // Unary/Deref node wrapping one (or more, chained) compiler-
            // synthesized `operator_arrow` Call node(s), e.g.
            // `p->field` -> `(*p.operator_arrow()).field`. Each such call
            // is a pure, receiver-tied accessor (guaranteed by
            // implicit_arrow_chain_safe -- ast.cppm's own doc comment:
            // "the one safe carve-out that does not itself require an
            // unsafe context") that always yields the same address as
            // `p` itself owns, so it's just as legitimate a copy-source
            // root as `p` directly -- walk back through the synthesized
            // call chain to that original receiver.
            if (!expr.implicit_arrow_chain_safe) return false;
            const Expr* receiver = expr.lhs.get();
            while (receiver->kind == ExprKind::Call && receiver->name == "operator_arrow" && receiver->lhs != nullptr) {
                receiver = receiver->lhs.get();
            }
            return is_lvalue_copy_source_shape(*receiver);
        }
        default:
            return false;
    }
}

// spec §6.5: true exactly when `expr` is a bare (non-move) lvalue of the
// exact same class type as `target_type`. This includes the simple
// variable form from the spec's own examples (`Foo b = a;`) plus other
// addressable lvalues like `array[i]` and `obj.member`, all of which can
// feed a copy constructor / by-value class boundary without first moving.
[[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type, const Body& body,
                                                 const Signatures& signatures) {
    auto same_named_record_type_ignoring_top_level_const = [](const Type& source_type, const Type& dest_type) {
        return source_type.kind == TypeKind::Named && dest_type.kind == TypeKind::Named &&
               source_type.name == dest_type.name;
    };
    if (!is_lvalue_copy_source_shape(expr)) return false;
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    if (!source_type.has_value()) return false;
    if (same_named_record_type_ignoring_top_level_const(*source_type, target_type) ||
        types_equal(*source_type, target_type)) {
        return true;
    }
    return source_type->kind == TypeKind::Reference && !source_type->is_rvalue_ref && source_type->pointee &&
           (same_named_record_type_ignoring_top_level_const(*source_type->pointee, target_type) ||
            types_equal(*source_type->pointee, target_type));
}

[[nodiscard]] std::expected<void, DataflowError> apply_statement(const MirStatement& stmt, DataflowState& state, const Body& body, const Signatures& signatures,
                      bool report_errors) {
    // See apply_expr's identical opening comment -- same reasoning, one
    // level up (statement rather than expression granularity).
    state.current_loc = stmt.loc;
    switch (stmt.kind) {
        case MirStatementKind::Declare:
            // ch04 §4.2: a constructor-call VarDecl (`ClassName name
            // (args);`) needs its own arguments' move/borrow effects
            // applied -- see MirStatement::ctor_args' own comment for
            // why this was previously entirely invisible here. A
            // std::move(x)-of-the-same-class single argument dispatches
            // to the compiler-synthesized move constructor directly
            // (spec §6.4(2)); anything else goes through ordinary
            // constructor-overload argument checking.
            if (stmt.ctor_args != nullptr) {
                if (is_move_construction_shape(*stmt.ctor_args, stmt.type, body)) {
                    if (auto _r = apply_expr(*(*stmt.ctor_args)[0], /*is_move_target_context=*/true, state, body, signatures,
                               report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else if (stmt.ctor_args->size() == 1 &&
                           is_freely_copyable_class_value_source(*(*stmt.ctor_args)[0], stmt.type, body, signatures)) {
                    if (auto _r = apply_expr(*(*stmt.ctor_args)[0], /*is_move_target_context=*/false, state, body, signatures,
                               report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else if (stmt.ctor_args->size() == 1 &&
                           body.program != nullptr && !has_user_declared_copy_ctor(stmt.type.name, *body.program) &&
                           is_copyable_class_lvalue_boundary_source(*(*stmt.ctor_args)[0], stmt.type, body, signatures)) {
                    if (auto _r = apply_expr(*(*stmt.ctor_args)[0], /*is_move_target_context=*/false, state, body, signatures,
                               report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                } else {
                    if (auto _r = check_constructor_arguments(stmt.type, *stmt.ctor_args, state, body, signatures,
                                                 report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
            }
            // scpp has no "uninitialized" state (see the LocalState
            // comment above): a bare declaration always zero-initializes,
            // so it's always Initialized from this point on.
            state.locals[stmt.local] = LocalState::Initialized;
            if (stmt.ctor_args != nullptr && stmt.type.is_reference_wrapper_lifetime_source && stmt.ctor_args->size() == 1) {
                state.local_lifetime_sources[stmt.local] =
                    resolve_lifetime_source_roots(*(*stmt.ctor_args)[0], state, body, signatures, report_errors);
            } else if (stmt.type.kind == TypeKind::Pointer) {
                state.local_lifetime_sources[stmt.local] = {};
            } else if (is_lifetime_eligible_type(stmt.type)) {
                state.local_lifetime_sources.erase(stmt.local);
            }
            return {};

        case MirStatementKind::BindReference:
            return apply_reference_binding(stmt, state, body, signatures, report_errors);

        case MirStatementKind::Assign: {
            // The target is a local (keyed by its own declaration) or,
            // when it has none, a global -- the one assignable place
            // that has no declaration in this body. `target_name` is the
            // source spelling of either, and is only ever used for
            // diagnostics.
            const Type* local_type = stmt.has_local ? &body.type_of(stmt.local) : nullptr;
            std::string target_name = stmt.has_local ? body.name_of(stmt.local)
                                                     : (stmt.target != nullptr ? stmt.target->name : std::string{});
            // ch05/ch06: a `const`-qualified local (LocalDecl::is_const)
            // is initialized exactly once, by the
            // very same Assign statement its own VarDecl lowers to (see
            // mir.cppm's VarDecl case) -- distinguished from a genuine
            // later reassignment attempt by whether `stmt.local` already
            // has a prior entry in `state.locals` at all, the identical
            // "first write vs. reassignment" test the class-typed-local
            // case below uses for its own, differently-motivated
            // restriction. Checked *before* every type-specific case
            // below (reference/span/class/unique_ptr/plain scalar) so it
            // uniformly covers all of them with one rule, rather than
            // needing to be threaded through each one separately.
            if (report_errors &&
                ((stmt.has_local && body.decl(stmt.local).is_const && state.locals.contains(stmt.local)) ||
                 (!stmt.has_local && is_visible_global_const(target_name, /*explicit_global_qualification=*/false, body)))) {
                return std::unexpected(DataflowError("cannot reassign 'const' variable '" + target_name + "' after initialization",
                    state.current_loc));
            }
            if (local_type == nullptr) {
                std::optional<Type> global_type =
                    find_visible_global_type(target_name, /*explicit_global_qualification=*/false, body);
                if (!global_type.has_value()) return {};
                if (auto _r = check_function_pointer_assignment(*global_type, *stmt.expr, body, signatures, state.current_loc, target_name,
                                                  report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (auto _r = check_raw_pointer_assignment(*global_type, *stmt.expr, body, signatures, state.current_loc, target_name,
                                             report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (auto _r = check_nullptr_assignment(*global_type, *stmt.expr, state.current_loc, target_name,
                                                      report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (auto _r = check_scalar_conversion(*global_type, *stmt.expr, body, signatures, state.current_loc,
                                                      "'" + target_name + "'", report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                return apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            }
            if (is_reference((*local_type))) {
                return apply_reference_write_through(stmt, state, body, signatures, report_errors);
            }
            if (is_span((*local_type))) {
                // Unlike real C++ (where std::span is an ordinary,
                // freely-reassignable value), v0.1 conservatively treats
                // it exactly like a reference: bound once at
                // declaration, never rebound (see mir.cppm's
                // BindReference comment) -- lifting that is a follow-up,
                // not a soundness requirement.
                if (report_errors) {
                    return std::unexpected(DataflowError("std::span '" + target_name +
                                         "' cannot be reassigned after initialization in this version",
                        state.current_loc));
                }
                return {};
            }
            if (state.class_names != nullptr &&
                (*local_type).kind == TypeKind::Named && state.class_names->contains((*local_type).name)) {
                // ch04 §4.2: unlike a plain `struct` (an ordinary,
                // freely-reassignable trivial value), a class-typed local
                // is conservatively bound once at construction and never
                // reassigned in this version -- this *is* a soundness
                // necessity, not just a temporary restriction: without a
                // real copy constructor/assignment operator (out of
                // scope for v0.1 -- ch04's own "full class checking
                // rules" note), a plain bitwise reassignment would copy
                // whatever resource-owning fields the class has (e.g. a
                // raw handle its destructor later frees), and both the
                // old and new bindings' destructors would then
                // independently try to release the *same* resource at
                // their respective scope exits -- a double-free/use-
                // after-free. Lifting this needs real copy semantics
                // designed first, not just permission to reassign.
                //
                // This MIR-level Assign statement, though, is *also* how
                // a `VarDecl`'s own `= expr` initializer lowers (see
                // mir.cppm's VarDecl case -- there is no separate
                // "construct with an initial value" MIR node) -- the
                // *only* spelling `auto name = expr;` (ch05 §5.12, the
                // sole way to name a closure's own otherwise-unspellable
                // type) can ever take. A genuine first initialization
                // must therefore still be allowed here: distinguished
                // from a later reassignment by whether `stmt.local` has
                // *any* prior entry in `state.locals` at all (a bare
                // `ClassName c;` -- the only other way to declare a
                // class-typed local -- always marks it Initialized
                // immediately, see the Declare case above; a plain
                // `auto f = expr;` has no such preceding Declare, so its
                // own Assign is always this variable's first-ever
                // appearance).
                //
                // spec §6.4(3): a genuine reassignment is nonetheless
                // allowed when the RHS is any rvalue of the exact same
                // class type (`y = std::move(x);`, a by-value call result,
                // a factory expression, ...) -- dispatching to the
                // compiler-synthesized move assignment operator --
                // provided the class has no reference-typed member (spec
                // §6.4(3)'s own exception: a reference member can't be
                // re-seated by assignment, only ever bound once at
                // construction, mirroring real C++'s
                // [class.copy.assign]). Anything else (including a
                // same-class rvalue source for a class *with* a reference
                // member) falls through to the unconditional "no copy
                // semantics" rejection just below, unchanged.
                bool is_move_assignment = produces_rvalue_of_type(*stmt.expr, (*local_type), body, signatures);
                if (is_move_assignment && state.locals.contains(stmt.local)) {
                    if (report_errors) {
                        bool has_reference_member = false;
                        if (state.class_field_types != nullptr) {
                            auto fields_it = state.class_field_types->find((*local_type).name);
                            if (fields_it != state.class_field_types->end()) {
                                for (const auto& [field_name, field_type] : fields_it->second) {
                                    if (is_reference(field_type)) {
                                        has_reference_member = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (has_reference_member) {
                            return std::unexpected(DataflowError(
                                "class '" + (*local_type).name +
                                    "' has a reference-typed member, so it has no move assignment operator "
                                    "(spec §6.4(3)) -- '" + target_name + "' cannot be reassigned",
                                state.current_loc));
                        }
                        auto borrow_it = state.borrows.find(stmt.local);
                        if (borrow_it != state.borrows.end() &&
                            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                            return std::unexpected(DataflowError("cannot assign to class variable '" + target_name +
                                                 "': it is currently borrowed",
                                state.current_loc));
                        }
                    }
                    if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/true, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    state.locals[stmt.local] = LocalState::Initialized;
                    return {};
                }
                // spec §6.5(3): `y = x;` (a bare, non-move reassignment)
                // is copy assignment when `x` is a plain variable of the
                // exact same class type -- licensed only when the class
                // is copy-assignable (user-declared or compiler-
                // eligible). A class ineligible for copy assignment
                // (e.g. one with a reference member, or one whose
                // destructor/copy-constructor declaration suppresses
                // auto-generation with no user-declared operator=) falls
                // through to the unconditional "no copy semantics"
                // rejection just below, unchanged. Unlike move
                // assignment, copying never changes `x`'s own state
                // (spec §6.5's own note) -- no MovedOut transition for
                // it, so apply_expr is called with is_move_target_context
                // irrelevant here (there is no std::move to license).
                bool freely_copyable_assign_source =
                    is_freely_copyable_class_value_source(*stmt.expr, (*local_type), body, signatures);
                if ((is_bare_same_type_copy_source(*stmt.expr, (*local_type), body, signatures) ||
                     freely_copyable_assign_source) &&
                    state.locals.contains(stmt.local)) {
                    if (report_errors) {
                        if (!freely_copyable_assign_source &&
                            (state.classes_with_copy_assign == nullptr ||
                             !state.classes_with_copy_assign->contains((*local_type).name))) {
                            return std::unexpected(DataflowError("class '" + (*local_type).name +
                                                 "' is not copy-assignable (spec §6.5(3)) -- '" + target_name +
                                                 "' cannot be reassigned this way",
                                state.current_loc));
                        }
                        auto borrow_it = state.borrows.find(stmt.local);
                        if (borrow_it != state.borrows.end() &&
                            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                            return std::unexpected(DataflowError("cannot assign to class variable '" + target_name +
                                                 "': it is currently borrowed",
                                state.current_loc));
                        }
                    }
                    if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
                               signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    state.locals[stmt.local] = LocalState::Initialized;
                    return {};
                }
                if (report_errors && state.locals.contains(stmt.local)) {
                    return std::unexpected(DataflowError("class '" + (*local_type).name + "'-typed variable '" + target_name +
                                         "' cannot be reassigned after construction in this version (no copy "
                                         "semantics are defined yet -- see ch04 §4.2)",
                        state.current_loc));
                }
                if (stmt.expr->kind == ExprKind::Lambda) {
                    // ch05 §5.12: unlike a *transient* lambda literal
                    // (apply_expr's own Lambda case -- an IIFE, a call
                    // argument, ...), one bound to a named `auto`
                    // variable genuinely can outlive this statement, so
                    // any by-reference capture's borrow must land
                    // directly in `state.borrows` (persisting for the
                    // rest of this function -- see
                    // apply_lambda_captures' own comment) rather than a
                    // throwaway map that apply_expr's generic Lambda
                    // handling would otherwise use.
                    std::vector<ClosureCaptureBorrow> closure_capture_borrows;
                    if (auto _r = apply_lambda_captures(*stmt.expr, state, state.borrows, body, signatures, report_errors,
                                          &closure_capture_borrows);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    if (!closure_capture_borrows.empty()) {
                        state.closure_capture_borrows[stmt.local] = std::move(closure_capture_borrows);
                    }
                } else {
                    if (produces_rvalue_of_type(*stmt.expr, (*local_type), body, signatures)) {
                        if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/true, state, body, signatures,
                                   report_errors); !_r.has_value()) {
                            return std::unexpected(std::move(_r).error());
                        }
                        state.locals[stmt.local] = LocalState::Initialized;
                        return {};
                    }
                    // spec §6.5: `ClassName y = x;` (a bare, non-move,
                    // non-lambda initializer -- this variable's first-
                    // ever appearance, per the surrounding comment) is
                    // copy construction when `x` is a plain variable of
                    // the exact same class type -- licensed only when
                    // the class is copy-constructible (user-declared or
                    // compiler-eligible, spec §6.5(2)/is_copy_
                    // constructible). Recognizes exactly the shape spec
                    // §6.5's own worked example uses (`Foo b = a;`);
                    // anything else (a different type, a non-plain-
                    // variable expression) is rejected -- this used to be
                    // an entirely unchecked, silent bitwise copy for
                    // *any* expression shape at all (a real gap, closed
                    // as part of implementing this feature).
                    //
                    // A source of some *other* type that the declared
                    // class has a single-argument converting constructor
                    // from (`std::string s = "hi";`) is neither of those
                    // two shapes and is not a copy at all -- it builds a
                    // fresh object. This boundary used to be the only one
                    // of the four that never asked the question (see
                    // resolve_converting_constructor_binding), which made
                    // `std::string s = "hi";` ill-formed while the very
                    // same conversion was accepted as a call argument and
                    // as a `return` operand.
                    auto init_converting_ctor =
                        resolve_converting_constructor_binding(*local_type, *stmt.expr, state, body, signatures, report_errors);
                    if (!init_converting_ctor.has_value()) {
                        return std::unexpected(std::move(init_converting_ctor).error());
                    }
                    if (init_converting_ctor->ctor != nullptr) {
                        bool converts_via_reference_parameter = is_reference(init_converting_ctor->effective_param_type);
                        if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/!converts_via_reference_parameter, state,
                                                 body, signatures, report_errors);
                            !_r.has_value()) {
                            return std::unexpected(std::move(_r).error());
                        }
                        state.locals[stmt.local] = LocalState::Initialized;
                        return {};
                    }
                    if (report_errors) {
                        bool freely_copyable_init_source =
                            is_freely_copyable_class_value_source(*stmt.expr, (*local_type), body, signatures);
                        if (!is_bare_same_type_copy_source(*stmt.expr, (*local_type), body, signatures) &&
                            !freely_copyable_init_source) {
                            // The advice here used to name "constructor-
                            // call syntax ('T v(args);')" -- a spelling
                            // parse_variable_declaration categorically
                            // rejects ("parenthesized direct-initialization
                            // is not allowed for object declarations; use
                            // brace-init instead"), so a reader who
                            // followed this message landed straight on
                            // that one. Name the syntax the language
                            // actually has.
                            if (std::optional<std::string> why = explain_unusable_class_value_source(*stmt.expr);
                                why.has_value()) {
                                return std::unexpected(DataflowError(*why, state.current_loc));
                            }
                            return std::unexpected(DataflowError(
                                "class '" + (*local_type).name + "'-typed variable '" + target_name +
                                    "' can only be initialized via brace-init ('" +
                                    (*local_type).name + " " + target_name +
                                    "{args};'), std::move of the same type, a converting constructor of '" +
                                    (*local_type).name + "', or (if the class is copy-"
                                    "constructible, spec §6.5) an implicitly copyable source of another '" +
                                    (*local_type).name + "' value",
                                state.current_loc));
                        }
                        if (!freely_copyable_init_source &&
                            (state.classes_with_copy_ctor == nullptr ||
                             !state.classes_with_copy_ctor->contains((*local_type).name))) {
                            return std::unexpected(DataflowError("class '" + (*local_type).name +
                                                 "' is not copy-constructible (spec §6.5(2)) -- '" + target_name +
                                                 "' cannot be initialized this way",
                                state.current_loc));
                        }
                    }
                    if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures,
                               report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
                state.locals[stmt.local] = LocalState::Initialized;
                return {};
            }

            if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
                       signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (local_type != nullptr) {
                if (auto _r = check_function_pointer_assignment((*local_type), *stmt.expr, body, signatures, state.current_loc,
                                                  target_name, report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (auto _r = check_raw_pointer_assignment((*local_type), *stmt.expr, body, signatures, state.current_loc,
                                             target_name, report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (auto _r = check_nullptr_assignment((*local_type), *stmt.expr, state.current_loc, target_name,
                                                      report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (auto _r = check_scalar_conversion((*local_type), *stmt.expr, body, signatures, state.current_loc,
                                                      "'" + target_name + "'", report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (report_errors) {
                    if (auto _r = check_enum_conversion_compatibility((*local_type), *stmt.expr, body, signatures,
                                                        state.current_loc);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
            }

            // `T* p = &expr;` (ch05 §5.7): if `p`'s declared type wants a
            // *mutable* T* but `expr`'s place is only reachable
            // read-only, reject -- the same const-widens-only-one-way
            // rule as check_call_arguments's identical guard for a call
            // argument (`const T*` never converts to `T*` in this
            // version, so there is no way to legitimately satisfy a
            // mutable-pointee declaration here). Scoped to exactly this
            // direct syntactic shape, not a general type-checker -- see
            // check_call_arguments's identical comment for why.
            if (report_errors && (*local_type).kind == TypeKind::Pointer &&
                (*local_type).is_mutable_pointee && stmt.expr->kind == ExprKind::Unary &&
                stmt.expr->unary_op == UnaryOp::AddressOf && is_read_only_reachable(*stmt.expr->lhs, body, signatures)) {
                return std::unexpected(DataflowError("cannot assign '&' of a read-only-reachable place to '" + target_name +
                                    "' (a mutable 'T*'): would need 'const T*', which '" + target_name +
                                    "' isn't declared as",
                    state.current_loc));
            }

            if (report_errors) {
                auto borrow_it = state.borrows.find(stmt.local);
                if (borrow_it != state.borrows.end() &&
                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                    return std::unexpected(DataflowError("cannot assign to '" + target_name + "' while it is borrowed",
                        state.current_loc));
                }
            }
            state.locals[stmt.local] = LocalState::Initialized;
            if (is_pointer((*local_type))) {
                state.local_lifetime_sources[stmt.local] =
                    resolve_lifetime_source_roots(*stmt.expr, state, body, signatures, report_errors);
            }
            return {};
        }

        case MirStatementKind::Eval:
            if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (report_errors) {
                if (const NodiscardInfo* info = nodiscard_info_for_discarded_call(*stmt.expr, body, signatures)) {
                    std::string message = "discarded return value of nodiscard " + info->subject;
                    if (!info->reason.empty()) message += ": " + info->reason;
                    return std::unexpected(DataflowError(message, stmt.expr->loc));
                }
            }
            return {};

        case MirStatementKind::Drop:
            // Purely a codegen-facing marker (no-op until heap-allocated
            // owning types exist); no dataflow state effect here.
            return {};

        case MirStatementKind::ScopeExit: {
            // Releases `stmt.local`'s borrow, in the (unusual) case it's
            // a reference that was never read after being bound, so the
            // liveness-driven release in check_function never got a
            // chance to fire for it (a no-op here otherwise -- see
            // release_reference_borrow). Moving or dropping a *borrowed*
            // root while a borrow is still live is not separately
            // checked here: it can't happen in v0.1, since a reference
            // can only ever be bound to a *plain* place (or a `.field`/
            // `[index]` projection off one) declared no later than (i.e.
            // in the same or an enclosing scope of) the reference
            // itself, so the borrow is always released -- at the latest
            // at the reference's own ScopeExit -- before the root's own
            // ScopeExit (if any) is ever reached, so "drop a still-
            // borrowed local at scope exit" can't arise here either.
            release_reference_borrow(stmt.local, state, body);
            release_closure_capture_borrows(stmt.local, state);
            // `stmt.local` just went out of lexical scope: forget its
            // tracked state entirely. Erasing is equivalent to setting
            // it to Bottom (lookup() treats a missing key as Bottom) and
            // keeps the map from growing with entries the rest of the
            // analysis no longer cares about.
            state.locals.erase(stmt.local);
            state.local_lifetime_sources.erase(stmt.local);
            return {};
        }

        case MirStatementKind::UnsafeEnter:
            state.unsafe_depth++;
            return {};

        case MirStatementKind::UnsafeExit:
            state.unsafe_depth--;
            return {};
    }
}

[[nodiscard]] std::expected<void, DataflowError> check_terminator(const Terminator& term, DataflowState& state, const Function& fn, const Body& body,
                       const Signatures& signatures) {
    // See apply_expr's identical opening comment.
    state.current_loc = term.loc;
    switch (term.kind) {
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
            return apply_expr(*term.condition, false, state, body, signatures, /*report_errors=*/true);
        case TerminatorKind::Return: {
            if (term.return_value == nullptr) return {};
            if (auto _r = check_scalar_conversion(fn.return_type, *term.return_value, body, signatures, term.loc,
                                                  "the return value of '" + fn.name + "'",
                                                  /*report_errors=*/true);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (is_pointer_return_lifetime_source_type(fn.return_type)) {
                bool null_pointer_return =
                    fn.return_type.kind == TypeKind::Pointer &&
                    expr_is_definitely_null_pointer(*term.return_value, state, body);
                if (null_pointer_return) {
                    return apply_expr(*term.return_value, /*is_move_target_context=*/false, state, body, signatures,
                               /*report_errors=*/true);
                }
                std::optional<Type> returned_type = infer_expr_type(*term.return_value, body, signatures);
                // ch05 §5.14: check_generic_type_methods_once
                // (movecheck/monomorphize.cppm) builds one synthetic,
                // witness-substituted "check" class *per method* of a
                // generic class (see its own comment on why), and applies
                // witness substitution only to the checked method's own
                // signature/body -- not transitively to every *other*
                // still-generic type (e.g. a sibling nested-generic class,
                // or a private helper method living in a different,
                // separately-generated synthetic check class) that the
                // body happens to touch. Two independent, already-expected
                // consequences of that, both harmless (a real, concrete
                // instantiation of the same method is produced and checked
                // separately, with every type fully monomorphized, so this
                // never weakens verification of actual generated code):
                //  - a call to a *sibling* method via `this->` (e.g. a
                //    private helper) can never resolve here -- the
                //    sibling's own witness-substituted copy lives in a
                //    different synthetic check class, under a different
                //    name -- so `infer_expr_type` correctly reports it as
                //    unresolved (nullopt), exactly like the already-
                //    tolerated "unqualified, unmangled lookup" case
                //    documented on resolve_callee_signature's own
                //    Member-receiver branch;
                //  - a field of a nested still-generic type (e.g.
                //    `Entry<K, V>::second`) keeps that nested type's own,
                //    un-substituted field type when looked up by name
                //    (infer_expr_type's Member case finds the nested
                //    class's *template* ClassDef, never a witness-
                //    substituted clone of it -- none is ever generated),
                //    so it reads back as the literal type-parameter name
                //    instead of this method's own witness type.
                // Both surface identically here as `returned_type` either
                // missing or (once resolved) not matching `fn.return_type`
                // -- see is_synthetic_check_only_function's use below,
                // gating only the throw itself; the lifetime/root
                // derivation checks further down remain fully in effect
                // (they reason about *which parameter a value flows from*,
                // not its exact type identity, so the witness-substitution
                // gap above doesn't affect their soundness).
                bool is_synthetic_check_only_function =
                    !fn.member_owner_class.empty() && body.program != nullptr &&
                    [&] {
                        const ClassDef* owner = find_class_def(*body.program, fn.member_owner_class);
                        return owner != nullptr && owner->is_synthetic_check_only;
                    }();
                bool return_type_compatible = false;
                if (returned_type.has_value()) {
                    return_type_compatible =
                        types_equal(*returned_type, fn.return_type) ||
                        types_compatible_with_base_conversion(*returned_type, fn.return_type, *body.program,
                                                              state.current_class);
                    if (!return_type_compatible && fn.return_type.pointee != nullptr) {
                        return_type_compatible =
                            types_equal(*returned_type, *fn.return_type.pointee) ||
                            types_compatible_with_base_conversion(*returned_type, *fn.return_type.pointee,
                                                                  *body.program, state.current_class);
                    }
                    if (!return_type_compatible && fn.return_type.is_reference_wrapper_lifetime_source) {
                        if (types_equal(*returned_type, fn.return_type)) {
                            return_type_compatible = true;
                        } else if (returned_type->kind == TypeKind::Pointer && returned_type->pointee != nullptr &&
                                   fn.return_type.template_args.size() == 1) {
                            const Type& wrapped = fn.return_type.template_args[0];
                            const Type& expected_referent = wrapped.template_args.size() == 1 ? wrapped.template_args[0] : wrapped;
                            return_type_compatible =
                                types_equal(*returned_type->pointee, expected_referent) ||
                                types_compatible_with_base_conversion(*returned_type->pointee, expected_referent,
                                                                      *body.program, state.current_class);
                        }
                    }
                    if (!return_type_compatible &&
                        is_wrapper_constructor_call_compatible_with_lifetime_return(*term.return_value, fn.return_type, body,
                                                                                   signatures)) {
                        return_type_compatible = true;
                    }
                }
                RootSet returned_roots =
                    resolve_lifetime_source_roots(*term.return_value, state, body, signatures, /*report_errors=*/true);
                if (fn.return_type.is_reference_wrapper_lifetime_source && returned_roots.empty()) {
                    return {};
                }
                if (!return_type_compatible && fn.return_type.is_reference_wrapper_lifetime_source && !returned_roots.empty()) {
                    return_type_compatible = true;
                }
                if (!return_type_compatible) {
                    // Tolerated for a synthetic check-only function (see
                    // is_synthetic_check_only_function's own comment
                    // above): the very same witness-substitution gap that
                    // makes `returned_type` unreliable here also breaks
                    // the root-derivation checks just below (an unresolved
                    // sibling-method call can't be traced back to `this`
                    // either), so there is nothing further this pass could
                    // reliably validate about this specific return
                    // statement -- bail out entirely rather than risk a
                    // second, equally spurious "derived from <unknown>"
                    // diagnosis immediately after.
                    if (is_synthetic_check_only_function) return {};
                    return std::unexpected(DataflowError("function '" + fn.name + "' returns a lifetime-tracked value from an incompatible source type",
                                        state.current_loc));
                }
                if (fn.return_type.kind == TypeKind::Pointer && returned_roots.empty()) {
                    return {};
                }
                // ch05 §5.14: is_synthetic_check_only_function's own gap,
                // completed -- `return_type_compatible` (just above) and
                // `returned_roots` (resolve_lifetime_source_roots, right
                // before this whole block) are two *independent*
                // computations over the same return expression: a member
                // access like `existing->second` (existing: a local
                // pointer assigned from `this->__find_entry(key)`, a
                // sibling private-helper call) infers a perfectly correct,
                // matching *type* from the field's own declared type alone
                // (no callee resolution needed), so return_type_compatible
                // can be -- and, for exactly this shape, is -- true even
                // though `existing`'s own *root* can never be traced
                // (resolve_lifetime_source_roots's ExprKind::Call case
                // returns an empty RootSet whenever resolve_callee_signature/
                // resolve_overload can't resolve the callee, which a
                // sibling `this->`-qualified call from a synthetic check-
                // only function's own witness-substituted copy never can,
                // per this function's own is_synthetic_check_only_function
                // comment above). So the two checks can genuinely diverge --
                // the comment on the `!return_type_compatible` branch above
                // ("the very same witness-substitution gap... also breaks
                // the root-derivation checks just below") was the right
                // intent but didn't account for that divergence, only
                // actually bailing out when the *type* check happened to
                // fail too. Gating this whole reference/pointer-return
                // lifetime-derivation block the same way closes that gap
                // for real: a real, concrete instantiation of this same
                // method (created via the ordinary get_or_create_clone/
                // instantiate_generic_type path, every concrete type fully
                // monomorphized, every callee genuinely resolvable) is
                // checked separately and unaffected by this tolerance.
                if (is_synthetic_check_only_function) return {};
                if (fn.return_lifetime.present()) {
                    if (!roots_satisfy_named_lifetime_group(returned_roots, fn, fn.return_lifetime.name)) {
                        return std::unexpected(DataflowError("function '" + fn.name + "' returns a value derived from " +
                                                format_roots(body, returned_roots) + ", not from lifetime group '" +
                                                fn.return_lifetime.name + "'",
                                            state.current_loc));
                    }
                } else if (is_reference(fn.return_type) || fn.return_type.kind == TypeKind::Pointer) {
                    if (fn.return_type.kind == TypeKind::Pointer && roots_are_program_lifetime_only(returned_roots)) {
                        return {};
                    }
                    auto source_indices_result = resolve_returned_lifetime_param_indices(fn);
                    if (!source_indices_result.has_value()) return std::unexpected(std::move(source_indices_result).error());
                    std::vector<std::size_t> source_indices = std::move(source_indices_result).value();
                    if (fn.return_type.kind == TypeKind::Pointer && source_indices.empty()) {
                        std::vector<std::size_t> inferred_pointer_sources = infer_pointer_return_source_param_indices(fn);
                        if (inferred_pointer_sources.empty()) {
                            return std::unexpected(DataflowError(
                                "function '" + fn.name +
                                    "' returns a raw pointer but has no eligible source parameter to infer its "
                                    "lifetime from (reference, pointer, span, or std::reference_wrapper-carried "
                                    "reference; spec ch05.3) -- add an explicit lifetime annotation, return "
                                    "nullptr, or refactor to return by value/std::unique_ptr instead",
                                state.current_loc));
                        }
                        return std::unexpected(DataflowError(
                            "function '" + fn.name +
                                "' returns a raw pointer but has more than one eligible source parameter; scpp "
                                "v0.1 can only infer a returned pointer's lifetime when there is exactly one "
                                "eligible source parameter (spec ch05.3) -- add an explicit lifetime annotation "
                                "or refactor the signature",
                            state.current_loc));
                    }
                    if (!source_indices.empty() &&
                        !return_roots_are_proven_to_outlive_call(returned_roots,
                                                                 static_cast<LocalId>(source_indices.front()))) {
                        return std::unexpected(DataflowError(
                            "function '" + fn.name + "' returns " +
                                std::string(is_reference(fn.return_type) ? "a reference" : "a raw pointer") +
                                " derived from " + format_roots(body, returned_roots) +
                                ", not from its sole eligible source parameter '" +
                                fn.params[source_indices.front()].name +
                                "'; scpp v0.1 can only prove the returned value doesn't dangle when it "
                                "borrows (directly or transitively) from that parameter (spec ch05.3)",
                            state.current_loc));
                    }
                }
                return {};
            }
            bool return_is_class_value = is_named_class_type(fn.return_type, body);
            bool implicit_move_source =
                return_is_class_value && is_implicit_move_return_source(*term.return_value, fn.return_type, body);
            bool freely_copyable_return_source =
                return_is_class_value &&
                is_freely_copyable_class_value_source(*term.return_value, fn.return_type, body, signatures);
            // Deliberately NOT mirroring check_call_arguments' bare-
            // lvalue escape hatch here: copying *into* a by-value
            // parameter is always implicit-safe (the callee's copy is
            // freshly made from whatever lvalue the caller names, and
            // the caller keeps its own), but copying *out* via a return
            // is different -- scpp requires it to be an explicit
            // move/fresh-value/converting-ctor rather than an implicit
            // bare-name copy of storage the function doesn't own (a
            // global, another object's field, a reference parameter's
            // referent, or `this`'s own field -- see the
            // bare_return_of_*_is_rejected.scpp fixtures). Accepting
            // is_copyable_class_lvalue_boundary_source here (as an
            // earlier, since-reverted revision of this function did)
            // silently defeats all four of those checks.
            // Mirrors check_call_arguments' identical escape hatch (this
            // file, ~line 815): a returned value need not itself be
            // fn.return_type's own type -- it may instead be some other
            // type T that fn.return_type has a single-argument converting
            // constructor from (e.g. `return std::unexpected(E{...});`
            // returning into a `std::expected<T, E>`-typed function,
            // via std::expected's `expected(const std::unexpected<E>&)`).
            // Without this, only a same-type source could ever be
            // returned, even though the exact same conversion is already
            // accepted when passed as a call *argument*.
            const FunctionSignature* return_converting_ctor = nullptr;
            if (return_is_class_value) {
                auto binding = resolve_converting_constructor_binding(fn.return_type, *term.return_value, state, body, signatures,
                                                                     /*report_errors=*/true);
                if (!binding.has_value()) return std::unexpected(std::move(binding).error());
                return_converting_ctor = binding->ctor;
            }
            bool move_target_context =
                (return_is_class_value && !freely_copyable_return_source) || term.return_value->kind == ExprKind::Move;
            if (auto _r = apply_expr(*term.return_value, move_target_context, state, body, signatures, /*report_errors=*/true); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (return_is_class_value && !implicit_move_source && !freely_copyable_return_source &&
                !produces_rvalue_of_type(*term.return_value, fn.return_type, body, signatures) &&
                return_converting_ctor == nullptr) {
                if (std::optional<std::string> why = explain_unusable_class_value_source(*term.return_value); why.has_value()) {
                    return std::unexpected(DataflowError(*why, state.current_loc));
                }
                return std::unexpected(DataflowError("returning class '" + fn.return_type.name +
                                     "' by value requires either an implicitly copyable same-type source or "
                                     "a fresh value such as std::move(x) or a call returning by value",
                    state.current_loc));
            }
            return {};
        }
        case TerminatorKind::Goto:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
            return {};
    }
}

struct SwitchCaseKey {
    long long value = 0;
};

[[nodiscard]] std::optional<long long> integer_case_label_value(const Expr& expr) {
    if (expr.kind == ExprKind::IntegerLiteral || expr.kind == ExprKind::CharLiteral) return expr.int_value;
    if (expr.kind == ExprKind::BoolLiteral) return expr.bool_value ? 1LL : 0LL;
    if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Neg && expr.lhs &&
        expr.lhs->kind == ExprKind::IntegerLiteral) {
        return -expr.lhs->int_value;
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<SwitchCaseKey, DataflowError> normalize_switch_case_label(const Expr& expr, const Type& condition_type, const Body& body,
                                                        const Signatures& signatures) {
    const Type& operand_type = binary_operand_type(condition_type);
    if (is_enum_type(operand_type, body.program)) {
        const EnumDef* owning_enum = nullptr;
        const EnumVariant* variant = expr.kind == ExprKind::Identifier ? find_enum_variant(body.program, expr.name, &owning_enum)
                                                                       : nullptr;
        if (variant == nullptr || owning_enum == nullptr || owning_enum->name != operand_type.name) {
            return std::unexpected(DataflowError("switch on enum type '" + operand_type.name +
                                    "' requires each 'case' label to name an enumerator of that same enum",
                                expr.loc));
        }
        return SwitchCaseKey{variant->value};
    }
    if (!(operand_type.kind == TypeKind::Named &&
          (operand_type.name == "bool" || is_integral_scalar_type_name(operand_type.name)))) {
        return std::unexpected(DataflowError("switch requires an integral or enum condition expression", expr.loc));
    }
    if (std::optional<long long> literal = integer_case_label_value(expr)) {
        return SwitchCaseKey{*literal};
    }
    std::optional<Type> label_type = infer_expr_type(expr, body, signatures);
    if (label_type.has_value() && binary_operand_type(*label_type).kind == TypeKind::Named &&
        binary_operand_type(*label_type).name == operand_type.name && expr.kind == ExprKind::Identifier) {
        return std::unexpected(DataflowError("switch case labels must be integer literals in this version", expr.loc));
    }
    return std::unexpected(DataflowError("switch case labels must be integer literals (or enum enumerators for enum switches) in this version",
                        expr.loc));
}

[[nodiscard]] std::expected<void, DataflowError> validate_switch_stmt_tree(const Stmt& stmt, const Body& body, const Signatures& signatures) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
        case StmtKind::Return:
        case StmtKind::Break:
        case StmtKind::Continue:
        case StmtKind::Fallthrough:
        case StmtKind::ExprStmt:
            return {};
        case StmtKind::If:
            if (stmt.then_branch) {
                if (auto _r = validate_switch_stmt_tree(*stmt.then_branch, body, signatures); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            if (stmt.else_branch) {
                if (auto _r = validate_switch_stmt_tree(*stmt.else_branch, body, signatures); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            return {};
        case StmtKind::While:
            if (stmt.then_branch) return validate_switch_stmt_tree(*stmt.then_branch, body, signatures);
            return {};
        case StmtKind::Block:
            for (const StmtPtr& nested : stmt.statements) {
                if (auto _r = validate_switch_stmt_tree(*nested, body, signatures); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            return {};
        case StmtKind::Switch: {
            std::optional<Type> condition_type = infer_expr_type(*stmt.condition, body, signatures);
            if (condition_type.has_value()) {
                const Type& operand_type = binary_operand_type(*condition_type);
                bool condition_ok =
                    is_enum_type(operand_type, body.program) ||
                    (operand_type.kind == TypeKind::Named &&
                     (operand_type.name == "bool" || is_integral_scalar_type_name(operand_type.name)));
                if (!condition_ok) {
                    return std::unexpected(DataflowError("switch requires an integral or enum condition expression", stmt.condition->loc));
                }
                std::unordered_map<long long, SourceLocation> seen_labels;
                for (const SwitchCase& switch_case : stmt.switch_cases) {
                    if (switch_case.value) {
                        auto key_result =
                            normalize_switch_case_label(*switch_case.value, operand_type, body, signatures);
                        if (!key_result.has_value()) return std::unexpected(std::move(key_result).error());
                        SwitchCaseKey key = std::move(key_result).value();
                        if (seen_labels.contains(key.value)) {
                            return std::unexpected(DataflowError("duplicate switch case value", switch_case.value->loc));
                        }
                        seen_labels[key.value] = switch_case.value->loc;
                    }
                    for (const StmtPtr& nested : switch_case.statements) {
                        if (auto _r = validate_switch_stmt_tree(*nested, body, signatures); !_r.has_value()) {
                            return std::unexpected(std::move(_r).error());
                        }
                    }
                }
            } else {
                for (const SwitchCase& switch_case : stmt.switch_cases) {
                    for (const StmtPtr& nested : switch_case.statements) {
                        if (auto _r = validate_switch_stmt_tree(*nested, body, signatures); !_r.has_value()) {
                            return std::unexpected(std::move(_r).error());
                        }
                    }
                }
            }
            return {};
        }
    }
    return {};
}

// Runs the worklist algorithm (see spec ch07/M3) to a fixed point over
// `body`'s CFG, computing a stable per-block entry ("IN") state for the
// definite-initialization/move/borrow lattice above, then makes one more
// pass reporting any unsafe use found using those now-stable states.
// Splitting into these two phases avoids both false positives (from
// not-yet-stable intermediate states) and duplicate diagnostics (a block
// can be visited many times during fixed-point iteration).
[[nodiscard]] std::expected<void, DataflowError> check_function(const Function& fn, const Program& program, const Signatures& signatures,
                     const std::unordered_set<std::string>& class_names,
                     const ClassFieldTypes& class_field_types, const ClassFieldAccess& class_field_access,
                     const std::unordered_set<std::string>& classes_with_copy_ctor,
                     const std::unordered_set<std::string>& classes_with_copy_assign,
                     [[maybe_unused]] const std::unordered_set<std::string>& witness_class_names) {
    Body body = build_mir(fn);
    body.program = &program;
    if (fn.body) {
        if (auto _r = validate_switch_stmt_tree(*fn.body, body, signatures); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }

    std::size_t n = body.blocks.size();

    std::vector<std::vector<std::size_t>> preds(n);
    for (std::size_t i = 0; i < n; i++) {
        for (std::size_t succ : successors(body.blocks[i].terminator)) {
            preds[succ].push_back(i);
        }
    }

    // Precomputed once, up front: which reference locals are still live
    // (may be used again) immediately after each statement -- see
    // compute_reference_liveness. Consulted after every apply_statement
    // call below (in *both* passes) to release a reference's borrow as
    // soon as its last use has happened, rather than waiting for its
    // ScopeExit -- the NLL upgrade from spec ch05.3.
    std::vector<std::vector<LiveSet>> live_after = compute_reference_liveness(body, preds);

    DataflowState entry_state;
    // ch01 §1.2/§1.3: every function is checked by default,
    // unconditionally -- there is no per-function way to start already
    // inside an implicit unsafe context via mere absence of any marker
    // (the old "native function" concept is fully retired). A function
    // whose own declaration carries the function-level `[[scpp::unsafe]]`
    // marker (`fn.is_unsafe`) is the one explicit exception: its entire
    // body is an unsafe context throughout, exactly as if that whole
    // body were itself wrapped in one `[[scpp::unsafe]] { }` block --
    // implemented identically, by starting unsafe_depth at 1 instead of
    // 0 (an ordinary nested block further increments/decrements this
    // same counter from whatever it started at, so nesting one inside
    // an already-unsafe function's body is harmless, matching ch01
    // §1.2's "neither form changes §5.1-§5.4's checking" guarantee).
    // Every other function's entry_state starts at 0; unsafe_depth then
    // only increases via an explicit, lexically nested
    // `[[scpp::unsafe]] { }` block within that function's own body.
    entry_state.unsafe_depth = fn.is_unsafe ? 1 : 0;
    // ch04 §4.2/ch05 §5.9: `this` is always params[0] when present (see
    // parser's make_this_param) -- a user can never spell a same-named
    // parameter themselves, since `this` is a keyword, not an ordinary
    // identifier token.
    if (!fn.member_owner_class.empty()) {
        entry_state.current_class = fn.member_owner_class;
    } else if (!fn.params.empty() && fn.params[0].name == "this") {
        entry_state.current_class = fn.params[0].type.pointee->name;
    }
    // A lambda's synthesized `_call` method (see monomorphize.cppm's
    // resolve_lambda) needs private access to *two* distinct classes at
    // once: its own closure class (current_class above, e.g. for the
    // captured-field accesses `rewrite_captured_identifiers_as_field_
    // access` rewrote into `this.name`) and its lexically enclosing
    // function's own class (for any private member/method of *that*
    // class the lambda's body happens to mention directly, exactly as
    // if the body were still written inline at the lambda-expression's
    // own position -- see Function::access_context_class's own doc
    // comment, ast.cppm). A single current_class string can't hold both
    // at once, so this second, independent field exists purely so
    // grants_private_access (below) can accept *either* -- every other
    // function leaves it empty, in which case that second alternative
    // simply never matches (no class is ever named "").
    entry_state.lexical_access_context_class = fn.access_context_class;
    entry_state.class_names = &class_names;
    entry_state.class_field_types = &class_field_types;
    entry_state.class_field_access = &class_field_access;
    entry_state.classes_with_copy_ctor = &classes_with_copy_ctor;
    entry_state.classes_with_copy_assign = &classes_with_copy_assign;
    // Parameters are the first local_decls entries, in declaration order
    // (LocalResolver::run declares them before walking the body), so a
    // parameter's index *is* its LocalId.
    for (std::size_t param_index = 0; param_index < fn.params.size(); ++param_index) {
        const Param& param = fn.params[param_index];
        LocalId param_local = static_cast<LocalId>(param_index);
        entry_state.locals[param_local] = LocalState::Initialized;
        if (param.lifetime.present()) entry_state.parameter_lifetimes[param.name] = param.lifetime;
        if (is_pointer_return_lifetime_source_type(param.type) || is_reference(param.type)) {
            entry_state.local_lifetime_sources[param_local] = single_root(param_local);
        }
    }

    std::vector<DataflowState> in_states(n);
    std::vector<DataflowState> out_states(n);
    if (n > 0) in_states[0] = entry_state;

    if (is_constructor_function(fn)) {
        if (const ClassDef* owner = find_class_def(program, fn.member_owner_class)) {
            if (auto _r = validate_constructor_base_initialization(fn, *owner, body, signatures); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (auto _r = validate_constructor_virtual_interface_base_initialization(fn, *owner, body, signatures);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
    }

    std::deque<std::size_t> worklist;
    std::vector<bool> queued(n, false);
    for (std::size_t i = 0; i < n; i++) {
        worklist.push_back(i);
        queued[i] = true;
    }

    while (!worklist.empty()) {
        std::size_t b = worklist.front();
        worklist.pop_front();
        queued[b] = false;

        DataflowState new_in;
        if (b == 0) {
            new_in = entry_state;
        } else {
            bool first = true;
            for (std::size_t p : preds[b]) {
                new_in = first ? out_states[p] : join_states(new_in, out_states[p]);
                first = false;
            }
        }

        DataflowState new_out = new_in;
        for (std::size_t i = 0; i < body.blocks[b].statements.size(); i++) {
            if (auto _r = apply_statement(body.blocks[b].statements[i], new_out, body, signatures, /*report_errors=*/false);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            release_dead_references(new_out, body, live_after[b][i]);
        }

        in_states[b] = new_in;
        bool out_changed = !(new_out == out_states[b]);
        out_states[b] = std::move(new_out);

        if (out_changed) {
            for (std::size_t succ : successors(body.blocks[b].terminator)) {
                if (!queued[succ]) {
                    worklist.push_back(succ);
                    queued[succ] = true;
                }
            }
        }
    }

    // Fixed point reached: `in_states` is now stable. Walk every block
    // once more, this time actually reporting diagnostics.
    for (std::size_t b = 0; b < n; b++) {
        DataflowState state = in_states[b];
        for (std::size_t i = 0; i < body.blocks[b].statements.size(); i++) {
            if (auto _r = apply_statement(body.blocks[b].statements[i], state, body, signatures, /*report_errors=*/true);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            release_dead_references(state, body, live_after[b][i]);
        }
        if (auto _r = check_terminator(body.blocks[b].terminator, state, fn, body, signatures); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    return {};
}

// Builds the ch05 §5.10 overload-resolution signature map from every
// Function in `program` -- factored out of check_moves so
// monomorphize_generics (ch05 §5.11, below) can build the same map for
// its own call-site type inference (infer_expr_type/resolve_overload)
// without duplicating this loop. Throws the same "redefinition" /
// "invalid elision" diagnostics check_moves itself always has, just
// possibly surfaced slightly earlier in the pipeline now that
// monomorphization runs before check_moves (see driver.cppm) -- the
// error is exactly as correct either way.
[[nodiscard]] std::expected<void, DataflowError> check_moves_impl(const Program& program) {
    auto signatures_result = build_signatures(program);
    if (!signatures_result.has_value()) return std::unexpected(std::move(signatures_result).error());
    Signatures signatures = std::move(signatures_result).value();
    if (auto _r = validate_class_semantics(program, signatures); !_r.has_value()) return std::unexpected(std::move(_r).error());
    // ch04 §4.2: every class name in the program, so Member-access
    // checking (apply_expr's own Member case) can tell a class-typed
    // base (access-controlled) apart from a struct-typed one (never
    // access-controlled, ch04 §4.1) -- see DataflowState::class_names.
    std::unordered_set<std::string> class_names;
    for (const ClassDef& def : program.classes) {
        class_names.insert(def.name);
    }
    // See DataflowState::class_field_types' own comment.
    ClassFieldTypes class_field_types;
    for (const ClassDef& def : program.classes) {
        for (const ClassField& field : def.fields) {
            class_field_types[def.name][field.name] = field.type;
        }
    }
    for (const StructDef& def : program.structs) {
        for (const StructField& field : def.fields) {
            class_field_types[def.name][field.name] = field.type;
        }
    }
    // See DataflowState::class_field_access's own comment -- struct
    // fields have no access control at all (ch04 §4.1), so only
    // program.classes populates this.
    ClassFieldAccess class_field_access;
    for (const ClassDef& def : program.classes) {
        for (const ClassField& field : def.fields) {
            class_field_access[def.name][field.name] = field.access;
        }
    }
    // spec §6.5: every class eligible for copy construction/assignment
    // (user-declared or compiler-provided) -- see DataflowState's own
    // comment and is_copy_constructible/is_copy_assignable. No cycle
    // protection needed unlike ch05 §5.15's thread-safety derivation: a
    // class can never contain itself by value (infinite size), so the
    // field-containment recursion this walks is always a DAG, not a
    // graph that could cycle.
    std::unordered_set<std::string> classes_with_copy_ctor;
    std::unordered_set<std::string> classes_with_copy_assign;
    for (const ClassDef& def : program.classes) {
        if (is_copy_constructible(def.name, program)) classes_with_copy_ctor.insert(def.name);
        if (is_copy_assignable(def.name, program)) classes_with_copy_assign.insert(def.name);
    }
    for (const ClassDef& def : program.classes) {
        for (const Function& fn : program.functions) {
            if (auto _r = validate_constructor_member_initialization(fn, def, program); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
    }
    // ch05 §5.11: every concept/bare-`auto` witness class (never a real,
    // user-written one) -- see ClassDef::is_concept_witness and
    // check_function's own by-value-parameter/return-type exemption for
    // why this is needed.
    std::unordered_set<std::string> witness_class_names;
    for (const ClassDef& def : program.classes) {
        if (def.is_concept_witness) witness_class_names.insert(def.name);
    }
    for (const Function& fn : program.functions) {
        // A bodyless `extern "C"` declaration (ch02 §2.1) has no
        // statements to run the dataflow analysis over -- it's already
        // registered in `signatures` above (so call sites into it are
        // still checked normally), but there's nothing here to check.
        if (!fn.body) continue;
        if (fn.skip_imported_body_verification) continue;
        // ch05 §5.11: a full-header-form generic function's own
        // template (e.g. `get`/`make`, Function::template_params
        // non-empty) is never checked directly here -- its own body may
        // reference a not-yet-bound template parameter's own name as if
        // it were a real type (e.g. `T x;`, or a base-class-deduction
        // pattern's own "Head"/"Tail"), which movecheck's Body-based
        // machinery has no way to make sense of abstractly (unlike the
        // abbreviated `Concept auto` form, whose constrained parameter's
        // declared type already names a real, though synthetic, witness
        // class). Only each concrete call site's own monomorphized
        // clone (an ordinary, fully-concrete Function by the time this
        // runs, synthesized by monomorphize_generic_function_call) is
        // ever checked -- a narrower, pragmatic scope than the
        // abbreviated form's "checked once abstractly, independent of
        // any call site" guarantee, accepted given this form's added
        // deduction-pattern complexity.
        if (!fn.template_params.empty()) continue;
        if (!fn.generic_method_owner_id.empty()) continue;
        if (auto _r = check_function(fn, program, signatures, class_names, class_field_types, class_field_access, classes_with_copy_ctor,
                       classes_with_copy_assign, witness_class_names);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    return {};
}

} // namespace scpp
