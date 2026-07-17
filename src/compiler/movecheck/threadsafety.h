#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "movecheck.h"
#include "ast.h"
#include "types.h"
#include "signatures.h"
#include "calls.h"

namespace scpp {

[[nodiscard]] bool evaluate_thread_bool_constant_expr_for_program(const Expr& expr, const Program& program,
                                                                  std::unordered_set<std::string> visiting = {});
[[nodiscard]] bool thread_movable_of(const Type& type, const Program& program,
                                     std::unordered_set<std::string> visiting = {});
[[nodiscard]] bool thread_shareable_of(const Type& type, const Program& program,
                                       std::unordered_set<std::string> visiting = {});
[[nodiscard]] bool parameter_requires_thread_safety_constraint(const FunctionSignature& sig, size_t param_index);
[[nodiscard]] std::string parameter_display_name(const FunctionSignature& sig, size_t param_index);
[[nodiscard]] bool parameter_names_interface_type(const Type& param_type, const Body& body);
[[nodiscard]] Type thread_safety_constraint_subject_type(const Expr& arg, const Type& param_type,
                                                         const Body& body, const Signatures& signatures);
void enforce_thread_safety_constraints_for_argument(const Expr& arg, const FunctionSignature& sig,
                                                    size_t param_index, std::string_view callee_kind,
                                                    const std::string& callee_name, const Body& body,
                                                    const Signatures& signatures, SourceLocation loc);

} // namespace scpp
