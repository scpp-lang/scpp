module;

module scpp.compiler.movecheck:state;

import std;
import scpp.ast;
import scpp.mir;

namespace scpp {

enum class LocalState { Bottom, Initialized, MovedOut, Conflict };

// Every one of these maps is keyed by *declaration* (LocalId), not by
// name. Two same-named locals in sibling scopes are two different keys,
// so one's state can never be read or clobbered as the other's -- and an
// inner shadow's ScopeExit can no longer reset the outer local it hides.
// Diagnostics recover the source spelling with Body::name_of.
//
// Move state specifically is keyed one step finer: by *place* (mir.cppm)
// -- the same declaration identity plus a projection path naming a
// subobject of it. A whole local is the empty path, so every key that
// used to be a bare LocalId still exists unchanged; what is new is that
// `s.a` and `s.b` are now two keys instead of one, which is what lets
// spec §6.2's states apply to an object "of member storage duration"
// (§6.2(1)) at all. There is exactly one move-state map, and this is it.
using StateMap = std::unordered_map<Place, LocalState, PlaceHash>;
using RootSet = std::vector<LocalId>;

struct BorrowState {
    int shared_count = 0;
    bool mutable_borrow = false;

    bool operator==(const BorrowState&) const = default;
};

using BorrowMap = std::unordered_map<LocalId, BorrowState>;

struct RefTarget {
    RootSet roots;
    // Set only when this reference was tracked by suspending a
    // mutable-reborrow lender rather than by incrementing root borrows.
    std::optional<LocalId> lender;
    // The place this binding was bound to, when it names one --
    // `roots` records which whole locals the borrow is *accounted*
    // against (deliberately coarse: a borrow of `s.a` conflicts with a
    // borrow of `s`), which is a different question from which object
    // the name now denotes. `S& r = s;` makes `r.a` and `s.a` the same
    // object, and move state has to agree about that or moving through
    // one alias would leave the other reading as still-initialized.
    std::optional<Place> bound_place;
    // False when bound_place merely *contains* the object bound (a
    // non-constant subscript: `S& r = arr[i];` records `arr`). Such a
    // place is enough to prove two accesses disjoint, but not to record
    // an ownership-state transition against -- marking `arr` moved-out
    // because `r` was moved would skip destroying every other element.
    bool bound_place_is_exact = false;
    // Whether *this* binding itself was mutable, captured once at bind
    // time (see apply_reference_binding). Now that local_decls is keyed
    // by declaration this could equally be re-derived at release time --
    // it is kept because it records the mutability the binding was
    // *tracked* with, which is what release must undo, and reading it
    // costs nothing.
    bool is_mutable = false;

    [[nodiscard]] bool is_reborrow() const { return lender.has_value(); }

    bool operator==(const RefTarget&) const = default;
};

using RefTargetMap = std::unordered_map<LocalId, RefTarget>;

// Mirrors BorrowState's own shared/mutable split exactly: a lender can
// simultaneously back any number of *shared* (const) reborrows -- e.g.
// two separate `const T& a = this->peek();`/`const U& b =
// this->other_const_accessor();` bindings both derived from the same
// mutable `this`, alive together, exactly as real Rust allows any number
// of simultaneous `&T` reborrows of a `&mut T` -- but at most one
// *mutable* reborrow, which (like a real `&mut` reborrow) requires
// exclusivity against every other reborrow, shared or mutable alike.
struct ReborrowSuspension {
    int shared_count = 0;
    bool mutable_suspended = false;

    bool operator==(const ReborrowSuspension&) const = default;
};

using ReborrowSuspensionMap = std::unordered_map<LocalId, ReborrowSuspension>;
using LocalLifetimeSourceMap = std::unordered_map<LocalId, RootSet>;
using ParameterLifetimeMap = std::unordered_map<std::string, LifetimeAnnotation>;
// A root that outlives every local: the storage behind a `static` local
// or a string literal. It is deliberately *not* an index into
// local_decls -- Body::is_valid_local rejects it -- so any attempt to ask
// for its type or its declaration fails loudly instead of aliasing a real
// local. format_roots spells it out for diagnostics.
inline constexpr LocalId kProgramLifetimeRoot = static_cast<LocalId>(static_cast<std::size_t>(-1));
inline constexpr std::string_view kProgramLifetimeRootName = "<program-lifetime>";

// One by-reference capture's hold on the enclosing frame, kept so it can
// be released when the closure variable dies
// (release_closure_capture_borrows). Which of the two forms it takes is
// the same distinction reborrow_is_tracked_against_lender draws for
// every other borrow in the language:
//  - `lender` unset: the capture borrowed an owned local outright, so it
//    holds `borrows[root]` -- `[&x]` where `x` is an `int`.
//  - `lender` set: the capture *reborrowed* an already-bound reference/
//    span local, which already holds the one live access to `root`, so
//    it holds `suspended_reborrows[*lender]` instead -- `[&r]` where `r`
//    is `int& r = x;`. Entering such a capture against `root` would
//    double-count the very borrow `r` itself installed.
struct ClosureCaptureBorrow {
    LocalId root{};
    bool is_mutable = false;
    std::optional<LocalId> lender;

    bool operator==(const ClosureCaptureBorrow&) const = default;
};

using ClosureCaptureBorrowMap = std::unordered_map<LocalId, std::vector<ClosureCaptureBorrow>>;
using ClassFieldTypes = std::unordered_map<std::string, std::unordered_map<std::string, Type>>;
using ClassFieldAccess = std::unordered_map<std::string, std::unordered_map<std::string, AccessSpecifier>>;

struct DataflowState {
    StateMap locals;
    BorrowMap borrows;
    RefTargetMap ref_targets;
    LocalLifetimeSourceMap local_lifetime_sources;
    ParameterLifetimeMap parameter_lifetimes;
    ReborrowSuspensionMap suspended_reborrows;
    ClosureCaptureBorrowMap closure_capture_borrows;
    int unsafe_depth = 0;
    std::string current_class;
    // Non-empty only inside a lambda's synthesized `_call` method (see
    // Function::access_context_class's own doc comment, ast.cppm) --
    // the *additional* class whose private members are also accessible
    // here (the lambda's lexically enclosing class), alongside
    // current_class (which for such a method names the closure's own,
    // unrelated synthetic class). grants_private_access (dataflow.cppm)
    // is the only reader; every ordinary function leaves this empty.
    std::string lexical_access_context_class;
    const std::unordered_set<std::string>* class_names = nullptr;
    const ClassFieldTypes* class_field_types = nullptr;
    const ClassFieldAccess* class_field_access = nullptr;
    const std::unordered_set<std::string>* classes_with_copy_ctor = nullptr;
    const std::unordered_set<std::string>* classes_with_copy_assign = nullptr;
    SourceLocation current_loc;

    [[nodiscard]] bool operator==(const DataflowState& other) const;
};

LocalState join(LocalState a, LocalState b);
StateMap join_maps(const StateMap& a, const StateMap& b);
BorrowState join_borrow(const BorrowState& a, const BorrowState& b);
BorrowMap join_borrow_maps(const BorrowMap& a, const BorrowMap& b);
RefTargetMap join_ref_targets(const RefTargetMap& a, const RefTargetMap& b);
LocalLifetimeSourceMap join_local_lifetime_sources(const LocalLifetimeSourceMap& a,
                                                   const LocalLifetimeSourceMap& b);
ReborrowSuspensionMap join_suspended_reborrows(const ReborrowSuspensionMap& a,
                                               const ReborrowSuspensionMap& b);
ClosureCaptureBorrowMap join_closure_capture_borrows(const ClosureCaptureBorrowMap& a,
                                                     const ClosureCaptureBorrowMap& b);
DataflowState join_states(const DataflowState& a, const DataflowState& b);

[[nodiscard]] std::string describe_bad_state(const std::string& name, LocalState state);
RootSet canonicalize_roots(RootSet roots);[[nodiscard]] RootSet single_root(LocalId root);
RootSet union_roots(RootSet lhs, const RootSet& rhs);
[[nodiscard]] std::string format_root(const Body& body, LocalId root);
[[nodiscard]] std::string format_roots(const Body& body, const RootSet& roots);
[[nodiscard]] LocalState lookup(const StateMap& state, LocalId local);
// The state `place` is in, accounting for every object that contains it.
[[nodiscard]] LocalState lookup(const StateMap& state, const Place& place);
// The place whose recorded entry gives `place` its state under lookup --
// `place` itself when it has its own entry, otherwise the containing
// object that was moved out. A diagnostic has to name what the program
// actually did: `Box b = std::move(a); a.v` moved `a`, not `a.v`.
[[nodiscard]] Place state_source_place(const StateMap& state, const Place& place);
// The lowest-pathed subobject of `place` that is not initialized, if
// any -- what makes `place` itself only *partially* owned, so that
// consuming it whole (spec §6.2(5)/(6)) has to be rejected.
[[nodiscard]] std::optional<Place> find_moved_subobject(const StateMap& state, const Place& place);
// spec §6.2(4): reinitializes `place` and everything it contains.
void reinitialize_place(StateMap& state, const Place& place);
// spec §6.2(3): places `place` -- and, by §6.4(5)'s memberwise move,
// everything it contains -- in the moved-out state.
void mark_place_moved_out(StateMap& state, const Place& place);
// Drops `place` and everything it contains from tracking entirely (its
// storage duration has ended -- MirStatementKind::ScopeExit).
void forget_place_tree(StateMap& state, const Place& place);
[[nodiscard]] RootSet program_lifetime_root();
[[nodiscard]] bool is_program_lifetime_root(LocalId root);

bool DataflowState::operator==(const DataflowState& other) const {
    if (parameter_lifetimes.size() != other.parameter_lifetimes.size()) return false;
    for (const auto& [name, lifetime] : parameter_lifetimes) {
        auto it = other.parameter_lifetimes.find(name);
        if (it == other.parameter_lifetimes.end()) return false;
        if (lifetime.name != it->second.name) return false;
    }
    return locals == other.locals && borrows == other.borrows && ref_targets == other.ref_targets &&
           local_lifetime_sources == other.local_lifetime_sources &&
           suspended_reborrows == other.suspended_reborrows &&
           closure_capture_borrows == other.closure_capture_borrows &&
           unsafe_depth == other.unsafe_depth && current_class == other.current_class &&
           lexical_access_context_class == other.lexical_access_context_class &&
           class_names == other.class_names && class_field_types == other.class_field_types &&
           class_field_access == other.class_field_access &&
           classes_with_copy_ctor == other.classes_with_copy_ctor &&
           classes_with_copy_assign == other.classes_with_copy_assign;
}

LocalState join(LocalState a, LocalState b) {
    if (a == b) return a;
    if (a == LocalState::Bottom) return b;
    if (b == LocalState::Bottom) return a;
    return LocalState::Conflict;
}

// Joins two per-block state snapshots (e.g. the OUT states of two
// predecessors flowing into a shared successor block).
//
// A key missing from one side is *not* Bottom there: a place's state is
// derived from the objects containing it (see lookup), so `s.a` absent
// from a branch that never touched it is Initialized there, by way of
// `s`. Joining against Bottom instead would make "moved on one path
// only" read back as plainly moved-out on both -- the wrong answer in
// the safe direction for the use check, but the wrong answer in the
// *unsafe* direction for anything that asks whether a place still needs
// destroying.
StateMap join_maps(const StateMap& a, const StateMap& b) {
    StateMap result = a;
    for (const auto& [place, state] : b) {
        auto it = result.find(place);
        result[place] = it == result.end() ? join(lookup(a, place), state) : join(it->second, state);
    }
    for (const auto& [place, state] : a) {
        if (b.contains(place)) continue;
        result[place] = join(state, lookup(b, place));
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
    for (const auto& [ref_name, target] : b) {
        result.insert_or_assign(ref_name, target);
    }
    return result;
}

LocalLifetimeSourceMap join_local_lifetime_sources(const LocalLifetimeSourceMap& a, const LocalLifetimeSourceMap& b) {
    LocalLifetimeSourceMap result = a;
    for (const auto& [name, roots] : b) {
        result.insert_or_assign(name, roots);
    }
    return result;
}

ReborrowSuspensionMap join_suspended_reborrows(const ReborrowSuspensionMap& a, const ReborrowSuspensionMap& b) {
    ReborrowSuspensionMap result = a;
    for (const auto& [name, suspension] : b) {
        auto it = result.find(name);
        if (it == result.end()) {
            result[name] = suspension;
        } else {
            it->second.shared_count = std::max(it->second.shared_count, suspension.shared_count);
            it->second.mutable_suspended = it->second.mutable_suspended || suspension.mutable_suspended;
        }
    }
    return result;
}

ClosureCaptureBorrowMap join_closure_capture_borrows(const ClosureCaptureBorrowMap& a, const ClosureCaptureBorrowMap& b) {
    ClosureCaptureBorrowMap result = a;
    for (const auto& [name, borrows] : b) {
        result.insert_or_assign(name, borrows);
    }
    return result;
}

DataflowState join_states(const DataflowState& a, const DataflowState& b) {
    return DataflowState{
        join_maps(a.locals, b.locals),
        join_borrow_maps(a.borrows, b.borrows),
        join_ref_targets(a.ref_targets, b.ref_targets),
        join_local_lifetime_sources(a.local_lifetime_sources, b.local_lifetime_sources),
        a.parameter_lifetimes,
        join_suspended_reborrows(a.suspended_reborrows, b.suspended_reborrows),
        join_closure_capture_borrows(a.closure_capture_borrows, b.closure_capture_borrows),
        // Lexical, not a dataflow fact: check_function overwrites this
        // from BasicBlock::unsafe_depth_on_entry immediately after every
        // join, so what happens here does not decide any block's depth.
        // It used to: joining with `min` pinned a loop head to 0,
        // because the back edge's `out_state` starts default-constructed
        // and `min` never recovers -- see BasicBlock::
        // unsafe_depth_on_entry. `min` is kept for the states this
        // function returns to any other caller, as the same defensive
        // "fail toward not-unsafe" tie-break join_ref_targets uses.
        std::min(a.unsafe_depth, b.unsafe_depth),
        // `current_class`/`class_names`/`class_field_types` are set once
        // per function and never change afterward (see DataflowState's
        // own comments) -- identical on both sides in a well-formed
        // program, so simply keeping `a`'s is enough (no real join
        // needed, same reasoning as `unsafe_depth` just above, minus the
        // "fail safe" tie-break since there's no meaningful direction to
        // fail toward here).
        a.current_class,
        // Same "set once per function, never changes" reasoning as
        // current_class just above.
        a.lexical_access_context_class,
        a.class_names,
        a.class_field_types,
        a.class_field_access,
        // Same "set once, never changes, no real join needed" reasoning
        // as class_names/class_field_types/class_field_access just
        // above.
        a.classes_with_copy_ctor,
        a.classes_with_copy_assign,
        // `current_loc` carries no dataflow meaning at all (see its own
        // comment on DataflowState) and is excluded from operator==, so
        // which side's value ends up here doesn't affect correctness --
        // apply_statement immediately overwrites it for whichever
        // statement runs next anyway. Keeping `a`'s is just the same
        // "no real join needed" shape as every other field above, not a
        // deliberate choice between the two.
        a.current_loc,
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
RootSet canonicalize_roots(RootSet roots) {
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

[[nodiscard]] RootSet single_root(LocalId root) { return RootSet{root}; }

[[nodiscard]] RootSet program_lifetime_root() { return single_root(kProgramLifetimeRoot); }

[[nodiscard]] bool is_program_lifetime_root(LocalId root) { return root == kProgramLifetimeRoot; }

RootSet union_roots(RootSet lhs, const RootSet& rhs) {
    lhs.insert(lhs.end(), rhs.begin(), rhs.end());
    return canonicalize_roots(std::move(lhs));
}

// Roots are LocalIds internally but must never be printed as such: this
// is the single place a root becomes user-visible text, and it always
// goes through Body::name_of (or spells out the synthetic
// program-lifetime root, which names no declaration at all).
[[nodiscard]] std::string format_root(const Body& body, LocalId root) {
    if (is_program_lifetime_root(root)) return "'" + std::string(kProgramLifetimeRootName) + "'";
    return "'" + body.name_of(root) + "'";
}

[[nodiscard]] std::string format_roots(const Body& body, const RootSet& roots) {
    if (roots.empty()) return "<unknown>";
    std::string joined;
    for (std::size_t i = 0; i < roots.size(); i++) {
        if (i != 0) joined += ", ";
        joined += format_root(body, roots[i]);
    }
    return joined;
}
[[nodiscard]] LocalState lookup(const StateMap& state, const Place& place) {
    // A place inherits the worst state of any object that contains it:
    // once `o.i` is moved out, `o.i.q` has been moved out with it (spec
    // §6.4(5) memberwise-moves every subobject), and there is no
    // separate entry recording that. Walking outward rather than
    // eagerly writing an entry per descendant is what keeps the number
    // of keys proportional to what the program actually names.
    Place current = place;
    LocalState result = LocalState::Bottom;
    while (true) {
        auto it = state.find(current);
        if (it != state.end()) {
            if (it->second != LocalState::Initialized) return it->second;
            if (result == LocalState::Bottom) result = LocalState::Initialized;
        }
        if (current.is_whole_local()) break;
        current = current.parent();
    }
    return result;
}

[[nodiscard]] Place state_source_place(const StateMap& state, const Place& place) {
    Place current = place;
    while (true) {
        auto it = state.find(current);
        if (it != state.end() && it->second != LocalState::Initialized) return current;
        if (current.is_whole_local()) return place;
        current = current.parent();
    }
}
[[nodiscard]] LocalState lookup(const StateMap& state, LocalId local) {
    return lookup(state, whole_local_place(local));
}

[[nodiscard]] std::optional<Place> find_moved_subobject(const StateMap& state, const Place& place) {
    std::optional<Place> found;
    for (const auto& [key, value] : state) {
        if (value == LocalState::Initialized || value == LocalState::Bottom) continue;
        if (!key.is_strictly_under(place)) continue;
        // Deterministic across runs: an unordered_map's iteration order
        // is not, and a diagnostic that names a different member on
        // different runs is not a diagnostic anyone can act on.
        if (!found.has_value() || key.path < found->path) found = key;
    }
    return found;
}

void reinitialize_place(StateMap& state, const Place& place) {
    // Writing a whole object reinitializes everything it contains (spec
    // §6.2(4)): the stale MovedOut entry on `s.a` must not outlive
    // `s = ...`, or the next read of `s.a` would report a move that the
    // assignment already undid.
    std::erase_if(state, [&](const auto& entry) { return entry.first.is_strictly_under(place); });
    state[place] = LocalState::Initialized;
}

void mark_place_moved_out(StateMap& state, const Place& place) {
    // Descendant entries are dropped rather than each set MovedOut:
    // lookup already derives a subobject's state from what contains it,
    // so keeping them would be a second copy of the same fact -- and a
    // stale one the moment the place is reinitialized.
    std::erase_if(state, [&](const auto& entry) { return entry.first.is_strictly_under(place); });
    state[place] = LocalState::MovedOut;
}

void forget_place_tree(StateMap& state, const Place& place) {
    std::erase_if(state, [&](const auto& entry) { return entry.first.is_at_or_under(place); });
}

} // namespace scpp
