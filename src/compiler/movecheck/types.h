#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "movecheck.h"
#include "ast.h"
#include "mir.h"
#include "state.h"

namespace scpp {

[[nodiscard]] bool is_reference(const Type& type);
[[nodiscard]] bool is_span(const Type& type);
[[nodiscard]] bool is_pointer(const Type& type);
[[nodiscard]] bool is_lifetime_eligible_type(const Type& type);
[[nodiscard]] bool is_function_pointer(const Type& type);
[[nodiscard]] bool is_for_range_size_builtin(const Expr& expr);
[[nodiscard]] bool is_synthesized_for_range_storage(std::string_view name);
[[nodiscard]] bool is_reborrowable_local_type(const Type& type);
[[nodiscard]] bool local_is_suspended_for_reborrow(std::string_view name, const DataflowState& state);
[[nodiscard]] bool is_explicit_star_this(const Expr& expr);

[[nodiscard]] bool is_scalar_type_name(const std::string& name);
[[nodiscard]] bool is_integral_scalar_type_name(const std::string& name);
[[nodiscard]] const EnumDef* find_enum_def(const Program* program, const std::string& name);
[[nodiscard]] const EnumVariant* find_enum_variant(const Program* program, const std::string& name,
                                                  const EnumDef** owning_enum = nullptr);
[[nodiscard]] bool is_enum_type(const Type& type, const Program* program);
[[nodiscard]] const Type* enum_underlying_type(const Type& type, const Program* program);

[[nodiscard]] const ClassDef* find_class_def(const Program& program, const std::string& class_name);
[[nodiscard]] bool type_contains_lifetime_carrying_state(const Type& type, const Program& program,
                                                         std::unordered_set<std::string> visiting = {});
[[nodiscard]] std::string named_type_name(const Type& type);
[[nodiscard]] bool types_equal(const Type& a, const Type& b);
[[nodiscard]] bool raw_pointer_implicitly_convertible(const Type& source, const Type& target);
[[nodiscard]] bool is_scalar_named_type(const Type& type);
[[nodiscard]] bool is_float_named_type(const Type& type);
[[nodiscard]] bool integer_literal_compatible_with_type(const Type& type);
[[nodiscard]] const Type& binary_operand_type(const Type& type);
[[nodiscard]] bool is_pointer_arithmetic_offset_type(const Type& type);
[[nodiscard]] bool pointer_supports_arithmetic(const Type& type);
[[nodiscard]] std::optional<Type> pointer_arithmetic_result_type(BinaryOp op, const Type& lhs, const Type& rhs);
[[nodiscard]] bool literal_compatible_with_type(const Expr& literal, const Type& type);

[[nodiscard]] std::string enclosing_class_name(const Body& body);
[[nodiscard]] bool is_interface_representation_type(const Type& type, const Program& program);
[[nodiscard]] bool has_accessible_base_conversion(const Program& program, const std::string& source_name,
                                                  const std::string& target_name,
                                                  std::string_view current_class);
[[nodiscard]] bool types_compatible_with_base_conversion(const Type& source_type, const Type& target_type,
                                                         const Program& program,
                                                         std::string_view current_class);

} // namespace scpp
