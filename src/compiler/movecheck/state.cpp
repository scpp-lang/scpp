#include "state.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "movecheck.h"
#include "ast.h"
#include "mir.h"

namespace scpp {

bool DataflowState::operator==(const DataflowState& other) const {
    return locals == other.locals && borrows == other.borrows && ref_targets == other.ref_targets &&
           local_lifetime_sources == other.local_lifetime_sources &&
           parameter_lifetimes == other.parameter_lifetimes &&
           suspended_reborrows == other.suspended_reborrows &&
           closure_capture_borrows == other.closure_capture_borrows &&
           unsafe_depth == other.unsafe_depth && current_class == other.current_class &&
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
    for (const auto& [name, count] : b) {
        auto it = result.find(name);
        result[name] = it == result.end() ? count : std::max(it->second, count);
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
        // In a well-formed program every incoming path agrees on the
        // unsafe nesting depth at a given program point (see
        // DataflowState::unsafe_depth) -- min is just a conservative,
        // defensive tie-break (fail toward "not unsafe", i.e. checks stay
        // on) for whatever a not-yet-rejected, malformed program's
        // fixed-point iteration computes along the way, mirroring
        // join_ref_targets' own comment above.
        std::min(a.unsafe_depth, b.unsafe_depth),
        // `current_class`/`class_names`/`class_field_types` are set once
        // per function and never change afterward (see DataflowState's
        // own comments) -- identical on both sides in a well-formed
        // program, so simply keeping `a`'s is enough (no real join
        // needed, same reasoning as `unsafe_depth` just above, minus the
        // "fail safe" tie-break since there's no meaningful direction to
        // fail toward here).
        a.current_class,
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

[[nodiscard]] RootSet single_root(std::string root) { return RootSet{std::move(root)}; }

RootSet union_roots(RootSet lhs, const RootSet& rhs) {
    lhs.insert(lhs.end(), rhs.begin(), rhs.end());
    return canonicalize_roots(std::move(lhs));
}

[[nodiscard]] std::string format_roots(const RootSet& roots) {
    if (roots.empty()) return "<unknown>";
    if (roots.size() == 1) return "'" + roots.front() + "'";
    std::string joined;
    for (size_t i = 0; i < roots.size(); i++) {
        if (i != 0) joined += ", ";
        joined += "'" + roots[i] + "'";
    }
    return joined;
}
[[nodiscard]] LocalState lookup(const StateMap& state, const std::string& name) {
    auto it = state.find(name);
    return it == state.end() ? LocalState::Bottom : it->second;
}

} // namespace scpp
