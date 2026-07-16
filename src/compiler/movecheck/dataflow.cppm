module;

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

module scpp.compiler.movecheck:dataflow;

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
void check_binary_expr_operand_types(const Expr& expr, const Body& body, const Signatures& signatures,
                                     const SourceLocation& loc);
[[nodiscard]] std::optional<Type> resolve_member_field_type(const Expr& member_expr, const Body& body,
                                                            const DataflowState& state);
void validate_deref_operand(const Expr& operand, const DataflowState& state, const Body& body,
                            const Signatures& signatures);
void apply_deref(const Expr& expr, const DataflowState& state, const Body& body, const Signatures& signatures,
                 bool report_errors);
void apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
                const Signatures& signatures, bool report_errors);
void check_call_arguments(const Expr& expr, DataflowState& state, const Body& body,
                          const Signatures& signatures, bool report_errors);
void apply_reference_argument(const Expr& arg, const Type& param_type, DataflowState& state,
                              BorrowMap& in_call_borrows, const Body& body,
                              const Signatures& signatures, bool report_errors);
void check_constructor_arguments(const std::string& class_name, const std::vector<ExprPtr>& ctor_args,
                                 DataflowState& state, const Body& body, const Signatures& signatures,
                                 bool report_errors);
[[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type,
                                                 const Body& body, const Signatures& signatures);
void apply_statement(const MirStatement& stmt, DataflowState& state, const Body& body,
                     const Signatures& signatures, bool report_errors);
void check_terminator(const Terminator& term, DataflowState& state, const Function& fn, const Body& body,
                      const Signatures& signatures);
void check_function(const Function& fn, const Program& program, const Signatures& signatures,
                    const std::unordered_set<std::string>& class_names,
                    const ClassFieldTypes& class_field_types,
                    const ClassFieldAccess& class_field_access,
                    const std::unordered_set<std::string>& classes_with_copy_ctor,
                    const std::unordered_set<std::string>& classes_with_copy_assign,
                    const std::unordered_set<std::string>& witness_class_names);
void check_moves_impl(const Program& program);

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
    if (!pointer_operand_present) return true;
    return pointer_arithmetic_result_type(expr.binary_op, *lhs_type, *rhs_type).has_value();
}

void check_binary_expr_operand_types(const Expr& expr, const Body& body, const Signatures& signatures,
                                     const SourceLocation& loc) {
    if (expr.binary_op == BinaryOp::Assign) return;
    if (expr.binary_op == BinaryOp::And || expr.binary_op == BinaryOp::Or) return;
    std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
    std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
    bool lhs_is_enum = lhs_type.has_value() && is_enum_type(binary_operand_type(*lhs_type), body.program);
    bool rhs_is_enum = rhs_type.has_value() && is_enum_type(binary_operand_type(*rhs_type), body.program);
    if ((lhs_is_enum || rhs_is_enum) && expr.binary_op != BinaryOp::Eq && expr.binary_op != BinaryOp::Ne) {
        throw DataflowError("enum class values only support '==' and '!=' in this version", loc);
    }
    bool arithmetic_op = expr.binary_op == BinaryOp::Add || expr.binary_op == BinaryOp::Sub || expr.binary_op == BinaryOp::Mul ||
                         expr.binary_op == BinaryOp::Div;
    if (arithmetic_op) {
        if (binary_expr_has_valid_arithmetic_types(expr, body, signatures)) return;
        if (!lhs_type.has_value() || !rhs_type.has_value()) return;
        const Type& lhs_operand = binary_operand_type(*lhs_type);
        const Type& rhs_operand = binary_operand_type(*rhs_type);
        if (lhs_operand.kind == TypeKind::Pointer || rhs_operand.kind == TypeKind::Pointer) {
            throw DataflowError("pointer arithmetic requires 'pointer +/- integer' or 'pointer - pointer' with matching "
                                "non-void pointer types",
                loc);
        }
        throw DataflowError("binary operator requires operands of the same type; scpp has no implicit conversion between '" +
                                lhs_operand.name + "' and '" + rhs_operand.name + "' (ch06)",
            loc);
    }
    if (expr.binary_op != BinaryOp::Eq && expr.binary_op != BinaryOp::Ne && expr.binary_op != BinaryOp::Lt &&
        expr.binary_op != BinaryOp::Gt && expr.binary_op != BinaryOp::Le && expr.binary_op != BinaryOp::Ge) {
        return;
    }
    if (binary_expr_has_compatible_types(expr, body, signatures)) return;
    if (!lhs_type.has_value() || !rhs_type.has_value()) return;
    const Type& lhs_operand = binary_operand_type(*lhs_type);
    const Type& rhs_operand = binary_operand_type(*rhs_type);
    throw DataflowError("binary operator requires operands of the same type; scpp has no implicit conversion between '" +
                            lhs_operand.name + "' and '" + rhs_operand.name + "' (ch06)",
                        loc);
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
                                                              const DataflowState& state) {
    if (member_expr.kind != ExprKind::Member) return std::nullopt;
    if (state.class_field_types == nullptr) return std::nullopt;
    std::string base_name;
    if (member_expr.lhs->kind == ExprKind::Identifier) {
        base_name = member_expr.lhs->name;
    } else if (is_explicit_star_this(*member_expr.lhs)) {
        base_name = "this";
    } else {
        return std::nullopt;
    }
    auto base_it = body.local_types.find(base_name);
    if (base_it == body.local_types.end()) return std::nullopt;
    const Type& base_type = base_it->second;
    const std::string& type_name = (base_type.kind == TypeKind::Reference ? *base_type.pointee : base_type).name;
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
void validate_deref_operand(const Expr& operand, const DataflowState& state, const Body& body,
                            const Signatures& signatures) {
    std::string describe = operand.kind == ExprKind::Member ? operand.lhs->name + "." + operand.name : operand.name;
    std::optional<Type> resolved =
        operand.kind == ExprKind::Member ? resolve_member_field_type(operand, body, state) : [&]() -> std::optional<Type> {
            auto it = body.local_types.find(operand.name);
            return it == body.local_types.end() ? std::nullopt : std::optional<Type>(it->second);
        }();
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
        throw DataflowError("cannot dereference ('*') '" + describe +
                             "': only a raw pointer (inside '[[scpp::unsafe]] { }'), a function pointer "
                             "being called, a class with operator*, or '*this' is supported here",
            state.current_loc);
    }
    if (is_raw_ptr && state.unsafe_depth == 0) {
        throw DataflowError("cannot dereference raw pointer '" + describe +
                             "': requires '[[scpp::unsafe]] { }' (spec ch01 §1.3/ch02)",
            state.current_loc);
    }
    if (operand.kind == ExprKind::Member) return; // no per-field move/borrow state -- see this function's own comment
    LocalState current = lookup(state.locals, operand.name);
    if (current != LocalState::Initialized) {
        throw DataflowError(describe_bad_state(operand.name, current),
            state.current_loc);
    }
}

// Handles a raw-pointer/function-pointer/`*this` Deref expression used as
// a plain read (not as a borrow source -- see resolve_borrow_source_root's
// own Deref case for that). Class overloads of `operator*` are rewritten
// to ordinary calls earlier in the pipeline, so they bypass this helper
// entirely. A raw pointer has no ownership/move state of its own to
// disturb. `*this` is likewise just an explicit spelling of the receiver
// object itself (ch05 §5.9), so it behaves exactly like reading `this`.
void apply_deref(const Expr& expr, const DataflowState& state, const Body& body, const Signatures& signatures,
                 bool report_errors) {
    if (is_explicit_star_this(expr)) {
        if (!report_errors) return;
        validate_deref_operand(*expr.lhs, state, body, signatures);
        return;
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
            throw DataflowError("dereference ('*') currently only supports a plain local raw/function pointer "
                                 "variable, '*this', or a captured field of one ('this.field') (not a subscript "
                                 "or other expression)",
                state.current_loc);
        }
        return;
    }
    if (!report_errors) return; // purely diagnostic: doesn't move p or change any tracked state
    validate_deref_operand(*expr.lhs, state, body, signatures);
    if (!is_plain_identifier) return; // no separate borrow-tracking key for a field -- see the comment above
    const std::string& name = expr.lhs->name;
    auto borrow_it = state.borrows.find(name);
    if (borrow_it != state.borrows.end() && borrow_it->second.mutable_borrow) {
        // `expr.lhs->loc` (the identifier `name` itself), not
        // `state.current_loc` (the enclosing `*`/Deref's own position,
        // one token earlier) -- both checks above are about `name`
        // specifically, so pointing at it directly is more precise.
        throw DataflowError("cannot use '" + name + "' while it is mutably borrowed",
            expr.lhs->loc);
    }
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
void apply_reference_argument(const Expr& arg, const Type& param_type, DataflowState& state,
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
        apply_expr(arg, /*is_move_target_context=*/arg.kind == ExprKind::Move, state, body, signatures, report_errors);
        return;
    }

    // resolve_borrow_source_root may have real (move-tracking) side
    // effects on `state` via nested apply_expr calls (e.g. a subscript
    // index) that must apply on *every* pass, not just the reporting
    // one -- unlike the rest of this function, which is purely
    // diagnostic (a call argument's borrow never outlives the call, so
    // there's nothing else here for a later statement's fixed-point
    // computation to observe).
    RootSet roots = resolve_borrow_source_root(arg, state, body, signatures, report_errors);
    if (!report_errors) return;

    if (body.program != nullptr && param_type.pointee != nullptr && param_type.pointee->kind == TypeKind::Named) {
        const ClassDef* param_interface = find_class_def(*body.program, param_type.pointee->name);
        if (param_interface != nullptr && param_interface->is_interface) {
        std::optional<Type> source_type = infer_expr_type(arg, body, signatures);
        if (source_type.has_value() &&
            !types_equal(*source_type, param_type) &&
            !types_compatible_with_base_conversion(*source_type, param_type, *body.program, enclosing_class_name(body))) {
            throw DataflowError("cannot bind reference parameter from an incompatible source type",
                                state.current_loc);
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
    // mutability has to be checked: a shared (`const T&`) reference can't
    // satisfy a `T&` parameter (that would manufacture a mutable alias
    // out of a shared one), but a mutable reference may always be lent
    // out as either mutable or shared.
    std::optional<std::string> lender = resolve_reborrow_lender(arg, body, signatures);
    bool lender_is_mutable =
        lender.has_value() && is_reborrowable_local_type(body.local_types.at(*lender)) && body.local_types.at(*lender).is_mutable_ref;
    if (lender.has_value() && lender_is_mutable) {
        validate_reborrow_lender(*lender, is_mutable, state, body, report_errors);
    } else {
        // The general case: `arg` isn't itself a directly-named, locally-
        // bound reference (that narrower case is handled above), so it
        // may instead be a `.field`/`[index]` projection or a plain
        // *parameter* (never entered into `ref_targets`, since a
        // parameter is never processed through BindReference -- see
        // apply_reference_binding) whose own declared type is `const T&`/
        // `std::span<const T>`, or a chain that dereferences a `const T*`
        // -- any of which must likewise reject manufacturing a mutable
        // reference out of a read-only one (spec ch05 §5.7's "projection
        // chain's const-reachability").
        if (is_mutable && is_read_only_reachable(arg, body, signatures)) {
            throw DataflowError("cannot pass " + format_roots(roots) + " by mutable reference: it is only reachable "
                                                          "through a read-only (const) reference",
                state.current_loc);
        }
        for (const std::string& root : roots) {
            auto persistent_it = state.borrows.find(root);
            bool persistent_conflict =
                persistent_it != state.borrows.end() &&
                (is_mutable ? (persistent_it->second.mutable_borrow || persistent_it->second.shared_count > 0)
                            : persistent_it->second.mutable_borrow);
            if (persistent_conflict) {
                throw DataflowError("cannot pass '" + root + "' by " + std::string(is_mutable ? "mutable " : "") +
                                        "reference: it is already borrowed",
                                    state.current_loc);
            }
        }
    }

    for (const std::string& root : roots) {
        auto in_call_it = in_call_borrows.find(root);
        bool in_call_conflict =
            in_call_it != in_call_borrows.end() &&
            (is_mutable ? (in_call_it->second.mutable_borrow || in_call_it->second.shared_count > 0)
                        : in_call_it->second.mutable_borrow);
        if (in_call_conflict) {
            throw DataflowError("cannot pass '" + root + "' by " + std::string(is_mutable ? "mutable " : "") +
                                    "reference more than once in the same call",
                                state.current_loc);
        }

        BorrowState& borrow = in_call_borrows[root];
        if (is_mutable) {
            borrow.mutable_borrow = true;
        } else {
            borrow.shared_count++;
        }
    }
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
void check_call_arguments(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
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
        check_constructor_arguments(direct_call_type->name, expr.args, state, body, signatures, report_errors);
        return;
    }
    CalleeSignature callee = resolve_callee_signature(expr, body, state.class_field_types);
    auto name_it = signatures.find(callee.key);
    const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
    if (expr.lhs) {
        bool receiver_is_reference =
            sig != nullptr && !sig->param_types.empty() && is_reference(sig->param_types[0]) && !sig->param_types[0].is_rvalue_ref;
        (void)receiver_is_reference;
        apply_expr(*expr.lhs, /*is_move_target_context=*/expr.lhs->kind == ExprKind::Move, state, body, signatures,
                   report_errors);
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
            throw DataflowError("cannot call non-const member function '" + callee_display +
                                    "' through a read-only (const) receiver",
                state.current_loc);
        }
        throw DataflowError("no overload of '" + expr.name +
                             "' matches these argument types (spec ch05.10 -- overload resolution is exact "
                             "type match only; an explicit cast may be required)",
            state.current_loc);
    }
    if (report_errors && sig != nullptr && sig->access == AccessSpecifier::Private &&
        !sig->member_owner_class.empty() && state.current_class != sig->member_owner_class) {
        throw DataflowError("cannot call private member function '" + callee_display + "' of class '" +
                             sig->member_owner_class + "' from outside its own methods",
            state.current_loc);
    }
    if (report_errors && sig != nullptr && sig->is_extern_c_declaration_only && state.unsafe_depth == 0) {
        throw DataflowError("cannot call 'extern \"C\"' function '" + callee_display +
                             "' outside '[[scpp::unsafe]] { }': no scpp compiler ever sees its real "
                             "implementation to check it (spec ch01 §1.3/ch02)",
            state.current_loc);
    }
    if (report_errors && sig != nullptr && sig->is_unsafe && state.unsafe_depth == 0) {
        throw DataflowError("cannot call '" + callee_display +
                             "' outside '[[scpp::unsafe]] { }': its own declaration is marked "
                             "'[[scpp::unsafe]]', so its soundness depends on a precondition only the "
                             "caller can guarantee (ch01 §1.2/§1.3)",
            state.current_loc);
    }
    // Scratch borrow-map shared by every reference argument of *this*
    // call only (see apply_reference_argument) -- never merged into
    // `state`, since none of these transient borrows outlive the call.
    BorrowMap in_call_borrows;
    for (size_t i = 0; i < expr.args.size(); i++) {
        const Expr& arg = *expr.args[i];
        size_t param_index = i + callee.param_offset;
        bool param_is_reference =
            sig != nullptr && param_index < sig->param_types.size() && is_reference(sig->param_types[param_index]);
        bool param_is_rvalue_reference = param_is_reference && sig->param_types[param_index].is_rvalue_ref;
        if (param_is_rvalue_reference) {
            // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- an ownership-
            // transfer argument, not a borrow: needs a genuine rvalue
            // (see produces_rvalue_of_type), never apply_reference_
            // argument's place-borrow bookkeeping. Still walked via
            // apply_expr (exactly like a by-value/unique_ptr argument
            // below) for its own side effects -- e.g. std::move(x)
            // marking x moved-out in `state`.
            if (report_errors &&
                !produces_rvalue_of_type(arg, *sig->param_types[param_index].pointee, body, signatures)) {
                throw DataflowError(
                    "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                    "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                    "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                    state.current_loc);
            }
            apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors);
        } else if (param_is_reference) {
            apply_reference_argument(arg, sig->param_types[param_index], state, in_call_borrows, body, signatures,
                                      report_errors);
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
                if (needs_class_pointer_validation && arg_type->kind == TypeKind::Pointer &&
                    !raw_pointer_implicitly_convertible(*arg_type, sig->param_types[param_index]) &&
                    !types_compatible_with_base_conversion(*arg_type, sig->param_types[param_index], *body.program,
                                                           enclosing_class_name(body))) {
                    throw DataflowError("cannot pass an incompatible pointer type to parameter '" +
                                            sig->param_names[param_index] + "'",
                                        state.current_loc);
                }
            }
            bool class_value_param =
                sig != nullptr && param_index < sig->param_types.size() &&
                is_named_record_type_for_call_binding(sig->param_types[param_index], body);
            bool copyable_lvalue_source =
                class_value_param && is_copyable_class_lvalue_boundary_source(arg, sig->param_types[param_index], body, signatures);
            const FunctionSignature* converting_ctor =
                class_value_param ? find_single_argument_converting_constructor_signature(sig->param_types[param_index], arg, body,
                                                                                         signatures)
                                  : nullptr;
            if (report_errors && class_value_param && !copyable_lvalue_source &&
                !produces_rvalue_of_type(arg, sig->param_types[param_index], body, signatures) && converting_ctor == nullptr) {
                throw DataflowError("passing class '" + sig->param_types[param_index].name +
                                     "' by value requires either a copyable bare local of that exact type or "
                                     "a fresh value such as std::move(x) or a call returning by value",
                    state.current_loc);
            }
            if (converting_ctor != nullptr) {
                if (report_errors && converting_ctor->is_unsafe && state.unsafe_depth == 0) {
                    throw DataflowError("cannot use '" + sig->param_types[param_index].name +
                                         "'s converting constructor outside '[[scpp::unsafe]] { }': its own declaration is "
                                         "marked '[[scpp::unsafe]]', so its soundness depends on a precondition only the "
                                         "caller can guarantee (ch01 §1.2/§1.3)",
                        state.current_loc);
                }
                const Type& ctor_param_type = converting_ctor->param_types[1];
                if (is_reference(ctor_param_type) && ctor_param_type.is_rvalue_ref) {
                    if (report_errors && !produces_rvalue_of_type(arg, *ctor_param_type.pointee, body, signatures)) {
                        throw DataflowError(
                            "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                            "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                            "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                            state.current_loc);
                    }
                    apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors);
                } else if (is_reference(ctor_param_type)) {
                    apply_reference_argument(arg, ctor_param_type, state, in_call_borrows, body, signatures, report_errors);
                } else {
                    apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors);
                }
                if (report_errors && sig != nullptr) {
                    enforce_thread_safety_constraints_for_argument(arg, *sig, param_index, "function", callee_display, body,
                                                                   signatures, state.current_loc);
                }
                continue;
            }
            apply_expr(arg, /*is_move_target_context=*/!copyable_lvalue_source, state, body, signatures, report_errors);

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
                throw DataflowError("cannot pass '&' of a read-only-reachable place as a mutable 'T*' "
                                    "argument (would need 'const T*', which this parameter doesn't accept)",
                    state.current_loc);
            }
        }
        if (report_errors && sig != nullptr) {
            enforce_thread_safety_constraints_for_argument(arg, *sig, param_index, "function", callee_display, body,
                                                           signatures, state.current_loc);
        }
    }
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
void check_constructor_arguments(const std::string& class_name, const std::vector<ExprPtr>& ctor_args,
                                  DataflowState& state, const Body& body, const Signatures& signatures,
                                  bool report_errors) {
    std::string ctor_name = class_name + "_new";
    const FunctionSignature* sig = nullptr;
    auto name_it = signatures.find(ctor_name);
    if (name_it != signatures.end()) {
        const std::vector<FunctionSignature>& candidates = name_it->second;
        std::vector<const FunctionSignature*> visible_arity_matches;
        for (const FunctionSignature& candidate : candidates) {
            if (!compile_time_dependency_visible_in_body(candidate, body)) continue;
            if (candidate.param_types.size() != ctor_args.size() + 1) continue;
            visible_arity_matches.push_back(&candidate);
        }
        if (visible_arity_matches.size() == 1) {
            sig = visible_arity_matches[0];
        }
        std::vector<const FunctionSignature*> matches;
        if (sig == nullptr) {
            for (const FunctionSignature& candidate : candidates) {
                if (!compile_time_dependency_visible_in_body(candidate, body)) continue;
                if (candidate.param_types.size() != ctor_args.size() + 1) continue;
                bool all_match = true;
                for (size_t i = 0; all_match && i < ctor_args.size(); i++) {
                    all_match = argument_matches_parameter_for_constructor_selection(*ctor_args[i],
                                                                                     candidate.param_types[i + 1], body,
                                                                                     signatures);
                }
                if (all_match) matches.push_back(&candidate);
            }
            if (matches.size() == 1) sig = matches[0];
        }
    }
    if (sig == nullptr && report_errors && ctor_args.empty()) {
        static const std::vector<ExprPtr> no_ctor_args;
        if (body.program != nullptr && !class_has_any_constructor(class_name, *body.program)) {
            ensure_implicit_default_construction_is_valid(class_name, state.current_class, body, signatures,
                                                          state.current_loc,
                                                          "implicit default construction of class '" + class_name +
                                                              "' is ill-formed");
            return;
        }
        sig = resolve_constructor_signature(class_name, no_ctor_args, body, signatures);
        if (sig == nullptr) {
            throw DataflowError("type '" + class_name +
                                    "' has no default constructor; no constructor of '" + class_name +
                                    "' matches 0 arguments",
                                state.current_loc);
        }
    }
    if (report_errors && sig != nullptr && sig->access == AccessSpecifier::Private &&
        !sig->member_owner_class.empty() && state.current_class != sig->member_owner_class) {
        throw DataflowError("cannot call private constructor of class '" + class_name +
                             "' from outside its own methods",
            state.current_loc);
    }
    if (report_errors && sig != nullptr && sig->is_unsafe && state.unsafe_depth == 0) {
        throw DataflowError("cannot call '" + class_name +
                             "'s constructor outside '[[scpp::unsafe]] { }': its own declaration is marked "
                             "'[[scpp::unsafe]]', so its soundness depends on a precondition only the "
                             "caller can guarantee (ch01 §1.2/§1.3)",
            state.current_loc);
    }
    BorrowMap in_call_borrows;
    bool constructed_state_can_carry_lifetimes =
        report_errors && body.program != nullptr &&
        type_contains_lifetime_carrying_state(named_type(class_name), *body.program);
    for (size_t i = 0; i < ctor_args.size(); i++) {
        const Expr& arg = *ctor_args[i];
        if (constructed_state_can_carry_lifetimes) {
            reject_lifetime_group_state_embedding(arg, state, body, signatures, report_errors, "constructed object state");
        }
        size_t param_index = i + 1;
        bool param_is_reference =
            sig != nullptr && param_index < sig->param_types.size() && is_reference(sig->param_types[param_index]);
        bool param_is_rvalue_reference = param_is_reference && sig->param_types[param_index].is_rvalue_ref;
        if (param_is_rvalue_reference) {
            if (report_errors &&
                !produces_rvalue_of_type(arg, *sig->param_types[param_index].pointee, body, signatures)) {
                throw DataflowError(
                    "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                    "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                    "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                    state.current_loc);
            }
            apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors);
        } else if (param_is_reference) {
            apply_reference_argument(arg, sig->param_types[param_index], state, in_call_borrows, body, signatures,
                                      report_errors);
        } else {
            bool class_value_param =
                sig != nullptr && param_index < sig->param_types.size() &&
                is_named_record_type_for_call_binding(sig->param_types[param_index], body);
            bool copyable_lvalue_source =
                class_value_param && is_copyable_class_lvalue_boundary_source(arg, sig->param_types[param_index], body, signatures);
            if (report_errors && class_value_param && !copyable_lvalue_source &&
                !produces_rvalue_of_type(arg, sig->param_types[param_index], body, signatures)) {
                throw DataflowError("passing class '" + sig->param_types[param_index].name +
                                     "' by value requires either a copyable bare local of that exact type or "
                                     "a fresh value such as std::move(x) or a call returning by value",
                    state.current_loc);
            }
            apply_expr(arg, /*is_move_target_context=*/!copyable_lvalue_source, state, body, signatures, report_errors);
        }
        if (report_errors && sig != nullptr) {
            enforce_thread_safety_constraints_for_argument(arg, *sig, param_index, "constructor", class_name, body,
                                                           signatures, state.current_loc);
        }
    }
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
void apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
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
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
            return;

        case ExprKind::Sizeof:
            if (report_errors) validate_sizeof_operand(expr, body, signatures, state.current_loc);
            return;

        case ExprKind::Identifier: {
            if (!report_errors) return;
            auto type_it = expr.explicit_global_qualification ? body.local_types.end() : body.local_types.find(expr.name);
            if (type_it == body.local_types.end()) return; // unknown name: left to codegen's own check
            LocalState current = lookup(state.locals, expr.name);
            if (current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(expr.name, current),
                    state.current_loc);
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
            auto borrow_it = state.borrows.find(expr.name);
            if (borrow_it != state.borrows.end() && borrow_it->second.mutable_borrow) {
                throw DataflowError("cannot use '" + expr.name + "' while it is mutably borrowed",
                    state.current_loc);
            }
            return;
        }

        case ExprKind::Move: {
            if (expr.lhs->kind != ExprKind::Identifier) {
                if (report_errors) {
                    throw DataflowError("std::move currently only supports a plain local variable "
                                         "(not a member, subscript, or other expression)",
                        state.current_loc);
                }
                return;
            }
            const std::string& name = expr.lhs->name;
            auto type_it = body.local_types.find(name);
            // spec §6.2(3): `std::move(E)` is a syntactic ownership-state
            // transition on any named object `E`, not just on class types.
            // The same named-object rule already covers an rvalue-
            // reference local/parameter (`Inner&& i`, ch03/ch05 §5.11):
            // `i` itself is still a name, and moving from it marks that
            // local/parameter moved-out exactly like any other local name.
            if (type_it == body.local_types.end()) {
                if (report_errors) {
                    throw DataflowError("unknown variable '" + name + "'",
                        state.current_loc);
                }
                return;
            }
            LocalState current = lookup(state.locals, name);
            if (report_errors && current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(name, current),
                    state.current_loc);
            }
            if (report_errors) {
                auto borrow_it = state.borrows.find(name);
                if (borrow_it != state.borrows.end() &&
                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                    throw DataflowError("cannot move '" + name + "' while it is borrowed",
                        state.current_loc);
                }
            }
            state.locals[name] = LocalState::MovedOut;
            if (report_errors && !is_move_target_context) {
                throw DataflowError("std::move(" + name + ") must be used to initialize, assign into, return, "
                                                            "pass, or capture a value",
                    state.current_loc);
            }
            return;
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
            apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            if (report_errors) {
                std::optional<Type> source_type = infer_expr_type(*expr.lhs, body, signatures);
                if ((source_type.has_value() && is_interface_representation_type(*source_type, *body.program)) ||
                    is_interface_representation_type(expr.type, *body.program)) {
                    throw DataflowError("cannot cast interface-typed pointers or references to other scalar or raw "
                                            "pointer representations",
                                        state.current_loc);
                }
                bool scalar_source = source_type.has_value() && source_type->kind == TypeKind::Named &&
                                     is_scalar_type_name(source_type->name);
                bool scalar_target = expr.type.kind == TypeKind::Named && is_scalar_type_name(expr.type.name);
                if (scalar_source && scalar_target) return;

                bool integral_source = source_type.has_value() && source_type->kind == TypeKind::Named &&
                                       is_integral_scalar_type_name(source_type->name);
                bool target_is_enum = is_enum_type(expr.type, body.program);
                if (integral_source && target_is_enum) {
                    throw DataflowError("cannot cast an integer value to enum class '" + expr.type.name +
                                            "'; use scpp::enum_cast<" + expr.type.name + ">(value) instead",
                                        state.current_loc);
                }

                const Type* source_enum_underlying =
                    source_type.has_value() && source_type->kind == TypeKind::Named ? enum_underlying_type(*source_type, body.program)
                                                                                    : nullptr;
                if (source_type.has_value() && source_enum_underlying != nullptr && expr.type.kind == TypeKind::Named &&
                    types_equal(*source_enum_underlying, expr.type)) {
                    return;
                }

                bool raw_pointer_source = source_type.has_value() && source_type->kind == TypeKind::Pointer;
                bool raw_pointer_target = expr.type.kind == TypeKind::Pointer;
                if (raw_pointer_source && raw_pointer_target) {
                    if (state.unsafe_depth == 0) {
                        throw DataflowError("cannot cast between raw pointer types outside '[[scpp::unsafe]] { }' "
                                                "(spec §5.1(5.2))",
                                            state.current_loc);
                    }
                    return;
                }

                {
                    throw DataflowError(
                        "a cast is only supported between two builtin scalar types, from an enum class to its "
                        "underlying integer type, or between two raw pointer types inside '[[scpp::unsafe]] { }', in "
                        "this version",
                        state.current_loc);
                }
            }
            return;
        }

        case ExprKind::Unary:
            if (expr.unary_op == UnaryOp::Deref) {
                apply_deref(expr, state, body, signatures, report_errors);
                return;
            }
            if (expr.unary_op == UnaryOp::AddressOf) {
                apply_address_of(expr, state, body, signatures, report_errors);
                return;
            }
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::New:
            if (report_errors && state.unsafe_depth == 0) {
                throw DataflowError("cannot use 'new' outside '[[scpp::unsafe]] { }' (spec §5.1(5.4))",
                                    state.current_loc);
            }
            if (expr.lhs) {
                apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors);
                if (report_errors) {
                    std::optional<Type> placement_type = infer_expr_type(*expr.lhs, body, signatures);
                    if (!placement_type.has_value() || placement_type->kind != TypeKind::Pointer ||
                        placement_type->pointee == nullptr || !types_equal(*placement_type->pointee, expr.type)) {
                        throw DataflowError("placement 'new' requires a raw pointer to the constructed type in this "
                                                "version",
                                            state.current_loc);
                    }
                }
            }
            if (expr.type.kind == TypeKind::Named && state.class_names != nullptr && state.class_names->contains(expr.type.name)) {
                bool move_shape = expr.args.size() == 1 && expr.args[0]->kind == ExprKind::Move &&
                                  produces_rvalue_of_type(*expr.args[0], expr.type, body, signatures);
                if (move_shape) {
                    apply_expr(*expr.args[0], /*is_move_target_context=*/true, state, body, signatures, report_errors);
                } else if (expr.args.size() == 1 &&
                           body.program != nullptr && !has_user_declared_copy_ctor(expr.type.name, *body.program) &&
                           is_copyable_class_lvalue_boundary_source(*expr.args[0], expr.type, body, signatures)) {
                    apply_expr(*expr.args[0], /*is_move_target_context=*/false, state, body, signatures, report_errors);
                } else {
                    check_constructor_arguments(expr.type.name, expr.args, state, body, signatures, report_errors);
                }
                return;
            }
            for (const auto& arg : expr.args) {
                apply_expr(*arg, /*is_move_target_context=*/arg->kind == ExprKind::Move, state, body, signatures,
                           report_errors);
            }
            return;

        case ExprKind::Delete:
            apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            if (report_errors && state.unsafe_depth == 0) {
                throw DataflowError("cannot use 'delete' outside '[[scpp::unsafe]] { }' (spec §5.1(5.4))",
                                    state.current_loc);
            }
            if (report_errors) {
                std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures);
                if (!operand_type.has_value() || operand_type->kind != TypeKind::Pointer) {
                    throw DataflowError("'delete' requires a raw pointer operand in this version",
                                        state.current_loc);
                }
            }
            return;

        case ExprKind::Destroy:
            apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            if (!report_errors) return;
            if (state.unsafe_depth == 0) {
                throw DataflowError("cannot call an explicit destructor outside '[[scpp::unsafe]] { }'", state.current_loc);
            }
            if (!expr.destroy_through_pointer) {
                throw DataflowError("explicit destructor calls currently require the pointer form 'ptr->~T()'",
                                    state.current_loc);
            }
            {
                std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures);
                if (!operand_type.has_value() || operand_type->kind != TypeKind::Pointer || operand_type->pointee == nullptr ||
                    !types_equal(*operand_type->pointee, expr.type)) {
                    throw DataflowError("explicit destructor calls require a raw pointer to the named type in this version",
                                        state.current_loc);
                }
            }
            return;

        case ExprKind::Fold:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            if (expr.rhs) apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::Conditional:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.third, false, state, body, signatures, report_errors);
            if (report_errors) {
                std::optional<Type> condition_type = infer_expr_type(*expr.lhs, body, signatures);
                if (!condition_type.has_value() || condition_type->kind != TypeKind::Named ||
                    condition_type->name != "bool") {
                    throw DataflowError("conditional operator requires a 'bool' condition", state.current_loc);
                }
                std::optional<Type> then_type = infer_expr_type(*expr.rhs, body, signatures);
                std::optional<Type> else_type = infer_expr_type(*expr.third, body, signatures);
                if (then_type.has_value() && else_type.has_value() && !types_equal(*then_type, *else_type)) {
                    throw DataflowError("conditional operator requires both arms to have the same type",
                                        state.current_loc);
                }
            }
            return;

        case ExprKind::Binary:
            if (expr.binary_op == BinaryOp::Assign) {
                bool target_is_movable_class = false;
                std::optional<Type> target_class_type;
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto it = body.local_types.find(expr.lhs->name);
                    if (it != body.local_types.end() && it->second.kind == TypeKind::Named &&
                        state.class_names != nullptr && state.class_names->contains(it->second.name)) {
                        target_is_movable_class = true;
                        target_class_type = it->second;
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
                    std::optional<Type> field_type = resolve_member_field_type(*expr.lhs, body, state);
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
                    throw DataflowError("class '" + target_class_type->name +
                                         "' is not copy-assignable (spec §6.5(3)) -- this assignment is not "
                                         "licensed",
                        state.current_loc);
                }
                bool is_move_target = target_is_movable_class || expr.rhs->kind == ExprKind::Move;
                apply_expr(*expr.rhs, is_move_target, state, body, signatures, report_errors);
                if (expr.lhs->kind == ExprKind::Member || expr.lhs->kind == ExprKind::Subscript) {
                    reject_lifetime_group_state_embedding(*expr.rhs, state, body, signatures, report_errors,
                                                          expr.lhs->kind == ExprKind::Member ? "object state"
                                                                                            : "an array element");
                }
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto it = body.local_types.find(expr.lhs->name);
                    if (it != body.local_types.end()) {
                        check_function_pointer_assignment(it->second, *expr.rhs, body, signatures, state.current_loc,
                                                          expr.lhs->name, report_errors);
                        if (report_errors) {
                            check_enum_conversion_compatibility(it->second, *expr.rhs, body, signatures,
                                                                state.current_loc);
                        }
                    }
                } else if (expr.lhs->kind == ExprKind::Member) {
                    std::optional<Type> field_type = resolve_member_field_type(*expr.lhs, body, state);
                    if (field_type.has_value()) {
                        check_function_pointer_assignment(*field_type, *expr.rhs, body, signatures, state.current_loc,
                                                          expr.lhs->name, report_errors);
                        if (report_errors) {
                            check_enum_conversion_compatibility(*field_type, *expr.rhs, body, signatures,
                                                                state.current_loc);
                        }
                    }
                }
                if (expr.lhs->kind == ExprKind::Identifier) {
                    // The assignment target is never a "read": whatever
                    // its previous state, assigning any value returns it
                    // to Initialized (spec ch05.1).
                    state.locals[expr.lhs->name] = LocalState::Initialized;
                } else {
                    // e.g. `p.x = 1;` or `arr[i] = 1;`: the base
                    // object/index are evaluated (as addresses / an
                    // index value), not read as "the assignment target",
                    // so still worth walking for nested reads.
                    apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
                    if (report_errors) {
                        if (assignment_target_is_read_only(*expr.lhs, body, signatures)) {
                            throw DataflowError("cannot assign to this place: it is reached through a "
                                                 "read-only (const) reference",
                                state.current_loc);
                        }
                        if (std::optional<std::string> lender = resolve_reborrow_lender(*expr.lhs, body, signatures);
                            lender.has_value()) {
                            validate_reborrow_lender_write(*lender, state, report_errors);
                        }
                        if (std::optional<std::string> root = direct_write_root(*expr.lhs, body)) {
                            auto borrow_it = state.borrows.find(*root);
                            if (borrow_it != state.borrows.end() &&
                                (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                                throw DataflowError("cannot assign to this place: '" + *root +
                                                     "' is currently borrowed",
                                    state.current_loc);
                            }
                        }
                    }
                }
                return;
            }
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            if (report_errors) check_binary_expr_operand_types(expr, body, signatures, state.current_loc);
            return;

        case ExprKind::Call:
            if (is_for_range_size_builtin(expr)) {
                apply_expr(*expr.args[0], false, state, body, signatures, report_errors);
                return;
            }
            check_call_arguments(expr, state, body, signatures, report_errors);
            return;

        case ExprKind::Member: {
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            if (report_errors && body.program != nullptr) {
                std::optional<Type> base_type = infer_expr_type(*expr.lhs, body, signatures);
                if (base_type.has_value()) {
                    const Type& named = base_type->kind == TypeKind::Reference ? *base_type->pointee : *base_type;
                    if (named.kind == TypeKind::Named) {
                        for (const StructDef& def : body.program->structs) {
                            if (def.name == named.name && def.is_union && state.unsafe_depth == 0) {
                                throw DataflowError("accessing a union member requires [[scpp::unsafe]] "
                                                    "(FFI union storage may alias multiple representations)",
                                    state.current_loc);
                            }
                        }
                    }
                }
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
                auto type_it = body.local_types.find(expr.lhs->name);
                if (type_it != body.local_types.end()) {
                    std::string class_name = named_type_name(type_it->second);
                    if (!class_name.empty() && state.class_names->contains(class_name) &&
                        class_name != state.current_class) {
                        AccessSpecifier access = AccessSpecifier::Private;
                        if (state.class_field_access != nullptr) {
                            auto class_it = state.class_field_access->find(class_name);
                            if (class_it != state.class_field_access->end()) {
                                auto field_it = class_it->second.find(expr.name);
                                if (field_it != class_it->second.end()) access = field_it->second;
                            }
                        }
                        if (access == AccessSpecifier::Private) {
                            throw DataflowError("cannot access private member '" + expr.name + "' of class '" +
                                                 class_name + "' from outside its own methods (ch04 §4.2)",
                                state.current_loc);
                        }
                    }
                }
            }
            return;
        }

        case ExprKind::Subscript:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::PackExpansion:
            if (report_errors) {
                throw DataflowError("unexpanded parameter-pack expression reached move checking",
                                    state.current_loc);
            }
            return;

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
            apply_lambda_captures(expr, state, capture_borrows, body, signatures, report_errors);
            return;
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
void apply_reference_binding(const MirStatement& stmt, DataflowState& state, const Body& body,
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
            throw DataflowError(std::string(kind_name) + " '" + stmt.local +
                                 "' must be initialized (bound to a variable) at declaration",
                state.current_loc);
        }
        state.locals[stmt.local] = LocalState::Initialized;
        return;
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
        apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body, signatures,
                   report_errors);
        state.locals[stmt.local] = LocalState::Initialized;
        return;
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
            throw DataflowError("cannot bind reference '" + stmt.local +
                                 "' from an incompatible source type",
                                state.current_loc);
        }
    }

    RootSet roots = resolve_borrow_source_root(*stmt.expr, state, body, signatures, report_errors);
    if (roots.empty()) {
        // Only reachable when report_errors=false and the source
        // expression's shape isn't (yet) a supported borrow source --
        // resolve_borrow_source_root would have thrown had report_errors
        // been true, so this whole program is already doomed to be
        // rejected by the upcoming reporting pass; just leave `stmt.local`
        // itself readable so this (discarded) silent fixed-point
        // iteration has *some* defined state to continue from.
        state.locals[stmt.local] = LocalState::Initialized;
        return;
    }

    bool is_mutable = stmt.type.is_mutable_ref;
    std::optional<std::string> lender = resolve_reborrow_lender(*stmt.expr, body, signatures);
    bool lender_is_mutable =
        lender.has_value() && is_reborrowable_local_type(body.local_types.at(*lender)) && body.local_types.at(*lender).is_mutable_ref;
    if (lender.has_value() && lender_is_mutable) {
        validate_reborrow_lender(*lender, is_mutable, state, body, report_errors);
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
        throw DataflowError(std::string("cannot bind a mutable ") + kind_name + " '" + stmt.local +
                             "': its source is only reachable through a read-only (const) reference",
            state.current_loc);
    }

    if (!(lender.has_value() && lender_is_mutable)) {
        for (const std::string& root : roots) {
            BorrowState& borrow = state.borrows[root];
            if (report_errors) {
                if (is_mutable && (borrow.mutable_borrow || borrow.shared_count > 0)) {
                    throw DataflowError("cannot mutably borrow '" + root + "': it is already borrowed",
                                        state.current_loc);
                }
                if (!is_mutable && borrow.mutable_borrow) {
                    throw DataflowError("cannot borrow '" + root + "': it is already mutably borrowed",
                                        state.current_loc);
                }
            }
            if (is_mutable) {
                borrow.mutable_borrow = true;
            } else {
                borrow.shared_count++;
            }
        }
    } else {
        state.suspended_reborrows[*lender]++;
    }
    state.ref_targets[stmt.local] = RefTarget{roots, lender.value_or("")};
    state.local_lifetime_sources[stmt.local] = roots;
    state.locals[stmt.local] = LocalState::Initialized;
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
void apply_reference_write_through(const MirStatement& stmt, DataflowState& state, const Body& body,
                                    const Signatures& signatures, bool report_errors) {
    const Type& ref_type = body.local_types.at(stmt.local);
    if (report_errors) {
        validate_reborrow_lender_write(stmt.local, state, report_errors);
        if (!ref_type.is_mutable_ref) {
            throw DataflowError("cannot assign through '" + stmt.local +
                                 "': it is a read-only (const) reference",
                state.current_loc);
        }
        LocalState current = lookup(state.locals, stmt.local);
        if (current != LocalState::Initialized) {
            throw DataflowError(describe_bad_state(stmt.local, current),
                state.current_loc);
        }
    }
    apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
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
    auto type_it = body.local_types.find(arg.lhs->name);
    return type_it != body.local_types.end() && types_equal(type_it->second, constructed_type);
}

[[nodiscard]] bool is_lvalue_copy_source_shape(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::Identifier:
            return true;
        case ExprKind::Member:
        case ExprKind::Subscript:
            return expr.lhs != nullptr && is_lvalue_copy_source_shape(*expr.lhs);
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
    if (!is_lvalue_copy_source_shape(expr)) return false;
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    if (!source_type.has_value()) return false;
    if (types_equal(*source_type, target_type)) return true;
    return source_type->kind == TypeKind::Reference && !source_type->is_rvalue_ref && source_type->pointee &&
           types_equal(*source_type->pointee, target_type);
}

void apply_statement(const MirStatement& stmt, DataflowState& state, const Body& body, const Signatures& signatures,
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
                    apply_expr(*(*stmt.ctor_args)[0], /*is_move_target_context=*/true, state, body, signatures,
                               report_errors);
                } else if (stmt.ctor_args->size() == 1 &&
                           body.program != nullptr && !has_user_declared_copy_ctor(stmt.type.name, *body.program) &&
                           is_copyable_class_lvalue_boundary_source(*(*stmt.ctor_args)[0], stmt.type, body, signatures)) {
                    apply_expr(*(*stmt.ctor_args)[0], /*is_move_target_context=*/false, state, body, signatures,
                               report_errors);
                } else {
                    check_constructor_arguments(stmt.type.name, *stmt.ctor_args, state, body, signatures,
                                                 report_errors);
                }
            }
            // scpp has no "uninitialized" state (see the LocalState
            // comment above): a bare declaration always zero-initializes,
            // so it's always Initialized from this point on.
            state.locals[stmt.local] = LocalState::Initialized;
            if (is_lifetime_eligible_type(stmt.type)) state.local_lifetime_sources.erase(stmt.local);
            return;

        case MirStatementKind::BindReference:
            apply_reference_binding(stmt, state, body, signatures, report_errors);
            return;

        case MirStatementKind::Assign: {
            auto type_it = body.local_types.find(stmt.local);
            // ch05/ch06: a `const`-qualified local (Stmt::is_const,
            // Body::const_locals) is initialized exactly once, by the
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
            if (report_errors && body.const_locals.contains(stmt.local) && state.locals.contains(stmt.local)) {
                throw DataflowError("cannot reassign 'const' variable '" + stmt.local + "' after initialization",
                    state.current_loc);
            }
            if (type_it != body.local_types.end() && is_reference(type_it->second)) {
                apply_reference_write_through(stmt, state, body, signatures, report_errors);
                return;
            }
            if (type_it != body.local_types.end() && is_span(type_it->second)) {
                // Unlike real C++ (where std::span is an ordinary,
                // freely-reassignable value), v0.1 conservatively treats
                // it exactly like a reference: bound once at
                // declaration, never rebound (see mir.cppm's
                // BindReference comment) -- lifting that is a follow-up,
                // not a soundness requirement.
                if (report_errors) {
                    throw DataflowError("std::span '" + stmt.local +
                                         "' cannot be reassigned after initialization in this version",
                        state.current_loc);
                }
                return;
            }
            if (type_it != body.local_types.end() && state.class_names != nullptr &&
                type_it->second.kind == TypeKind::Named && state.class_names->contains(type_it->second.name)) {
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
                bool is_move_assignment = produces_rvalue_of_type(*stmt.expr, type_it->second, body, signatures);
                if (is_move_assignment && state.locals.contains(stmt.local)) {
                    if (report_errors) {
                        bool has_reference_member = false;
                        if (state.class_field_types != nullptr) {
                            auto fields_it = state.class_field_types->find(type_it->second.name);
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
                            throw DataflowError(
                                "class '" + type_it->second.name +
                                    "' has a reference-typed member, so it has no move assignment operator "
                                    "(spec §6.4(3)) -- '" + stmt.local + "' cannot be reassigned",
                                state.current_loc);
                        }
                        auto borrow_it = state.borrows.find(stmt.local);
                        if (borrow_it != state.borrows.end() &&
                            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                            throw DataflowError("cannot assign to class variable '" + stmt.local +
                                                 "': it is currently borrowed",
                                state.current_loc);
                        }
                    }
                    apply_expr(*stmt.expr, /*is_move_target_context=*/true, state, body, signatures, report_errors);
                    state.locals[stmt.local] = LocalState::Initialized;
                    return;
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
                if (is_bare_same_type_copy_source(*stmt.expr, type_it->second, body, signatures) &&
                    state.locals.contains(stmt.local)) {
                    if (report_errors) {
                        if (state.classes_with_copy_assign == nullptr ||
                            !state.classes_with_copy_assign->contains(type_it->second.name)) {
                            throw DataflowError("class '" + type_it->second.name +
                                                 "' is not copy-assignable (spec §6.5(3)) -- '" + stmt.local +
                                                 "' cannot be reassigned this way",
                                state.current_loc);
                        }
                        auto borrow_it = state.borrows.find(stmt.local);
                        if (borrow_it != state.borrows.end() &&
                            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                            throw DataflowError("cannot assign to class variable '" + stmt.local +
                                                 "': it is currently borrowed",
                                state.current_loc);
                        }
                    }
                    apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
                               signatures, report_errors);
                    state.locals[stmt.local] = LocalState::Initialized;
                    return;
                }
                if (report_errors && state.locals.contains(stmt.local)) {
                    throw DataflowError("class '" + type_it->second.name + "'-typed variable '" + stmt.local +
                                         "' cannot be reassigned after construction in this version (no copy "
                                         "semantics are defined yet -- see ch04 §4.2)",
                        state.current_loc);
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
                    apply_lambda_captures(*stmt.expr, state, state.borrows, body, signatures, report_errors,
                                          &closure_capture_borrows);
                    if (!closure_capture_borrows.empty()) {
                        state.closure_capture_borrows[stmt.local] = std::move(closure_capture_borrows);
                    }
                } else {
                    if (produces_rvalue_of_type(*stmt.expr, type_it->second, body, signatures)) {
                        apply_expr(*stmt.expr, /*is_move_target_context=*/true, state, body, signatures,
                                   report_errors);
                        state.locals[stmt.local] = LocalState::Initialized;
                        return;
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
                    if (report_errors) {
                        if (!is_bare_same_type_copy_source(*stmt.expr, type_it->second, body, signatures)) {
                            throw DataflowError(
                                "class '" + type_it->second.name + "'-typed variable '" + stmt.local +
                                    "' can only be initialized via constructor-call syntax ('" +
                                    type_it->second.name + " " + stmt.local +
                                    "(args);'), std::move of the same type, or (if the class is copy-"
                                    "constructible, spec §6.5) a plain copy of another '" + type_it->second.name +
                                    "' variable",
                                state.current_loc);
                        }
                        if (state.classes_with_copy_ctor == nullptr ||
                            !state.classes_with_copy_ctor->contains(type_it->second.name)) {
                            throw DataflowError("class '" + type_it->second.name +
                                                 "' is not copy-constructible (spec §6.5(2)) -- '" + stmt.local +
                                                 "' cannot be initialized this way",
                                state.current_loc);
                        }
                    }
                    apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures,
                               report_errors);
                }
                state.locals[stmt.local] = LocalState::Initialized;
                return;
            }

            apply_expr(*stmt.expr, /*is_move_target_context=*/stmt.expr->kind == ExprKind::Move, state, body,
                       signatures, report_errors);
            if (type_it != body.local_types.end()) {
                check_function_pointer_assignment(type_it->second, *stmt.expr, body, signatures, state.current_loc,
                                                  stmt.local, report_errors);
                check_raw_pointer_assignment(type_it->second, *stmt.expr, body, signatures, state.current_loc,
                                             stmt.local, report_errors);
                if (report_errors) {
                    check_enum_conversion_compatibility(type_it->second, *stmt.expr, body, signatures,
                                                        state.current_loc);
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
            if (report_errors && type_it != body.local_types.end() && type_it->second.kind == TypeKind::Pointer &&
                type_it->second.is_mutable_pointee && stmt.expr->kind == ExprKind::Unary &&
                stmt.expr->unary_op == UnaryOp::AddressOf && is_read_only_reachable(*stmt.expr->lhs, body, signatures)) {
                throw DataflowError("cannot assign '&' of a read-only-reachable place to '" + stmt.local +
                                    "' (a mutable 'T*'): would need 'const T*', which '" + stmt.local +
                                    "' isn't declared as",
                    state.current_loc);
            }

            if (report_errors) {
                auto borrow_it = state.borrows.find(stmt.local);
                if (borrow_it != state.borrows.end() &&
                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                    throw DataflowError("cannot assign to '" + stmt.local + "' while it is borrowed",
                        state.current_loc);
                }
            }
            state.locals[stmt.local] = LocalState::Initialized;
            if (type_it != body.local_types.end() && is_pointer(type_it->second)) {
                state.local_lifetime_sources[stmt.local] =
                    resolve_lifetime_source_roots(*stmt.expr, state, body, signatures, report_errors);
            }
            return;
        }

        case MirStatementKind::Eval:
            apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            if (report_errors) {
                if (const NodiscardInfo* info = nodiscard_info_for_discarded_call(*stmt.expr, body, signatures)) {
                    std::string message = "discarded return value of nodiscard " + info->subject;
                    if (!info->reason.empty()) message += ": " + info->reason;
                    throw DataflowError(message, stmt.expr->loc);
                }
            }
            return;

        case MirStatementKind::Drop:
            // Purely a codegen-facing marker (no-op until heap-allocated
            // owning types exist); no dataflow state effect here.
            return;

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
            return;
        }

        case MirStatementKind::UnsafeEnter:
            state.unsafe_depth++;
            return;

        case MirStatementKind::UnsafeExit:
            state.unsafe_depth--;
            return;
    }
}

void check_terminator(const Terminator& term, DataflowState& state, const Function& fn, const Body& body,
                       const Signatures& signatures) {
    // See apply_expr's identical opening comment.
    state.current_loc = term.loc;
    switch (term.kind) {
        case TerminatorKind::Branch:
            apply_expr(*term.condition, false, state, body, signatures, /*report_errors=*/true);
            return;
        case TerminatorKind::Return: {
            if (term.return_value == nullptr) return;
            if (is_lifetime_eligible_type(fn.return_type)) {
                std::optional<Type> returned_type = infer_expr_type(*term.return_value, body, signatures);
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
                }
                if (!return_type_compatible) {
                    throw DataflowError("function '" + fn.name + "' returns a lifetime-tracked value from an incompatible source type",
                                        state.current_loc);
                }
                RootSet returned_roots =
                    resolve_lifetime_source_roots(*term.return_value, state, body, signatures, /*report_errors=*/true);
                if (fn.return_lifetime.present()) {
                    if (!roots_satisfy_named_lifetime_group(returned_roots, fn, fn.return_lifetime.name)) {
                        throw DataflowError("function '" + fn.name + "' returns a value derived from " +
                                                format_roots(returned_roots) + ", not from lifetime group '" +
                                                fn.return_lifetime.name + "'",
                                            state.current_loc);
                    }
                } else if (is_reference(fn.return_type)) {
                    std::vector<size_t> source_indices = resolve_returned_lifetime_param_indices(fn);
                    if (!source_indices.empty()) {
                        RootSet expected = single_root(fn.params[source_indices.front()].name);
                        if (returned_roots != expected) {
                            throw DataflowError(
                                "function '" + fn.name + "' returns a reference derived from " +
                                    format_roots(returned_roots) + ", not from its sole reference parameter '" +
                                    fn.params[source_indices.front()].name +
                                    "'; scpp v0.1 can only prove a returned reference doesn't dangle when it "
                                    "borrows (directly or transitively) from that parameter (spec ch05.3)",
                                state.current_loc);
                        }
                    }
                }
                return;
            }
            bool return_is_class_value = is_named_class_type(fn.return_type, body);
            bool implicit_move_source =
                return_is_class_value && is_implicit_move_return_source(*term.return_value, fn.return_type, body);
            bool move_target_context = return_is_class_value || term.return_value->kind == ExprKind::Move;
            apply_expr(*term.return_value, move_target_context, state, body, signatures, /*report_errors=*/true);
            if (return_is_class_value && !implicit_move_source &&
                !produces_rvalue_of_type(*term.return_value, fn.return_type, body, signatures)) {
                throw DataflowError("returning class '" + fn.return_type.name +
                                     "' by value requires either a bare same-type local/parameter or "
                                     "a fresh value such as std::move(x) or a call returning by value",
                    state.current_loc);
            }
            return;
        }
        case TerminatorKind::Goto:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
            return;
    }
}

// Runs the worklist algorithm (see spec ch07/M3) to a fixed point over
// `body`'s CFG, computing a stable per-block entry ("IN") state for the
// definite-initialization/move/borrow lattice above, then makes one more
// pass reporting any unsafe use found using those now-stable states.
// Splitting into these two phases avoids both false positives (from
// not-yet-stable intermediate states) and duplicate diagnostics (a block
// can be visited many times during fixed-point iteration).
void check_function(const Function& fn, const Program& program, const Signatures& signatures,
                     const std::unordered_set<std::string>& class_names,
                     const ClassFieldTypes& class_field_types, const ClassFieldAccess& class_field_access,
                     const std::unordered_set<std::string>& classes_with_copy_ctor,
                     const std::unordered_set<std::string>& classes_with_copy_assign,
                     [[maybe_unused]] const std::unordered_set<std::string>& witness_class_names) {
    Body body = build_mir(fn);
    body.program = &program;

    size_t n = body.blocks.size();

    std::vector<std::vector<size_t>> preds(n);
    for (size_t i = 0; i < n; i++) {
        for (size_t succ : successors(body.blocks[i].terminator)) {
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
    entry_state.class_names = &class_names;
    entry_state.class_field_types = &class_field_types;
    entry_state.class_field_access = &class_field_access;
    entry_state.classes_with_copy_ctor = &classes_with_copy_ctor;
    entry_state.classes_with_copy_assign = &classes_with_copy_assign;
    for (const Param& param : fn.params) {
        entry_state.locals[param.name] = LocalState::Initialized;
        if (param.lifetime.present()) entry_state.parameter_lifetimes[param.name] = param.lifetime;
        if (is_lifetime_eligible_type(param.type)) {
            entry_state.local_lifetime_sources[param.name] = single_root(param.name);
        }
    }

    std::vector<DataflowState> in_states(n);
    std::vector<DataflowState> out_states(n);
    if (n > 0) in_states[0] = entry_state;

    if (is_constructor_function(fn)) {
        if (const ClassDef* owner = find_class_def(program, fn.member_owner_class)) {
            validate_constructor_base_initialization(fn, *owner, body, signatures);
            validate_constructor_virtual_interface_base_initialization(fn, *owner, body, signatures);
        }
    }

    std::deque<size_t> worklist;
    std::vector<bool> queued(n, false);
    for (size_t i = 0; i < n; i++) {
        worklist.push_back(i);
        queued[i] = true;
    }

    while (!worklist.empty()) {
        size_t b = worklist.front();
        worklist.pop_front();
        queued[b] = false;

        DataflowState new_in;
        if (b == 0) {
            new_in = entry_state;
        } else {
            bool first = true;
            for (size_t p : preds[b]) {
                new_in = first ? out_states[p] : join_states(new_in, out_states[p]);
                first = false;
            }
        }

        DataflowState new_out = new_in;
        for (size_t i = 0; i < body.blocks[b].statements.size(); i++) {
            apply_statement(body.blocks[b].statements[i], new_out, body, signatures, /*report_errors=*/false);
            release_dead_references(new_out, body, live_after[b][i]);
        }

        in_states[b] = new_in;
        bool out_changed = !(new_out == out_states[b]);
        out_states[b] = std::move(new_out);

        if (out_changed) {
            for (size_t succ : successors(body.blocks[b].terminator)) {
                if (!queued[succ]) {
                    worklist.push_back(succ);
                    queued[succ] = true;
                }
            }
        }
    }

    // Fixed point reached: `in_states` is now stable. Walk every block
    // once more, this time actually reporting diagnostics.
    for (size_t b = 0; b < n; b++) {
        DataflowState state = in_states[b];
        for (size_t i = 0; i < body.blocks[b].statements.size(); i++) {
            apply_statement(body.blocks[b].statements[i], state, body, signatures, /*report_errors=*/true);
            release_dead_references(state, body, live_after[b][i]);
        }
        check_terminator(body.blocks[b].terminator, state, fn, body, signatures);
    }
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
void check_moves_impl(const Program& program) {
    Signatures signatures = build_signatures(program);
    validate_class_semantics(program, signatures);
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
            validate_constructor_member_initialization(fn, def, program);
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
        check_function(fn, program, signatures, class_names, class_field_types, class_field_access, classes_with_copy_ctor,
                       classes_with_copy_assign, witness_class_names);
    }
}

} // namespace scpp
