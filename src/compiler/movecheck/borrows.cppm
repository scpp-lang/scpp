module;

module scpp.compiler.movecheck:borrows;

import std;
import scpp.ast;
import :errors;
import scpp.mir;
import :state;
import :types;
import :signatures;
import :calls;

namespace scpp {

using LiveSet = std::unordered_set<LocalId>;

RootSet resolve_root_place(LocalId local, const DataflowState& state);
std::optional<LocalId> resolve_reborrow_lender(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool reborrow_is_tracked_against_lender(const std::optional<LocalId>& lender, const Body& body);
[[nodiscard]] std::expected<void, DataflowError> validate_reborrow_lender(LocalId lender, bool child_is_mutable,
                              const DataflowState& state, const Body& body, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> validate_reborrow_lender_write(LocalId lender, const DataflowState& state,
                                    const Body& body, bool report_errors);
void release_reference_borrow(LocalId local, DataflowState& state, const Body& body);
void release_closure_capture_borrows(LocalId local, DataflowState& state);
std::vector<std::size_t> successors(const Terminator& term);
void collect_reference_use(const Expr& expr, const Body& body, LiveSet& out);
void collect_reference_uses(const Expr* expr, const Body& body, LiveSet& out);
std::optional<LocalId> reference_def(const MirStatement& stmt);
LiveSet reference_uses(const MirStatement& stmt, const Body& body);
LiveSet reference_uses(const Terminator& term, const Body& body);
std::vector<std::vector<LiveSet>> compute_reference_liveness(const Body& body,
                                                             const std::vector<std::vector<std::size_t>>& preds);
void release_dead_references(DataflowState& state, const Body& body, const LiveSet& live_after_stmt);

[[nodiscard]] std::expected<RootSet, DataflowError> resolve_borrow_source_root(const Expr& expr, DataflowState& state, const Body& body,
                                                 const Signatures& signatures, bool report_errors);
[[nodiscard]] RootSet resolve_lifetime_source_roots(const Expr& expr, DataflowState& state, const Body& body,
                                                    const Signatures& signatures, bool report_errors);
[[nodiscard]] std::optional<std::size_t> find_function_param_by_root(const Function& fn, const std::string& root);
[[nodiscard]] bool roots_satisfy_named_lifetime_group(const RootSet& roots, const Function& fn,
                                                      std::string_view group_name);
[[nodiscard]] bool roots_include_parameter_lifetime(const RootSet& roots, const DataflowState& state);
[[nodiscard]] std::expected<void, DataflowError> reject_lifetime_group_state_embedding(const Expr& expr, DataflowState& state, const Body& body,
                                           const Signatures& signatures, bool report_errors,
                                           std::string_view context,
                                           const Type* destination_type = nullptr);
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool place_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> validate_deref_expr(const Expr& expr, const DataflowState& state, const Body& body,
                         const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> validate_subscript_expr(const Expr& expr, const DataflowState& state, const Body& body,
                             const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> apply_address_of(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                      bool report_errors);

[[nodiscard]] bool is_single_arg_lifetime_wrapper_call(const Expr& expr) {
    return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.args.size() == 1 &&
           (expr.name == "std::reference_wrapper" || expr.name == "reference_wrapper" ||
            expr.name == "std::optional" || expr.name == "optional");
}

[[nodiscard]] bool is_zero_arg_lifetime_wrapper_call(const Expr& expr, const Body& body,
                                                     const Signatures& signatures) {
    if (expr.kind != ExprKind::Call || expr.lhs != nullptr || !expr.args.empty()) return false;
    std::optional<Type> expr_type = infer_expr_type(expr, body, signatures);
    return expr_type.has_value() && expr_type->is_reference_wrapper_lifetime_source;
}

[[nodiscard]] bool is_bare_reference_wrapper_constructor_call(const Expr& expr, const Body& body,
                                                             const Signatures& signatures) {
    if (expr.kind != ExprKind::Call || expr.lhs != nullptr || expr.args.size() != 1) return false;
    std::optional<Type> expr_type = infer_expr_type(expr, body, signatures);
    return expr_type.has_value() && expr_type->is_reference_wrapper_lifetime_source &&
           expr_type->name.contains("reference_wrapper");
}

[[nodiscard]] bool expr_is_wrapper_lifetime_source_form(const Expr& expr, const Body& body,
                                                        const Signatures& signatures) {
    std::optional<Type> expr_type = infer_expr_type(expr, body, signatures);
    if (expr_type.has_value() && expr_type->is_reference_wrapper_lifetime_source) return true;
    if (expr.kind != ExprKind::Identifier) return false;
    const Type* type = body.type_if_local(expr);
    return type != nullptr && type->is_reference_wrapper_lifetime_source;
}

[[nodiscard]] bool expr_contains_wrapper_lifetime_source_form(const Expr& expr, const Body& body,
                                                             const Signatures& signatures) {
    if (expr_is_wrapper_lifetime_source_form(expr, body, signatures)) return true;
    if (expr.lhs != nullptr && expr_contains_wrapper_lifetime_source_form(*expr.lhs, body, signatures)) return true;
    if (expr.rhs != nullptr && expr_contains_wrapper_lifetime_source_form(*expr.rhs, body, signatures)) return true;
    if (expr.third != nullptr && expr_contains_wrapper_lifetime_source_form(*expr.third, body, signatures)) return true;
    for (const ExprPtr& arg : expr.args) {
        if (arg != nullptr && expr_contains_wrapper_lifetime_source_form(*arg, body, signatures)) return true;
    }
    return false;
}

[[nodiscard]] std::optional<LocalId> direct_write_root(const Expr& expr, const Body& body) {
    switch (expr.kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::NullptrLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
            return {};
        case ExprKind::Identifier: {
            std::optional<LocalId> local = body.local_of(expr);
            if (!local.has_value()) return std::nullopt;
            const Type& type = body.type_of(*local);
            if (is_reference(type) || is_span(type)) return std::nullopt;
            return local;
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
            return direct_write_root(*expr.lhs, body);
        case ExprKind::Unary:
            if (is_explicit_star_this(expr)) return body.this_local();
            if (expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PreDec) {
                return direct_write_root(*expr.lhs, body);
            }
            if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) {
                return std::nullopt;
            }
            return body.local_of(*expr.lhs);
        case ExprKind::Call:
            if (expr.name == "operator_deref" && expr.lhs != nullptr) {
                if (expr.lhs->kind == ExprKind::Identifier) return body.local_of(*expr.lhs);
                if (expr.lhs->kind == ExprKind::Member && expr.lhs->lhs) {
                    return direct_write_root(*expr.lhs->lhs, body);
                }
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}
RootSet resolve_root_place(LocalId local, const DataflowState& state) {
    auto it = state.ref_targets.find(local);
    return it == state.ref_targets.end() ? single_root(local) : it->second.roots;
}

std::optional<LocalId> resolve_reborrow_lender(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            std::optional<LocalId> local = body.local_of(expr);
            if (local.has_value() && is_reborrowable_local_type(body.type_of(*local))) return local;
            return std::nullopt;
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
        case ExprKind::Cast:
            return expr.lhs ? resolve_reborrow_lender(*expr.lhs, body, signatures) : std::nullopt;
        case ExprKind::Unary:
            return expr.lhs ? resolve_reborrow_lender(*expr.lhs, body, signatures) : std::nullopt;
        case ExprKind::Call: {
            if (is_single_arg_lifetime_wrapper_call(expr)) {
                return resolve_reborrow_lender(*expr.args[0], body, signatures);
            }
            CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            bool returns_reference =
                sig != nullptr && !sig->returned_lifetime_param_indices.empty() && is_pointer_return_lifetime_source_type(sig->return_type);
            if (!returns_reference) return std::nullopt;
            if (sig->returned_lifetime_param_indices.size() != 1) return std::nullopt;
            std::size_t source_index = sig->returned_lifetime_param_indices.front();
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

// Whether a borrow derived from `lender` is tracked against `lender`
// itself (suspended_reborrows) rather than against the root it
// ultimately reaches (borrows). It is, whenever the lender is a
// reference/span local at all: `lender` already holds the one live
// access to that root along this path, so a borrow derived from it is a
// *reborrow* of an access that has already been accounted for, and can
// only ever conflict with another borrow also derived from `lender` --
// which is exactly what suspended_reborrows tracks.
//
// This used to additionally require the lender to be *mutable*, which
// made the safer spelling of a program the illegal one. Given
//
//     for (const Item& item : items) { total += score(item); }
//
// passing `item` (a shared lender) to a `const Item&` parameter fell
// through to the root-level check, saw the mutable borrow the range-for
// desugaring holds on `items`, and was rejected -- while the very same
// loop written `for (Item& item : items)` was accepted, because a
// mutable lender took this path. A borrow rule that rejects `const`
// where it accepts non-`const` is answering the wrong question.
//
// The rest of the machinery was already written for shared lenders:
// validate_reborrow_lender's "it is itself only a shared (const)
// reference/view" escalation error could not fire at all under the old
// gate, ReborrowSuspension carries a shared_count beside its
// mutable_suspended flag, and release_reference_borrow already releases
// either. Only the gate was wrong.
[[nodiscard]] bool reborrow_is_tracked_against_lender(const std::optional<LocalId>& lender, const Body& body) {
    return lender.has_value() && is_reborrowable_local_type(body.type_of(*lender));
}

std::expected<void, DataflowError> validate_reborrow_lender(LocalId lender, bool child_is_mutable, const DataflowState& state,
                              const Body& body, bool report_errors) {
    if (!report_errors) return {};
    const Type& lender_type = body.type_of(lender);
    if (child_is_mutable && !lender_type.is_mutable_ref) {
        return std::unexpected(DataflowError("cannot reborrow '" + body.name_of(lender) + "' as mutable: it is itself only a shared (const) "
                            "reference/view",
            state.current_loc));
    }
    // A new *mutable* reborrow needs exclusivity against every other
    // reborrow, shared or mutable alike -- but a new *shared* reborrow
    // only conflicts with an already-outstanding *mutable* one (any
    // number of simultaneous shared reborrows of the same lender, e.g.
    // `const T& a = this->peek();` alongside `const U& b =
    // this->other_const_accessor();`, coexist safely; see
    // ReborrowSuspension's own comment).
    bool conflicts = child_is_mutable ? local_is_suspended_for_reborrow(lender, state)
                                       : local_has_mutable_reborrow_suspended(lender, state);
    if (conflicts) {
        return std::unexpected(DataflowError("cannot form another reborrow from '" + body.name_of(lender) +
                                 "' while a nested reborrow derived from it is still live",
            state.current_loc));
    }
    return {};
}

std::expected<void, DataflowError> validate_reborrow_lender_write(LocalId lender, const DataflowState& state,
                                                                 const Body& body, bool report_errors) {
    if (!report_errors) return {};
    if (local_is_suspended_for_reborrow(lender, state)) {
        return std::unexpected(DataflowError("cannot write through '" + body.name_of(lender) +
                                 "' while a nested reborrow derived from it is still live",
            state.current_loc));
    }
    return {};
}

// Releases the borrow (if any) that reference-typed local `local` holds
// against its root, and forgets that `local` is a currently-bound
// reference. A no-op if `local` isn't (or is no longer) tracked in
// `ref_targets`, so it's safe to call speculatively.
//
// Called from two places (see check_function): as soon as the liveness
// analysis says `local` is no longer live (right after its last use --
// the NLL upgrade from spec ch05.3), and as a fallback at `local`'s
// lexical ScopeExit, for the unusual case of a reference that's never
// read after being bound at all (liveness alone would have released it
// immediately after its BindReference, before ScopeExit is even
// reached). Whichever fires first does the actual work; the other is
// then a harmless no-op, since both leave the exact same state.
//
// A lender with an outstanding reborrow is *not* releasable, however far
// past its own last use it is. `local`'s borrow of the root is the only
// thing standing between that root and the reborrow still aliasing it,
// so handing the root back while a derived borrow is live would let the
// root be written or re-borrowed underneath it. Liveness of the lender's
// *name* is the wrong question once it has lent: what keeps the borrow
// alive is the derived borrow, and that is what suspended_reborrows
// records. The suspension is dropped by whoever took it -- another
// reference's own release_reference_borrow, or a closure's
// release_closure_capture_borrows -- and this then releases normally at
// the next opportunity (ScopeExit at the latest).
void release_reference_borrow(LocalId local, DataflowState& state, [[maybe_unused]] const Body& body) {
    if (local_is_suspended_for_reborrow(local, state)) return;
    auto ref_it = state.ref_targets.find(local);
    if (ref_it == state.ref_targets.end()) return;
    RefTarget target = ref_it->second;
    if (target.is_reborrow()) {
        auto suspension_it = state.suspended_reborrows.find(*target.lender);
        if (suspension_it != state.suspended_reborrows.end()) {
            if (target.is_mutable) {
                suspension_it->second.mutable_suspended = false;
            } else if (suspension_it->second.shared_count > 0) {
                suspension_it->second.shared_count--;
            }
            if (suspension_it->second.shared_count == 0 && !suspension_it->second.mutable_suspended) {
                state.suspended_reborrows.erase(suspension_it);
            }
        }
    } else {
        for (LocalId root : target.roots) {
            auto borrow_it = state.borrows.find(root);
            if (borrow_it != state.borrows.end()) {
                if (target.is_mutable) {
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

void release_closure_capture_borrows(LocalId local, DataflowState& state) {
    auto closure_it = state.closure_capture_borrows.find(local);
    if (closure_it == state.closure_capture_borrows.end()) return;
    for (const ClosureCaptureBorrow& capture_borrow : closure_it->second) {
        // Release whichever hold this capture took -- see
        // ClosureCaptureBorrow's own comment for why a reborrowing
        // capture holds its lender suspended rather than the root
        // borrowed.
        if (capture_borrow.lender.has_value()) {
            auto suspension_it = state.suspended_reborrows.find(*capture_borrow.lender);
            if (suspension_it == state.suspended_reborrows.end()) continue;
            if (capture_borrow.is_mutable) {
                suspension_it->second.mutable_suspended = false;
            } else if (suspension_it->second.shared_count > 0) {
                suspension_it->second.shared_count--;
            }
            if (!suspension_it->second.mutable_suspended && suspension_it->second.shared_count == 0) {
                state.suspended_reborrows.erase(suspension_it);
            }
            continue;
        }
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

std::vector<std::size_t> successors(const Terminator& term) {
    switch (term.kind) {
        case TerminatorKind::Goto: return {term.target};
        case TerminatorKind::Branch: return {term.true_target, term.false_target};
        case TerminatorKind::Switch: {
            std::vector<std::size_t> out;
            out.reserve(term.switch_targets.size());
            for (const SwitchTarget& target : term.switch_targets) out.push_back(target.block);
            return out;
        }
        case TerminatorKind::Return:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
        default: return {};
    }
}

// Collects every currently-declared *reference-or-span*-typed
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
// The Identifier/bare-Call half of collect_reference_uses: both spell a
// use of a local the same way, and both must consult the *declaration*
// the use resolves to rather than any declaration sharing its spelling.
void collect_reference_use(const Expr& expr, const Body& body, LiveSet& out) {
    std::optional<LocalId> local = body.local_of(expr);
    if (!local.has_value()) return;
    const Type& type = body.type_of(*local);
    if (is_reference(type) || is_span(type) || body.decl(*local).is_borrow_holding_closure) {
        out.insert(*local);
    }
}

void collect_reference_uses(const Expr* expr, const Body& body, LiveSet& out) {
    if (expr == nullptr) return;
    switch (expr->kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::NullptrLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
        case ExprKind::Sizeof:
        case ExprKind::Alignof:
        case ExprKind::ValueInit:
            return;
        case ExprKind::New:
            if (expr->lhs) collect_reference_uses(expr->lhs.get(), body, out);
            for (const auto& arg : expr->args) collect_reference_uses(arg.get(), body, out);
            return;
        // A nested brace-enclosed initializer list is not a leaf: its
        // elements are ordinary expressions and any of them may name a
        // reference, so the walk has to descend into them exactly as it
        // does into a call's arguments.
        case ExprKind::BracedInitList:
            for (const auto& arg : expr->args) collect_reference_uses(arg.get(), body, out);
            return;
        case ExprKind::Delete:
        case ExprKind::Destroy:
        case ExprKind::PackExpansion:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Identifier:
            collect_reference_use(*expr, body, out);
            return;
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
                collect_reference_use(*expr, body, out);
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
                std::optional<LocalId> captured = body.local_of(capture);
                if (captured.has_value() &&
                    (is_reference(body.type_of(*captured)) || is_span(body.type_of(*captured)))) {
                    out.insert(*captured);
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
std::optional<LocalId> reference_def(const MirStatement& stmt) {
    if (stmt.kind == MirStatementKind::BindReference && stmt.has_local) return stmt.local;
    return std::nullopt;
}

LiveSet reference_uses(const MirStatement& stmt, const Body& body) {
    LiveSet uses;
    switch (stmt.kind) {
        case MirStatementKind::BindReference:
        case MirStatementKind::Eval:
        case MirStatementKind::MemberInit:
            collect_reference_uses(stmt.expr, body, uses);
            return uses;
        case MirStatementKind::Assign: {
            collect_reference_uses(stmt.expr, body, uses);
            if (stmt.has_local && body.is_valid_local(stmt.local) && is_reference(body.type_of(stmt.local))) {
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
        case TerminatorKind::Switch:
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
                                                              const std::vector<std::vector<std::size_t>>& preds) {
    std::size_t n = body.blocks.size();
    std::vector<LiveSet> block_live_in(n);

    auto block_live_out = [&](std::size_t b) {
        LiveSet live;
        for (std::size_t succ : successors(body.blocks[b].terminator)) {
            live.insert(block_live_in[succ].begin(), block_live_in[succ].end());
        }
        return live;
    };

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

        LiveSet live = block_live_out(b);
        const BasicBlock& block = body.blocks[b];
        for (LocalId use : reference_uses(block.terminator, body)) {
            live.insert(use);
        }
        for (auto it = block.statements.rbegin(); it != block.statements.rend(); ++it) {
            if (std::optional<LocalId> def = reference_def(*it)) live.erase(*def);
            for (LocalId use : reference_uses(*it, body)) live.insert(use);
        }

        if (live != block_live_in[b]) {
            block_live_in[b] = std::move(live);
            for (std::size_t p : preds[b]) {
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
    for (std::size_t b = 0; b < n; b++) {
        const BasicBlock& block = body.blocks[b];
        LiveSet live = block_live_out(b);
        for (LocalId use : reference_uses(block.terminator, body)) {
            live.insert(use);
        }
        live_after[b].resize(block.statements.size());
        for (std::size_t i = block.statements.size(); i-- > 0;) {
            live_after[b][i] = live;
            if (std::optional<LocalId> def = reference_def(block.statements[i])) live.erase(*def);
            for (LocalId use : reference_uses(block.statements[i], body)) live.insert(use);
        }
    }
    return live_after;
}

// After executing statement index `i` of a block (whose precomputed
// live-out set is `live_after_stmt`), releases the borrow of every
// currently-tracked reference that's no longer live -- i.e. whose last
// use was this statement or earlier. Collects the locals to release
// first rather than erasing while iterating `state.ref_targets` directly.
void release_dead_references(DataflowState& state, const Body& body, const LiveSet& live_after_stmt) {
    std::vector<LocalId> dead;
    // Releasing a dead closure can drop the suspension that was keeping
    // a dead lender alive, and releasing a dead reborrow can do the same
    // -- so a single pass in a fixed order would leave whichever came
    // first still holding its root, purely because of map iteration
    // order. Repeat while something actually goes away; measuring real
    // progress rather than assuming it matters because
    // release_reference_borrow declines to release a lender that is
    // still lending, and would otherwise be re-attempted forever.
    for (;;) {
        std::size_t before = state.ref_targets.size() + state.closure_capture_borrows.size();
        dead.clear();
        for (const auto& [local, root] : state.ref_targets) {
            if (!live_after_stmt.contains(local)) dead.push_back(local);
        }
        for (LocalId local : dead) {
            release_reference_borrow(local, state, body);
        }
        dead.clear();
        for (const auto& [local, borrows] : state.closure_capture_borrows) {
            if (!live_after_stmt.contains(local)) dead.push_back(local);
        }
        for (LocalId local : dead) {
            release_closure_capture_borrows(local, state);
        }
        if (state.ref_targets.size() + state.closure_capture_borrows.size() == before) return;
    }
}

[[nodiscard]] std::expected<void, DataflowError> apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
                 const Signatures& signatures, bool report_errors);

// Checks every argument of a Call expression against its callee's
// signature (if known), exactly the same way regardless of context --
// shared by apply_expr's own Call case (a call used as a plain
// statement or value sub-expression) and resolve_borrow_source_root's
// Call case below (a call to a reference-returning function used
// itself as a further reference-binding source).
[[nodiscard]] std::expected<void, DataflowError> check_call_arguments(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
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
[[nodiscard]] std::expected<RootSet, DataflowError> resolve_borrow_source_root(const Expr& expr, DataflowState& state, const Body& body,
                                                 const Signatures& signatures, bool report_errors) {
    auto literal_has_no_borrow_root = [&](const Expr& candidate) {
        return candidate.kind == ExprKind::IntegerLiteral || candidate.kind == ExprKind::FloatLiteral ||
               candidate.kind == ExprKind::BoolLiteral || candidate.kind == ExprKind::CharLiteral ||
               candidate.kind == ExprKind::StringLiteral;
    };
    if (literal_has_no_borrow_root(expr)) {
        if (!report_errors) return RootSet{};
    }
    switch (expr.kind) {
        case ExprKind::Identifier: {
            std::optional<LocalId> bound = body.local_of(expr);
            if (!bound.has_value()) return RootSet{};
            if (report_errors) {
                LocalState current = lookup(state.locals, *bound);
                if (current != LocalState::Initialized) {
                    return std::unexpected(DataflowError(describe_bad_state(body.name_of(*bound), current),
                        state.current_loc));
                }
            }

            return resolve_root_place(*bound, state);
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
            //
            // The §5.1(5.1) gate has to be applied here too, exactly as
            // the Deref case below calls validate_deref_expr: `&p[i]`
            // and `int& r = p[i]` reach a Subscript through this
            // function and never through apply_expr, so without this
            // call they performed unlicensed raw-pointer arithmetic
            // while the otherwise identical `p[i]` read was rejected.
            if (report_errors) {
                if (auto _r = validate_subscript_expr(expr, state, body, signatures); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            if (auto _r = apply_expr(*expr.rhs, /*is_move_target_context=*/false, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (expr.lhs->kind == ExprKind::Identifier) {
                if (std::optional<LocalId> base = body.local_of(*expr.lhs); base.has_value()) {
                    const Type& base_type = body.type_of(*base);
                    const Type& local_type = base_type.kind == TypeKind::Reference && base_type.pointee != nullptr
                                                 ? *base_type.pointee
                                                 : base_type;
                    if (local_type.kind == TypeKind::Span) return single_root(*base);
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
            if (is_explicit_star_this(expr)) {
                std::optional<LocalId> self = body.this_local();
                return self.has_value() ? single_root(*self) : RootSet{};
            }
            if (expr.unary_op == UnaryOp::AddressOf) {
                return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
            }
            if (expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PreDec) {
                return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
            }
            if (expr.unary_op != UnaryOp::Deref) {
                if (report_errors) {
                    return std::unexpected(DataflowError("a reference can currently only borrow a plain local variable, a "
                                         "field of one ('a.b'), an array element of one ('arr[i]'), a "
                                         "dereferenced raw-pointer local ('*p'/'p->x'), or "
                                         "the result of a call to a reference-returning function -- not an "
                                         "arbitrary expression",
                        state.current_loc));
                }
                return RootSet{};
            }
            if (report_errors) {
                if (auto _r = validate_deref_expr(expr, state, body, signatures); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            if (expr.lhs->kind == ExprKind::Identifier) {
                std::optional<LocalId> pointer = body.local_of(*expr.lhs);
                return pointer.has_value() ? resolve_root_place(*pointer, state) : RootSet{};
            }
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
            CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            bool returns_reference =
                sig != nullptr && !sig->returned_lifetime_param_indices.empty() && is_pointer_return_lifetime_source_type(sig->return_type);
            if (!returns_reference) {
                if (report_errors) {
                    return std::unexpected(DataflowError("cannot borrow the result of calling '" + expr.name +
                                         "': it doesn't return a reference with an inferrable lifetime (spec "
                                         "ch05.3)",
                        state.current_loc));
                }
                // Still check the arguments themselves so a genuinely
                // invalid call (wrong callee, bad arguments) is still
                // reported through the ordinary path once report_errors
                // is true; harmless to also run silently here.
                if (auto _r = check_call_arguments(expr, state, body, signatures, report_errors); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                return RootSet{};
            }
            if (auto _r = check_call_arguments(expr, state, body, signatures, report_errors); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            RootSet roots;
            for (std::size_t source_index : sig->returned_lifetime_param_indices) {
                if (expr.name == "operator_deref" && expr.lhs != nullptr && source_index < callee.param_offset) {
                    if (expr.lhs->kind == ExprKind::Identifier) {
                        if (std::optional<LocalId> receiver = body.local_of(*expr.lhs); receiver.has_value()) {
                            roots = union_roots(std::move(roots), resolve_root_place(*receiver, state));
                        }
                    } else if (expr.lhs->kind == ExprKind::Member && expr.lhs->lhs) {
                        auto _r = resolve_borrow_source_root(*expr.lhs->lhs, state, body, signatures, report_errors);
                        if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                        roots = union_roots(std::move(roots), std::move(_r).value());
                    } else {
                        auto _r = resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
                        if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                        roots = union_roots(std::move(roots), std::move(_r).value());
                    }
                    continue;
                }
                if (source_index < callee.param_offset) {
                    auto _r = resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
                    if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                    roots = union_roots(std::move(roots), std::move(_r).value());
                    continue;
                }
                auto _r = resolve_borrow_source_root(*expr.args[source_index - callee.param_offset], state,
                                                      body, signatures, report_errors);
                if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                roots = union_roots(std::move(roots), std::move(_r).value());
            }
            return roots;
        }

        default:
            if (report_errors) {
                return std::unexpected(DataflowError("a reference can currently only borrow a plain local variable, a field of "
                                     "one ('a.b'), an array element of one ('arr[i]'), a dereferenced "
                                     "raw-pointer local ('*p'/'p->x'), or the result of a call "
                                     "to a reference-returning function -- not an arbitrary expression",
                    state.current_loc));
            }
            return RootSet{};
    }
}

[[nodiscard]] RootSet resolve_lifetime_source_roots(const Expr& expr, DataflowState& state, const Body& body,
                                                    const Signatures& signatures, bool report_errors) {
    switch (expr.kind) {
        // `nullptr` borrows nothing and can therefore never dangle: it
        // designates no object, so there is no root for a later use to
        // outlive. An empty RootSet is exactly that statement.
        case ExprKind::NullptrLiteral: return {};

        case ExprKind::Identifier: {
            if (std::optional<LocalId> local = body.local_of(expr); local.has_value()) {
                auto local_it = state.local_lifetime_sources.find(*local);
                if (local_it != state.local_lifetime_sources.end()) return local_it->second;
                if (body.decl(*local).is_static_lifetime) return program_lifetime_root();
                return single_root(*local);
            }
            const GlobalVar* visible_global = nullptr;
            if (body.program != nullptr) {
                std::reference_wrapper<const Program> program_ref{*body.program};
                visible_global = find_visible_global(OptionalProgramRef{program_ref}, body.function_namespace_path,
                                                     expr.name, expr.explicit_global_qualification);
            } else {
                visible_global =
                    find_visible_global(OptionalProgramRef{}, body.function_namespace_path, expr.name,
                                        expr.explicit_global_qualification);
            }
            if (visible_global != nullptr) {
                return program_lifetime_root();
            }
            // A name that resolved to neither a local nor a visible
            // global names no storage at all (an enum constant, a
            // function, an unresolved name left to a later phase), so
            // there is nothing here for a reference to outlive.
            return {};
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
            if (is_bare_reference_wrapper_constructor_call(expr, body, signatures)) {
                return resolve_lifetime_source_roots(*expr.args[0], state, body, signatures, report_errors);
            }
            if (is_single_arg_lifetime_wrapper_call(expr)) {
                return resolve_lifetime_source_roots(*expr.args[0], state, body, signatures, report_errors);
            }
            if (is_zero_arg_lifetime_wrapper_call(expr, body, signatures)) {
                return {};
            }
            CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig == nullptr) return {};
            if (sig->returned_lifetime_param_indices.empty() || !is_pointer_return_lifetime_source_type(sig->return_type)) return {};
            RootSet roots;
            for (std::size_t source_index : sig->returned_lifetime_param_indices) {
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

// Parameters are the first local_decls entries, in declaration order
// (see LocalResolver::run), so a root that is a parameter *is* its own
// index -- no search by name, and no risk of matching a later local that
// merely reuses a parameter's spelling.
[[nodiscard]] std::optional<std::size_t> find_function_param_by_root(const Function& fn, LocalId root) {
    std::size_t index = local_index(root);
    if (index >= fn.params.size()) return std::nullopt;
    return index;
}

[[nodiscard]] bool roots_satisfy_named_lifetime_group(const RootSet& roots, const Function& fn,
                                                      std::string_view group_name) {
    if (roots.empty()) return false;
    for (LocalId root : roots) {
        if (is_program_lifetime_root(root)) return false;
        if (local_index(root) == 0 && !fn.member_owner_class.empty() && fn.return_lifetime.name == group_name &&
            (fn.name.ends_with("_operator_arrow") || group_name == "this")) {
            continue;
        }
        std::optional<std::size_t> param_index = find_function_param_by_root(fn, root);
        if (!param_index.has_value()) return false;
        if (fn.params[*param_index].lifetime.name != group_name) return false;
    }
    return true;
}

[[nodiscard]] bool roots_include_parameter_lifetime(const RootSet& roots, const Body& body,
                                                   const DataflowState& state) {
    for (LocalId root : roots) {
        if (!body.is_valid_local(root)) continue;
        auto it = state.parameter_lifetimes.find(body.name_of(root));
        if (it != state.parameter_lifetimes.end() && it->second.present()) return true;
    }
    return false;
}

std::expected<void, DataflowError> reject_lifetime_group_state_embedding(const Expr& expr, DataflowState& state, const Body& body,
                                           const Signatures& signatures, bool report_errors, std::string_view context,
                                           const Type* destination_type) {
    if (!report_errors) return {};
    std::optional<Type> expr_type = infer_expr_type(expr, body, signatures);
    if (!expr_type.has_value() || !is_pointer_return_lifetime_source_type(*expr_type)) return {};
    if (destination_type != nullptr &&
        (destination_type->is_reference_wrapper_lifetime_source ||
         (destination_type->kind == TypeKind::Reference && destination_type->pointee != nullptr &&
          destination_type->pointee->is_reference_wrapper_lifetime_source) ||
         expr_contains_wrapper_lifetime_source_form(expr, body, signatures))) {
        return {};
    }
    RootSet roots = resolve_lifetime_source_roots(expr, state, body, signatures, report_errors);
    if (expr_contains_wrapper_lifetime_source_form(expr, body, signatures) &&
        roots_include_parameter_lifetime(roots, body, state)) {
        return {};
    }
    if (!roots_include_parameter_lifetime(roots, body, state)) return {};
    return std::unexpected(DataflowError("cannot store a reference, pointer, or span derived from " + format_roots(body, roots) +
                            " into " + std::string(context) +
                            "; named and any lifetime groups propagate only through the direct bare return value",
                        state.current_loc));
}

// Determines whether `expr` (a borrow-source place -- the same shape
// resolve_borrow_source_root accepts) is only reachable *read-only*,
// i.e. whether obtaining a *mutable* `T&`/`T*` from it must be rejected.
// The "projection chain's const-reachability" resolve_borrow_source_root's
// own callers need but that function alone doesn't answer (it only
// resolves *which root* to check for borrow conflicts, not whether the
// path to it crossed a read-only step) -- used to reject binding a `T&`
// (apply_reference_binding) or passing a `T&` call argument
// (apply_reference_argument) through a `const T&`/`std::span<const T>`/
// `const T*` anywhere along the chain, and to decide whether `&expr`
// (ch05 §5.7) may produce a mutable `T*` or only a `const T*`.
//
// This was a second, independent answer to exactly the question
// assignment_target_is_read_only was already answering, and the two had
// drifted apart in five places -- see place_is_read_only (calls.cppm),
// which is now the single definition both names denote.
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures) {
    return place_is_read_only(expr, body, signatures);
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
// afterward is unaffected. For a plain place, check conservatively at
// this instant: the root must have no existing borrow at all, shared or
// mutable -- the same exclusivity a *new* `T&` binding would require,
// rejected the same way taking a second one would be. But if the
// operand's static type is already `T&`/`const T&`, `&expr` merely
// derives a raw pointer from that existing borrow rather than creating a
// second borrow of the root, so no extra exclusivity check applies here.
std::expected<void, DataflowError> apply_address_of(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                       bool report_errors) {
    if (expr.lhs->kind == ExprKind::Identifier && !body.local_of(*expr.lhs).has_value() &&
        signatures.contains(expr.lhs->name)) {
        return {};
    }
    auto roots_result = resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
    if (!roots_result.has_value()) return std::unexpected(std::move(roots_result).error());
    RootSet roots = std::move(roots_result).value();
    if (!report_errors || roots.empty()) return {};
    if (std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures);
        operand_type.has_value() && operand_type->kind == TypeKind::Reference) {
        return {};
    }
    for (LocalId root : roots) {
        auto borrow_it = state.borrows.find(root);
        if (borrow_it != state.borrows.end() &&
            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
            return std::unexpected(DataflowError("cannot take the address of " + format_root(body, root) +
                                                     ": it is already borrowed",
                                state.current_loc));
        }
    }
    return {};
}

} // namespace scpp
