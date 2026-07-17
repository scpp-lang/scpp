#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "movecheck.h"
#include "ast.h"
#include "mir.h"
#include "state.h"
#include "types.h"

namespace scpp {

struct FunctionSignature {
    std::vector<Type> param_types;
    std::vector<std::string> param_names;
    std::vector<bool> param_require_thread_movable;
    std::vector<bool> param_require_thread_shareable;
    std::vector<LifetimeAnnotation> param_lifetimes;
    Type return_type;
    LifetimeAnnotation return_lifetime;
    std::vector<size_t> returned_lifetime_param_indices;
    std::optional<size_t> elided_param_index;
    bool is_extern_c_declaration_only = false;
    bool is_unsafe = false;
    bool is_nodiscard = false;
    std::string nodiscard_reason;
    bool is_compile_time_dependency = false;
    std::string owning_module;
    std::string member_owner_class;
    bool is_static = false;
    AccessSpecifier access = AccessSpecifier::Public;
    SourceLocation loc;
    ReceiverRefQualifier receiver_ref_qualifier = ReceiverRefQualifier::None;
};

using Signatures = std::unordered_map<std::string, std::vector<FunctionSignature>>;

[[nodiscard]] bool has_user_declared_copy_ctor(const std::string& class_name, const Program& program);
[[nodiscard]] bool has_user_declared_copy_assign(const std::string& class_name, const Program& program);
[[nodiscard]] bool has_user_declared_dtor(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_field_copy_constructible(const Type& type, const Program& program);
[[nodiscard]] bool is_field_copy_assignable(const Type& type, const Program& program);
[[nodiscard]] bool is_constructor_function(const Function& fn);
[[nodiscard]] bool class_has_any_constructor(const std::string& class_name, const Program& program);
[[nodiscard]] std::string unqualified_template_base_name(std::string_view class_name);
[[nodiscard]] bool names_direct_base(const std::string& member_name, const ClassDef& def);
void collect_virtual_interface_bases_in_construction_order(const Program& program, const ClassDef& def,
                                                           std::vector<const ClassDef*>& out,
                                                           std::unordered_set<std::string>& seen);
[[nodiscard]] std::vector<const ClassDef*> collect_virtual_interface_bases_in_construction_order(
    const Program& program, const ClassDef& def);
[[nodiscard]] const MemberInitializer* find_explicit_interface_initializer(const Function& ctor,
                                                                           const ClassDef& interface_def);
[[nodiscard]] const MemberInitializer* find_explicit_base_initializer(const Function& ctor, const ClassDef& def);
void validate_constructor_member_initialization(const Function& ctor, const ClassDef& def, const Program& program);
[[nodiscard]] bool is_copy_constructible(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_copy_assignable(const std::string& class_name, const Program& program);

[[nodiscard]] const FunctionSignature* resolve_constructor_signature(const std::string& class_name,
                                                                     const std::vector<ExprPtr>& ctor_args,
                                                                     const Body& body,
                                                                     const Signatures& signatures);
void ensure_implicit_default_construction_is_valid(const std::string& class_name,
                                                   std::string_view current_class,
                                                   const Body& body,
                                                   const Signatures& signatures,
                                                   const SourceLocation& loc,
                                                   std::string_view context_message);
void validate_constructor_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
                                              const Signatures& signatures);
void validate_constructor_virtual_interface_base_initialization(const Function& ctor, const ClassDef& def,
                                                                const Body& body,
                                                                const Signatures& signatures);
[[nodiscard]] std::optional<size_t> resolve_elided_param_index(const Function& fn);
[[nodiscard]] bool param_can_outlive_call_for_lifetime_return(const Param& param);
void validate_lifetime_annotation_placement(const Function& fn);
[[nodiscard]] std::vector<size_t> resolve_returned_lifetime_param_indices(const Function& fn);
[[nodiscard]] Signatures build_signatures(const Program& program);

} // namespace scpp
