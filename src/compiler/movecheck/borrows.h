#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "movecheck.h"
#include "ast.h"
#include "mir.h"
#include "state.h"
#include "types.h"
#include "signatures.h"
#include "calls.h"

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
void apply_address_of(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                      bool report_errors);

} // namespace scpp
