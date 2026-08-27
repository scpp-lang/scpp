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
                                     const SourceLocation& loc, int unsafe_depth);
[[nodiscard]] std::optional<Type> resolve_member_field_type(const Expr& member_expr, const Body& body,
                                                            const DataflowState& state, const Signatures& signatures);
[[nodiscard]] std::optional<Type> resolve_assignment_place_type(const Expr& place, const Body& body,
                                                                const DataflowState& state,
                                                                const Signatures& signatures);
[[nodiscard]] std::string describe_assignment_place(const Expr& place);
[[nodiscard]] std::optional<Place> tracked_place_of(const Expr& expr, const DataflowState& state, const Body& body,
                                                   std::vector<LocalId>* traversed_references = nullptr,
                                                   PlacePrecision precision = PlacePrecision::Exact);
[[nodiscard]] bool write_is_blocked_by_a_borrow(LocalId root, const std::optional<Place>& written,
                                                const DataflowState& state,
                                                const std::vector<LocalId>& written_through);
[[nodiscard]] std::expected<void, DataflowError> check_value_binding_conversions(const Type& target_type,
                                                                                const Expr& value, const Body& body,
                                                                                const Signatures& signatures,
                                                                                const SourceLocation& loc,
                                                                                const std::string& target_name,
                                                                                bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_aggregate_element_conversions(
    const Type& aggregate_type, const std::vector<ExprPtr>& elements, const Body& body, const Signatures& signatures,
    const SourceLocation& loc, const std::string& target_name, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_record_copy_element_binding(
    const Type& field_type, const Expr& element, const Body& body, const Signatures& signatures,
    const SourceLocation& loc, const std::string& target_name, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_assignment_target_conversions(const Expr& place, const Expr& value,
                                                                                     const Body& body,
                                                                                     const DataflowState& state,
                                                                                     const Signatures& signatures,
                                                                                     const SourceLocation& loc,
                                                                                     bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_member_initializer_conversions(
    const Function& fn, const Body& body, const Signatures& signatures, const ClassFieldTypes& class_field_types);
[[nodiscard]] std::expected<void, DataflowError> check_initializer_scope_conversions(const Program& program,
                                                                                     const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> validate_deref_expr(const Expr& expr, const DataflowState& state, const Body& body,
                         const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> validate_place_indirections(const Expr& expr, const DataflowState& state,
                                                                             const Body& body,
                                                                             const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> validate_subscript_expr(const Expr& expr, const DataflowState& state, const Body& body,
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
[[nodiscard]] bool is_lvalue_copy_source_shape(const Expr& expr, const Body& body, const Signatures& signatures);
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

[[nodiscard]] bool is_string_named_type(const Type& type) {
    return type.kind == TypeKind::Named && (type.name == "std::string" || type.name == "string");
}

[[nodiscard]] bool is_const_char_pointer_type(const Type& raw_type) {
    // A string literal's type is an array of `const char` ([lex.string]),
    // and decays to `const char*` -- the two are indistinguishable at
    // every use this predicate guards, so ask about the decayed type.
    Type type = decay_array_to_pointer(raw_type);
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

// `++`/`--` apply to every scalar that can be added to, which is every
// one but `bool` -- `is_scalar_type_name` already excludes the
// non-numeric named types, so this needs no list of its own.
[[nodiscard]] bool is_increment_decrement_numeric_type(const Type& type) {
    return type.kind == TypeKind::Named && type.name != "bool" &&
           scpp::is_scalar_type_name(std::string_view{type.name});
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
    if (!expr_is_assignable_place(*expr.lhs, body)) {
        return std::unexpected(DataflowError("operand of '" +
                                std::string(expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PostInc ? "++" : "--") +
                                "' must be an assignable place",
                            expr.loc));
    }
    if (assignment_target_is_read_only(*expr.lhs, body, signatures)) {
        return std::unexpected(read_only_write_error(
            *expr.lhs, body, signatures,
            std::string(expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PostInc ? "++" : "--"), expr.loc));
    }
    auto write_roots_result = resolve_borrow_source_root(*expr.lhs, state, body, signatures, /*report_errors=*/false);
    if (!write_roots_result.has_value()) return std::unexpected(std::move(write_roots_result).error());
    RootSet write_roots = std::move(write_roots_result).value();
    if (std::optional<LocalId> lender = resolve_reborrow_lender(*expr.lhs, body, signatures); lender.has_value()) {
        if (auto _r = validate_reborrow_lender_write(*lender, state, body, report_errors,
                                                     tracked_place_of(*expr.lhs, state, body, nullptr,
                                                                      PlacePrecision::Enclosing));
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    if (!write_is_licensed_by_mutable_reborrow_lender(*expr.lhs, state, body, signatures)) {
        std::optional<Place> written = tracked_place_of(*expr.lhs, state, body, nullptr, PlacePrecision::Enclosing);
        for (LocalId root : write_roots) {
            if (write_is_blocked_by_a_borrow(root, written, state, {})) {
                return std::unexpected(DataflowError("cannot apply '" +
                                        std::string(expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PostInc ? "++" : "--") +
                                        "' to this place: " + format_root(body, root) + " is currently borrowed",
                                    expr.loc));
            }
        }
    }
    if (std::optional<LocalId> target = body.local_of(*expr.lhs); target.has_value()) {
        reinitialize_place(state.locals, whole_local_place(*target));
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
        if (auto _r = check_binary_expr_operand_types(arithmetic_check, body, signatures, expr.loc, state.unsafe_depth); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    if (!expr_is_assignable_place(*expr.lhs, body)) {
        return std::unexpected(DataflowError("left operand of '" + std::string(compound_operator_spelling(expr.binary_op)) +
                                "' must be an assignable place",
                            expr.loc));
    }
    if (assignment_target_is_read_only(*expr.lhs, body, signatures)) {
        return std::unexpected(read_only_write_error(*expr.lhs, body, signatures,
                                                     std::string(compound_operator_spelling(expr.binary_op)),
                                                     state.current_loc));
    }
    if (std::optional<LocalId> lender = resolve_reborrow_lender(*expr.lhs, body, signatures); lender.has_value()) {
        if (auto _r = validate_reborrow_lender_write(*lender, state, body, report_errors,
                                                     tracked_place_of(*expr.lhs, state, body, nullptr,
                                                                      PlacePrecision::Enclosing));
            !_r.has_value()) {
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
    if (!write_through_mutable_reborrow) {
        std::optional<Place> written = tracked_place_of(*expr.lhs, state, body, nullptr, PlacePrecision::Enclosing);
        for (LocalId root : write_roots) {
            if (write_is_blocked_by_a_borrow(root, written, state, {})) {
                return std::unexpected(DataflowError("cannot assign to this place: " + format_root(body, root) +
                                                         " is currently borrowed", state.current_loc));
            }
        }
    }
    if (std::optional<LocalId> target = body.local_of(*expr.lhs); target.has_value()) {
        reinitialize_place(state.locals, whole_local_place(*target));
    }
    return {};
}

[[nodiscard]] std::expected<void, DataflowError> check_binary_expr_operand_types(const Expr& expr, const Body& body, const Signatures& signatures,
                                     const SourceLocation& loc, int unsafe_depth) {
    if (expr.binary_op == BinaryOp::Assign) return {};
    // spec §16.3(1.5) names both operands of a binary operator as
    // positions where a value is required, and this is the one function
    // that judges them -- for a compound assignment too, which reaches
    // it through validate_compound_assignment_expr's synthesized
    // arithmetic expression. Asked before every early-out below,
    // including the one for `&&`/`||`: those two have their own
    // "expected a 'bool' value" diagnostic, which describes the wrong
    // problem for an operand that is not a value at all.
    if (auto _r = check_expression_yields_a_value(*expr.lhs, body, signatures, loc,
                                                  "a binary operator's left operand", /*report_errors=*/true);
        !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (auto _r = check_expression_yields_a_value(*expr.rhs, body, signatures, loc,
                                                  "a binary operator's right operand", /*report_errors=*/true);
        !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
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
        // spec §5.1(5.1) makes "indirection through, *or pointer
        // arithmetic on*, a value of pointer type ([expr.unary.op],
        // [expr.add])" a gated operation, ill-formed in a safe context
        // by §5.1(6). Only the indirection half was ever implemented
        // (validate_deref_expr); forming `p + i`, `p - i` or `p - q` was
        // accepted anywhere, so a program could walk a raw pointer off
        // the end of an object without an unsafe block in sight. This is
        // the [expr.add] half of the same sentence, and it deliberately
        // does not extend to the relational and equality operators
        // below: §5.1(5.1) cites [expr.add] alone, and comparing two
        // pointers reads no storage and produces no new address, so
        // `p < q` and `p == q` stay ungated.
        //
        // Reached for `p += i` too: validate_compound_assignment_expr
        // routes a synthesized `p + i` through here, which is exactly
        // right -- [expr.ass] defines `E1 op= E2` as `E1 = E1 op E2`,
        // so a compound assignment is the same [expr.add] arithmetic.
        if (lhs_type.has_value() && rhs_type.has_value() && unsafe_depth == 0 &&
            pointer_arithmetic_result_type(expr.binary_op, *lhs_type, *rhs_type).has_value()) {
            return std::unexpected(DataflowError("cannot do pointer arithmetic on a raw pointer outside '[[scpp::unsafe]] { }' "
                                 "(spec §5.1(5.1))",
                loc));
        }
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

// Maps a reference/span local onto the place it is currently bound to,
// so that every alias of one object resolves to one move-state key.
// Handed to mir.cppm's place_of, which knows the syntax of a place but
// nothing about bindings.
[[nodiscard]] auto place_root_resolver(const DataflowState& state, std::vector<LocalId>* traversed = nullptr) {
    return [&state, traversed](LocalId local) -> std::optional<Place> {
        auto it = state.ref_targets.find(local);
        if (it == state.ref_targets.end()) return std::nullopt;
        if (!it->second.bound_place.has_value() || !it->second.bound_place_is_exact) return std::nullopt;
        if (traversed != nullptr) traversed->push_back(local);
        return it->second.bound_place;
    };
}

// The place `expr` names, with references resolved to what they are
// bound to. nullopt when `expr` names no statically identifiable
// storage -- see explain_untrackable_place for why.
[[nodiscard]] std::optional<Place> tracked_place_of(const Expr& expr, const DataflowState& state, const Body& body,
                                                   std::vector<LocalId>* traversed_references,
                                                   PlacePrecision precision) {
    return place_of(expr, body, place_root_resolver(state, traversed_references), precision);
}

// Whether a live borrow can observe a write to `written` (an assignment,
// a `++`/`--`, or a move taking the object out of it).
//
// This is the single answer to "does a live borrow conflict with this
// write?". It had four implementations: three copies of a `for (root :
// write_roots) if (state.borrows[root])` loop -- in `++`/`--`, in
// assignment, and in the assignment case of the expression walker -- and
// the move check. `state.borrows` is keyed by root local, one
// granularity coarser than a place, so all three said "`p` is borrowed"
// when only `p.x` was, and rejected a write to `p.y`. §6.2(7) makes a
// borrow an alias of "the same underlying object", so a borrow of `p.x`
// is not a hold on `p.y`.
//
// The reference through which the write is *made* (`T& r = s.a; ...
// std::move(r);`) is the write itself, not a bystander observing it, so
// `written_through` exempts it. The precise answer is only available
// when every live hold on this root is attributable to a reference whose
// bound place is known -- address-of borrows and closure captures also
// increment the same counters without a RefTarget, so when the counts do
// not add up, or when the written place cannot be resolved at all, the
// conservative whole-root answer stands.
[[nodiscard]] bool write_is_blocked_by_a_borrow(LocalId root, const std::optional<Place>& written,
                                                const DataflowState& state,
                                                const std::vector<LocalId>& written_through) {
    auto borrow_it = state.borrows.find(root);
    if (borrow_it == state.borrows.end()) return false;
    const BorrowState& hold = borrow_it->second;
    if (!hold.mutable_borrow && hold.shared_count <= 0) return false;
    if (!written.has_value() || written->local != root) return true;

    int attributable_shared = 0;
    bool attributable_mutable = false;
    bool overlaps = false;
    for (const auto& [ref_local, target] : state.ref_targets) {
        if (target.is_reborrow()) continue;
        if (std::ranges::find(target.roots, root) == target.roots.end()) continue;
        if (target.is_mutable) {
            attributable_mutable = true;
        } else {
            attributable_shared++;
        }
        if (std::ranges::find(written_through, ref_local) != written_through.end()) continue;
        if (!target.bound_place.has_value()) {
            overlaps = true;
            continue;
        }
        if (target.bound_place->is_at_or_under(*written) || written->is_at_or_under(*target.bound_place)) overlaps = true;
    }
    bool fully_attributed = attributable_shared == hold.shared_count && attributable_mutable == hold.mutable_borrow;
    if (!fully_attributed) return true;
    return overlaps;
}

// A field's declared type, following base classes: class_field_types is
// built per *declaring* record (see build_class_field_types), so an
// inherited member is not an entry of the derived record's own map.
[[nodiscard]] const Type* find_record_field_type(const std::string& record_name, const std::string& field_name,
                                                 const DataflowState& state, const Body& body) {
    if (state.class_field_types == nullptr) return nullptr;
    auto record_it = state.class_field_types->find(record_name);
    if (record_it != state.class_field_types->end()) {
        auto field_it = record_it->second.find(field_name);
        if (field_it != record_it->second.end()) return &field_it->second;
    }
    if (body.program == nullptr) return nullptr;
    for (const ClassDef& def : body.program->classes) {
        if (def.name != record_name) continue;
        if (auto base = def.direct_ordinary_base()) {
            return find_record_field_type(base->get().base_type.name, field_name, state, body);
        }
        return nullptr;
    }
    return nullptr;
}

// The declared type of `place`, walking its projection path from the
// root local's own declared type.
[[nodiscard]] std::optional<Type> place_type(const Place& place, const DataflowState& state, const Body& body) {
    if (!body.is_valid_local(place.local)) return std::nullopt;
    Type current = body.type_of(place.local);
    for (const Projection& step : place.path) {
        if (current.kind == TypeKind::Reference || current.kind == TypeKind::Pointer) {
            if (current.pointee == nullptr) return std::nullopt;
            current = *current.pointee;
        }
        if (step.is_deref) {
            // A `Named` receiver here is a class whose `operator*`/
            // `operator->` was selected; its pointee type needs the
            // signature table, which this walk deliberately does not
            // reach into -- every caller that matters for such a place
            // stops before asking (see place_requires_restore_before_
            // teardown), so declining is not a silent skip.
            if (current.kind == TypeKind::Span) {
                if (current.element == nullptr) return std::nullopt;
                current = *current.element;
                continue;
            }
            return std::nullopt;
        }
        if (step.is_index) {
            // A span is a borrowed range: `s[i]` names an element of
            // storage some other object owns, and place_type is asked
            // about it by the §6.3(1) restore check. Failing here used
            // to make that check skip every span element silently.
            if (current.kind == TypeKind::Span) {
                if (current.element == nullptr) return std::nullopt;
                current = *current.element;
                continue;
            }
            if (current.kind != TypeKind::Array || current.element == nullptr) return std::nullopt;
            current = *current.element;
            continue;
        }
        if (current.kind != TypeKind::Named) return std::nullopt;
        const Type* field_type = find_record_field_type(current.name, step.field, state, body);
        if (field_type == nullptr) return std::nullopt;
        current = *field_type;
    }
    return current;
}

// Whether this function itself emits the teardown of the object that
// directly contains `place`, and of every object containing *that*, so
// that a per-place "already moved out" flag it records is still in
// scope where the destruction happens.
//
// It is not whenever some containing object's destruction goes through
// a destructor *call*: the member teardown then runs inside the callee,
// which has no access to the caller's flags and no parameter to carry
// them (spec §6.2's closing note leaves subobject state at destruction
// unspecified, so there is no clause licensing a hidden per-member drop
// flag in the object's own layout either). Such a place has to be put
// back before the object it belongs to is destroyed -- which is exactly
// what the code that motivated per-place tracking already does.
[[nodiscard]] bool place_teardown_is_emitted_here(const Place& place, const DataflowState& state, const Body& body) {
    if (place.is_whole_local()) return true;
    if (body.program == nullptr) return false;
    // `this`, a reference or a raw pointer roots the place in storage
    // some other function owns and destroys.
    const Type& root_type = body.type_of(place.local);
    if (root_type.kind == TypeKind::Reference || root_type.kind == TypeKind::Pointer ||
        root_type.kind == TypeKind::Span) {
        return false;
    }
    if (std::optional<LocalId> self = body.this_local(); self.has_value() && place.local == *self) return false;
    if (body.decl(place.local).is_static_lifetime) return false;
    if (place_goes_through_deref(place)) return false;
    for (Place ancestor = place.parent();; ancestor = ancestor.parent()) {
        std::optional<Type> ancestor_type = place_type(ancestor, state, body);
        if (!ancestor_type.has_value()) return false;
        if (ancestor_type->kind == TypeKind::Named &&
            class_destruction_chain_has_destructor(ancestor_type->name, *body.program)) {
            return false;
        }
        if (ancestor.is_whole_local()) break;
    }
    return true;
}

// Whether §6.3(1) obliges the program to put `place` back before the
// exit, given that this function's teardown cannot be told to skip it.
//
// It does not, across a dereference. §6.3(1) conditions on the object
// being "in the moved-out state (6.2)", and §6.2(1) puts only objects
// "of automatic, static, thread, or member storage duration" into the
// two states at all. The object a pointer designates has whichever
// storage duration its creator gave it -- dynamic, for anything reached
// through `unique_ptr`, `shared_ptr` or a *new-expression* -- and
// dynamic storage duration is not in that enumeration, so the pointee
// is never in the moved-out state and §6.3(1)'s "no destructor is
// invoked" has no object to speak about. Its destructor runs by the
// ordinary C++ rule ([class.dtor], [expr.delete]) on storage the move
// has already zeroed, exactly once.
//
// The state recorded against the deref place is still what makes a use
// of it after the move ill-formed (§6.2(6)); what does not follow is a
// restore obligation, and demanding one would make a move out of an
// object about to be deleted inexpressible.
//
// [This leaves a residual the spec does not resolve: a pointer into
// *automatic* storage names an object that is in the model, and a
// destructor with observable effects then runs on the zeroed subobject.
// §6.2 states no rule identifying `*p` with the object `p` was taken
// from, and §5.1 already puts raw-pointer dereference behind an unsafe
// context; the gap is §6.2's, not this predicate's.]
[[nodiscard]] bool place_requires_restore_before_teardown(const Place& place, const DataflowState& state,
                                                          const Body& body) {
    if (place_goes_through_deref(place)) return false;
    return !place_teardown_is_emitted_here(place, state, body);
}

// spec §6.3(1): an object in the moved-out state is not destroyed. For a
// subobject that is enough only where the code emitting the teardown can
// be told to skip it (see place_teardown_is_emitted_here); where it
// cannot -- because the containing object outlives this function, or is
// destroyed by a destructor call whose body cannot see this function's
// bookkeeping -- the program has to reinitialize it before that point.
// This reports the exits that reach one still moved out, which is the
// program point where the omission first has a consequence.
[[nodiscard]] std::expected<void, DataflowError> check_moved_subobjects_were_restored(
    const DataflowState& state, const Body& body, const std::optional<LocalId>& scope_root,
    std::string_view when) {
    std::optional<Place> worst;
    for (const auto& [place, place_state] : state.locals) {
        if (place.is_whole_local()) continue;
        if (place_state == LocalState::Initialized || place_state == LocalState::Bottom) continue;
        if (scope_root.has_value() ? place.local != *scope_root : false) continue;
        // Nothing is emitted to destroy this place in the first place
        // (a scalar, or a host type scpp does not own the layout of), so
        // there is nothing for the teardown to have to skip.
        std::optional<Type> moved_type = place_type(place, state, body);
        if (!moved_type.has_value() || !type_needs_teardown(*moved_type, *body.program)) continue;
        // Only a *partial* move is a problem: if what contains this
        // place was itself moved out (or is moved out on some incoming
        // path), the containing object is not destroyed either (spec
        // §6.3(1)), so there is nothing for its teardown to skip.
        if (lookup(state.locals, place.parent()) != LocalState::Initialized) continue;
        if (!place_requires_restore_before_teardown(place, state, body)) continue;
        // Deterministic choice, for the same reason find_moved_subobject
        // sorts: an unordered_map's iteration order is not stable.
        if (!worst.has_value() || (place.local < worst->local) ||
            (place.local == worst->local && place.path < worst->path)) {
            worst = place;
        }
    }
    if (!worst.has_value()) return {};
    std::string name = body.describe_place(*worst);
    std::string message;
    message += "'";
    message += name;
    message += "' was moved out and is still moved out ";
    message += when;
    message += "; the object it belongs to is destroyed elsewhere, which cannot be told to skip a "
               "moved-out subobject (spec §6.3(1)) -- assign to '";
    message += name;
    message += "' before this point";
    // A ScopeExit statement carries no location of its own (mir.cppm's
    // pop_scope), so state.current_loc here is the last statement's, or
    // nothing at all. The declaration of the object being destroyed is
    // always available and is the thing the message is about.
    SourceLocation loc = state.current_loc;
    if (loc.line == 0 && body.is_valid_local(worst->local)) loc = body.decl(worst->local).decl_loc;
    return std::unexpected(DataflowError(message, loc));
}

// The declared type of the place an assignment writes to, for any place
// shape the language allows on the left of `=`.//
// Every conversion rule an assignment has to obey (scalar, raw pointer,
// function pointer, `nullptr`, enum) is a question about *this* type,
// and is therefore the same question no matter how the place was
// spelled. It was previously asked per-shape, and so got a different
// answer per shape: `x = v` was fully checked, `s.f = v` was checked for
// two of the five rules, and `a[i] = v` was checked for none at all --
// see check_assignment_target_conversions' own comment for how the
// shapes came to diverge. Resolving the type in one place is what lets
// the checks be applied in one place.
[[nodiscard]] std::optional<Type> resolve_assignment_place_type(const Expr& place, const Body& body,
                                                                const DataflowState& state,
                                                                const Signatures& signatures) {
    if (place.kind == ExprKind::Identifier) {
        if (const Type* local_type = body.type_if_local(place); local_type != nullptr) return *local_type;
        return find_visible_global_type(place.name, /*explicit_global_qualification=*/false, body);
    }
    if (place.kind == ExprKind::Member) {
        // resolve_member_field_type reads the precomputed field-type
        // cache, which is what makes an implicit `this.f` resolvable at
        // all; infer_expr_type answers the same question from the
        // Program for the base-typed spellings the cache cannot reach.
        // Neither subsumes the other, so both are consulted.
        if (std::optional<Type> field_type = resolve_member_field_type(place, body, state, signatures);
            field_type.has_value()) {
            return field_type;
        }
        return infer_expr_type(place, body, signatures);
    }
    if (place.kind == ExprKind::Subscript) return infer_expr_type(place, body, signatures);
    return std::nullopt;
}

// How to name an assignment target in a diagnostic. Mirrors
// validate_deref_expr's own place description, which faced the same
// problem: an Expr carries a `name` only for the shapes that have one,
// so a Member has to be rebuilt from its base and field, and a Subscript
// has no name of its own at all. Getting this wrong is visible -- a
// `nullptr` assigned to an array element used to reach codegen's
// representation backstop and be reported against an empty name
// ("assigning ''"), because the only description available that far down
// was a name the expression never had.
[[nodiscard]] std::string describe_assignment_place(const Expr& place) {
    if (place.kind == ExprKind::Member) {
        if (place.lhs == nullptr) return place.name;
        std::string base = describe_assignment_place(*place.lhs);
        return base.empty() ? place.name : base + "." + place.name;
    }
    if (place.kind == ExprKind::Subscript) {
        if (place.lhs == nullptr) return "this element";
        std::string base = describe_assignment_place(*place.lhs);
        return base.empty() ? std::string("this element") : base + "[...]";
    }
    return place.name;
}

// spec §6: the complete set of conversion rules a value must satisfy
// against the declared type of the place it is bound to. The single
// definition of that question, so that every position which binds a
// value to a declared type gets the same answer.
//
// The five conversion checks existed before this was extracted, but each
// position that ran them spelled the sequence out by hand, and no two
// spellings agreed: the local declaration/assignment path ran all five,
// the global assignment path ran four (no enum), the expression-level
// assignment path ran five, and four positions -- a global declaration's
// own initializer, a constructor's member-initializer list, a field's
// default member initializer, and a parameter's default argument -- ran
// none at all. So `uint8_t u = a;` was rejected as a local and accepted
// as a global; `S() : u{a} {}` accepted what `s.u = a;` rejected. A
// hand-copied sequence is exactly the shape that drifts, so it is now
// one function called from every position rather than a convention to
// be re-followed at the next one.
[[nodiscard]] std::expected<void, DataflowError> check_value_binding_conversions(const Type& target_type,
                                                                                const Expr& value, const Body& body,
                                                                                const Signatures& signatures,
                                                                                const SourceLocation& loc,
                                                                                const std::string& target_name,
                                                                                bool report_errors) {
    // Before all of them, because a braced list has no type of its own
    // for them to ask about ([dcl.init.list]): its *elements* are what
    // bind, each to the field or element it initializes, and each of
    // them comes straight back here.
    if (value.kind == ExprKind::BracedInitList) {
        return check_aggregate_element_conversions(target_type, value.args, body, signatures, loc, target_name,
                                                   report_errors);
    }
    // First, because it is not a conversion question: the other five ask
    // what `value` converts to, and a `void` expression has nothing to
    // convert. See check_expression_yields_a_value.
    if (auto _r = check_expression_yields_a_value(value, body, signatures, loc, "the value bound to '" + target_name + "'",
                                                  report_errors);
        !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (auto _r = check_function_pointer_assignment(target_type, value, body, signatures, loc, target_name,
                                                    report_errors);
        !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (auto _r = check_raw_pointer_assignment(target_type, value, body, signatures, loc, target_name, report_errors);
        !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (auto _r = check_nullptr_assignment(target_type, value, loc, target_name, report_errors); !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (auto _r = check_scalar_conversion(target_type, value, body, signatures, loc, "'" + target_name + "'",
                                          report_errors);
        !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    if (report_errors) {
        if (auto _r = check_enum_conversion_compatibility(target_type, value, body, signatures, loc); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    return {};
}

// [dcl.init.aggr]/4: "the explicitly initialized elements of the
// aggregate are the elements ... copy-initialized from the corresponding
// initializer-clause". Each element therefore gets exactly the checks
// its field would get from any other binding position -- this recurses
// straight back into check_value_binding_conversions rather than
// re-deciding what a legal binding is, so a nested aggregate's elements
// are checked to the same standard as a top-level declaration's.
//
// Nothing checked those elements before, in either of the two spellings.
// `struct H { int* p; }; bool b = true; H h{&b};` bound a `bool*` into
// an `int*` field, `H h = {&b};` likewise, and `H h{&c};` for a `const
// int c` handed out a mutable pointer into a const object -- which is
// how the const escape survived the checks aimed at declarations,
// assignments and call arguments: this position was not one of them.
//
// Fewer elements than fields is fine ([dcl.init.aggr]/5 value-initializes
// the rest); more is another rule's error, not this one's.
[[nodiscard]] std::expected<void, DataflowError> check_aggregate_element_conversions(
    const Type& aggregate_type, const std::vector<ExprPtr>& elements, const Body& body, const Signatures& signatures,
    const SourceLocation& loc, const std::string& target_name, bool report_errors) {
    if (!report_errors || elements.empty() || body.program == nullptr) return {};
    // [dcl.init.list]/3.3: "if T is a class type and the initializer list
    // has a single element of type cv U, where U is T or a class derived
    // from T, the object is initialized from that element" -- by
    // *constructor*, so §6.5(2) decides, and the field-wise aggregate
    // path below never applies. Reached for a class, whose fields
    // find_struct_def cannot resolve at all, and for a struct, whose
    // first field would otherwise be bound to the whole object.
    if (aggregate_type.kind == TypeKind::Named && elements.size() == 1 && elements[0] != nullptr &&
        is_named_record_type(aggregate_type, body) &&
        is_bare_same_type_copy_source(*elements[0], aggregate_type, body, signatures)) {
        return check_record_copy_element_binding(aggregate_type, *elements[0], body, signatures, loc, target_name,
                                                 report_errors);
    }
    if (aggregate_type.kind == TypeKind::Array && aggregate_type.element != nullptr) {
        // Not every caller has a name to give ({} initialization of a
        // temporary, for one), and "''s elements" reads as a defect in
        // the compiler rather than in the program.
        const std::string element_name =
            target_name.empty() ? std::string("an array element") : target_name + "'s elements";
        for (const ExprPtr& element : elements) {
            if (element == nullptr || element->kind == ExprKind::BracedInitList) continue;
            // An array element is copy-initialized exactly like a struct
            // field is ([dcl.init.aggr]/4 covers both), so it asks the
            // same §6.5(2) question -- this branch used to run only the
            // conversion checks below, which is how `C arr[1]{x};` copied
            // a class that has no copy constructor while `C y = x;` and
            // `struct Holder { C c; } h{x};` were both rejected.
            if (auto _r = check_record_copy_element_binding(*aggregate_type.element, *element, body, signatures, loc,
                                                           element_name, report_errors);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (auto _r = check_value_binding_conversions(*aggregate_type.element, *element, body, signatures, loc,
                                                          element_name, report_errors);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        return {};
    }
    if (aggregate_type.kind != TypeKind::Named) return {};
    const StructDef* def = find_struct_def(*body.program, aggregate_type.name);
    if (def == nullptr) return {};
    for (std::size_t index = 0; index < elements.size() && index < def->fields.size(); ++index) {
        if (elements[index] == nullptr) continue;
        const Expr& element = *elements[index];
        if (element.kind == ExprKind::BracedInitList) {
            if (auto _r = check_aggregate_element_conversions(def->fields[index].type, element.args, body, signatures,
                                                              loc, aggregate_type.name + "::" + def->fields[index].name,
                                                              report_errors);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            continue;
        }
        // Struct-typed fields only -- see check_record_copy_element_binding's
        // own comment: a class-typed field of a struct is rejected by the
        // field validation in codegen/layout.cppm, and answering the copy
        // question first would name a fix that rejection then forbids.
        if (def->fields[index].type.kind == TypeKind::Named &&
            find_struct_def(*body.program, def->fields[index].type.name) != nullptr) {
            if (auto _r = check_record_copy_element_binding(def->fields[index].type, element, body, signatures, loc,
                                                           aggregate_type.name + "::" + def->fields[index].name,
                                                           report_errors);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        if (auto _r = check_value_binding_conversions(def->fields[index].type, element, body, signatures, loc,
                                                      aggregate_type.name + "::" + def->fields[index].name,
                                                      report_errors);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    return {};
}

// spec §6.5(2): an aggregate element of *record* type is copy-initialized
// from a same-type lvalue element, and a record with a user-declared
// destructor or copy assignment operator (and no user-declared copy
// constructor) has no copy constructor to call. Every other position that
// copy-initializes a record -- a local declaration, a by-value argument,
// a `return` operand -- already asks this; the aggregate-element position
// did not, so `struct Holder { S s; }; Holder h{a};` copied an `S` the
// spec says cannot be copied, and (before the destructor fix in
// codegen/lifetime.cppm) leaked the copy on top.
//
// Asks about any named record, struct or class: the same question is
// reached from three element positions -- a struct field, an *array*
// element, and a braced list holding a single operand of the
// destination's own type ([dcl.init.list]/3.3, which is copy
// construction and not aggregate initialization at all). Only the
// struct-field caller narrows it to a struct-typed field, and it does so
// at its own call site because the reason belongs to it: a class-typed
// field of a struct is ill-formed independently (codegen/layout.cppm's
// field validation), so answering "'S' is not copy-constructible" there
// would name a fix (give 'S' a copy constructor) that the next
// diagnostic then forbids.
[[nodiscard]] std::expected<void, DataflowError> check_record_copy_element_binding(
    const Type& field_type, const Expr& element, const Body& body, const Signatures& signatures,
    const SourceLocation& loc, const std::string& target_name, bool report_errors) {
    if (!report_errors || body.program == nullptr) return {};
    if (field_type.kind != TypeKind::Named) return {};
    if (!is_named_record_type(field_type, body)) return {};
    if (is_freely_copyable_class_value_source(element, field_type, body, signatures)) return {};
    if (!is_bare_same_type_copy_source(element, field_type, body, signatures)) return {};
    if (is_copy_constructible(field_type.name, *body.program)) return {};
    // [dcl.fct.def.delete]/2 answers first: naming a deleted function is
    // a strictly more specific answer than §6.5(2)'s suppression rule.
    const Function* user_copy_ctor = find_user_declared_copy_ctor(field_type.name, *body.program);
    if (user_copy_ctor != nullptr && user_copy_ctor->is_deleted) {
        return std::unexpected(DataflowError(
            deleted_function_error_message("the copy constructor of '" + field_type.name + "'", user_copy_ctor->loc),
            loc));
    }
    return std::unexpected(DataflowError(std::string(record_keyword(field_type.name, *body.program)) + " '" +
                                             field_type.name + "' is not copy-constructible (spec §6.5(2)) -- '" +
                                             target_name + "' cannot be initialized this way",
                                         loc));
}

// spec §6: the conversion rules above, applied to an assignment whose
// target is written as an arbitrary place expression.
//
// Only the MirStatementKind::Assign *statement* path used to run them,
// and that path only exists for a bare-name target: mir.cppm lowers `x =
// v;` to an Assign node and everything else -- `s.f = v;`, `a[i] = v;`
// -- to an opaque Eval of the whole expression, which is sound for the
// initialization tracking that choice was made for (a field write never
// changes the enclosing local's own state) but leaves this expression
// handler as the only place those assignments are ever seen. It checked
// two of the five for a Member target and none for a Subscript one, so
// `s.u = some_int8;` and `a[0] = some_unsigned;` silently converted,
// `s.p = &wrong_type;` silently type-confused a raw pointer, and
// `enum_array[0] = OtherEnum::X;` was accepted outright -- each one the
// exact defect the corresponding check was written to prevent, reachable
// by spelling the target differently.
//
// Applied to an Identifier target here too, which is not redundant: a
// statement-level `x = v;` never reaches this handler at all (it became
// an Assign node), so the only Identifier assignments that arrive here
// are the nested ones -- `f(x = v)`, `while ((x = v))` -- which had the
// same two-of-five coverage a Member target did.
[[nodiscard]] std::expected<void, DataflowError> check_assignment_target_conversions(const Expr& place, const Expr& value,
                                                                                     const Body& body,
                                                                                     const DataflowState& state,
                                                                                     const Signatures& signatures,
                                                                                     const SourceLocation& loc,
                                                                                     bool report_errors) {
    std::optional<Type> target_type = resolve_assignment_place_type(place, body, state, signatures);
    if (!target_type.has_value()) return {};
    return check_value_binding_conversions(*target_type, value, body, signatures, loc, describe_assignment_place(place),
                                           report_errors);
}

// The one value an initializer binds, or nullptr when it binds none or
// more than one. `{}` zero-initializes and binds nothing; `{a, b, ...}`
// is a constructor argument list, which check_constructor_arguments
// already checks against the constructor's own parameters. A single
// brace argument is deliberately treated as a bound value: for a scalar,
// pointer, function pointer or enum target -- the only kinds the five
// checks look at -- `x{v}` *is* direct initialization, and a class
// target makes every one of them a no-op anyway.
[[nodiscard]] const Expr* single_bound_value(const ExprPtr& assigned, const std::vector<ExprPtr>& brace_args) {
    if (assigned != nullptr) return assigned.get();
    if (brace_args.size() == 1) return brace_args[0].get();
    return nullptr;
}

// The same answer for an InitializerScope, whose two spellings are held
// as raw pointers because they are borrowed from whichever declaration
// the position came from.
[[nodiscard]] const Expr* single_bound_value_ptr(const Expr* assigned, const std::vector<ExprPtr>* brace_args) {
    if (assigned != nullptr) return assigned;
    if (brace_args != nullptr && brace_args->size() == 1) return (*brace_args)[0].get();
    return nullptr;
}

// spec §6: a constructor's member-initializer list binds a value to a
// field's declared type, so it is checked exactly like an assignment to
// that field would be. It was checked by nothing at all before: `S() :
// u{a} {}` accepted an `int8_t` into a `uint8_t` field that `s.u = a;`
// rejects, and `fp{&wrong}` bound a mismatched function pointer.
//
// The expressions are checked against `body`, the constructor's own,
// because they may name its parameters -- resolve_locals deliberately
// walks the member-initializer list alongside the body for that reason,
// so a parameter use in `fp{f}` already knows which declaration it
// refers to.
//
// An entry naming a direct base class rather than a field is skipped:
// its arguments are the base constructor's, checked by that call.
[[nodiscard]] std::expected<void, DataflowError> check_member_initializer_conversions(
    const Function& fn, const Body& body, const Signatures& signatures, const ClassFieldTypes& class_field_types) {
    if (fn.member_initializers.empty() || fn.member_owner_class.empty()) return {};
    auto fields = class_field_types.find(fn.member_owner_class);
    if (fields == class_field_types.end()) return {};
    for (const MemberInitializer& init : fn.member_initializers) {
        auto field = fields->second.find(init.member_name);
        if (field == fields->second.end()) continue;
        const Expr* value = single_bound_value(init.initializer.expr, init.initializer.brace_args);
        if (value == nullptr) continue;
        if (auto _r = check_value_binding_conversions(field->second, *value, body, signatures, init.loc,
                                                      init.member_name, /*report_errors=*/true);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    return {};
}

// spec §6: the expression positions that bind a value to a declared type
// outside any function body -- a global variable's initializer, a class
// or struct field's default member initializer, and a parameter's
// default argument -- none of which ran a single one of the five checks
// before. A global `uint8_t u = a;` was accepted where the identical
// local declaration is rejected, `int* g_p = &g_b;` bound a `bool*`, and
// a default argument converted whatever it liked at every call site that
// took it.
//
// The list of those positions is not written here: it comes from
// for_each_initializer_scope, which is also what interface validation
// walks, so the two cannot disagree about which positions exist.
[[nodiscard]] std::expected<void, DataflowError> check_initializer_scope_conversions(const Program& program,
                                                                                     const Signatures& signatures) {
    return for_each_initializer_scope(program, [&](const InitializerScope& scope)
                                                   -> std::expected<void, DataflowError> {
        const Expr* value = single_bound_value_ptr(scope.expr, scope.brace_args);
        if (value == nullptr) return {};
        return check_value_binding_conversions(*scope.declared_type, *value, scope.body, signatures, scope.loc,
                                               scope.name, /*report_errors=*/true);
    });
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
    // Spec §7.1(4)/§5.1(5.1) gate `*E` on `E` being "of pointer type",
    // which is a question about the pointer, not about how the program
    // reached it. A reference *to* a pointer is still that pointer --
    // binding one changes nothing about what indirection through it
    // does or what it must be licensed by -- so the reference is looked
    // through here exactly as the class-`operator*` test just above
    // already does. Testing `resolved` directly instead meant every
    // spelling that produces a `T*&` (an `int*& q = p;` local, an
    // `int*&` parameter, and -- because by_reference_capture_type wraps
    // the captured type -- a `[&p]` capture, which is how this
    // surfaced) failed the "is it a pointer at all?" test and was
    // rejected before the unsafe context was ever consulted. That made
    // `[[scpp::unsafe]]` powerless over exactly the operation it exists
    // to license, and reported it as though a pointer had never been
    // involved.
    bool is_raw_ptr = underlying != nullptr && underlying->kind == TypeKind::Pointer;
    bool is_fn_ptr = underlying != nullptr && is_function_pointer(*underlying);
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
    // Only an *identifier* operand names a local whose move/borrow state
    // there is anything to look up. Everything else -- a Member (no
    // separate per-field state), a Call's freshly-returned pointer, a
    // nested `*` in `**pp`, a subscript -- has only the type-level and
    // unsafe-context rules already applied above, and `Expr::name` is
    // either empty or holds something that is not a variable name at
    // all (a callee, a field). Listing the shapes that opt *out* left
    // every unlisted one falling into the lookup below and being
    // reported as "use of variable '' that is out of scope here": a
    // message naming no variable, about a variable that was never
    // involved. The question is "is there a local to consult?", so ask
    // that.
    if (operand.kind != ExprKind::Identifier || expr.implicit_arrow_deref) return {};
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

// Every dereference on the way to a place, validated for its own sake.
//
// `std::move(E)` resolves E to a place and returns without walking it,
// so no other arm of this checker ever visits the `*` inside E. That
// was invisible while a dereference could not be part of a place at
// all: E was rejected outright. Now that it can, the rule §5.1(5.1) and
// §7.1(4) state about `*` -- a raw-pointer dereference is licensed only
// inside `[[scpp::unsafe]] { }` -- has to be applied here, because it is
// a rule about the dereference wherever it occurs, not about what the
// surrounding expression does with the object.
[[nodiscard]] std::expected<void, DataflowError> validate_place_indirections(const Expr& expr, const DataflowState& state,
                                                                             const Body& body,
                                                                             const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Member:
        case ExprKind::Subscript:
            if (expr.lhs == nullptr) return {};
            return validate_place_indirections(*expr.lhs, state, body, signatures);
        case ExprKind::Call:
            // `*h`/`h->m` on a class type: the selected operator's own
            // receiver is the next step inwards.
            if (expr.lhs == nullptr || (expr.name != "operator_deref" && expr.name != "operator_arrow")) return {};
            return validate_place_indirections(*expr.lhs, state, body, signatures);
        case ExprKind::Unary: {
            if (expr.unary_op != UnaryOp::Deref || expr.lhs == nullptr) return {};
            if (auto _r = validate_place_indirections(*expr.lhs, state, body, signatures); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            return validate_deref_expr(expr, state, body, signatures);
        }
        default:
            return {};
    }
}

// The subscript counterpart of validate_deref_expr, and deliberately a
// peer of it rather than an inline check: [expr.sub]/1 defines `E1[E2]`
// as `*((E1)+(E2))`, so a raw-pointer subscript is *both* halves of spec
// §5.1(5.1) at once -- the pointer arithmetic and the indirection
// through its result -- and §5.1(6) makes it ill-formed in a safe
// context. Neither half caught it before: validate_deref_expr never sees
// a Subscript node (it handles a written `*E`), and the arithmetic is
// implied by the subscript rather than spelled as an [expr.add]
// operator, so it never reached check_binary_expr_operand_types either.
//
// It has to be a shared function because a Subscript is reached from two
// independent entry points that must agree -- apply_expr's Subscript
// case for a plain read, and resolve_borrow_source_root's Subscript case
// for `&p[i]`/`int& r = p[i]`, which does not go through apply_expr at
// all. validate_deref_expr is already called from both for exactly this
// reason; the subscript half simply never grew the counterpart, so
// `&p[1]` performed unlicensed pointer arithmetic while `p[1]` did not.
//
// The reference-stripped operand type is what decides this, per #465:
// `int*& r = p; r[0]` reaches the same pointer and must be gated
// identically. An array (`a[0]`) and a class with `operator[]`
// (`std::span`, `std::vector`) are not pointer types and stay ungated --
// checked indexing is the safe way to do this and must remain available
// in a safe context.
[[nodiscard]] std::expected<void, DataflowError> validate_subscript_expr(const Expr& expr, const DataflowState& state, const Body& body,
                             const Signatures& signatures) {
    if (state.unsafe_depth != 0) return {};
    std::optional<Type> object_type = infer_expr_type(*expr.lhs, body, signatures);
    if (!object_type.has_value() || binary_operand_type(*object_type).kind != TypeKind::Pointer) return {};
    return std::unexpected(DataflowError("cannot subscript a raw pointer outside '[[scpp::unsafe]] { }' "
                         "(spec §5.1(5.1))",
        state.current_loc));
}

// Handles a raw-pointer/function-pointer/`*this` Deref expression used as
// a plain read (not as a borrow source -- see resolve_borrow_source_root's
// own Deref case for that). Class overloads of `operator*` are rewritten// to ordinary calls earlier in the pipeline, so they bypass this helper
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
    // Every other operand shape -- a call result (`p.get()->m()`), a
    // subscript, a parenthesized expression -- used to be rejected here
    // outright ("dereference ('*') currently only supports a plain local
    // raw/function pointer variable, '*this', or a captured field of one
    // ('this.field')"). That was a pass enumerating the operand shapes it
    // had been taught rather than deciding anything: nothing below needs
    // an Identifier except the per-local borrow bookkeeping, which
    // already opts out for any non-Identifier operand, and
    // validate_deref_expr -- the function that actually enforces
    // §5.1(5.1)/§7.1(4) -- had already grown its own explicit
    // `ExprKind::Call` arm for precisely this shape. So the type-level
    // and unsafe-context rules were fully in place and unreachable behind
    // a shape test.
    if (!report_errors) return {}; // purely diagnostic: doesn't move p or change any tracked state
    if (auto _r = validate_deref_expr(expr, state, body, signatures); !_r.has_value()) return std::unexpected(std::move(_r).error());
    if (!is_plain_identifier) return {}; // no separate borrow-tracking key -- see the comment above
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

    // spec ch05 §5.7 / §6.2(9)-(10): a `T&` parameter may only be
    // satisfied by a place that is itself reachable mutably. Asked here,
    // once, of the argument *expression* -- the place actually being
    // borrowed -- and before the reborrow bookkeeping below, because it
    // is a property of the argument and not of how the borrow happens to
    // be accounted for.
    //
    // It used to live inside the `else` of that bookkeeping, so it was
    // skipped entirely whenever the argument resolved to a tracked
    // reborrow lender; validate_reborrow_lender's own copy of the
    // question -- keyed on the *lender local's* declared type rather than
    // on the argument -- stood in for it there. Two answers to one
    // question in two positions, and the lender-keyed one is wrong
    // exactly when the borrow crosses an indirection: `bump(*handle)`
    // with `handle` a `const std::shared_ptr<Cell>&` borrows the
    // *pointee*, whose mutability is `operator*`'s declared return type,
    // not the handle's.
    if (is_mutable && is_read_only_reachable(arg, body, signatures)) {
        // format_roots answers "which *local* does this borrow reach?",
        // and a const global reaches none -- so this used to print
        // "cannot pass <unknown> by mutable reference", naming nothing the
        // reader could act on. The argument expression is right here;
        // describe it, and say where its constness comes from.
        std::string subject = roots.empty() ? describe_assignment_place(arg) : format_roots(body, roots);
        if (roots.empty()) subject = subject.empty() ? std::string("this argument") : "'" + subject + "'";
        std::string message{"cannot pass "};
        message += subject;
        message += " by mutable reference: it is only reachable through a read-only (const) reference";
        std::string const_source = describe_const_source(arg, body, signatures);
        if (!const_source.empty()) {
            message += " (";
            message += const_source;
            message += ")";
        }
        return std::unexpected(DataflowError(message, state.current_loc));
    }

    // Passing an *already-bound* local reference variable directly (`f(r)`
    // where `r` is itself `T& r = ...;`/`const T& r = ...;`) is a
    // reborrow, not a fresh independent borrow: `r` already holds the one
    // live access to `root` (nothing else can coexist with it -- any
    // other attempt to borrow `root` while `r` is alive is already
    // rejected by apply_reference_binding/this same function's
    // persistent-conflict check below), so temporarily re-lending that
    // same access to a callee can't create a new conflict. What remains
    // to check here is exclusivity only.
    std::optional<LocalId> lender = resolve_reborrow_lender(arg, body, signatures);
    bool tracked_reborrow = reborrow_is_tracked_against_lender(lender, body);
    if (tracked_reborrow) {
        if (auto _r = validate_reborrow_lender(*lender, is_mutable, state, body, report_errors); !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    } else {
        // The general case: `arg` doesn't reach a locally-bound
        // reference/span lender at all, so the borrow is asserted
        // directly against the root place it reaches.
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

    // Which key this argument occupies for the duration of the call is
    // the same question reborrow_is_tracked_against_lender already
    // answers everywhere else: a tracked reborrow is accounted for
    // against its *lender*, not against the root the lender reaches.
    // Entering it against the root instead would re-assert the very
    // borrow the lender itself installed, and so report the lender's own
    // binding as a conflicting second borrow -- which is exactly what
    // used to happen, and why `[&r]` (whose captures are checked through
    // this function against a map that outlives the construction) could
    // not name a reference at all. Keying on the lender still catches a
    // genuine duplicate: `f(r, r)` lends the same access twice and both
    // arguments land on the same key.
    RootSet in_call_keys = tracked_reborrow ? RootSet{*lender} : roots;
    for (LocalId root : in_call_keys) {
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
    if (!is_named_record_type(destination_type, body)) return binding;
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
    // spec §16.3(1.3): an argument of a function call or of a
    // constructor call. Asked here, ahead of the constructor redirect
    // and of overload resolution, so that both kinds of call get it and
    // so that the diagnostic describes the argument rather than the
    // absence of a matching overload -- a generic callee has no such
    // absence to report at all (`ident(h())` deduced `T = void` and
    // instantiated a function with a `void` parameter, left for codegen
    // to reject).
    for (std::size_t index = 0; index < expr.args.size(); ++index) {
        if (expr.args[index] == nullptr) continue;
        if (auto _r = check_expression_yields_a_value(*expr.args[index], body, signatures, state.current_loc,
                                                      "argument " + std::to_string(index + 1) + " of a call",
                                                      report_errors);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    if (!expr.lhs && direct_call_type.has_value() && direct_call_type->kind == TypeKind::Named &&
        direct_call_type->name == expr.name && is_named_record_type(*direct_call_type, body)) {
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
    if (report_errors && sig != nullptr && sig->is_deleted) {
        return std::unexpected(DataflowError(deleted_function_error_message("'" + callee_display + "'", sig->loc), state.current_loc));
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
            // [dcl.init]/17.9 copy-initializes a by-value parameter from
            // its argument, so it is a binding position like any other
            // and gets the same raw-pointer conversion check a
            // declaration or an assignment gets. Overload resolution's
            // own argument_matches_parameter is a *selection* rule and is
            // deliberately lenient about pointee qualification; it is not
            // the place that answers whether the selected binding is
            // sound. Without this, `take_ptr(&c)` for a `const int c` was
            // reachable only through a guard scoped to the syntactic
            // `&expr` shape -- which said nothing about `take_ptr(w)` for
            // a const array, or about any pointer that reached the
            // argument through some other expression.
            if (report_errors && have_effective_param_type) {
                std::string argument_name{"parameter "};
                argument_name += std::to_string(param_index + 1);
                argument_name += " of ";
                argument_name += callee_display;
                if (auto _r = check_raw_pointer_assignment(effective_param_type, arg, body, signatures,
                                                           state.current_loc, argument_name, report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
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
                is_named_record_type(sig->param_types[param_index], body);
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
                return std::unexpected(DataflowError("passing " +
                                     std::string(record_keyword(sig->param_types[param_index].name, *body.program)) +
                                     " '" + sig->param_types[param_index].name +
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
    // spec §16.3(1.3): the constructor-call half of the same rule
    // check_call_arguments applies to a function call. Also the only
    // check a braced *scalar* declaration (`int t{h()};`) gets from
    // movecheck at all -- it has no constructor to resolve against, so
    // everything below no-ops for it and the question would otherwise
    // fall through to codegen.
    for (std::size_t index = 0; index < ctor_args.size(); ++index) {
        if (ctor_args[index] == nullptr) continue;
        if (auto _r = check_expression_yields_a_value(*ctor_args[index], body, signatures, state.current_loc,
                                                      "argument " + std::to_string(index + 1) + " of a call",
                                                      report_errors);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    std::string class_name = constructed_type.name;
    // [class.copy.ctor]/6 + [dcl.init]/16.6.2: a record with no
    // user-declared copy constructor still has one *implicitly declared*,
    // and direct-initialization from a single operand of the
    // destination's own type is *copy construction* -- never an aggregate
    // element bound to field 0 (which is what `T(other)` used to become,
    // reporting "enum class values do not implicitly convert" for
    // `Token` -> `Token::kind` inside a monomorphized std::vector<Token>).
    // When the record declares none of its own, spec §6.5(2) alone
    // decides whether one exists, and that question has to be asked
    // before a constructor is selected: the arity shortcut below (a lone
    // candidate of the right arity wins with no type check at all -- the
    // #484 arity gate) otherwise picks the record's unrelated
    // one-parameter constructor, and `T y{x};` / `T y = {x};` / a copy
    // into an array element slip past §6.5(2) entirely while `T y = x;`
    // is correctly rejected. One question, one message: every spelling of
    // the same copy must reach the same rule.
    if (report_errors && ctor_args.size() == 1 && ctor_args[0] != nullptr && body.program != nullptr &&
        is_named_record_type(constructed_type, body) &&
        !has_user_declared_copy_ctor(class_name, *body.program) &&
        !is_freely_copyable_class_value_source(*ctor_args[0], constructed_type, body, signatures) &&
        is_bare_same_type_copy_source(*ctor_args[0], constructed_type, body, signatures) &&
        !is_copy_constructible(class_name, *body.program)) {
        return std::unexpected(
            DataflowError(std::string(record_keyword(class_name, *body.program)) + " '" + class_name +
                              "' is not copy-constructible (spec §6.5(2)) -- this construction is not permitted",
                          state.current_loc));
    }
    // [over.match.ctor]: which constructor a call selects has exactly one
    // answer, and resolve_constructor_signature is the one implementation
    // of it -- the same one this function already used for the
    // zero-argument case just below, the same [over.ics.rank] algebra
    // codegen's resolve_constructor_overload_exact and the constant
    // evaluator rank with.
    //
    // What stood here was a second, weaker answer to the same question: a
    // candidate of the right *arity* won outright whenever it was the only
    // one of that arity, with no argument type examined at all -- and only
    // when it was *not* the only one did any type matching happen, in a
    // chain that then took the first exact match and, failing that, the
    // first non-generic candidate. A lone constructor was therefore
    // selected for arguments it cannot accept, and everything downstream
    // of `sig` (the deleted-, private- and unsafe-constructor
    // diagnostics, and every argument's borrow/move effect) was applied
    // against a signature overload resolution would never choose. The
    // deleted-constructor message asserted the very thing the arity gate
    // had skipped checking -- "a deleted function takes part in overload
    // resolution and was selected here" -- for a call with no viable
    // constructor at all.
    const FunctionSignature* sig = resolve_constructor_signature(class_name, ctor_args, body, signatures);
    if (sig == nullptr && report_errors && ctor_args.empty()) {
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
        return std::unexpected(DataflowError("type '" + class_name +
                                "' has no default constructor; no constructor of '" + class_name +
                                "' matches 0 arguments",
                            state.current_loc));
    }
    // [class.copy.ctor]/6: a record type with no user-declared copy
    // constructor has one *implicitly declared*, and [dcl.init]/16.6.2.2
    // only reaches parenthesized aggregate initialization when "no
    // constructor is viable" -- so a single argument already of the
    // constructed type is copy or move construction, never an aggregate
    // element bound to field 0. Nothing modelled that implicit
    // constructor here: a plain aggregate has no constructor *signature*
    // at all, so `sig` stayed null and `T(other)` fell straight into the
    // aggregate branch below, which bound the whole `T` to its first
    // field -- e.g. `new (&fresh[i]) T(other.at(i))` inside a
    // monomorphized std::vector<Token> reported "enum class values do
    // not implicitly convert" for `Token` -> `Token::kind`.
    //
    // Whether that constructor exists is spec §6.5(2)'s question, and it
    // is now asked at the top of this function instead of here, because
    // `sig` is not null in every spelling that asks it -- see that
    // block's own comment. What is left here is only the "this is not
    // aggregate initialization" half, which still has to run for a
    // *deleted* copy constructor (the one case the block above steps
    // aside for, since a deleted function is selected and then rejected
    // by name -- [dcl.fct.def.delete]/2).
    if (sig == nullptr && ctor_args.size() == 1 && ctor_args[0] != nullptr && body.program != nullptr &&
        is_named_record_type(constructed_type, body)) {
        std::optional<Type> arg_type = infer_expr_type(*ctor_args[0], body, signatures);
        const Type* source = arg_type.has_value() ? &*arg_type : nullptr;
        if (source != nullptr && source->kind == TypeKind::Reference && source->pointee != nullptr) {
            source = source->pointee.get();
        }
        if (source != nullptr && source->kind == TypeKind::Named && source->name == constructed_type.name) {
            if (report_errors && !is_copy_constructible(class_name, *body.program) &&
                is_bare_same_type_copy_source(*ctor_args[0], constructed_type, body, signatures)) {
                return std::unexpected(DataflowError(
                    std::string(record_keyword(class_name, *body.program)) + " '" + class_name +
                        "' is not copy-constructible (spec §6.5(2)) -- this construction is not permitted",
                    state.current_loc));
            }
            return {};
        }
    }
    // [dcl.init.aggr]: a record that declares constructors is not an
    // aggregate, so with candidates present and none selected the call is
    // ill-formed -- it is not silently something else. Reached only after
    // the same-type block above, which owns spec §6.4(2)/§6.5(2).
    if (sig == nullptr && report_errors && !ctor_args.empty() && body.program != nullptr &&
        is_named_record_type(constructed_type, body)) {
        if (std::optional<std::string> failure =
                describe_constructor_selection_failure(class_name, ctor_args, body, signatures);
            failure.has_value()) {
            return std::unexpected(DataflowError(*std::move(failure), state.current_loc));
        }
    }
    // [dcl.init.aggr]/4: with no constructor resolved and arguments
    // present, this is aggregate initialization -- the same rule, and the
    // same function, that the `T x = {...}` spelling goes through in
    // check_value_binding_conversions.
    if (sig == nullptr && !ctor_args.empty()) {
        if (auto _r = check_aggregate_element_conversions(constructed_type, ctor_args, body, signatures,
                                                          state.current_loc, constructed_type.name, report_errors);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    if (report_errors && sig != nullptr && sig->is_deleted) {
        return std::unexpected(DataflowError(deleted_function_error_message("the constructor of '" + class_name + "'", sig->loc),
            state.current_loc));
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
                is_named_record_type(sig->param_types[param_index], body);
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
                return std::unexpected(DataflowError("passing " +
                                     std::string(record_keyword(sig->param_types[param_index].name, *body.program)) +
                                     " '" + sig->param_types[param_index].name +
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

        // A nested brace-enclosed initializer list is not a leaf. Its
        // elements are ordinary expressions and are evaluated exactly
        // where they are written, so each one is walked here: a
        // `std::move` or a borrow inside `Out o{{std::move(x)}, 3}` has
        // to be seen by the move checker just as it would inside a
        // call's argument list.
        case ExprKind::BracedInitList:
            for (const ExprPtr& element : expr.args) {
                if (element == nullptr) continue;
                if (auto _r = apply_expr(*element, /*is_move_target_context=*/false, state, body, signatures,
                                         report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
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
            // spec §6.2(3): `std::move(E)` is a syntactic ownership-state
            // transition on the object `E` designates, not just on class
            // types, and §6.2(1) puts objects "of automatic, static,
            // thread, or member storage duration" in the two-state model
            // -- member storage included. `E` is any expression naming a
            // place: a local, a member of it however deeply nested, a
            // member of the current object, an element at a literal
            // index, or any of those reached through a reference
            // binding. What it may *not* be is an expression that names
            // no identifiable object, because then there is nowhere to
            // record the state transition; explain_untrackable_place
            // says which of those it was rather than listing the forms
            // that would have worked.
            std::vector<LocalId> moved_through;
            std::optional<Place> moved = tracked_place_of(*expr.lhs, state, body, &moved_through);
            if (!moved.has_value()) {
                if (expr.lhs->kind == ExprKind::Identifier &&
                    find_visible_global_for_name(expr.lhs->name, expr.lhs->explicit_global_qualification, body) !=
                        nullptr) {
                    return {};
                }
                if (report_errors) {
                    if (expr.lhs->kind == ExprKind::Identifier) {
                        return std::unexpected(DataflowError("unknown variable '" + expr.lhs->name + "'",
                            state.current_loc));
                    }
                    return std::unexpected(DataflowError("cannot move from " + explain_untrackable_place(*expr.lhs),
                        state.current_loc));
                }
                return {};
            }
            if (report_errors) {
                if (auto _r = validate_place_indirections(*expr.lhs, state, body, signatures); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            std::string name = body.describe_place(*moved);
            LocalState current = lookup(state.locals, *moved);
            if (report_errors && current != LocalState::Initialized) {
                return std::unexpected(DataflowError(describe_bad_state(name, current),
                    state.current_loc));
            }
            if (report_errors) {
                // No read-only check here, deliberately. §6.2(3) attaches
                // the state transition to the *syntactic form*
                // `std::move(E)` and conditions it on nothing else; it
                // says nothing about `E` being read-only, and the
                // ordinary C++ meaning of `std::move` on a `const`
                // lvalue -- a value-preserving cast to `const T&&`, used
                // to select a `const&&`-qualified overload -- is
                // well-formed. libs/std's
                // `__move_only_function_invoke_const_rvalue` has no other
                // spelling for that. Whether §6.2(3) is meant to apply to
                // a `const` object at all is a gap in the spec, not a
                // licence to reject here.
                //
                // Consuming an object consumes everything it contains,
                // so a subobject that is already gone makes the whole
                // one unusable (spec §6.2(5)/(6) applied to the
                // memberwise move §6.4(5) would perform).
                if (std::optional<Place> gone = find_moved_subobject(state.locals, *moved); gone.has_value()) {
                    return std::unexpected(DataflowError("cannot move '" + name + "': its member '" +
                                         body.describe_place(*gone) + "' has already been moved out",
                        state.current_loc));
                }
                if (write_is_blocked_by_a_borrow(moved->local, *moved, state, moved_through)) {
                    return std::unexpected(DataflowError("cannot move '" + name + "' while it is borrowed",
                        state.current_loc));
                }
            }
            mark_place_moved_out(state.locals, *moved);
            if (report_errors && !is_move_target_context) {
                return std::unexpected(DataflowError("std::move(" + name + ") must be used to initialize, assign into, return, "
                                                            "pass, or capture a value",
                    state.current_loc));
            }
            return {};
        }

        // `static_cast<T>(expr)`/`(T)expr` (ch06 §16.3(2)): visits the
        // operand for its own move/borrow bookkeeping exactly like any
        // other sub-expression (never itself a move-target-context -- a
        // cast reads its operand's value, it doesn't take ownership of
        // it), then asks scpp::diagnose_explicit_cast whether the
        // (source, target) pair is a conversion the language provides.
        // That rule lives in scpp.ast because codegen must answer the
        // same question; this case used to carry its own copy of it.
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
                std::expected<CastKind, std::string> diagnosis = classify_explicit_cast(source_type, expr.type, *body.program);
                if (!diagnosis.has_value()) {
                    return std::unexpected(DataflowError(std::move(diagnosis).error(), state.current_loc));
                }
                if (*diagnosis == CastKind::UnsafePointerConversion && state.unsafe_depth == 0) {
                    return std::unexpected(DataflowError(
                        "cannot cast '" + describe_type_brief(binary_operand_type(*source_type)) + "' to '" + describe_type_brief(expr.type) +
                            "': a conversion between two pointer types that no implicit conversion relates is a gated "
                            "operation; write it inside '[[scpp::unsafe]] { }' (spec ch01 §1(5.2), §1(6))",
                        state.current_loc));
                }
                // CastKind::OperandTypeUnknown: whatever left the operand
                // untypeable here -- an unresolved call, a generic still
                // awaiting substitution -- has its own diagnostic in a
                // later pass (codegen's "call to unknown function ...").
                // Reporting the cast instead would hide it, which is how
                // `static_cast<std::int8_t>(s.at(0))` came to be reported
                // as an unsupported cast when the real cause was that
                // std::string had no at().
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
            // `-`, `!` and `~`: spec §16.3 does not list a unary
            // operator's operand among its positions, but the reason is
            // that it is covered by the same rule -- an operator applies
            // to a value, and `-h()` had no check of any kind, so it
            // reached codegen and faulted there.
            if (auto _r = check_expression_yields_a_value(*expr.lhs, body, signatures, state.current_loc,
                                                          "a unary operator's operand", report_errors);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
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
            if (is_named_record_type(expr.type, body)) {
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
                // Only the condition. [basic.types.general] explicitly
                // permits a `void` second and third operand, and the
                // result is then a `void` expression like any other --
                // caught by whichever position tries to bind it, or
                // legal if the whole conditional is discarded.
                if (auto _r = check_expression_yields_a_value(*expr.lhs, body, signatures, state.current_loc,
                                                              "the condition of a conditional expression", report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
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
                    if (target_type != nullptr && is_named_record_type(*target_type, body)) {
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
                    if (field_type.has_value() && is_named_record_type(*field_type, body)) {
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
                if (report_errors && target_class_type.has_value() && body.program != nullptr &&
                    is_bare_same_type_copy_source(*expr.rhs, *target_class_type, body, signatures)) {
                    // [dcl.fct.def.delete]/2 answers first: "this operator
                    // is deleted" is a strictly more specific answer than
                    // "this class is not copy-assignable", which is what
                    // §6.5(3)'s suppression rule below reports.
                    const Function* user_assign = find_user_declared_copy_assign(target_class_type->name, *body.program);
                    if (user_assign != nullptr && user_assign->is_deleted) {
                        return std::unexpected(DataflowError(
                            deleted_function_error_message("the copy assignment operator of '" + target_class_type->name + "'",
                                                           user_assign->loc),
                            state.current_loc));
                    }
                }
                if (report_errors && target_class_type.has_value() &&
                    is_bare_same_type_copy_source(*expr.rhs, *target_class_type, body, signatures) &&
                    (state.classes_with_copy_assign == nullptr ||
                     !state.classes_with_copy_assign->contains(target_class_type->name))) {
                    return std::unexpected(DataflowError(
                        std::string(record_keyword(target_class_type->name, *body.program)) + " '" +
                                         target_class_type->name +
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
                if (auto _r = check_assignment_target_conversions(*expr.lhs, *expr.rhs, body, state, signatures,
                                                                  state.current_loc, report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                // The assignment target is never a "read": whatever its
                // previous state, assigning any value returns it to
                // Initialized (spec §6.2(4)). It must happen *before*
                // the base is walked -- the walk reads the target as a
                // place, and a moved-out one would be rejected as a use
                // of what this very statement is putting back.
                //
                // Which *place* that is, not which local: `r = v;` where
                // `r` is a reference bound to `o.in.p` reinitializes
                // `o.in.p`, since §6.2(4) speaks of the object assigned
                // to and a reference is not one. Asking body.local_of
                // here (and tracked_place_of only for the shapes that
                // are not a bare identifier) reinitialized the *binding*
                // instead, leaving the referent reading as still
                // moved-out -- so a moved-out member could not be put
                // back through the alias the program already held, which
                // is exactly what §6.3(1)'s "assign to it before this
                // point" tells the user to do.
                if (std::optional<Place> target_place = tracked_place_of(*expr.lhs, state, body);
                    target_place.has_value()) {
                    reinitialize_place(state.locals, *target_place);
                }
                if (expr.lhs->kind != ExprKind::Identifier) {
                    // e.g. `p.x = 1;` or `arr[i] = 1;`: the base
                    // object/index are evaluated (as addresses / an
                    // index value), not read as "the assignment target",
                    // so still worth walking for nested reads.
                    if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    if (report_errors) {
                        if (assignment_target_is_read_only(*expr.lhs, body, signatures)) {
                            return std::unexpected(
                                read_only_write_error(*expr.lhs, body, signatures, "=", state.current_loc));
                        }
                        if (std::optional<LocalId> lender = resolve_reborrow_lender(*expr.lhs, body, signatures);
                            lender.has_value()) {
                            if (auto _r = validate_reborrow_lender_write(*lender, state, body, report_errors,
                                                                         tracked_place_of(*expr.lhs, state, body, nullptr,
                                                                      PlacePrecision::Enclosing));
                                !_r.has_value()) {
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
                            std::optional<Place> written =
                                tracked_place_of(*expr.lhs, state, body, nullptr, PlacePrecision::Enclosing);
                            for (LocalId root : write_roots) {
                                if (write_is_blocked_by_a_borrow(root, written, state, {})) {
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
                if (auto _r = check_binary_expr_operand_types(expr, body, signatures, state.current_loc, state.unsafe_depth); !_r.has_value()) {
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
            // spec §6.2(5)/(6): reading a member is a use of *that*
            // object. Asked before the base is walked, because the base
            // -- `s` in `s.a` -- is not itself being read as a value and
            // may legitimately be only partially owned; asking about the
            // whole root instead is what made moving one member poison
            // every sibling.
            if (report_errors) {
                if (std::optional<Place> place = tracked_place_of(expr, state, body); place.has_value()) {
                    LocalState place_state = lookup(state.locals, *place);
                    if (place_state != LocalState::Initialized && place_state != LocalState::Bottom) {
                        Place named = state_source_place(state.locals, *place);
                        return std::unexpected(DataflowError(describe_bad_state(body.describe_place(named), place_state),
                            state.current_loc));
                    }
                }
            }
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
            if (report_errors) {
                if (std::optional<Place> place = tracked_place_of(expr, state, body); place.has_value()) {
                    LocalState place_state = lookup(state.locals, *place);
                    if (place_state != LocalState::Initialized && place_state != LocalState::Bottom) {
                        Place named = state_source_place(state.locals, *place);
                        return std::unexpected(DataflowError(describe_bad_state(body.describe_place(named), place_state),
                            state.current_loc));
                    }
                }
            }
            if (auto _r = apply_expr(*expr.lhs, false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (auto _r = check_expression_yields_a_value(*expr.lhs, body, signatures, state.current_loc,
                                                          "the object a subscript is applied to", report_errors);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (auto _r = check_expression_yields_a_value(*expr.rhs, body, signatures, state.current_loc,
                                                          "a subscript index", report_errors);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            // [expr.sub]/1 defines `E1[E2]` as `*((E1)+(E2))`, so a raw
            // pointer subscript is *both* halves of spec §5.1(5.1) at
            // once. See validate_subscript_expr, which is shared with
            // resolve_borrow_source_root's Subscript case so that
            // `&p[i]` is gated identically to `p[i]`.
            if (report_errors) {
                if (auto _r = validate_subscript_expr(expr, state, body, signatures); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
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
            // case, which asks apply_lambda_captures to report what the
            // closure holds so it can install it), so taking nothing
            // persistent here is sound -- see apply_lambda_captures' own
            // comment for the shared per-capture logic.
            if (auto _r = apply_lambda_captures(expr, state, body, signatures, report_errors); !_r.has_value()) {
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
        reinitialize_place(state.locals, whole_local_place(stmt.local));
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
        reinitialize_place(state.locals, whole_local_place(stmt.local));
        return {};
    }

    if (report_errors && !is_span(stmt.type)) {
        std::optional<Type> source_type = infer_expr_type(*stmt.expr, body, signatures);
        bool reference_binding_compatible = false;
        if (source_type.has_value()) {
            // Compared ignoring the *referent's* own const-qualification,
            // because that is not the question this check asks. `int& r =
            // p;` for a `const int& p` differs from `int&` only in the
            // qualifier, and answering it here produced "cannot bind
            // reference 'r' from an incompatible source type" -- a
            // message that names the wrong problem and offers no fix,
            // when the const-reachability guard immediately below already
            // has the right one. Anything genuinely of a different type
            // still lands here.
            Type source_unqualified = type_ignoring_top_level_const(*source_type);
            Type target_unqualified = type_ignoring_top_level_const(stmt.type);
            reference_binding_compatible =
                types_equal(source_unqualified, target_unqualified) ||
                types_compatible_with_base_conversion(source_unqualified, target_unqualified, *body.program,
                                                      state.current_class);
            if (!reference_binding_compatible && target_unqualified.pointee != nullptr) {
                Type target_referent = type_ignoring_top_level_const(*target_unqualified.pointee);
                reference_binding_compatible =
                    types_equal(source_unqualified, target_referent) ||
                    types_compatible_with_base_conversion(source_unqualified, target_referent, *body.program,
                                                          state.current_class);
            }
        }
        if (!reference_binding_compatible) {
            return std::unexpected(DataflowError("cannot bind reference '" + body.name_of(stmt.local) +
                                 "' from an incompatible source type",
                                state.current_loc));
        }
    }

    // Reject manufacturing a mutable `T&`/`std::span<T>` out of a place
    // that's only reachable read-only (e.g. `int& r = p.x;`/
    // `std::span<int> s = p.arr;` where `p` is `const Foo&`) -- spec
    // ch05 §5.7's "projection chain's const-reachability" check, shared
    // with apply_reference_argument's identical guard for a call
    // argument. A `const T&`/`std::span<const T>` binding is always fine
    // regardless (read-only never needs to widen).
    //
    // Asked *before* the borrow roots are resolved, because whether the
    // source is writable has nothing to do with which local owns it --
    // and a `const` global owns no local root at all, so resolving first
    // made `roots.empty()` return success and skip this entirely. That is
    // how `const int g = 5; int& r = g; r = 9;` compiled and wrote 9.
    if (report_errors && stmt.type.is_mutable_ref && place_is_read_only(*stmt.expr, body, signatures)) {
        const char* kind_name = is_span(stmt.type) ? "span" : "reference";
        // A range-`for` lowers to a synthesized range storage bound with
        // the loop variable's own mutability, so this is where
        // `for (int& v : c)` over a const range lands. Naming the
        // synthesized local would print `$for_range_0`, which is not in
        // the user's source at all -- and the fix is not spelled on it
        // either, it is spelled on the loop variable.
        bool is_range = is_synthesized_for_range_storage(body.name_of(stmt.local));
        std::string message{"cannot bind a mutable "};
        if (is_range) {
            message += kind_name;
            message += " to the range of this 'for' loop";
        } else {
            message += kind_name;
            message += " '";
            message += body.name_of(stmt.local);
            message += "'";
        }
        message += ": its source is only reachable through a read-only (const) reference";
        std::string const_source = describe_const_source(*stmt.expr, body, signatures);
        if (!const_source.empty()) {
            message += " (";
            message += const_source;
            message += ")";
        }
        if (is_range) message += " -- declare the loop variable a 'const' reference to read through it";
        return std::unexpected(DataflowError(message, state.current_loc));
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
        reinitialize_place(state.locals, whole_local_place(stmt.local));
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
    std::optional<Place> exact_bound =
        stmt.expr != nullptr ? tracked_place_of(*stmt.expr, state, body) : std::nullopt;
    std::optional<Place> containing_bound =
        stmt.expr != nullptr ? tracked_place_of(*stmt.expr, state, body, nullptr, PlacePrecision::Enclosing)
                             : std::nullopt;
    state.ref_targets[stmt.local] =
        RefTarget{roots, uses_lender_suspension ? lender : std::optional<LocalId>{},
                  containing_bound.has_value() ? containing_bound : exact_bound, exact_bound.has_value(), is_mutable};
    state.local_lifetime_sources[stmt.local] = roots;
    reinitialize_place(state.locals, whole_local_place(stmt.local));
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
    // spec §6.2(4): the assignment reinitializes the object written to,
    // which through a reference is the *referent*. Recording it against
    // the binding alone (BindReference already does that) left the
    // referent moved-out forever.
    if (std::optional<Place> written = place_root_resolver(state)(stmt.local); written.has_value()) {
        reinitialize_place(state.locals, *written);
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

[[nodiscard]] bool is_lvalue_copy_source_shape(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier:
            return true;
        case ExprKind::Member:
        case ExprKind::Subscript:
            return expr.lhs != nullptr && is_lvalue_copy_source_shape(*expr.lhs, body, signatures);
        case ExprKind::Unary: {
            if (expr.unary_op != UnaryOp::Deref || expr.lhs == nullptr) return false;
            // A user-written `*p` (a raw pointer or smart-pointer local
            // dereferenced to reach a field): the same addressable-place
            // shape resolve_borrow_source_root already recognizes for
            // borrow sources (borrows.cppm) -- a dereferenced pointer is
            // just as legitimate an lvalue copy source as a plain
            // Member/Subscript root.
            if (!expr.implicit_arrow_deref) return is_lvalue_copy_source_shape(*expr.lhs, body, signatures);
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
            return is_lvalue_copy_source_shape(*receiver, body, signatures);
        }
        case ExprKind::Call: {
            // A call that returns by reference names already-existing
            // storage -- `container.at(i)`, `expected.value()` -- and is
            // just as much an lvalue copy source as a named place is. A
            // by-value call is a fresh prvalue and stays excluded, so it
            // is handled by the produces_rvalue_of_type branch that runs
            // before every caller of this predicate.
            //
            // The one exception is the same one produces_rvalue_of_type
            // carves out and must agree with: a method called directly on
            // a `std::move(...)` receiver (`std::move(r).value()`, this
            // codebase's idiom for extracting a std::expected payload) is
            // an *rvalue*, because the signature database models
            // `.value()` with a single receiver-category-agnostic `T&`
            // return rather than a separate `&&`-qualified overload.
            // Treating it as a place would classify 321 such extractions
            // in the compiler's own sources as copy assignments of
            // non-copy-assignable types.
            //
            // Codegen::is_lvalue_copy_source_shape (codegen/semantics.cppm)
            // is a second copy of this predicate and already had this
            // case, citing `container.at(i)` by name; this one did not,
            // so movecheck rejected a copy codegen was perfectly willing
            // to emit. Masked while §6.5 only gated `class` types: no
            // class-typed local is initialized from a reference-returning
            // call anywhere in the compiler's own sources, but
            // `Token using_tok = using_tok_result.value();`
            // (parser.cppm) is exactly that shape on a struct.
            if (expr.lhs != nullptr && expr.lhs->kind == ExprKind::Move) return false;
            std::optional<Type> call_type = infer_expr_type(expr, body, signatures);
            return call_type.has_value() && call_type->kind == TypeKind::Reference;
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
    if (!is_lvalue_copy_source_shape(expr, body, signatures)) return false;
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
            if (report_errors && body.program != nullptr && stmt.type.kind == TypeKind::Named &&
                has_deleted_dtor(stmt.type.name, *body.program)) {
                return std::unexpected(DataflowError("cannot create an object of type '" + stmt.type.name +
                                     "': its destructor is defined as '= delete' ([class.dtor]/14 -- the object "
                                     "would be destroyed at the end of this scope, and a deleted destructor "
                                     "cannot be invoked)",
                    state.current_loc));
            }
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
            reinitialize_place(state.locals, whole_local_place(stmt.local));
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
            // ch05/ch06: a read-only place is written exactly once, by
            // the very same Assign statement its own VarDecl lowers to
            // (see mir.cppm's VarDecl case) -- distinguished from a
            // genuine later reassignment attempt by whether `stmt.local`
            // already has a prior entry in `state.locals` at all, the
            // identical "first write vs. reassignment" test the
            // class-typed-local case below uses for its own,
            // differently-motivated restriction. A target that is not a
            // local at all (a global) has no such initializing Assign in
            // this body, so it needs no exemption. Checked *before* every
            // type-specific case below (reference/span/class/unique_ptr/
            // plain scalar) so it uniformly covers all of them with one
            // rule, rather than needing to be threaded through each one
            // separately.
            //
            // Asked of `place_is_read_only`, the one predicate that
            // answers this. It used to be `LocalDecl::is_const` for a
            // local and a private `is_visible_global_const` for a global
            // -- neither of which looks at the *type*, so a `const`
            // parameter (whose qualifier lives nowhere else) was freely
            // assignable: `int f(const int v) { v = 6; return v; }`
            // compiled and returned 6.
            bool target_is_reassignment = (stmt.has_local && state.locals.contains(whole_local_place(stmt.local))) || !stmt.has_local;
            if (report_errors && stmt.target != nullptr && target_is_reassignment &&
                place_is_read_only(*stmt.target, body, signatures)) {
                return std::unexpected(read_only_write_error(*stmt.target, body, signatures, "=", state.current_loc));
            }
            if (local_type == nullptr) {
                std::optional<Type> global_type =
                    find_visible_global_type(target_name, /*explicit_global_qualification=*/false, body);
                if (!global_type.has_value()) return {};
                if (auto _r = check_value_binding_conversions(*global_type, *stmt.expr, body, signatures,
                                                             state.current_loc, target_name, report_errors);
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
            // [dcl.fct.def.delete]/2: `a = b` with an lvalue `b` of the same
            // record type *names* the copy assignment operator, so deleting
            // it makes this assignment ill-formed. Asked here, before the
            // `class`-only branch below, because nothing in docs/spec/
            // distinguishes `struct` from `class` for §6.5 -- and a struct
            // reaching codegen with this spelling emitted a call to a
            // function that has no callable definition at all.
            if (report_errors && body.program != nullptr && (*local_type).kind == TypeKind::Named &&
                stmt.expr != nullptr && is_bare_same_type_copy_source(*stmt.expr, (*local_type), body, signatures)) {
                const Function* user_assign = find_user_declared_copy_assign((*local_type).name, *body.program);
                if (user_assign != nullptr && user_assign->is_deleted) {
                    return std::unexpected(DataflowError(
                        deleted_function_error_message("the copy assignment operator of '" + (*local_type).name + "'",
                                                       user_assign->loc),
                        state.current_loc));
                }
            }
            if (is_named_record_type(*local_type, body)) {
                // [dcl.init.list]: a braced initializer is not a copy, a
                // move or a conversion of anything -- its *elements* bind,
                // one to each field, so nothing below has a question to
                // ask about it and every exit below returns before the
                // single check_value_binding_conversions call at the end
                // of this case. Asked here so the element checks run for
                // a record-typed local exactly as they do for every other
                // target: `struct H { int* p; }; const int c = 5;
                // H h = {&c};` otherwise handed out a mutable pointer
                // into a const object, while the `H h{&c};` spelling of
                // the same initialization was rejected -- that one lowers
                // to a constructor-call expression and reaches
                // check_aggregate_element_conversions through
                // check_constructor_arguments instead.
                if (stmt.expr != nullptr && stmt.expr->kind == ExprKind::BracedInitList) {
                    if (auto _r = check_value_binding_conversions((*local_type), *stmt.expr, body, signatures,
                                                                  state.current_loc, target_name, report_errors);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
                // spec §6.4/§6.5 govern every *class type*, which in
                // [class.pre]/[dcl.type] terms includes `struct` -- §6.5's
                // own worked example is spelled `struct RefCounted`. This
                // gate previously asked DataflowState::class_names (the
                // `class`-only set access control needs), citing a
                // "ch04 §4.2" clause that does not exist in docs/spec/, so
                // a record-typed local declared `struct` was reassigned by
                // a plain bitwise copy no matter what §6.5(2)/(3) said
                // about it. A `struct S { int* p; ~S(); }` reassigned that
                // way left the old value's resource unreleased and the new
                // one's released twice -- exactly the double-free this
                // gate exists to prevent, reachable through the keyword
                // the gate did not look at.
                //
                // A record with no user-declared destructor, copy
                // constructor or copy assignment operator still *has* an
                // implicitly-defined copy assignment operator (§6.5(3)),
                // so the ordinary trivial struct stays freely assignable;
                // what changes is that the ones §6.5(3) says have no such
                // operator are now told so.
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
                if (is_move_assignment && state.locals.contains(whole_local_place(stmt.local))) {
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
                                std::string(record_keyword((*local_type).name, *body.program)) + " '" +
                                    (*local_type).name +
                                    "' has a reference-typed member, so it has no move assignment operator "
                                    "(spec §6.4(3)) -- '" + target_name + "' cannot be reassigned",
                                state.current_loc));
                        }
                        auto borrow_it = state.borrows.find(stmt.local);
                        if (borrow_it != state.borrows.end() &&
                            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                            return std::unexpected(DataflowError("cannot assign to " +
                                                 std::string(record_keyword((*local_type).name, *body.program)) +
                                                 " variable '" + target_name +
                                                 "': it is currently borrowed",
                                state.current_loc));
                        }
                    }
                    if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/true, state, body, signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    reinitialize_place(state.locals, whole_local_place(stmt.local));
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
                    state.locals.contains(whole_local_place(stmt.local))) {
                    if (report_errors) {
                        if (!freely_copyable_assign_source &&
                            (state.classes_with_copy_assign == nullptr ||
                             !state.classes_with_copy_assign->contains((*local_type).name))) {
                            return std::unexpected(DataflowError(
                                std::string(record_keyword((*local_type).name, *body.program)) + " '" +
                                                 (*local_type).name +
                                                 "' is not copy-assignable (spec §6.5(3)) -- '" + target_name +
                                                 "' cannot be reassigned this way",
                                state.current_loc));
                        }
                        auto borrow_it = state.borrows.find(stmt.local);
                        if (borrow_it != state.borrows.end() &&
                            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                            return std::unexpected(DataflowError("cannot assign to " +
                                                 std::string(record_keyword((*local_type).name, *body.program)) +
                                                 " variable '" + target_name +
                                                 "': it is currently borrowed",
                                state.current_loc));
                        }
                    }
                    if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
                               signatures, report_errors); !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    reinitialize_place(state.locals, whole_local_place(stmt.local));
                    return {};
                }
                if (report_errors && state.locals.contains(whole_local_place(stmt.local))) {
                    return std::unexpected(DataflowError(
                        std::string(record_keyword((*local_type).name, *body.program)) + " '" +
                                         (*local_type).name + "'-typed variable '" + target_name +
                                         "' cannot be reassigned from this expression: spec §6.4(3) licenses an "
                                         "rvalue of the same type (std::move(x), or a call returning by value) "
                                         "and spec §6.5(3) an lvalue of the same type when the type is "
                                         "copy-assignable",
                        state.current_loc));
                }
                if (stmt.expr->kind == ExprKind::Lambda) {
                    // ch05 §5.12: unlike a *transient* lambda literal
                    // (apply_expr's own Lambda case -- an IIFE, a call
                    // argument, ...), one bound to a named `auto`
                    // variable genuinely can outlive this statement, so
                    // what its by-reference captures hold has to persist
                    // for the rest of this function -- see
                    // apply_lambda_captures' own comment.
                    std::vector<ClosureCaptureBorrow> closure_capture_borrows;
                    if (auto _r = apply_lambda_captures(*stmt.expr, state, body, signatures, report_errors,
                                          &closure_capture_borrows);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                    // Installed here rather than as a side effect of the
                    // per-construction duplicate-detection map, and
                    // split the same way apply_reference_binding splits
                    // an ordinary reference binding: a reborrowing
                    // capture suspends its lender, any other borrows its
                    // root. release_closure_capture_borrows undoes
                    // whichever it was.
                    for (const ClosureCaptureBorrow& capture_borrow : closure_capture_borrows) {
                        if (capture_borrow.lender.has_value()) {
                            if (capture_borrow.is_mutable) {
                                state.suspended_reborrows[*capture_borrow.lender].mutable_suspended = true;
                            } else {
                                state.suspended_reborrows[*capture_borrow.lender].shared_count++;
                            }
                            continue;
                        }
                        BorrowState& borrow = state.borrows[capture_borrow.root];
                        if (capture_borrow.is_mutable) {
                            borrow.mutable_borrow = true;
                        } else {
                            borrow.shared_count++;
                        }
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
                        reinitialize_place(state.locals, whole_local_place(stmt.local));
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
                        reinitialize_place(state.locals, whole_local_place(stmt.local));
                        return {};
                    }
                    if (report_errors) {
                        bool freely_copyable_init_source =
                            is_freely_copyable_class_value_source(*stmt.expr, (*local_type), body, signatures);
                        if (!is_bare_same_type_copy_source(*stmt.expr, (*local_type), body, signatures) &&
                            !freely_copyable_init_source) {
                            // [dcl.init]/16.6: the source is neither a copy
                            // nor a move of the same type, so this is a call
                            // to a converting constructor and failed as one.
                            // Say which rule it failed -- no viable candidate
                            // or an ambiguity -- rather than re-listing every
                            // initializer shape the language has.
                            std::vector<ExprPtr> init_args;
                            init_args.push_back(deep_clone_expr(*stmt.expr));
                            if (std::optional<std::string> failure = describe_constructor_selection_failure(
                                    (*local_type).name, init_args, body, signatures);
                                failure.has_value()) {
                                return std::unexpected(DataflowError(*failure, state.current_loc));
                            }
                            return std::unexpected(DataflowError(
                                std::string(record_keyword((*local_type).name, *body.program)) + " '" +
                                    (*local_type).name + "'-typed variable '" + target_name +
                                    "' can only be initialized via brace-init ('" +
                                    (*local_type).name + " " + target_name +
                                    "{args};'), std::move of the same type, a converting constructor of '" +
                                    (*local_type).name + "', or (if the type is copy-"
                                    "constructible, spec §6.5) an implicitly copyable source of another '" +
                                    (*local_type).name + "' value",
                                state.current_loc));
                        }
                        if (!freely_copyable_init_source &&
                            (state.classes_with_copy_ctor == nullptr ||
                             !state.classes_with_copy_ctor->contains((*local_type).name))) {
                            return std::unexpected(DataflowError(
                                std::string(record_keyword((*local_type).name, *body.program)) + " '" +
                                                 (*local_type).name +
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
                reinitialize_place(state.locals, whole_local_place(stmt.local));
                return {};
            }

            if (auto _r = apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
                       signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (local_type != nullptr) {
                if (auto _r = check_value_binding_conversions((*local_type), *stmt.expr, body, signatures,
                                                             state.current_loc, target_name, report_errors);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }

            if (report_errors) {
                auto borrow_it = state.borrows.find(stmt.local);
                if (borrow_it != state.borrows.end() &&
                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                    return std::unexpected(DataflowError("cannot assign to '" + target_name + "' while it is borrowed",
                        state.current_loc));
                }
            }
            reinitialize_place(state.locals, whole_local_place(stmt.local));
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

        // One expression of one entry of a constructor's member-
        // initializer list (mir.cppm's lower_member_initializers), which
        // used to reach no dataflow check at all -- the list was never
        // lowered, so `: p_{std::move(p)} { use(p); }` and
        // `: a_{std::move(p)}, b_{std::move(p)}` were both accepted.
        //
        // The move-target context matches MirStatementKind::Assign's:
        // `x_{std::move(p)}` moves out of `p` exactly as `T x =
        // std::move(p);` does. Unlike Eval, no discarded-[[nodiscard]]
        // check -- the value initializes a member, it is not thrown away.
        //
        // The member's own declared type is checked against this
        // expression by check_member_initializer_conversions, which asks
        // a pure type question and so needs no dataflow state; this
        // statement carries the state half of the same position.
        case MirStatementKind::MemberInit:
            return apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
                              signatures, report_errors);

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
            if (report_errors) {
                if (auto _r = check_moved_subobjects_were_restored(state, body, stmt.local,
                                                                   "where its scope ends");
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            // `stmt.local` just went out of lexical scope: forget its
            // tracked state entirely. Erasing is equivalent to setting
            // it to Bottom (lookup() treats a missing key as Bottom) and
            // keeps the map from growing with entries the rest of the
            // analysis no longer cares about.
            forget_place_tree(state.locals, whole_local_place(stmt.local));
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
            // spec §16.3(4) requires a `bool` here (an integral or enum
            // value for a `switch`), which is checked by codegen; the
            // question this asks is the earlier one, because "not a
            // bool" describes the wrong problem for an expression that
            // is not a value at all.
            if (auto _r = check_expression_yields_a_value(
                    *term.condition, body, signatures, term.loc,
                    term.kind == TerminatorKind::Switch ? "a 'switch' condition" : "an 'if'/'while' condition",
                    /*report_errors=*/true);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            return apply_expr(*term.condition, false, state, body, signatures, /*report_errors=*/true);
        case TerminatorKind::Return: {
            if (auto _r = check_moved_subobjects_were_restored(state, body, std::nullopt,
                                                               "where '" + fn.name + "' returns");
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (term.return_value == nullptr) return {};
            // spec §16.3(1.4). The one position where a `void` operand
            // is legal rather than forbidden: [basic.types.general]
            // allows `return f();` from a function that itself returns
            // `void`, and that form is in use, so the check is asked
            // only when this function returns something.
            if (!is_void_named_type(fn.return_type)) {
                if (auto _r = check_expression_yields_a_value(*term.return_value, body, signatures, term.loc,
                                                              "the operand of a 'return' in '" + fn.name + "'",
                                                              /*report_errors=*/true);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            if (auto _r = check_scalar_conversion(fn.return_type, *term.return_value, body, signatures, term.loc,
                                                  "the return value of '" + fn.name + "'",
                                                  /*report_errors=*/true);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            // [stmt.return]/2 copy-initializes the returned object from
            // the operand, so `return` is a binding position like any
            // other and takes the questions check_value_binding_conversions
            // asks. Two of the five it had no answer to at all: `A f(B b)
            // { return b; }` between two distinct enum classes was
            // accepted outright, and `int* leak() { return &g; }` for a
            // `const int g` returned a mutable handle to a const object.
            //
            // The raw-pointer question used to be left to the return-
            // specific lifetime machinery below on the grounds that it
            // "knows what is being returned and says so". It does not: it
            // answers a different question (which roots the returned
            // pointer may outlive), and for a const-dropping return it
            // reported "returns a lifetime-tracked value from an
            // incompatible source type" -- a message that names neither
            // the const nor a fix. Asked here, before it, the answer
            // comes from the same function every other position uses.
            //
            // The remaining two are not borrowed: a function pointer
            // cannot be spelled as a return type in this version, and
            // `return nullptr;` against a non-pointer return type is
            // already rejected by the return-specific type-mismatch
            // check. The scalar question is asked just above, in the
            // return position's own words.
            if (auto _r = check_enum_conversion_compatibility(fn.return_type, *term.return_value, body, signatures,
                                                              term.loc);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (auto _r = check_raw_pointer_assignment(fn.return_type, *term.return_value, body, signatures, term.loc,
                                                       "the value returned from " + fn.name,
                                                       /*report_errors=*/true);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            // [dcl.init.ref]/5 + [dcl.type.cv]/4: a mutable `T&` (or
            // `std::span<T>`) may only be bound to a place that is itself
            // reachable mutably. This is the same guard already applied to
            // `int& r = <expr>;` (check_ref_decl below) and to a
            // mutable-reference call argument (apply_reference_argument),
            // asked of the same single predicate -- and it is where the
            // question belongs. It used to be answered from the *signature*
            // instead, by resolve_elided_param_index / Codegen::
            // validate_reference_return_elision, which rejected every
            // `T& f() const` and every `T& f(const U&)` on sight: not a
            // rule C++ has, and not even a sound approximation of one,
            // since both returned early for a function carrying any
            // `[[scpp::lifetime]]` annotation. The raw-pointer form of this
            // check has always been done here, on the expression, by
            // check_raw_pointer_assignment just above.
            if ((is_reference(fn.return_type) || is_span(fn.return_type)) && fn.return_type.is_mutable_ref &&
                place_is_read_only(*term.return_value, body, signatures)) {
                std::string message{"cannot return a mutable "};
                message += is_span(fn.return_type) ? "span" : "reference";
                message += " from function '";
                message += fn.name;
                message += "': the returned place is only reachable through a read-only ('const') binding";
                return std::unexpected(DataflowError(message, term.loc));
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
                // [stmt.return]/2 implicitly converts the operand to the
                // return type, and the array-to-pointer conversion
                // ([conv.array]) is part of that: `return "text/html";` from
                // a `const char*` function returns the decayed pointer, not
                // the string-literal array object itself.
                if (returned_type.has_value()) returned_type = decay_array_to_pointer(*returned_type);
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
            bool return_is_class_value = is_named_record_type(fn.return_type, body);
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
    if (auto _r = check_member_initializer_conversions(fn, body, signatures, class_field_types); !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
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
    //
    // A `consteval` body is the second exception, and for the opposite
    // reason: not because it is trusted, but because ch06 §7.3(1) makes
    // the licence *unobtainable* there -- evaluating a
    // `[[scpp::unsafe]]` compound-statement during required constant
    // evaluation is itself ill-formed. Gating a consteval body would
    // therefore not mean "say you meant it", it would mean "you cannot
    // write this at all", which §5.1(6) does not say and which would
    // make `std::format`'s own compile-time format-string validation
    // (a `consteval` constructor that walks a `const char*` literal)
    // unwritable.
    //
    // That is safe because ch06 §7(3) already supplies, by construction,
    // exactly what §5.1(5.1) exists to guarantee: during required
    // constant evaluation a pointer value is *permitted only if* it is
    // null, designates an element or one-past-the-end of a
    // string-literal object (3.2), or designates a subobject of a
    // constant-initialized static (3.3). There is no unprovenanced
    // pointer for the evaluator to walk off the end of, and the
    // evaluator diagnoses an out-of-range access rather than executing
    // it. `consteval` specifically -- and not `constexpr` -- because
    // only `consteval` guarantees the body is *never* lowered as
    // runtime code, where none of ch06 §7(3)'s restrictions apply.
    //
    // The implementation already agrees for consteval *free functions*,
    // which are never lowered to MIR at all and so were never
    // move-checked; a consteval *constructor* is lowered, which is the
    // only reason the asymmetry was visible here.
    entry_state.unsafe_depth = (fn.is_unsafe || fn.eval_mode == FunctionEvalMode::Consteval) ? 1 : 0;
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
        reinitialize_place(entry_state.locals, whole_local_place(param_local));
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
        // `unsafe_depth` is lexical, not a dataflow fact, so it is read
        // off the block rather than flowed into it -- see
        // BasicBlock::unsafe_depth_on_entry for what joining it did to a
        // loop head. The UnsafeEnter/UnsafeExit statements still adjust
        // it *within* a block, which is where a marker and the code it
        // governs do share a block.
        new_in.unsafe_depth = entry_state.unsafe_depth + body.blocks[b].unsafe_depth_on_entry;

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
    if (auto _r = check_initializer_scope_conversions(program, signatures); !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
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
    // spec §6.5 governs every *class type*, which in [class.pre]/[dcl.type]
    // terms includes `struct` (see is_copy_constructible/is_copy_assignable
    // themselves, which both search program.structs as well). Enumerating
    // only program.classes here left every struct outside the set, so the
    // three gates that consult it -- field copy assignment, whole-local
    // copy assignment, and lambda capture-by-copy -- silently licensed
    // copies of structs the predicate says have no such operation at all.
    std::unordered_set<std::string> classes_with_copy_ctor;
    std::unordered_set<std::string> classes_with_copy_assign;
    for (const ClassDef& def : program.classes) {
        if (is_copy_constructible(def.name, program)) classes_with_copy_ctor.insert(def.name);
        if (is_copy_assignable(def.name, program)) classes_with_copy_assign.insert(def.name);
    }
    for (const StructDef& def : program.structs) {
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
    for (const StructDef& def : program.structs) {
        for (const Function& fn : program.functions) {
            if (auto _r = validate_struct_constructor_member_initialization(fn, def, program); !_r.has_value()) {
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
