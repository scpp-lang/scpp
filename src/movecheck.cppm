module;

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

export module scpp.movecheck;

import scpp.ast;
import scpp.mir;

export namespace scpp {

struct DataflowError : std::runtime_error {
    explicit DataflowError(const std::string& message) : std::runtime_error(message) {}
};

} // namespace scpp

namespace scpp {
namespace {

// The lattice tracked per local variable:
//   Bottom      -- not currently in scope on this path: either not yet
//                  declared here (e.g. a name declared only in a sibling
//                  `if` branch), or already gone out of lexical scope (a
//                  `ScopeExit` MIR statement resets a local back to this
//                  state at the end of its enclosing block/if-branch/
//                  while-body, mirroring codegen's own scope_stack_).
//   Initialized -- holds a valid, readable value. scpp has no concept of an
//                  "uninitialized" scalar: every local without an explicit
//                  initializer is zero-initialized (0 / false / null / all-
//                  zero fields) by codegen, for every type, so a `Declare`
//                  MIR statement always yields Initialized, never a
//                  separate "not yet given a value" state. For a reference
//                  (`T&`/`const T&`), Initialized means "currently bound
//                  and in scope" -- see `BindReference` below.
//   MovedOut    -- was holding a unique_ptr value that has been moved away
//   Conflict    -- different incoming paths disagree (e.g. Initialized on
//                  one, MovedOut on another) -- treated as unsafe to read,
//                  same as Bottom/MovedOut.
// Only `Initialized` is safe to read; every other state is rejected (with a
// tailored message) if the checker sees a use of it.
enum class LocalState { Bottom, Initialized, MovedOut, Conflict };

using StateMap = std::unordered_map<std::string, LocalState>;

// Per-*root-place* borrow bookkeeping for alias-XOR-mutability (ch05.2). A
// "root place" is a plain local/parameter that isn't itself tracked as
// borrowing something else (see `resolve_root_place`): a chain of
// reference-to-reference bindings is flattened so every borrow is always
// checked/recorded against the one real place ultimately being aliased,
// not an intermediate reference's name.
struct BorrowState {
    int shared_count = 0;
    bool mutable_borrow = false;

    bool operator==(const BorrowState&) const = default;
};

using BorrowMap = std::unordered_map<std::string, BorrowState>;

// Maps a currently-bound reference local/parameter's name directly to the
// *root* place it (transitively) borrows -- see `resolve_root_place`.
using RefTargetMap = std::unordered_map<std::string, std::string>;

// The full per-program-point dataflow state: ownership/move state per
// local (`locals`), active borrows per root place (`borrows`), and which
// root each currently-bound reference aliases (`ref_targets`). Bundled
// into one struct so the worklist algorithm in check_function can thread
// (and join) all three together as a single unit.
struct DataflowState {
    StateMap locals;
    BorrowMap borrows;
    RefTargetMap ref_targets;

    bool operator==(const DataflowState&) const = default;
};

// Function name -> parameter types, built once from the whole Program by
// check_moves. Needed only for call-site reference-parameter binding (see
// apply_reference_argument): the checker otherwise never resolves a call
// target's declared signature.
using Signatures = std::unordered_map<std::string, std::vector<Type>>;

LocalState join(LocalState a, LocalState b) {
    if (a == b) return a;
    if (a == LocalState::Bottom) return b;
    if (b == LocalState::Bottom) return a;
    return LocalState::Conflict;
}

// Joins two per-block state snapshots (e.g. the OUT states of two
// predecessors flowing into a shared successor block).
StateMap join_maps(const StateMap& a, const StateMap& b) {
    StateMap result = a;
    for (const auto& [name, state] : b) {
        auto it = result.find(name);
        result[name] = it == result.end() ? state : join(it->second, state);
    }
    return result;
}

// Conservatively merges two borrow snapshots for the same root place: if
// the incoming paths disagree, pick the *more restrictive* combination
// (mutable if either says mutable; the larger shared count) rather than
// silently under-restricting. In well-formed programs this should always
// be a same-value merge in practice, since every borrow is released (via
// ScopeExit) no later than the end of its own lexically-nested scope, so
// it can't still be "half alive" at a join point coming from only one
// predecessor -- see the BorrowState/ScopeExit comments below.
BorrowState join_borrow(const BorrowState& a, const BorrowState& b) {
    BorrowState result;
    result.mutable_borrow = a.mutable_borrow || b.mutable_borrow;
    result.shared_count = std::max(a.shared_count, b.shared_count);
    return result;
}

BorrowMap join_borrow_maps(const BorrowMap& a, const BorrowMap& b) {
    BorrowMap result = a;
    for (const auto& [place, borrow] : b) {
        auto it = result.find(place);
        result[place] = it == result.end() ? borrow : join_borrow(it->second, borrow);
    }
    return result;
}

// A reference is bound exactly once and never rebound (ch03), so in a
// well-formed program every incoming path agrees on what a given
// reference name targets; last-write-wins is just a harmless tie-break
// for whatever a not-yet-rejected, malformed program's fixed-point
// iteration computes along the way.
RefTargetMap join_ref_targets(const RefTargetMap& a, const RefTargetMap& b) {
    RefTargetMap result = a;
    for (const auto& [ref_name, root] : b) {
        result.insert_or_assign(ref_name, root);
    }
    return result;
}

DataflowState join_states(const DataflowState& a, const DataflowState& b) {
    return DataflowState{
        join_maps(a.locals, b.locals),
        join_borrow_maps(a.borrows, b.borrows),
        join_ref_targets(a.ref_targets, b.ref_targets),
    };
}

std::string describe_bad_state(const std::string& name, LocalState state) {
    switch (state) {
        case LocalState::MovedOut:
            return "use of moved-out variable '" + name + "'";
        case LocalState::Conflict:
            return "use of variable '" + name +
                   "' whose initialization state is inconsistent across incoming control-flow paths "
                   "(initialized on some, not on others)";
        case LocalState::Bottom:
            return "use of variable '" + name + "' that is out of scope here";
        case LocalState::Initialized:
        default:
            return "use of possibly-uninitialized variable '" + name + "'";
    }
}

[[nodiscard]] bool is_unique_ptr(const Type& type) { return type.kind == TypeKind::UniquePtr; }
[[nodiscard]] bool is_reference(const Type& type) { return type.kind == TypeKind::Reference; }

// The only expressions allowed to produce a std::unique_ptr rvalue:
// moving an existing one out, or freshly heap-allocating one via
// std::make_unique (scpp has no `new` expression at all -- make_unique is
// the sole sanctioned allocation syntax, and is itself a compiler builtin
// rather than a real generic call, same treatment as std::move).
[[nodiscard]] bool produces_unique_ptr_rvalue(const Expr& expr) {
    return expr.kind == ExprKind::Move || expr.kind == ExprKind::MakeUnique;
}

[[nodiscard]] LocalState lookup(const StateMap& state, const std::string& name) {
    auto it = state.find(name);
    return it == state.end() ? LocalState::Bottom : it->second;
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
std::string resolve_root_place(const std::string& name, const DataflowState& state) {
    auto it = state.ref_targets.find(name);
    return it == state.ref_targets.end() ? name : it->second;
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

void apply_expr(const Expr& expr, bool is_unique_ptr_rvalue_context, DataflowState& state, const Body& body,
                 const Signatures& signatures, bool report_errors);

// Checks (and, on success, has no lasting effect on `state`) a reference
// function-call argument: a transient borrow that begins right before
// the call and ends right after it returns, since v0.1 never lets a
// reference escape a call (returning one is rejected by codegen's
// declare_function) -- so unlike a local reference variable's borrow
// (released at its own ScopeExit), there's nothing to record past this
// single Call, only a conflict check against whatever is *already*
// borrowed. `in_call_borrows` is a scratch map shared by every reference
// argument of the *same* call (see the Call case below): it catches
// aliasing *within* one argument list (e.g. `f(x, x)` where both
// parameters are references, or one `&mut` and one `&`), which
// `state.borrows` alone can't, since none of these transient borrows are
// ever written back into `state`.
void apply_reference_argument(const Expr& arg, const Type& param_type, const DataflowState& state,
                               BorrowMap& in_call_borrows, const Body& body, bool report_errors) {
    if (arg.kind != ExprKind::Identifier) {
        if (report_errors) {
            throw DataflowError("a reference argument currently only supports passing a plain local variable "
                                 "directly (not a member, subscript, or other expression)");
        }
        return;
    }
    if (!report_errors) return; // purely diagnostic: nothing to mutate either way

    const std::string& bound_name = arg.name;
    auto type_it = body.local_types.find(bound_name);
    if (type_it != body.local_types.end() && is_unique_ptr(type_it->second)) {
        throw DataflowError("cannot pass std::unique_ptr variable '" + bound_name + "' by reference in this version");
    }
    LocalState current = lookup(state.locals, bound_name);
    if (current != LocalState::Initialized) {
        throw DataflowError(describe_bad_state(bound_name, current));
    }

    std::string root = resolve_root_place(bound_name, state);
    bool is_mutable = param_type.is_mutable_ref;

    auto persistent_it = state.borrows.find(root);
    bool persistent_conflict =
        persistent_it != state.borrows.end() &&
        (is_mutable ? (persistent_it->second.mutable_borrow || persistent_it->second.shared_count > 0)
                    : persistent_it->second.mutable_borrow);
    if (persistent_conflict) {
        throw DataflowError("cannot pass '" + root + "' by " + std::string(is_mutable ? "mutable " : "") +
                             "reference: it is already borrowed");
    }

    auto in_call_it = in_call_borrows.find(root);
    bool in_call_conflict =
        in_call_it != in_call_borrows.end() &&
        (is_mutable ? (in_call_it->second.mutable_borrow || in_call_it->second.shared_count > 0)
                    : in_call_it->second.mutable_borrow);
    if (in_call_conflict) {
        throw DataflowError("cannot pass '" + root + "' by " + std::string(is_mutable ? "mutable " : "") +
                             "reference more than once in the same call");
    }

    BorrowState& borrow = in_call_borrows[root];
    if (is_mutable) {
        borrow.mutable_borrow = true;
    } else {
        borrow.shared_count++;
    }
}

// Walks `expr`, updating `state` for any std::move / assignment / borrow
// side effects and, when `report_errors` is true, throwing DataflowError
// on an unsafe read. `is_unique_ptr_rvalue_context` is true exactly where
// a bare `std::move(x)` is allowed to appear: a var-decl initializer, an
// assignment RHS, a return value, or a call argument.
//
// This function is run twice per program point: once during the
// worklist's fixed-point iteration (report_errors=false, just to compute
// stable per-block states) and once more in the final reporting pass
// (report_errors=true). Both runs must apply the *same* state mutations so
// the two phases stay consistent.
void apply_expr(const Expr& expr, bool is_unique_ptr_rvalue_context, DataflowState& state, const Body& body,
                 const Signatures& signatures, bool report_errors) {
    switch (expr.kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::BoolLiteral:
            return;

        case ExprKind::Identifier: {
            if (!report_errors) return;
            auto type_it = body.local_types.find(expr.name);
            if (type_it == body.local_types.end()) return; // unknown name: left to codegen's own check
            if (is_unique_ptr(type_it->second)) {
                throw DataflowError("use of std::unique_ptr variable '" + expr.name +
                                     "' requires std::move (copying is not allowed)");
            }
            LocalState current = lookup(state.locals, expr.name);
            if (current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(expr.name, current));
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
                throw DataflowError("cannot use '" + expr.name + "' while it is mutably borrowed");
            }
            return;
        }

        case ExprKind::Move: {
            if (expr.lhs->kind != ExprKind::Identifier) {
                if (report_errors) {
                    throw DataflowError("std::move currently only supports a plain local variable "
                                         "(not a member, subscript, or other expression)");
                }
                return;
            }
            const std::string& name = expr.lhs->name;
            auto type_it = body.local_types.find(name);
            if (type_it == body.local_types.end() || !is_unique_ptr(type_it->second)) {
                if (report_errors) {
                    throw DataflowError("std::move is only supported for std::unique_ptr variables in this "
                                         "version; '" +
                                         name + "' is not one");
                }
                return;
            }
            LocalState current = lookup(state.locals, name);
            if (report_errors && current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(name, current));
            }
            state.locals[name] = LocalState::MovedOut;
            if (report_errors && !is_unique_ptr_rvalue_context) {
                throw DataflowError("std::move(" + name + ") must be used to initialize or assign into a "
                                                            "std::unique_ptr");
            }
            return;
        }

        case ExprKind::Unary:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::Binary:
            if (expr.binary_op == BinaryOp::Assign) {
                bool target_is_unique_ptr = false;
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto it = body.local_types.find(expr.lhs->name);
                    target_is_unique_ptr = it != body.local_types.end() && is_unique_ptr(it->second);
                }
                apply_expr(*expr.rhs, target_is_unique_ptr, state, body, signatures, report_errors);
                if (report_errors && target_is_unique_ptr && !produces_unique_ptr_rvalue(*expr.rhs)) {
                    throw DataflowError("assigning to a std::unique_ptr variable requires std::move or "
                                        "std::make_unique (copying is not allowed)");
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
                }
                return;
            }
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::Call: {
            auto sig_it = signatures.find(expr.name);
            // Scratch borrow-map shared by every reference argument of
            // *this* call only (see apply_reference_argument) -- never
            // merged into `state`, since none of these borrows outlive
            // the call.
            BorrowMap in_call_borrows;
            for (size_t i = 0; i < expr.args.size(); i++) {
                const Expr& arg = *expr.args[i];
                bool param_is_reference = sig_it != signatures.end() && i < sig_it->second.size() &&
                                           is_reference(sig_it->second[i]);
                if (param_is_reference) {
                    apply_reference_argument(arg, sig_it->second[i], state, in_call_borrows, body, report_errors);
                } else {
                    // Parameter types aren't resolved here for anything
                    // other than reference detection above; a unique_ptr
                    // argument must be `std::move(x)` regardless of
                    // position, so std::move is simply allowed in every
                    // non-reference argument slot.
                    apply_expr(arg, /*is_unique_ptr_rvalue_context=*/true, state, body, signatures, report_errors);
                }
            }
            return;
        }

        case ExprKind::Member:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::Subscript:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::MakeUnique:
            // std::make_unique<T>(args...) itself never reads a
            // unique_ptr (it *produces* one); its constructor arguments
            // are ordinary values, checked as plain reads.
            for (const auto& arg : expr.args) {
                apply_expr(*arg, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures, report_errors);
            }
            return;
    }
}

// Handles a `BindReference` MIR statement (`T& r = place;` / `const T& r
// = place;`), emitted only by a Reference-typed VarDecl (see
// mir.cppm). Checks the borrowed place is currently readable and not
// already borrowed in a conflicting way (ch05.2's alias-XOR-mutability),
// then records the new borrow -- against the place's *root* (see
// resolve_root_place), not necessarily `place` itself, so a chain of
// reference-to-reference bindings is tracked precisely.
void apply_reference_binding(const MirStatement& stmt, DataflowState& state, const Body& body,
                              bool report_errors) {
    if (stmt.expr == nullptr) {
        // No initializer (`int& r;`): illegal, since unlike every other
        // scpp type, a reference has no zero/default state to fall back
        // to -- real C++ has no such thing as a null or later-bound
        // reference either.
        if (report_errors) {
            throw DataflowError("reference '" + stmt.local + "' must be initialized (bound to a variable) at "
                                                              "declaration");
        }
        state.locals[stmt.local] = LocalState::Initialized;
        return;
    }
    if (stmt.expr->kind != ExprKind::Identifier) {
        if (report_errors) {
            throw DataflowError("a reference ('" + stmt.local + "') can currently only bind directly to a "
                                                                 "plain local variable (not a member, subscript, "
                                                                 "or other expression)");
        }
        state.locals[stmt.local] = LocalState::Initialized;
        return;
    }

    const std::string& bound_name = stmt.expr->name;
    if (report_errors) {
        auto type_it = body.local_types.find(bound_name);
        if (type_it != body.local_types.end() && is_unique_ptr(type_it->second)) {
            throw DataflowError("cannot bind a reference to std::unique_ptr variable '" + bound_name +
                                 "' in this version");
        }
        LocalState current = lookup(state.locals, bound_name);
        if (current != LocalState::Initialized) {
            throw DataflowError(describe_bad_state(bound_name, current));
        }
    }

    std::string root = resolve_root_place(bound_name, state);
    bool is_mutable = stmt.type.is_mutable_ref;
    BorrowState& borrow = state.borrows[root];
    if (report_errors) {
        if (is_mutable && (borrow.mutable_borrow || borrow.shared_count > 0)) {
            throw DataflowError("cannot mutably borrow '" + root + "': it is already borrowed");
        }
        if (!is_mutable && borrow.mutable_borrow) {
            throw DataflowError("cannot borrow '" + root + "': it is already mutably borrowed");
        }
    }
    if (is_mutable) {
        borrow.mutable_borrow = true;
    } else {
        borrow.shared_count++;
    }
    state.ref_targets[stmt.local] = root;
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
        if (!ref_type.is_mutable_ref) {
            throw DataflowError("cannot assign through '" + stmt.local +
                                 "': it is a read-only (const) reference");
        }
        LocalState current = lookup(state.locals, stmt.local);
        if (current != LocalState::Initialized) {
            throw DataflowError(describe_bad_state(stmt.local, current));
        }
    }
    apply_expr(*stmt.expr, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures, report_errors);
}

void apply_statement(const MirStatement& stmt, DataflowState& state, const Body& body, const Signatures& signatures,
                      bool report_errors) {
    switch (stmt.kind) {
        case MirStatementKind::Declare:
            // scpp has no "uninitialized" state (see the LocalState
            // comment above): a bare declaration always zero-initializes,
            // so it's always Initialized from this point on.
            state.locals[stmt.local] = LocalState::Initialized;
            return;

        case MirStatementKind::BindReference:
            apply_reference_binding(stmt, state, body, report_errors);
            return;

        case MirStatementKind::Assign: {
            auto type_it = body.local_types.find(stmt.local);
            if (type_it != body.local_types.end() && is_reference(type_it->second)) {
                apply_reference_write_through(stmt, state, body, signatures, report_errors);
                return;
            }

            bool target_is_unique_ptr = type_it != body.local_types.end() && is_unique_ptr(type_it->second);
            apply_expr(*stmt.expr, target_is_unique_ptr, state, body, signatures, report_errors);
            if (report_errors && target_is_unique_ptr && !produces_unique_ptr_rvalue(*stmt.expr)) {
                throw DataflowError("variable '" + stmt.local +
                                    "' of type std::unique_ptr must be initialized via std::move or "
                                    "std::make_unique (copying a unique_ptr is not allowed)");
            }
            if (report_errors) {
                auto borrow_it = state.borrows.find(stmt.local);
                if (borrow_it != state.borrows.end() &&
                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                    throw DataflowError("cannot assign to '" + stmt.local + "' while it is borrowed");
                }
            }
            state.locals[stmt.local] = LocalState::Initialized;
            return;
        }

        case MirStatementKind::Eval:
            apply_expr(*stmt.expr, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures, report_errors);
            return;

        case MirStatementKind::Drop:
            // Purely a codegen-facing marker (no-op until heap-allocated
            // owning types exist); no dataflow state effect here.
            return;

        case MirStatementKind::ScopeExit: {
            // If `stmt.local` is a currently-bound reference, release
            // the borrow it holds against its root before forgetting it
            // (mirrors codegen's scope_stack_-driven drop/removal, just
            // for borrows instead of heap ownership). Moving or
            // dropping a *borrowed* root while a borrow is still live is
            // not separately checked here: it can't happen in v0.1,
            // since a reference can only ever be bound to a *plain*
            // place declared no later than (i.e. in the same or an
            // enclosing scope of) the reference itself, so the
            // reference's own ScopeExit always fires first, releasing
            // the borrow before the root's own ScopeExit (if any) is
            // ever reached -- and the only *movable* type (unique_ptr)
            // can't be referenced at all yet (see codegen's
            // validate_reference_pointee), so "move a borrowed place"
            // can't arise either.
            auto ref_it = state.ref_targets.find(stmt.local);
            if (ref_it != state.ref_targets.end()) {
                auto borrow_it = state.borrows.find(ref_it->second);
                if (borrow_it != state.borrows.end()) {
                    if (body.local_types.at(stmt.local).is_mutable_ref) {
                        borrow_it->second.mutable_borrow = false;
                    } else if (borrow_it->second.shared_count > 0) {
                        borrow_it->second.shared_count--;
                    }
                    if (!borrow_it->second.mutable_borrow && borrow_it->second.shared_count == 0) {
                        state.borrows.erase(borrow_it);
                    }
                }
                state.ref_targets.erase(ref_it);
            }
            // `stmt.local` just went out of lexical scope: forget its
            // tracked state entirely. Erasing is equivalent to setting
            // it to Bottom (lookup() treats a missing key as Bottom) and
            // keeps the map from growing with entries the rest of the
            // analysis no longer cares about.
            state.locals.erase(stmt.local);
            return;
        }
    }
}

void check_terminator(const Terminator& term, DataflowState& state, const Function& fn, const Body& body,
                       const Signatures& signatures) {
    switch (term.kind) {
        case TerminatorKind::Branch:
            apply_expr(*term.condition, false, state, body, signatures, /*report_errors=*/true);
            return;
        case TerminatorKind::Return: {
            if (term.return_value == nullptr) return;
            bool return_is_unique_ptr = is_unique_ptr(fn.return_type);
            apply_expr(*term.return_value, return_is_unique_ptr, state, body, signatures, /*report_errors=*/true);
            if (return_is_unique_ptr && !produces_unique_ptr_rvalue(*term.return_value)) {
                throw DataflowError(
                    "returning a std::unique_ptr requires std::move or std::make_unique (copying is not allowed)");
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
void check_function(const Function& fn, const Signatures& signatures) {
    Body body = build_mir(fn);
    size_t n = body.blocks.size();

    std::vector<std::vector<size_t>> preds(n);
    for (size_t i = 0; i < n; i++) {
        for (size_t succ : successors(body.blocks[i].terminator)) {
            preds[succ].push_back(i);
        }
    }

    DataflowState entry_state;
    for (const Param& param : fn.params) {
        entry_state.locals[param.name] = LocalState::Initialized;
    }

    std::vector<DataflowState> in_states(n);
    std::vector<DataflowState> out_states(n);
    if (n > 0) in_states[0] = entry_state;

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
        for (const MirStatement& stmt : body.blocks[b].statements) {
            apply_statement(stmt, new_out, body, signatures, /*report_errors=*/false);
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
        for (const MirStatement& stmt : body.blocks[b].statements) {
            apply_statement(stmt, state, body, signatures, /*report_errors=*/true);
        }
        check_terminator(body.blocks[b].terminator, state, fn, body, signatures);
    }
}

} // namespace
} // namespace scpp

export namespace scpp {

void check_moves(const Program& program) {
    Signatures signatures;
    for (const Function& fn : program.functions) {
        std::vector<Type> param_types;
        param_types.reserve(fn.params.size());
        for (const Param& param : fn.params) {
            param_types.push_back(param.type);
        }
        signatures[fn.name] = std::move(param_types);
    }
    for (const Function& fn : program.functions) {
        check_function(fn, signatures);
    }
}

} // namespace scpp
