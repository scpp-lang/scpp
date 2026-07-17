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
#include "borrows.h"
#include "threadsafety.h"
#include "lambdas.h"

namespace scpp {

[[nodiscard]] bool binary_expr_has_compatible_types(const Expr& expr, const Body& body,
                                                    const Signatures& signatures);
[[nodiscard]] bool binary_expr_has_valid_arithmetic_types(const Expr& expr, const Body& body,
                                                          const Signatures& signatures);
void check_binary_expr_operand_types(const Expr& expr, const Body& body, const Signatures& signatures,
                                     const SourceLocation& loc);
[[nodiscard]] std::optional<Type> resolve_member_field_type(const Expr& member_expr, const Body& body,
                                                            const DataflowState& state);
void validate_deref_operand(const Expr& operand, const DataflowState& state, const Body& body,
                            const Signatures& signatures);
void apply_deref(const Expr& expr, const DataflowState& state, const Body& body, const Signatures& signatures,
                 bool report_errors);
void apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
                const Signatures& signatures, bool report_errors);
void check_call_arguments(const Expr& expr, DataflowState& state, const Body& body,
                          const Signatures& signatures, bool report_errors);
void apply_reference_argument(const Expr& arg, const Type& param_type, DataflowState& state,
                              BorrowMap& in_call_borrows, const Body& body,
                              const Signatures& signatures, bool report_errors);
void check_constructor_arguments(const std::string& class_name, const std::vector<ExprPtr>& ctor_args,
                                 DataflowState& state, const Body& body, const Signatures& signatures,
                                 bool report_errors);
[[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type,
                                                 const Body& body, const Signatures& signatures);
void apply_statement(const MirStatement& stmt, DataflowState& state, const Body& body,
                     const Signatures& signatures, bool report_errors);
void check_terminator(const Terminator& term, DataflowState& state, const Function& fn, const Body& body,
                      const Signatures& signatures);
void check_function(const Function& fn, const Program& program, const Signatures& signatures,
                    const std::unordered_set<std::string>& class_names,
                    const ClassFieldTypes& class_field_types,
                    const ClassFieldAccess& class_field_access,
                    const std::unordered_set<std::string>& classes_with_copy_ctor,
                    const std::unordered_set<std::string>& classes_with_copy_assign,
                    const std::unordered_set<std::string>& witness_class_names);
void check_moves_impl(const Program& program);

} // namespace scpp
