#pragma once

#include <optional>
#include <string>
#include <vector>

#include "movecheck.h"
#include "ast.h"
#include "mir.h"
#include "state.h"
#include "types.h"
#include "signatures.h"

namespace scpp {

struct CalleeSignature {
    std::string key;
    size_t param_offset = 0;
    std::optional<FunctionSignature> direct_signature;
};

[[nodiscard]] FunctionSignature function_pointer_signature(const Type& type);
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);
        void check_enum_conversion_compatibility(const Type& target_type, const Expr& source_expr, const Body& body,
                                                 const Signatures& signatures, const SourceLocation& loc);
[[nodiscard]] CalleeSignature resolve_callee_signature(const Expr& call_expr, const Body& body,
                                                       const ClassFieldTypes* class_field_types = nullptr);
struct NodiscardInfo {
            std::string subject;
            std::string reason;
        };

        [[nodiscard]] const NodiscardInfo* nodiscard_info_for_named_type(const Type& type, const Body& body);
        [[nodiscard]] const NodiscardInfo* nodiscard_info_for_discarded_call(const Expr& expr, const Body& body,
                                                                             const Signatures& signatures);

        [[nodiscard]] bool is_named_class_type(const Type& type, const Body& body);
[[nodiscard]] bool is_named_record_type_for_call_binding(const Type& type, const Body& body);
[[nodiscard]] bool compile_time_dependency_visible_in_body(const FunctionSignature& candidate, const Body& body);
[[nodiscard]] bool is_copyable_class_lvalue_boundary_source(const Expr& expr, const Type& target_type,
                                                            const Body& body,
                                                            const Signatures& signatures);
[[nodiscard]] bool is_implicit_move_return_source(const Expr& expr, const Type& target_type, const Body& body);
[[nodiscard]] const FunctionSignature* find_single_argument_converting_constructor_signature(
            const Type& class_type, const Expr& arg, const Body& body, const Signatures& signatures);
        [[nodiscard]] bool argument_type_matches_parameter(const Type& arg_type, const Type& param_type, const Body& body);
[[nodiscard]] bool const_reference_binds_materialized_temporary(const Expr& arg, const Type& param_type,
                                                                const Body& body,
                                                                const Signatures& signatures);
[[nodiscard]] bool argument_matches_parameter(const Expr& arg, const Type& param_type, const Body& body,
                                              const Signatures& signatures);
[[nodiscard]] bool argument_matches_parameter_for_constructor_selection(const Expr& arg,
                                                                        const Type& param_type,
                                                                        const Body& body,
                                                                        const Signatures& signatures);
[[nodiscard]] bool receiver_matches_method_qualifier(const Expr& receiver_expr,
                                                     const FunctionSignature& candidate,
                                                     const Body& body,
                                                     const Signatures& signatures);
[[nodiscard]] const FunctionSignature* resolve_overload(const Expr& call_expr, const CalleeSignature& callee,
                                                        const Body& body, const Signatures& signatures);
[[nodiscard]] const FunctionSignature* find_const_blocked_method_candidate(const Expr& call_expr,
                                                                           const CalleeSignature& callee,
                                                                           const Body& body,
                                                                           const Signatures& signatures);
[[nodiscard]] Type function_pointer_type_from_signature(const FunctionSignature& sig);
[[nodiscard]] bool same_function_pointer_shape_ignoring_unsafe(const Type& a, const Type& b);
[[nodiscard]] std::optional<Type> resolve_function_designator_type(const Expr& expr, const Type& target_type,
                                                                   const Body& body,
                                                                   const Signatures& signatures);
void check_function_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                       const Signatures& signatures, SourceLocation loc,
                                       const std::string& target_name, bool report_errors);
void check_raw_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                  const Signatures& signatures, SourceLocation loc,
                                  const std::string& target_name, bool report_errors);
[[nodiscard]] bool assignment_target_is_read_only(const Expr& expr, const Body& body,
                                                  const Signatures& signatures);
void validate_sizeof_operand(const Expr& expr, const Body& body, const Signatures& signatures,
                                    const SourceLocation& loc);
        [[nodiscard]] std::optional<std::string> direct_write_root(const Expr& expr, const Body& body);
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                           const Signatures& signatures);

} // namespace scpp
