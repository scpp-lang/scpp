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

module scpp.compiler.movecheck:borrows;

import scpp.ast;
import :errors;
import scpp.mir;
import :state;
import :types;
import :signatures;
import :calls;

namespace scpp {

using LiveSet = std::unordered_set<std::string>;

RootSet resolve_root_place(const std::string& name, const DataflowState& state);
std::optional<std::string> resolve_reborrow_lender(const Expr& expr, const Body& body,
                                                   const Signatures& signatures);
void validate_reborrow_lender(const std::string& lender, bool child_is_mutable, const DataflowState& state,
                              const Body& body, bool report_errors);
void validate_reborrow_lender_write(const std::string& lender, const DataflowState& state,
                                    bool report_errors);
void release_reference_borrow(const std::string& name, DataflowState& state, const Body& body);
void release_closure_capture_borrows(const std::string& name, DataflowState& state);
std::vector<size_t> successors(const Terminator& term);
void collect_reference_uses(const Expr* expr, const Body& body, LiveSet& out);
std::optional<std::string> reference_def(const MirStatement& stmt);
LiveSet reference_uses(const MirStatement& stmt, const Body& body);
LiveSet reference_uses(const Terminator& term, const Body& body);
std::vector<std::vector<LiveSet>> compute_reference_liveness(const Body& body,
                                                             const std::vector<std::vector<size_t>>& preds);
void release_dead_references(DataflowState& state, const Body& body, const LiveSet& live_after_stmt);

[[nodiscard]] RootSet resolve_borrow_source_root(const Expr& expr, DataflowState& state, const Body& body,
                                                 const Signatures& signatures, bool report_errors);
[[nodiscard]] RootSet resolve_lifetime_source_roots(const Expr& expr, DataflowState& state, const Body& body,
                                                    const Signatures& signatures, bool report_errors);
[[nodiscard]] std::optional<size_t> find_function_param_by_root(const Function& fn, const std::string& root);
[[nodiscard]] bool roots_satisfy_named_lifetime_group(const RootSet& roots, const Function& fn,
                                                      std::string_view group_name);
[[nodiscard]] bool roots_include_parameter_lifetime(const RootSet& roots, const DataflowState& state);
void reject_lifetime_group_state_embedding(const Expr& expr, DataflowState& state, const Body& body,
                                           const Signatures& signatures, bool report_errors,
                                           std::string_view context);
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures);
void validate_deref_operand(const Expr& operand, const DataflowState& state, const Body& body,
                            const Signatures& signatures);
void apply_address_of(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                      bool report_errors);

[[nodiscard]] std::optional<std::string> direct_write_root(const Expr& expr, const Body& body) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            if (it != body.local_types.end() && (is_reference(it->second) || is_span(it->second))) {
                return std::nullopt;
            }
            return expr.name;
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
            return direct_write_root(*expr.lhs, body);
        case ExprKind::Unary:
            if (is_explicit_star_this(expr)) return "this";
            if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) {
                return std::nullopt;
            }
            return expr.lhs->name;
        case ExprKind::Call:
            if (expr.name == "operator_deref" && expr.lhs != nullptr) {
                if (expr.lhs->kind == ExprKind::Identifier) return expr.lhs->name;
                if (expr.lhs->kind == ExprKind::Member && expr.lhs->lhs) {
                    return direct_write_root(*expr.lhs->lhs, body);
                }
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}
RootSet resolve_root_place(const std::string& name, const DataflowState& state) {
    auto it = state.ref_targets.find(name);
    return it == state.ref_targets.end() ? single_root(name) : it->second.roots;
}

std::optional<std::string> resolve_reborrow_lender(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            if (it != body.local_types.end() && is_reborrowable_local_type(it->second)) return expr.name;
            return std::nullopt;
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
        case ExprKind::Cast:
            return expr.lhs ? resolve_reborrow_lender(*expr.lhs, body, signatures) : std::nullopt;
        case ExprKind::Unary:
            return expr.lhs ? resolve_reborrow_lender(*expr.lhs, body, signatures) : std::nullopt;
        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            bool returns_reference = sig != nullptr && !sig->returned_lifetime_param_indices.empty();
            if (!returns_reference) return std::nullopt;
            if (sig->returned_lifetime_param_indices.size() != 1) return std::nullopt;
            size_t source_index = sig->returned_lifetime_param_indices.front();
            if (expr.name == "operator_deref" && expr.lhs != nullptr && source_index < callee.param_offset) {
                return resolve_reborrow_lender(*expr.lhs, body, signatures);
            }
            if (source_index < callee.param_offset) {
                return expr.lhs ? resolve_reborrow_lender(*expr.lhs, body, signatures) : std::nullopt;
            }
            return resolve_reborrow_lender(*expr.args[source_index - callee.param_offset], body, signatures);
        }
        default:
            return std::nullopt;
    }
}

void validate_reborrow_lender(const std::string& lender, bool child_is_mutable, const DataflowState& state,
                              const Body& body, bool report_errors) {
    if (!report_errors) return;
    const Type& lender_type = body.local_types.at(lender);
    if (child_is_mutable && !lender_type.is_mutable_ref) {
        throw DataflowError("cannot reborrow '" + lender + "' as mutable: it is itself only a shared (const) "
                            "reference/view",
            state.current_loc);
    }
    if (local_is_suspended_for_reborrow(lender, state)) {
        throw DataflowError("cannot form another reborrow from '" + lender +
                                 "' while a nested reborrow derived from it is still live",
            state.current_loc);
    }
}

void validate_reborrow_lender_write(const std::string& lender, const DataflowState& state, bool report_errors) {
    if (!report_errors) return;
    if (local_is_suspended_for_reborrow(lender, state)) {
        throw DataflowError("cannot write through '" + lender +
                                 "' while a nested reborrow derived from it is still live",
            state.current_loc);
    }
}

// Releases the borrow (if any) that reference-typed local `name` holds
// against its root, and forgets that `name` is a currently-bound
// reference. A no-op if `name` isn't (or is no longer) tracked in
// `ref_targets`, so it's safe to call speculatively.
//
// Called from two places (see check_function): as soon as the liveness
// analysis says `name` is no longer live (right after its last use --
// the NLL upgrade from spec ch05.3), and as a fallback at `name`'s
// lexical ScopeExit, for the unusual case of a reference that's never
// read after being bound at all (liveness alone would have released it
// immediately after its BindReference, before ScopeExit is even
// reached). Whichever fires first does the actual work; the other is
// then a harmless no-op, since both leave the exact same state.
void release_reference_borrow(const std::string& name, DataflowState& state, const Body& body) {
    auto ref_it = state.ref_targets.find(name);
    if (ref_it == state.ref_targets.end()) return;
    RefTarget target = ref_it->second;
    if (target.is_reborrow()) {
        auto suspension_it = state.suspended_reborrows.find(target.lender);
        if (suspension_it != state.suspended_reborrows.end()) {
            if (suspension_it->second > 1) {
                suspension_it->second--;
            } else {
                state.suspended_reborrows.erase(suspension_it);
            }
        }
    } else {
        for (const std::string& root : target.roots) {
            auto borrow_it = state.borrows.find(root);
            if (borrow_it != state.borrows.end()) {
                if (body.local_types.at(name).is_mutable_ref) {
                    borrow_it->second.mutable_borrow = false;
                } else if (borrow_it->second.shared_count > 0) {
                    borrow_it->second.shared_count--;
                }
                if (!borrow_it->second.mutable_borrow && borrow_it->second.shared_count == 0) {
                    state.borrows.erase(borrow_it);
                }
            }
        }
    }
    state.ref_targets.erase(ref_it);
}

void release_closure_capture_borrows(const std::string& name, DataflowState& state) {
    auto closure_it = state.closure_capture_borrows.find(name);
    if (closure_it == state.closure_capture_borrows.end()) return;
    for (const ClosureCaptureBorrow& capture_borrow : closure_it->second) {
        auto borrow_it = state.borrows.find(capture_borrow.root);
        if (borrow_it == state.borrows.end()) continue;
        if (capture_borrow.is_mutable) {
            borrow_it->second.mutable_borrow = false;
        } else if (borrow_it->second.shared_count > 0) {
            borrow_it->second.shared_count--;
        }
        if (!borrow_it->second.mutable_borrow && borrow_it->second.shared_count == 0) {
            state.borrows.erase(borrow_it);
        }
    }
    state.closure_capture_borrows.erase(closure_it);
}

std::vector<size_t> successors(const Terminator& term) {
    switch (term.kind) {
        case TerminatorKind::Goto: return {term.target};
        case TerminatorKind::Branch: return {term.true_target, term.false_target};
        case TerminatorKind::Return:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
        default: return {};
    }
}

using LiveSet = std::unordered_set<std::string>;

// Collects the name of every currently-declared *reference-or-span*-typed
// local mentioned anywhere in `expr` (recursively) into `out`. Used by
// the liveness analysis below to find where a reference/span is "used"
// (in the sense of needing its current borrow to stay valid), without
// having to duplicate apply_expr's exact per-case dataflow semantics:
// this walk is deliberately a plain, generic tree traversal,
// over-inclusive rather than clever. A spurious extra "use" only makes a
// borrow's computed live range *longer* than strictly necessary (a
// missed early-release opportunity, but still sound); missing a real one
// would instead be an actual bug (releasing a borrow while it's still
// genuinely needed) -- which is exactly why std::span must be included
// here alongside Reference: without it, a span's borrow would look dead
// (and be released) immediately after its own BindReference, since
// nothing would ever record it as "live", regardless of how long it's
// actually used for afterward.
void collect_reference_uses(const Expr* expr, const Body& body, LiveSet& out) {
    if (expr == nullptr) return;
    switch (expr->kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
        case ExprKind::Sizeof:
            return;
        case ExprKind::New:
            if (expr->lhs) collect_reference_uses(expr->lhs.get(), body, out);
            for (const auto& arg : expr->args) collect_reference_uses(arg.get(), body, out);
            return;
        case ExprKind::Delete:
        case ExprKind::Destroy:
        case ExprKind::PackExpansion:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr->name);
            if (it != body.local_types.end() &&
                (is_reference(it->second) || is_span(it->second) ||
                 body.borrow_holding_closure_locals.contains(expr->name))) {
                out.insert(expr->name);
            }
            return;
        }
        case ExprKind::Binary:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            return;
        case ExprKind::Conditional:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            collect_reference_uses(expr->third.get(), body, out);
            return;
        case ExprKind::Unary:
        case ExprKind::Move:
        case ExprKind::Cast:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Call:
            if (expr->lhs != nullptr) {
                collect_reference_uses(expr->lhs.get(), body, out);
            } else {
                auto it = body.local_types.find(expr->name);
                if (it != body.local_types.end() &&
                    (is_reference(it->second) || is_span(it->second) ||
                     body.borrow_holding_closure_locals.contains(expr->name))) {
                    out.insert(expr->name);
                }
            }
            for (const auto& arg : expr->args) {
                collect_reference_uses(arg.get(), body, out);
            }
            return;
        case ExprKind::Fold:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            return;
        case ExprKind::Member:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Subscript:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            return;
        case ExprKind::Lambda:
            // ch05 §5.12: a plain (non-init) capture reads whatever
            // local already exists under that name in the enclosing
            // scope -- if that local is itself reference/span-typed,
            // this is a genuine "use" of it, exactly like an ordinary
            // Identifier reference (mirrors the Identifier case above).
            // An init-capture's own expression is walked normally
            // instead (it may itself reference an existing reference-
            // typed local, e.g. `[r = some_ref]`).
            for (const LambdaCapture& capture : expr->lambda_captures) {
                if (capture.init) {
                    collect_reference_uses(capture.init.get(), body, out);
                    continue;
                }
                auto it = body.local_types.find(capture.name);
                if (it != body.local_types.end() && (is_reference(it->second) || is_span(it->second))) {
                    out.insert(capture.name);
                }
            }
            return;
    }
}

// The reference-or-span-typed local that `stmt` freshly brings into
// existence, if any -- only a BindReference does (neither a reference
// nor a span is rebound in this version, so this is also the one and
// only point before which `stmt.local`'s liveness must not extend
// backward; see compute_reference_liveness). Purely keyed off the MIR
// statement kind here, not the local's own type, since mir.cppm already
// emits BindReference for both.
std::optional<std::string> reference_def(const MirStatement& stmt) {
    if (stmt.kind == MirStatementKind::BindReference) return stmt.local;
    return std::nullopt;
}

LiveSet reference_uses(const MirStatement& stmt, const Body& body) {
    LiveSet uses;
    switch (stmt.kind) {
        case MirStatementKind::BindReference:
        case MirStatementKind::Eval:
            collect_reference_uses(stmt.expr, body, uses);
            return uses;
        case MirStatementKind::Assign: {
            collect_reference_uses(stmt.expr, body, uses);
            auto type_it = body.local_types.find(stmt.local);
            if (type_it != body.local_types.end() && is_reference(type_it->second)) {
                // A write-through (`r = expr;` where `r` is itself a
                // reference -- see apply_reference_write_through) reads
                // r's own stored address to know where to write, even
                // though it never reads *through* it.
                uses.insert(stmt.local);
            }
            return uses;
        }
        case MirStatementKind::Declare:
        case MirStatementKind::Drop:
        case MirStatementKind::ScopeExit:
        case MirStatementKind::UnsafeEnter:
        case MirStatementKind::UnsafeExit:
            return uses;
    }
    return uses;
}

LiveSet reference_uses(const Terminator& term, const Body& body) {
    LiveSet uses;
    switch (term.kind) {
        case TerminatorKind::Branch:
            collect_reference_uses(term.condition, body, uses);
            return uses;
        case TerminatorKind::Return:
            collect_reference_uses(term.return_value, body, uses);
            return uses;
        case TerminatorKind::Goto:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
            return uses;
    }
    return uses;
}

// Computes, for every statement in `body`, the set of reference-typed
// locals that are *live* (may still be used on some path forward from
// here) immediately after that statement executes -- the backward dual
// of the forward move/borrow dataflow in check_function, and what makes
// this milestone's borrow release NLL-style (spec ch05.3) rather than
// only lexically-scoped (pre-NLL Rust's original, more conservative
// behavior, which is all M4 had).
//
// Standard backward liveness equations, solved to a fixed point over
// the CFG (a single backward pass isn't enough whenever the CFG has a
// loop -- see the `while` case below):
//   live-out(block) = union of live-in(successor) for every successor
//   live-in(block)  = (live-out(block) - defs(block)) + uses(block)
// Verified by hand for a reference declared *and* used entirely inside
// a `while` body: its own BindReference's `defs` kill reverses the
// `uses` gen from later in the *same* iteration before the walk ever
// reaches the block's own entry, so it never appears live going into
// the loop from the back edge (i.e. as if demanded by a *previous*
// iteration) -- exactly as it shouldn't.
//
// `live_after[b][i]` is the live-out set immediately after statement i
// of block b (i.e. live-in to statement i+1, or to the terminator if i
// is the last statement) -- exactly the set check_function needs right
// after applying statement i to decide whether a currently-tracked
// reference has just become dead and should have its borrow released.
std::vector<std::vector<LiveSet>> compute_reference_liveness(const Body& body,
                                                              const std::vector<std::vector<size_t>>& preds) {
    size_t n = body.blocks.size();
    std::vector<LiveSet> block_live_in(n);

    auto block_live_out = [&](size_t b) {
        LiveSet live;
        for (size_t succ : successors(body.blocks[b].terminator)) {
            live.insert(block_live_in[succ].begin(), block_live_in[succ].end());
        }
        return live;
    };

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

        LiveSet live = block_live_out(b);
        const BasicBlock& block = body.blocks[b];
        for (const std::string& use : reference_uses(block.terminator, body)) {
            live.insert(use);
        }
        for (auto it = block.statements.rbegin(); it != block.statements.rend(); ++it) {
            if (std::optional<std::string> def = reference_def(*it)) live.erase(*def);
            for (const std::string& use : reference_uses(*it, body)) live.insert(use);
        }

        if (live != block_live_in[b]) {
            block_live_in[b] = std::move(live);
            for (size_t p : preds[b]) {
                if (!queued[p]) {
                    worklist.push_back(p);
                    queued[p] = true;
                }
            }
        }
    }

    // Fixed point reached (`block_live_in` is now stable): replay every
    // block once more, this time also recording the live-out-after-each-
    // statement snapshot the forward pass needs.
    std::vector<std::vector<LiveSet>> live_after(n);
    for (size_t b = 0; b < n; b++) {
        const BasicBlock& block = body.blocks[b];
        LiveSet live = block_live_out(b);
        for (const std::string& use : reference_uses(block.terminator, body)) {
            live.insert(use);
        }
        live_after[b].resize(block.statements.size());
        for (size_t i = block.statements.size(); i-- > 0;) {
            live_after[b][i] = live;
            if (std::optional<std::string> def = reference_def(block.statements[i])) live.erase(*def);
            for (const std::string& use : reference_uses(block.statements[i], body)) live.insert(use);
        }
    }
    return live_after;
}

// After executing statement index `i` of a block (whose precomputed
// live-out set is `live_after_stmt`), releases the borrow of every
// currently-tracked reference that's no longer live -- i.e. whose last
// use was this statement or earlier. Collects names to release first
// rather than erasing while iterating `state.ref_targets` directly.
void release_dead_references(DataflowState& state, const Body& body, const LiveSet& live_after_stmt) {
    std::vector<std::string> dead;
    for (const auto& [name, root] : state.ref_targets) {
        if (!live_after_stmt.contains(name)) dead.push_back(name);
    }
    for (const std::string& name : dead) {
        release_reference_borrow(name, state, body);
    }
    dead.clear();
    for (const auto& [name, borrows] : state.closure_capture_borrows) {
        if (!live_after_stmt.contains(name)) dead.push_back(name);
    }
    for (const std::string& name : dead) {
        release_closure_capture_borrows(name, state);
    }
}

void apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
                 const Signatures& signatures, bool report_errors);

// Checks every argument of a Call expression against its callee's
// signature (if known), exactly the same way regardless of context --
// shared by apply_expr's own Call case (a call used as a plain
// statement or value sub-expression) and resolve_borrow_source_root's
// Call case below (a call to a reference-returning function used
// itself as a further reference-binding source).
void check_call_arguments(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                           bool report_errors);

// Resolves the root place that `expr` would be borrowing from if used as
// a reference-binding (`T& r = expr;`) or reference-argument (`f(expr)`
// where the parameter is a reference) source. Supports a plain local
// variable, a chain of `.field`/`[index]` projections off one --
// root-resolved to the *outermost* variable in the chain (see the
// whole-root conservatism note below) -- or a call to a function that
// itself returns a reference, resolved transitively through its own
// elided parameter (spec ch05.3), so a chain of reference-returning
// calls is followed all the way back to a real place.
//
// v0.1 deliberately does *not* do field-sensitive aliasing: borrowing
// `a.x` and `a.y` are both recorded against the *same* root `a`, so they
// conflict with each other exactly as if both borrowed the whole of `a`,
// even though the two fields never actually overlap in memory (spec
// ch05.2). This mirrors how Rust itself treats a dynamically-indexed
// array/slice element (`arr[i]`/`arr[j]` conflict there too, absent an
// explicit split API scpp doesn't have yet) and applies it uniformly to
// struct fields as well, for simplicity. Two workarounds exist for a
// genuinely-disjoint-fields use case: pass each field as its own,
// separate call argument (each such borrow begins and ends within its
// own call -- see apply_reference_argument below -- so sequential calls
// never overlap), or keep the two named reference locals' own live
// ranges (shortened by the liveness analysis below) from overlapping.
[[nodiscard]] RootSet resolve_borrow_source_root(const Expr& expr, DataflowState& state, const Body& body,
                                                 const Signatures& signatures, bool report_errors) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            const std::string& bound_name = expr.name;
            if (report_errors) {
                LocalState current = lookup(state.locals, bound_name);
                if (current != LocalState::Initialized) {
                    throw DataflowError(describe_bad_state(bound_name, current),
                        state.current_loc);
                }
            }

            return resolve_root_place(bound_name, state);
        }

        case ExprKind::Member:
            // Whole-root conservative (see above): a field projection
            // resolves to the same root as its own base.
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);

        case ExprKind::Subscript:
            // The index is a genuine value-producing sub-expression (it
            // could itself read/move/call), so it's checked exactly like
            // any other read; the array base contributes the (whole-)
            // root, same as Member above.
            apply_expr(*expr.rhs, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            if (expr.lhs->kind == ExprKind::Identifier) {
                auto it = body.local_types.find(expr.lhs->name);
                if (it != body.local_types.end()) {
                    const Type& local_type = it->second.kind == TypeKind::Reference && it->second.pointee != nullptr
                                                 ? *it->second.pointee
                                                 : it->second;
                    if (local_type.kind == TypeKind::Span) return single_root(expr.lhs->name);
                }
            }
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);

        case ExprKind::Unary: {
            // `*p`/`p->x` (a raw pointer local here, or a class
            // `operator*` call rewritten elsewhere; see
            // validate_deref_operand): the root is `p` itself, *not* p's
            // pointee, so that moving or reassigning `p` while a
            // reference into `*p` is alive is rejected by the exact same
            // borrow-conflict checks that already guard every other root
            // (apply_statement's Assign case, and the Move case's own
            // borrow check above) -- freeing or reassigning p's
            // allocation out from under a live reference would otherwise
            // be a use-after-free.
            if (is_explicit_star_this(expr)) return single_root("this");
            if (expr.unary_op == UnaryOp::AddressOf) {
                return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
            }
            if (expr.unary_op != UnaryOp::Deref) {
                if (report_errors) {
                    throw DataflowError("a reference can currently only borrow a plain local variable, a "
                                         "field of one ('a.b'), an array element of one ('arr[i]'), a "
                                         "dereferenced raw-pointer local ('*p'/'p->x'), or "
                                         "the result of a call to a reference-returning function -- not an "
                                         "arbitrary expression",
                        state.current_loc);
                }
                return {};
            }
            if (report_errors) validate_deref_operand(*expr.lhs, state, body, signatures);
            if (expr.lhs->kind == ExprKind::Identifier) return resolve_root_place(expr.lhs->name, state);
            if (expr.lhs->kind == ExprKind::Member && expr.lhs->lhs) {
                return resolve_borrow_source_root(*expr.lhs->lhs, state, body, signatures, report_errors);
            }
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
        }

        case ExprKind::Cast:
            // A cast changes only the static view of the same underlying
            // storage/root place. This is specifically needed for manual-
            // lifetime patterns like `(T*)&slot`, where the cast itself
            // doesn't manufacture a fresh borrow source; the root still
            // comes from the operand (`&slot`, or a field thereof).
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);

        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            bool returns_reference = sig != nullptr && !sig->returned_lifetime_param_indices.empty();
            if (!returns_reference) {
                if (report_errors) {
                    throw DataflowError("cannot borrow the result of calling '" + expr.name +
                                         "': it doesn't return a reference with an inferrable lifetime (spec "
                                         "ch05.3)",
                        state.current_loc);
                }
                // Still check the arguments themselves so a genuinely
                // invalid call (wrong callee, bad arguments) is still
                // reported through the ordinary path once report_errors
                // is true; harmless to also run silently here.
                check_call_arguments(expr, state, body, signatures, report_errors);
                return {};
            }
            check_call_arguments(expr, state, body, signatures, report_errors);
            RootSet roots;
            for (size_t source_index : sig->returned_lifetime_param_indices) {
                if (expr.name == "operator_deref" && expr.lhs != nullptr && source_index < callee.param_offset) {
                    if (expr.lhs->kind == ExprKind::Identifier) {
                        roots = union_roots(std::move(roots), resolve_root_place(expr.lhs->name, state));
                    } else if (expr.lhs->kind == ExprKind::Member && expr.lhs->lhs) {
                        roots = union_roots(std::move(roots),
                                            resolve_borrow_source_root(*expr.lhs->lhs, state, body, signatures,
                                                                       report_errors));
                    } else {
                        roots = union_roots(std::move(roots),
                                            resolve_borrow_source_root(*expr.lhs, state, body, signatures,
                                                                       report_errors));
                    }
                    continue;
                }
                if (source_index < callee.param_offset) {
                    roots = union_roots(std::move(roots),
                                        resolve_borrow_source_root(*expr.lhs, state, body, signatures,
                                                                   report_errors));
                    continue;
                }
                roots = union_roots(std::move(roots),
                                    resolve_borrow_source_root(*expr.args[source_index - callee.param_offset], state,
                                                               body, signatures, report_errors));
            }
            return roots;
        }

        default:
            if (report_errors) {
                throw DataflowError("a reference can currently only borrow a plain local variable, a field of "
                                     "one ('a.b'), an array element of one ('arr[i]'), a dereferenced "
                                     "raw-pointer local ('*p'/'p->x'), or the result of a call "
                                     "to a reference-returning function -- not an arbitrary expression",
                    state.current_loc);
            }
            return {};
    }
}

[[nodiscard]] RootSet resolve_lifetime_source_roots(const Expr& expr, DataflowState& state, const Body& body,
                                                    const Signatures& signatures, bool report_errors) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto local_it = state.local_lifetime_sources.find(expr.name);
            if (local_it != state.local_lifetime_sources.end()) return local_it->second;
            return single_root(expr.name);
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
        case ExprKind::Cast:
            return expr.lhs ? resolve_lifetime_source_roots(*expr.lhs, state, body, signatures, report_errors) : RootSet{};
        case ExprKind::Unary:
            return expr.lhs ? resolve_lifetime_source_roots(*expr.lhs, state, body, signatures, report_errors) : RootSet{};
        case ExprKind::Conditional: {
            RootSet roots = resolve_lifetime_source_roots(*expr.rhs, state, body, signatures, report_errors);
            return union_roots(std::move(roots),
                               resolve_lifetime_source_roots(*expr.third, state, body, signatures, report_errors));
        }
        case ExprKind::Move:
            return expr.lhs ? resolve_lifetime_source_roots(*expr.lhs, state, body, signatures, report_errors) : RootSet{};
        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig == nullptr || sig->returned_lifetime_param_indices.empty()) return {};
            RootSet roots;
            for (size_t source_index : sig->returned_lifetime_param_indices) {
                if (source_index < callee.param_offset) {
                    if (expr.lhs) {
                        roots = union_roots(std::move(roots),
                                            resolve_lifetime_source_roots(*expr.lhs, state, body, signatures,
                                                                          report_errors));
                    }
                    continue;
                }
                roots = union_roots(std::move(roots),
                                    resolve_lifetime_source_roots(*expr.args[source_index - callee.param_offset], state,
                                                                  body, signatures, report_errors));
            }
            return roots;
        }
        default:
            return {};
    }
}

[[nodiscard]] std::optional<size_t> find_function_param_by_root(const Function& fn, const std::string& root) {
    for (size_t i = 0; i < fn.params.size(); i++) {
        if (fn.params[i].name == root) return i;
    }
    return std::nullopt;
}

[[nodiscard]] bool roots_satisfy_named_lifetime_group(const RootSet& roots, const Function& fn,
                                                      std::string_view group_name) {
    if (roots.empty()) return false;
    for (const std::string& root : roots) {
        std::optional<size_t> param_index = find_function_param_by_root(fn, root);
        if (!param_index.has_value()) return false;
        if (fn.params[*param_index].lifetime.name != group_name) return false;
    }
    return true;
}

[[nodiscard]] bool roots_include_parameter_lifetime(const RootSet& roots, const DataflowState& state) {
    for (const std::string& root : roots) {
        auto it = state.parameter_lifetimes.find(root);
        if (it != state.parameter_lifetimes.end() && it->second.present()) return true;
    }
    return false;
}

void reject_lifetime_group_state_embedding(const Expr& expr, DataflowState& state, const Body& body,
                                           const Signatures& signatures, bool report_errors, std::string_view context) {
    if (!report_errors) return;
    std::optional<Type> expr_type = infer_expr_type(expr, body, signatures);
    if (!expr_type.has_value() || !is_lifetime_eligible_type(*expr_type)) return;
    RootSet roots = resolve_lifetime_source_roots(expr, state, body, signatures, report_errors);
    if (!roots_include_parameter_lifetime(roots, state)) return;
    throw DataflowError("cannot store a reference, pointer, or span derived from " + format_roots(roots) +
                            " into " + std::string(context) +
                            "; named and generic lifetime groups propagate only through the direct bare return value",
                        state.current_loc);
}

// Determines whether `expr` (a borrow-source place -- the same shape
// resolve_borrow_source_root accepts) is only reachable *read-only*,
// i.e. whether obtaining a *mutable* `T&`/`T*` from it must be rejected.
// This is the "projection chain's const-reachability" resolve_borrow_
// source_root's own callers need but that function alone doesn't answer
// (it only resolves *which root* to check for borrow conflicts, not
// whether the path to it crossed a read-only step) -- used to reject
// binding a `T&` (apply_reference_binding) or passing a `T&` call
// argument (apply_reference_argument) through a `const T&`/`std::span
// <const T>`/`const T*` anywhere along the chain, and to decide whether
// `&expr` (ch05 §5.7) may produce a mutable `T*` or only a `const T*`.
// Once a chain crosses a read-only step, everything beyond it stays
// read-only: a struct field/array element can never itself be a
// reference or span (ch04.1), so there's no way for a later `.field`/
// `[index]` step to "regain" mutability -- only a chain that never
// crosses one at all is mutable.
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            if (it == body.local_types.end()) return false; // unknown name: left to codegen's own check
            if (body.const_locals.contains(expr.name)) return true;
            if (is_reference(it->second) || is_span(it->second)) {
                return !it->second.is_mutable_ref;
            }
            return false; // an owned local (or a by-value parameter) is fully mutable to its owner
        }

        case ExprKind::Member:
        case ExprKind::Subscript:
            return is_read_only_reachable(*expr.lhs, body, signatures);

        case ExprKind::Unary: {
            // `*p`/`p->x`: read-only iff `p` (necessarily a plain
            // Identifier -- see resolve_borrow_source_root's own Unary
            // case) is itself a `const T*` (is_mutable_pointee == false).
            // Class `operator*` rewrites that yield a read-only view are
            // handled by the Call case just below, via the method's own
            // declared return type.
            if (is_explicit_star_this(expr)) return is_read_only_reachable(*expr.lhs, body, signatures);
            if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) return false;
            auto it = body.local_types.find(expr.lhs->name);
            if (it == body.local_types.end() || it->second.kind != TypeKind::Pointer) return false;
            return !it->second.is_mutable_pointee;
        }

        case ExprKind::Call: {
            // The call's *own* declared return type is authoritative --
            // it doesn't matter whether the elided argument behind it was
            // itself mutable or read-only-reachable: a signature that
            // promises `const T&` back hands back a read-only view
            // regardless (exactly like a plain `const T&`-typed
            // Identifier above), and a signature promising `T&` could
            // only have been called successfully with a mutable-
            // reachable argument in the first place (apply_reference_
            // argument already enforces that at the call site).
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig == nullptr) return false;
            return !sig->return_type.is_mutable_ref;
        }

        default:
            return false;
    }
}

// Handles `&expr` (UnaryOp::AddressOf, ch05 §5.7) used as a plain value.
// Reuses resolve_borrow_source_root to resolve/validate `expr`'s root --
// exactly the same structural resolution (and the same nested side
// effects, e.g. a Subscript index's own apply_expr walk) a `T&`/
// `const T&` binding already goes through (apply_reference_binding) --
// but, unlike that function, registers no lasting borrow afterward: the
// produced `T*` is never move/borrow-tracked (ch05.2 is unchanged by
// this addition), so there's nothing left to later release, and an
// ordinary `T&`/`const T&` borrow of the same place immediately
// afterward is unaffected. Checked only at this instant, conservatively
// (the resulting pointer's eventual use -- read or write -- can't be
// known here): the root must have no existing borrow at all, shared or
// mutable -- the same exclusivity a *new* `T&` binding would require,
// rejected the same way taking a second one would be.
void apply_address_of(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                       bool report_errors) {
    if (expr.lhs->kind == ExprKind::Identifier && !body.local_types.contains(expr.lhs->name) &&
        signatures.contains(expr.lhs->name)) {
        return;
    }
    RootSet roots = resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
    if (!report_errors || roots.empty()) return;
    for (const std::string& root : roots) {
        auto borrow_it = state.borrows.find(root);
        if (borrow_it != state.borrows.end() &&
            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
            throw DataflowError("cannot take the address of '" + root + "': it is already borrowed",
                                state.current_loc);
        }
    }
}

} // namespace scpp
