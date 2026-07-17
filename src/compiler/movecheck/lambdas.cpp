#include "lambdas.h"

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

#include "ast.h"
#include "mir.h"
#include "state.h"
#include "signatures.h"
#include "types.h"
#include "calls.h"
#include "borrows.h"
#include "dataflow.h"
#include "interfaces.h"
#include "threadsafety.h"
#include "generics_support.h"
#include "monomorphize.h"

namespace scpp {
// ch05 §5.12: checks every capture of a resolved Lambda literal --
// shared by apply_expr's own Lambda case (a *transient* use: an IIFE, a
// call argument, ... -- the closure literal itself can never outlive
// this statement, so `reference_capture_borrows` is a fresh, local,
// discarded-afterward map there) and apply_statement's class-typed
// Assign case (a closure literal being bound to a *named* `auto`
// variable, ch05 §5.12's only spelling for this -- which genuinely can
// outlive this statement, so that caller passes `state.borrows` itself,
// making any by-reference capture's borrow last for the rest of this
// function, exactly like an ordinary `T& r = x;` binding's own borrow
// would -- a deliberately conservative simplification: released only at
// function end rather than at the closure variable's own precise last
// use, since v0.1 has no liveness analysis for a class-typed local the
// way it already has for a plain reference/span, see
// compute_reference_liveness). Checked the same way passing each
// capture as an argument already would be (reusing existing machinery,
// zero new move/borrow logic beyond this per-capture dispatch):
//  - an init-capture's own expression is evaluated normally (e.g.
//    permitting std::move(p) for a move-only type).
//  - a by-value capture of a class type uses the same copy/move boundary
//    rules as any other class value construction site in the language:
//    a bare same-type lvalue copies only if the class is
//    copy-constructible, otherwise the source must be an rvalue (e.g.
//    `std::move(p)` in an init-capture).
//  - a by-reference capture is checked exactly like a reference-typed
//    call argument (apply_reference_argument): the closure's own field
//    genuinely borrows it, for as long as `reference_capture_borrows`
//    (see above) says it lasts.
void apply_lambda_captures(const Expr& expr, DataflowState& state, BorrowMap& reference_capture_borrows,
                            const Body& body, const Signatures& signatures, bool report_errors,
                            std::vector<ClosureCaptureBorrow>* out_closure_capture_borrows) {
    auto apply_by_value_capture_source = [&](const Expr& source, const Type& source_type,
                                             const std::string& capture_display) {
        if (is_named_class_type(source_type, body)) {
            bool is_copy_source = is_bare_same_type_copy_source(source, source_type, body, signatures);
            bool is_rvalue_source = produces_rvalue_of_type(source, source_type, body, signatures);
            if (report_errors) {
                if (is_copy_source) {
                    if (state.classes_with_copy_ctor == nullptr ||
                        !state.classes_with_copy_ctor->contains(source_type.name)) {
                        throw DataflowError(
                            "capture '" + capture_display + "' of class '" + source_type.name +
                               "' requires std::move or another rvalue source because the class is not "
                               "copy-constructible (spec §6.5/§5.12)",
                            state.current_loc);
                    }
                } else if (!is_rvalue_source) {
                    throw DataflowError(
                        "capture '" + capture_display + "' of class '" + source_type.name +
                            "' must use a plain same-typed variable (if copy-constructible) or an rvalue such as "
                            "std::move(...) (spec §6.5/§5.12)",
                        state.current_loc);
                }
            }
            apply_expr(source, /*is_move_target_context=*/is_rvalue_source, state, body, signatures, report_errors);
            return;
        }
        apply_expr(source, /*is_move_target_context=*/source.kind == ExprKind::Move, state, body, signatures,
                   report_errors);
    };
    for (const LambdaCapture& capture : expr.lambda_captures) {
        if (capture.init) {
            reject_lifetime_group_state_embedding(*capture.init, state, body, signatures, report_errors, "a closure capture");
            std::optional<Type> init_type = infer_expr_type(*capture.init, body, signatures);
            if (init_type.has_value()) {
                apply_by_value_capture_source(*capture.init, *init_type, capture.name);
            } else {
                apply_expr(*capture.init, /*is_move_target_context=*/true, state, body, signatures, report_errors);
            }
            continue;
        }
        if (!capture.by_reference) {
            auto type_it = body.local_types.find(capture.name);
            if (type_it != body.local_types.end()) {
                Expr capture_ident;
                capture_ident.kind = ExprKind::Identifier;
                capture_ident.loc = expr.loc;
                capture_ident.name = capture.name;
                reject_lifetime_group_state_embedding(capture_ident, state, body, signatures, report_errors,
                                                      "a closure capture");
                apply_by_value_capture_source(capture_ident, type_it->second, capture.name);
                continue;
            }
            LocalState current = lookup(state.locals, capture.name);
            if (report_errors && current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(capture.name, current), state.current_loc);
            }
            continue;
        }
        Expr capture_ident;
        capture_ident.kind = ExprKind::Identifier;
        capture_ident.loc = expr.loc;
        capture_ident.name = capture.name;
        reject_lifetime_group_state_embedding(capture_ident, state, body, signatures, report_errors, "a closure capture");
        RootSet roots =
            resolve_borrow_source_root(capture_ident, state, body, signatures, report_errors);
        Type ref_type;
        ref_type.kind = TypeKind::Reference;
        ref_type.is_mutable_ref = true; // matches resolve_lambda's own field choice
        apply_reference_argument(capture_ident, ref_type, state, reference_capture_borrows, body, signatures,
                                  report_errors);
        if (out_closure_capture_borrows != nullptr) {
            for (const std::string& root : roots) {
                out_closure_capture_borrows->push_back(ClosureCaptureBorrow{root, true});
            }
        }
    }
}
// ch05 §5.12: collects every VarDecl's own name inside `stmt`
// (recursively, ignoring lexical scope -- same "whole-body, flat"
// pragmatism as mir.cppm's own Body::local_types) into `out` -- used to
// exclude a lambda's own locally-declared variables from blanket-
// capture free-variable resolution (they're the lambda's own locals,
// never a capture).
void collect_locally_declared_names(const Stmt& stmt, std::unordered_set<std::string>& out) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
            out.insert(stmt.var_name);
            return;
        case StmtKind::If:
            collect_locally_declared_names(*stmt.then_branch, out);
            if (stmt.else_branch) collect_locally_declared_names(*stmt.else_branch, out);
            return;
        case StmtKind::While:
            collect_locally_declared_names(*stmt.then_branch, out);
            return;
        case StmtKind::Block:
            for (const StmtPtr& s : stmt.statements) collect_locally_declared_names(*s, out);
            return;
        default:
            return;
    }
}

// Forward declarations: collect_free_identifiers's Expr/Stmt overloads
// are mutually recursive (an If/While/Block statement recurses into its
// own sub-expressions/sub-statements; a Lambda expression recurses into
// its own body statement).
void collect_free_identifiers(const Expr& expr, const std::unordered_set<std::string>& excluded,
                                std::unordered_set<std::string>& out);
void collect_free_identifiers(const Stmt& stmt, const std::unordered_set<std::string>& excluded,
                                std::unordered_set<std::string>& out);

// ch05 §5.12: collects every free Identifier reference inside `expr`
// (skipping any name in `excluded` -- a lambda's own params/locals,
// already-explicit captures, known function names, known type names)
// into `out`, for a blanket `[=]`/`[&]` capture's own free-variable
// resolution. A Call's own callee name and a Member's own field name
// are never variable references (skipped entirely); their receiver/
// base sub-expressions are still walked normally. A nested lambda's own
// body is conservatively walked too (v1 simplification: over-
// approximates rather than precisely tracking what a nested lambda
// already captures itself -- harmless, since capturing an unused name
// is safe, just not maximally minimal; see this codebase's general
// "pragmatic over exhaustive" style elsewhere).
void collect_free_identifiers(const Expr& expr, const std::unordered_set<std::string>& excluded,
                                std::unordered_set<std::string>& out) {
    if (expr.kind == ExprKind::Identifier) {
        if (!excluded.contains(expr.name)) out.insert(expr.name);
        return;
    }
    if (expr.lhs) collect_free_identifiers(*expr.lhs, excluded, out);
    if (expr.rhs) collect_free_identifiers(*expr.rhs, excluded, out);
    if (expr.third) collect_free_identifiers(*expr.third, excluded, out);
    for (const ExprPtr& arg : expr.args) collect_free_identifiers(*arg, excluded, out);
    if (expr.kind == ExprKind::Lambda && expr.lambda_body) {
        collect_free_identifiers(*expr.lambda_body, excluded, out);
    }
}

void collect_free_identifiers(const Stmt& stmt, const std::unordered_set<std::string>& excluded,
                                std::unordered_set<std::string>& out) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
            if (stmt.init) collect_free_identifiers(*stmt.init, excluded, out);
            for (const ExprPtr& arg : stmt.ctor_args) collect_free_identifiers(*arg, excluded, out);
            return;
        case StmtKind::Return:
        case StmtKind::ExprStmt:
            if (stmt.expr) collect_free_identifiers(*stmt.expr, excluded, out);
            return;
        case StmtKind::If:
            collect_free_identifiers(*stmt.condition, excluded, out);
            collect_free_identifiers(*stmt.then_branch, excluded, out);
            if (stmt.else_branch) collect_free_identifiers(*stmt.else_branch, excluded, out);
            return;
        case StmtKind::While:
            collect_free_identifiers(*stmt.condition, excluded, out);
            collect_free_identifiers(*stmt.then_branch, excluded, out);
            return;
        case StmtKind::Break:
        case StmtKind::Continue:
            return;
        case StmtKind::Block:
            for (const StmtPtr& s : stmt.statements) collect_free_identifiers(*s, excluded, out);
            return;
    }
}

// ch05 §5.12: rewrites every bare Identifier reference to a captured
// name inside `expr` into an explicit `this.name` Member access.
// Necessary because a lambda's body, as originally written, refers to
// what were then ordinary enclosing-scope locals by their bare names --
// but scpp requires *explicit* `this.field` for a class's own fields
// (there is no implicit-field-lookup fallback the way real C++ allows
// a bare `field` inside a method body, verified empirically: this
// codebase's own movecheck rejects a bare field reference with "use of
// undeclared variable"). Once the lambda's body becomes the synthesized
// closure class's own "call" method, each captured name is a *field*,
// not a local, so every such reference must be rewritten this way. The
// lambda's own parameters and any of its own locally-declared variables
// are left as ordinary bare identifiers -- only names in
// `captured_names` are ever rewritten. A nested lambda's own body is
// conservatively rewritten too (matching collect_free_identifiers'
// identical reasoning above); this codebase does not attempt to prove a
// nested lambda's own parameter/local never shadows a captured name
// from this outer scope -- an accepted v1 limitation (not demonstrated
// by any documented example, and real C++ closure-capture-chain nesting
// is itself a well-known deep-end topic).
void rewrite_captured_identifiers_as_field_access(Stmt& stmt, const std::unordered_set<std::string>& captured_names);

void rewrite_captured_identifiers_as_field_access(Expr& expr, const std::unordered_set<std::string>& captured_names) {
    if (expr.kind == ExprKind::Identifier && captured_names.contains(expr.name)) {
        auto this_ref = std::make_unique<Expr>();
        this_ref->kind = ExprKind::Identifier;
        this_ref->loc = expr.loc;
        this_ref->name = "this";
        expr.kind = ExprKind::Member;
        expr.lhs = std::move(this_ref);
        // expr.name already holds the captured name -- Member's own
        // field-name slot, unchanged.
        return;
    }
    if (expr.lhs) rewrite_captured_identifiers_as_field_access(*expr.lhs, captured_names);
    if (expr.rhs) rewrite_captured_identifiers_as_field_access(*expr.rhs, captured_names);
    if (expr.third) rewrite_captured_identifiers_as_field_access(*expr.third, captured_names);
    for (ExprPtr& arg : expr.args) rewrite_captured_identifiers_as_field_access(*arg, captured_names);
    if (expr.kind == ExprKind::Lambda && expr.lambda_body) {
        rewrite_captured_identifiers_as_field_access(*expr.lambda_body, captured_names);
    }
}

void rewrite_captured_identifiers_as_field_access(Stmt& stmt, const std::unordered_set<std::string>& captured_names) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
            if (stmt.init) rewrite_captured_identifiers_as_field_access(*stmt.init, captured_names);
            for (ExprPtr& arg : stmt.ctor_args) rewrite_captured_identifiers_as_field_access(*arg, captured_names);
            return;
        case StmtKind::Return:
        case StmtKind::ExprStmt:
            if (stmt.expr) rewrite_captured_identifiers_as_field_access(*stmt.expr, captured_names);
            return;
        case StmtKind::If:
            rewrite_captured_identifiers_as_field_access(*stmt.condition, captured_names);
            rewrite_captured_identifiers_as_field_access(*stmt.then_branch, captured_names);
            if (stmt.else_branch) rewrite_captured_identifiers_as_field_access(*stmt.else_branch, captured_names);
            return;
        case StmtKind::While:
            rewrite_captured_identifiers_as_field_access(*stmt.condition, captured_names);
            rewrite_captured_identifiers_as_field_access(*stmt.then_branch, captured_names);
            return;
        case StmtKind::Break:
        case StmtKind::Continue:
            return;
        case StmtKind::Block:
            for (StmtPtr& s : stmt.statements) rewrite_captured_identifiers_as_field_access(*s, captured_names);
            return;
    }
}

// ch05 §5.12: "By default a closure's `operator()` is const (a by-value
// capture can't be reassigned inside the body)"; `mutable` opts out.
// Checked directly here -- on the *original*, pre field-access-rewrite
// body, where a captured name is still an ordinary bare Identifier, so
// no field-type information is needed -- rather than via the general
// const-`this`-propagation mechanism (assignment_target_is_read_only),
// which cannot by itself distinguish "writing to a by-value field"
// (should require `mutable`) from "writing *through* a by-reference
// field's own referent" (always allowed, regardless of `mutable` --
// matching real C++, where a reference member's constness is
// independent of its enclosing object's own) -- see resolve_lambda's
// own `this_type.is_mutable_ref` comment for why the "call" method's
// receiver is unconditionally mutable. Only ever called when the
// lambda is *not* `mutable` (see resolve_lambda). A nested lambda's own
// body is deliberately not recursed into: it has its own independent
// capture list, checked when *it* is itself resolved.
void reject_write_to_nonmutable_by_value_capture(const Expr& expr, const std::unordered_set<std::string>& by_value_names) {
    if (expr.kind == ExprKind::Binary && expr.binary_op == BinaryOp::Assign && expr.lhs->kind == ExprKind::Identifier &&
        by_value_names.contains(expr.lhs->name)) {
        throw DataflowError("cannot assign to by-value-captured '" + expr.lhs->name +
                                 "' inside a non-'mutable' lambda (ch05 §5.12 -- a closure's own call operator "
                                 "is 'const' by default; add 'mutable' to opt out)",
            expr.loc);
    }
    if (expr.lhs) reject_write_to_nonmutable_by_value_capture(*expr.lhs, by_value_names);
    if (expr.rhs) reject_write_to_nonmutable_by_value_capture(*expr.rhs, by_value_names);
    if (expr.third) reject_write_to_nonmutable_by_value_capture(*expr.third, by_value_names);
    for (const ExprPtr& arg : expr.args) reject_write_to_nonmutable_by_value_capture(*arg, by_value_names);
}

void reject_write_to_nonmutable_by_value_capture(const Stmt& stmt, const std::unordered_set<std::string>& by_value_names) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
            if (stmt.init) reject_write_to_nonmutable_by_value_capture(*stmt.init, by_value_names);
            for (const ExprPtr& arg : stmt.ctor_args) reject_write_to_nonmutable_by_value_capture(*arg, by_value_names);
            return;
        case StmtKind::Return:
        case StmtKind::ExprStmt:
            if (stmt.expr) reject_write_to_nonmutable_by_value_capture(*stmt.expr, by_value_names);
            return;
        case StmtKind::If:
            reject_write_to_nonmutable_by_value_capture(*stmt.condition, by_value_names);
            reject_write_to_nonmutable_by_value_capture(*stmt.then_branch, by_value_names);
            if (stmt.else_branch) reject_write_to_nonmutable_by_value_capture(*stmt.else_branch, by_value_names);
            return;
        case StmtKind::While:
            reject_write_to_nonmutable_by_value_capture(*stmt.condition, by_value_names);
            reject_write_to_nonmutable_by_value_capture(*stmt.then_branch, by_value_names);
            return;
        case StmtKind::Break:
        case StmtKind::Continue:
            return;
        case StmtKind::Block:
            for (const StmtPtr& s : stmt.statements) reject_write_to_nonmutable_by_value_capture(*s, by_value_names);
            return;
    }
}


// ch05 §5.12: implements call-site monomorphization for every generic
// (concept-constrained) function in a Program -- run once, before
// check_moves (see driver.cppm/monomorphize_generics below), so that by
// the time movecheck's ordinary exact-type-match call-argument checking
// runs, every call site targets an already-concrete, already-
// monomorphized Function (an ordinary function by then, needing zero
// special-casing) rather than the original generic template (whose own
// witness-typed signature would otherwise never structurally match a
// concrete argument). The original generic template itself is left
// completely untouched in `program.functions` -- it's still separately,
// abstractly checked by movecheck's ordinary per-function pass (its
// witness-typed `this`/parameters make that check exactly as
// intraprocedural as any other function's), just never itself reachable
// from a real call site anymore, and excluded from codegen entirely
// (Codegen::generate, keyed off Function::is_generic_template).
//
// Also resolves every closure literal (ch05 §5.12) in the same pass --
// both features need the same thing (concrete per-function type
// information the parser itself never has), so they share this single
// pre-check_moves walk rather than needing two separate passes over
// every function body.

} // namespace scpp
