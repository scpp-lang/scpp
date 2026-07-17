#pragma once

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

enum class LocalState { Bottom, Initialized, MovedOut, Conflict };

using StateMap = std::unordered_map<std::string, LocalState>;
using RootSet = std::vector<std::string>;

struct BorrowState {
    int shared_count = 0;
    bool mutable_borrow = false;

    bool operator==(const BorrowState&) const = default;
};

using BorrowMap = std::unordered_map<std::string, BorrowState>;

struct RefTarget {
    RootSet roots;
    std::string lender;

    [[nodiscard]] bool is_reborrow() const { return !lender.empty(); }

    bool operator==(const RefTarget&) const = default;
};

using RefTargetMap = std::unordered_map<std::string, RefTarget>;
using ReborrowSuspensionMap = std::unordered_map<std::string, int>;
using LocalLifetimeSourceMap = std::unordered_map<std::string, RootSet>;
using ParameterLifetimeMap = std::unordered_map<std::string, LifetimeAnnotation>;

struct ClosureCaptureBorrow {
    std::string root;
    bool is_mutable = false;

    bool operator==(const ClosureCaptureBorrow&) const = default;
};

using ClosureCaptureBorrowMap = std::unordered_map<std::string, std::vector<ClosureCaptureBorrow>>;
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
RootSet canonicalize_roots(RootSet roots);
[[nodiscard]] RootSet single_root(std::string root);
RootSet union_roots(RootSet lhs, const RootSet& rhs);
[[nodiscard]] std::string format_roots(const RootSet& roots);
[[nodiscard]] LocalState lookup(const StateMap& state, const std::string& name);

} // namespace scpp
