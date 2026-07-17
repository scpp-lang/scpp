#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "movecheck.h"
#include "ast.h"
#include "state.h"
#include "types.h"
#include "signatures.h"
#include "calls.h"
#include "borrows.h"

namespace scpp {

void apply_lambda_captures(const Expr& expr, DataflowState& state, BorrowMap& reference_capture_borrows,
                           const Body& body, const Signatures& signatures, bool report_errors,
                           std::vector<ClosureCaptureBorrow>* out_closure_capture_borrows = nullptr);
void collect_locally_declared_names(const Stmt& stmt, std::unordered_set<std::string>& out);
void collect_free_identifiers(const Expr& expr, const std::unordered_set<std::string>& excluded,
                              std::unordered_set<std::string>& out);
void collect_free_identifiers(const Stmt& stmt, const std::unordered_set<std::string>& excluded,
                              std::unordered_set<std::string>& out);
void rewrite_captured_identifiers_as_field_access(Stmt& stmt,
                                                  const std::unordered_set<std::string>& captured_names);
void rewrite_captured_identifiers_as_field_access(Expr& expr,
                                                  const std::unordered_set<std::string>& captured_names);
void reject_write_to_nonmutable_by_value_capture(const Expr& expr,
                                                 const std::unordered_set<std::string>& by_value_names);
void reject_write_to_nonmutable_by_value_capture(const Stmt& stmt,
                                                 const std::unordered_set<std::string>& by_value_names);

} // namespace scpp
