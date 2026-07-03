module;

#include <algorithm>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// A function's checked signature, built once for the whole Program by
// check_moves. Needed for call-site reference-parameter binding (see
// apply_reference_argument) and for resolving/validating a reference
// return value against spec ch05.3's elision rule (see
// resolve_elided_param_index and check_terminator's Return case).
struct FunctionSignature {
    std::vector<Type> param_types;
    Type return_type;
    // Set only when return_type is itself a Reference: the index into
    // param_types of the sole reference parameter this function's own
    // returned reference must (transitively) resolve back to, and that
    // a caller's argument for it is assumed to alias for the duration
    // of a call to this function returning a reference derived from
    // that argument (see resolve_borrow_source_root's Call case).
    // Computed once by resolve_elided_param_index, which throws if
    // return_type is a Reference but no valid elision exists.
    std::optional<size_t> elided_param_index;
};

using Signatures = std::unordered_map<std::string, FunctionSignature>;

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

// Structurally validates and resolves spec ch05.3's elision rule for a
// single function's signature: if `fn.return_type` is a Reference, `fn`
// must have *exactly* one reference-typed parameter (this language has
// no `this`/method-receiver concept at all yet, so that half of the
// spec's elision rule never applies -- every reference-returning
// function is a free function, so its sole eligible parameter is
// whichever plain parameter happens to be a reference), and that
// parameter's mutability must be able to license the return type's: a
// `const T&` parameter can only license a `const T&` return, never a
// plain `T&` (that would manufacture a mutable alias out of a shared
// one). Throws DataflowError describing the mismatch otherwise; returns
// nullopt (meaning "elision doesn't apply") when `fn.return_type` isn't
// a Reference at all.
[[nodiscard]] std::optional<size_t> resolve_elided_param_index(const Function& fn) {
    if (!is_reference(fn.return_type)) return std::nullopt;

    std::optional<size_t> found;
    for (size_t i = 0; i < fn.params.size(); i++) {
        if (!is_reference(fn.params[i].type)) continue;
        if (found.has_value()) {
            throw DataflowError(
                "function '" + fn.name +
                "' returns a reference but has more than one reference parameter; scpp v0.1 can only infer a "
                "returned reference's lifetime when there is exactly one (spec ch05.3) -- refactor to take a "
                "single reference parameter, or return by value/std::unique_ptr instead");
        }
        found = i;
    }
    if (!found.has_value()) {
        throw DataflowError(
            "function '" + fn.name +
            "' returns a reference but has no reference parameter to infer its lifetime from (spec ch05.3) -- "
            "refactor to take a single reference parameter, or return by value/std::unique_ptr instead");
    }
    if (fn.return_type.is_mutable_ref && !fn.params[*found].type.is_mutable_ref) {
        throw DataflowError("function '" + fn.name +
                             "' returns a mutable reference ('T&') but its sole reference parameter '" +
                             fn.params[*found].name +
                             "' is a shared reference ('const T&'); a mutable reference cannot be manufactured "
                             "from a shared one");
    }
    return found;
}

// Whether assigning through `expr` (used as an assignment's *target* --
// see apply_expr's Binary/Assign case) would write through a read-only
// (`const T&`) reference somewhere in its chain. A `.field`/`[index]`
// projection's constness always comes from its outermost base (struct
// fields can never themselves be references -- ch04.1 permanently
// forbids that -- so there's never a *nested* reference to find deeper
// in the chain); a call's constness comes from its own return type. A
// plain local (not itself a reference) is always writable here -- move/
// initialization-state legality is checked separately, this is purely
// about const-ness.
[[nodiscard]] bool assignment_target_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            return it != body.local_types.end() && is_reference(it->second) && !it->second.is_mutable_ref;
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
            return assignment_target_is_read_only(*expr.lhs, body, signatures);
        case ExprKind::Call: {
            auto sig_it = signatures.find(expr.name);
            return sig_it != signatures.end() && is_reference(sig_it->second.return_type) &&
                   !sig_it->second.return_type.is_mutable_ref;
        }
        default:
            return false;
    }
}

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
    auto borrow_it = state.borrows.find(ref_it->second);
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
    state.ref_targets.erase(ref_it);
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

// Collects the name of every currently-declared *reference*-typed local
// mentioned anywhere in `expr` (recursively) into `out`. Used by the
// liveness analysis below to find where a reference is "used" (in the
// sense of needing its current borrow to stay valid), without having to
// duplicate apply_expr's exact per-case dataflow semantics: this walk is
// deliberately a plain, generic tree traversal, over-inclusive rather
// than clever. A spurious extra "use" only makes a borrow's computed
// live range *longer* than strictly necessary (a missed early-release
// opportunity, but still sound); missing a real one would instead be an
// actual bug (releasing a borrow while it's still genuinely needed).
void collect_reference_uses(const Expr* expr, const Body& body, LiveSet& out) {
    if (expr == nullptr) return;
    switch (expr->kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::BoolLiteral:
            return;
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr->name);
            if (it != body.local_types.end() && is_reference(it->second)) {
                out.insert(expr->name);
            }
            return;
        }
        case ExprKind::Binary:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            return;
        case ExprKind::Unary:
        case ExprKind::Move:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Call:
        case ExprKind::MakeUnique:
            for (const auto& arg : expr->args) {
                collect_reference_uses(arg.get(), body, out);
            }
            return;
        case ExprKind::Member:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Subscript:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            return;
    }
}

// The reference-typed local that `stmt` freshly brings into existence,
// if any -- only a BindReference does (references are never rebound, so
// this is also the one and only point before which `stmt.local`'s
// liveness must not extend backward; see compute_reference_liveness).
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
}

void apply_expr(const Expr& expr, bool is_unique_ptr_rvalue_context, DataflowState& state, const Body& body,
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
// variable, or a chain of `.field`/`[index]` projections off one --
// root-resolved to the *outermost* variable in the chain (see the
// whole-root conservatism note below).
//
// `allow_call_source` additionally permits (and, when true, is passed
// through unchanged into every recursive call, so a chain is followed
// end to end) a call to a function that itself returns a reference,
// resolved transitively through its own elided parameter (spec ch05.3).
// This is only ever passed true from check_terminator's Return case --
// v0.1 can prove a *returned* reference doesn't dangle this way, but
// deliberately doesn't yet support binding a call's returned reference
// to a new named local reference, or passing it onward as a reference
// argument (both callers below always pass false): unlike a return,
// neither has an elided parameter of its own to fall back on if the
// chain needs to keep going, and working out exactly when that's still
// sound is left for a follow-up.
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
[[nodiscard]] std::string resolve_borrow_source_root(const Expr& expr, DataflowState& state, const Body& body,
                                                       const Signatures& signatures, bool report_errors,
                                                       bool allow_call_source) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            const std::string& bound_name = expr.name;
            if (report_errors) {
                auto type_it = body.local_types.find(bound_name);
                if (type_it != body.local_types.end() && is_unique_ptr(type_it->second)) {
                    throw DataflowError("cannot borrow std::unique_ptr variable '" + bound_name +
                                         "' in this version");
                }
                LocalState current = lookup(state.locals, bound_name);
                if (current != LocalState::Initialized) {
                    throw DataflowError(describe_bad_state(bound_name, current));
                }
            }
            return resolve_root_place(bound_name, state);
        }

        case ExprKind::Member:
            // Whole-root conservative (see above): a field projection
            // resolves to the same root as its own base.
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors, allow_call_source);

        case ExprKind::Subscript:
            // The index is a genuine value-producing sub-expression (it
            // could itself read/move/call), so it's checked exactly like
            // any other read; the array base contributes the (whole-)
            // root, same as Member above.
            apply_expr(*expr.rhs, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures, report_errors);
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors, allow_call_source);

        case ExprKind::Call: {
            auto sig_it = signatures.find(expr.name);
            bool returns_reference = sig_it != signatures.end() && sig_it->second.elided_param_index.has_value();
            if (!allow_call_source || !returns_reference) {
                if (report_errors) {
                    throw DataflowError(
                        "cannot borrow the result of calling '" + expr.name +
                        "': " + (returns_reference
                                     ? "binding a call's returned reference to a new reference isn't supported "
                                       "yet in this version -- assign it to a plain (non-reference) variable "
                                       "instead"
                                     : "it doesn't return a reference with an inferrable lifetime (spec ch05.3)"));
                }
                // Still check the arguments themselves so a genuinely
                // invalid call (wrong callee, bad arguments) is still
                // reported through the ordinary path once report_errors
                // is true; harmless to also run silently here.
                check_call_arguments(expr, state, body, signatures, report_errors);
                return "";
            }
            check_call_arguments(expr, state, body, signatures, report_errors);
            size_t elided_index = *sig_it->second.elided_param_index;
            // The elided parameter's *own* argument is what the call's
            // result transitively borrows from -- resolve it the exact
            // same way as any other borrow source (recursively, so a
            // chain of reference-returning calls is followed all the
            // way back to a real place).
            return resolve_borrow_source_root(*expr.args[elided_index], state, body, signatures, report_errors,
                                               allow_call_source);
        }

        default:
            if (report_errors) {
                throw DataflowError("a reference can currently only borrow a plain local variable, a field of "
                                     "one ('a.b'), an array element of one ('arr[i]'), or the result of a call "
                                     "to a reference-returning function -- not an arbitrary expression");
            }
            return "";
    }
}

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
void apply_reference_argument(const Expr& arg, const Type& param_type, DataflowState& state,
                               BorrowMap& in_call_borrows, const Body& body, const Signatures& signatures,
                               bool report_errors) {
    // resolve_borrow_source_root may have real (move-tracking) side
    // effects on `state` via nested apply_expr calls (e.g. a subscript
    // index) that must apply on *every* pass, not just the reporting
    // one -- unlike the rest of this function, which is purely
    // diagnostic (a call argument's borrow never outlives the call, so
    // there's nothing else here for a later statement's fixed-point
    // computation to observe).
    std::string root = resolve_borrow_source_root(arg, state, body, signatures, report_errors,
                                                   /*allow_call_source=*/false);
    if (!report_errors) return;

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
    bool is_reborrow_of_live_reference = arg.kind == ExprKind::Identifier && state.ref_targets.contains(arg.name);
    if (is_reborrow_of_live_reference) {
        bool source_is_mutable = body.local_types.at(arg.name).is_mutable_ref;
        if (is_mutable && !source_is_mutable) {
            throw DataflowError("cannot pass '" + arg.name + "' by mutable reference: it is itself only a "
                                                              "shared (const) reference");
        }
    } else {
        auto persistent_it = state.borrows.find(root);
        bool persistent_conflict =
            persistent_it != state.borrows.end() &&
            (is_mutable ? (persistent_it->second.mutable_borrow || persistent_it->second.shared_count > 0)
                        : persistent_it->second.mutable_borrow);
        if (persistent_conflict) {
            throw DataflowError("cannot pass '" + root + "' by " + std::string(is_mutable ? "mutable " : "") +
                                 "reference: it is already borrowed");
        }
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

void check_call_arguments(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                           bool report_errors) {
    auto sig_it = signatures.find(expr.name);
    // Scratch borrow-map shared by every reference argument of *this*
    // call only (see apply_reference_argument) -- never merged into
    // `state`, since none of these transient borrows outlive the call.
    BorrowMap in_call_borrows;
    for (size_t i = 0; i < expr.args.size(); i++) {
        const Expr& arg = *expr.args[i];
        bool param_is_reference =
            sig_it != signatures.end() && i < sig_it->second.param_types.size() &&
            is_reference(sig_it->second.param_types[i]);
        if (param_is_reference) {
            apply_reference_argument(arg, sig_it->second.param_types[i], state, in_call_borrows, body, signatures,
                                      report_errors);
        } else {
            // Parameter types aren't resolved here for anything other
            // than reference detection above; a unique_ptr argument
            // must be `std::move(x)` regardless of position, so
            // std::move is simply allowed in every non-reference
            // argument slot.
            apply_expr(arg, /*is_unique_ptr_rvalue_context=*/true, state, body, signatures, report_errors);
        }
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
                    if (report_errors && assignment_target_is_read_only(*expr.lhs, body, signatures)) {
                        throw DataflowError("cannot assign to this place: it is reached through a read-only "
                                             "(const) reference");
                    }
                }
                return;
            }
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::Call:
            check_call_arguments(expr, state, body, signatures, report_errors);
            return;

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
// resolve_borrow_source_root), not necessarily `place` itself, so a
// chain of reference-to-reference bindings (and a `.field`/`[index]`
// projection off a plain place) is tracked precisely.
void apply_reference_binding(const MirStatement& stmt, DataflowState& state, const Body& body,
                              const Signatures& signatures, bool report_errors) {
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

    std::string root = resolve_borrow_source_root(*stmt.expr, state, body, signatures, report_errors,
                                                   /*allow_call_source=*/false);
    if (root.empty()) {
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
            apply_reference_binding(stmt, state, body, signatures, report_errors);
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
            // ScopeExit (if any) is ever reached -- and the only
            // *movable* type (unique_ptr) can't be referenced at all yet
            // (see codegen's validate_reference_pointee), so "move a
            // borrowed place" can't arise either.
            release_reference_borrow(stmt.local, state, body);
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
            if (is_reference(fn.return_type)) {
                // The elision rule (spec ch05.3) was already validated
                // structurally once for the whole program (see
                // resolve_elided_param_index, called from check_moves)
                // -- signatures.at(fn.name).elided_param_index is
                // guaranteed to have a value here. What's left to check
                // per return statement is the actual *dangling* risk:
                // does this specific returned expression really borrow
                // (directly, or transitively through a chain of
                // locals/calls) from that one parameter, or does it
                // borrow something else -- most importantly, a purely
                // local place, which would dangle the instant this
                // function returns and its stack frame is popped.
                const std::string& elided_param_name =
                    fn.params[*signatures.at(fn.name).elided_param_index].name;
                std::string returned_root = resolve_borrow_source_root(
                    *term.return_value, state, body, signatures, /*report_errors=*/true, /*allow_call_source=*/true);
                if (returned_root != elided_param_name) {
                    throw DataflowError(
                        "function '" + fn.name + "' returns a reference derived from '" + returned_root +
                        "', not from its sole reference parameter '" + elided_param_name +
                        "'; scpp v0.1 can only prove a returned reference doesn't dangle when it borrows "
                        "(directly or transitively) from that parameter (spec ch05.3)");
                }
                return;
            }
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

    // Precomputed once, up front: which reference locals are still live
    // (may be used again) immediately after each statement -- see
    // compute_reference_liveness. Consulted after every apply_statement
    // call below (in *both* passes) to release a reference's borrow as
    // soon as its last use has happened, rather than waiting for its
    // ScopeExit -- the NLL upgrade from spec ch05.3.
    std::vector<std::vector<LiveSet>> live_after = compute_reference_liveness(body, preds);

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

} // namespace
} // namespace scpp

export namespace scpp {

void check_moves(const Program& program) {
    Signatures signatures;
    for (const Function& fn : program.functions) {
        FunctionSignature sig;
        sig.param_types.reserve(fn.params.size());
        for (const Param& param : fn.params) {
            sig.param_types.push_back(param.type);
        }
        sig.return_type = fn.return_type;
        sig.elided_param_index = resolve_elided_param_index(fn);
        signatures[fn.name] = std::move(sig);
    }
    for (const Function& fn : program.functions) {
        check_function(fn, signatures);
    }
}

} // namespace scpp
