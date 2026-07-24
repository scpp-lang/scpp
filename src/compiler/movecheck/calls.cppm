module;

module scpp.compiler.movecheck:calls;

import std;
import scpp.ast;
import :errors;
import scpp.mir;
import :state;
import :types;
import :signatures;

namespace scpp {

[[nodiscard]] const GlobalVar* find_visible_global_for_expr(const Expr& expr, const Body& body) {
    return find_visible_global(body.program, body.function_namespace_path, expr.name, expr.explicit_global_qualification);
}

struct CalleeSignature {
    std::string key;
    std::size_t param_offset = 0;
    std::optional<FunctionSignature> direct_signature;
};

[[nodiscard]] FunctionSignature function_pointer_signature(const Type& type);
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);
        void check_enum_conversion_compatibility(const Type& target_type, const Expr& source_expr, const Body& body,
                                                 const Signatures& signatures, const SourceLocation& loc);
[[nodiscard]] CalleeSignature resolve_callee_signature(const Expr& call_expr, const Body& body,
                                                       const Signatures& signatures,
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
void validate_alignof_operand(const Expr& expr, const Body& body, const SourceLocation& loc);
        [[nodiscard]] std::optional<std::string> direct_write_root(const Expr& expr, const Body& body);
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                           const Signatures& signatures);

void check_enum_conversion_compatibility(const Type& target_type, const Expr& source_expr, const Body& body,
                                         const Signatures& signatures, const SourceLocation& loc) {
    const Type& target_operand = binary_operand_type(target_type);
    std::optional<Type> source_type = infer_expr_type(source_expr, body, signatures);
    if (!source_type.has_value()) return;
    const Type& source_operand = binary_operand_type(*source_type);
    bool target_is_enum = is_enum_type(target_operand, body.program);
    bool source_is_enum = is_enum_type(source_operand, body.program);
    if (!(target_is_enum || source_is_enum)) return;
    if (types_equal(target_operand, source_operand)) return;
    throw DataflowError("enum class values do not implicitly convert to or from integers (or other enum types) in "
                        "this version; use an explicit cast to the enum's underlying type",
                        loc);
}

[[nodiscard]] FunctionSignature function_pointer_signature(const Type& type) {
    FunctionSignature sig;
    sig.param_types = type.function_params;
    sig.param_names.resize(sig.param_types.size());
    sig.param_default_exprs.assign(sig.param_types.size(), nullptr);
    sig.param_require_thread_movable.assign(sig.param_types.size(), false);
    sig.param_require_thread_shareable.assign(sig.param_types.size(), false);
    sig.return_type = *type.function_return;
    sig.is_unsafe = type.is_unsafe_function_pointer;
    return sig;
}

// Resolves a Call expression's signature-lookup key, accounting for a
// method call's receiver (ch04 §4.2/ch05 §5.9): `obj.name(...)`/
// `this->name(...)` stores its receiver in `call_expr.lhs` and only the
// unqualified method name in `call_expr.name`, but `signatures` (like
// codegen's own `module_->getFunction`) is keyed by the synthesized
// `ClassName_methodName` form (see parse_class_def) -- exactly like
// codegen_call independently resolves the same fact from the receiver's
// type. Scoped to a plain Identifier receiver (covers `this->method()`
// and `obj.method()` for a local/parameter `obj`), a Lambda literal
// receiver (ch05 §5.12's IIFE, e.g. `[](int x){...}(5)` -- already
// resolved to its own synthesized closure class name by the time
// check_moves runs, see monomorphize_generics), or -- only when
// `class_field_types` is supplied (optional: most callers have no
// Program-level field-type info to give it, see DataflowState::
// class_field_types' own comment) -- one more Member projection off a
// plain-Identifier base (`this.field.method()`/`obj.field.method()`),
// resolved through the field's own declared type. A more complex
// receiver expression still falls back to the unqualified name and a
// zero offset, same as an ordinary free-function call (this is *not* a
// general type-checker). Shared by check_call_arguments and
// produces_rvalue_of_type so both resolve a method call's callee
// identically.
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);

namespace {
[[nodiscard]] bool signature_accepts_argument_count(const FunctionSignature& sig, std::size_t arg_count,
                                                    std::size_t param_offset) {
    if (sig.param_types.size() < param_offset) return false;
    std::size_t fixed_param_count = sig.param_types.size() - param_offset;
    std::size_t min_required = fixed_param_count;
    while (min_required > 0 && sig.param_default_exprs[param_offset + min_required - 1] != nullptr) {
        min_required--;
    }
    if (arg_count < min_required) return false;
    if (!sig.has_varargs && arg_count > fixed_param_count) return false;
    return sig.has_varargs || arg_count <= fixed_param_count;
}
}

void check_constructor_arguments(const std::string& class_name, const std::vector<ExprPtr>& ctor_args,
                                  DataflowState& state, const Body& body, const Signatures& signatures,
                                  bool report_errors);
[[maybe_unused]] void maybe_instantiate_generic_constructor_overloads(const std::string& class_name,
                                                                       const std::vector<ExprPtr>& args, Body& body,
                                                                       SourceLocation loc);
[[nodiscard]] CalleeSignature resolve_callee_signature(const Expr& call_expr, const Body& body,
                                                       const Signatures& signatures,
                                                        const ClassFieldTypes* class_field_types) {
    if (call_expr.lhs && call_expr.name.empty()) {
        const Expr* callee_expr = call_expr.lhs.get();
        if (callee_expr->kind == ExprKind::Unary && callee_expr->unary_op == UnaryOp::Deref && callee_expr->lhs) {
            callee_expr = callee_expr->lhs.get();
        }
        if (callee_expr->kind == ExprKind::Identifier) {
            auto type_it = body.local_types.find(callee_expr->name);
            if (type_it != body.local_types.end() && is_function_pointer(type_it->second)) {
                return CalleeSignature{"", 0, function_pointer_signature(type_it->second)};
            }
        } else if (class_field_types != nullptr && callee_expr->kind == ExprKind::Member && callee_expr->lhs &&
                   callee_expr->lhs->kind == ExprKind::Identifier) {
            auto base_it = body.local_types.find(callee_expr->lhs->name);
            if (base_it != body.local_types.end()) {
                const Type& base_type =
                    base_it->second.kind == TypeKind::Reference ? *base_it->second.pointee : base_it->second;
                if (base_type.kind == TypeKind::Named) {
                    auto fields_it = class_field_types->find(base_type.name);
                    if (fields_it != class_field_types->end()) {
                        auto field_it = fields_it->second.find(callee_expr->name);
                        if (field_it != fields_it->second.end() && is_function_pointer(field_it->second)) {
                            return CalleeSignature{"", 0, function_pointer_signature(field_it->second)};
                        }
                    }
                }
            }
        }
    }
    if (call_expr.lhs && !call_expr.name.empty() && class_field_types != nullptr &&
        call_expr.lhs->kind == ExprKind::Identifier) {
        auto base_it = body.local_types.find(call_expr.lhs->name);
        if (base_it != body.local_types.end()) {
            const Type& base_type =
                base_it->second.kind == TypeKind::Reference ? *base_it->second.pointee : base_it->second;
            if (base_type.kind == TypeKind::Named) {
                auto fields_it = class_field_types->find(base_type.name);
                if (fields_it != class_field_types->end()) {
                    auto field_it = fields_it->second.find(call_expr.name);
                    if (field_it != fields_it->second.end() && is_function_pointer(field_it->second)) {
                        return CalleeSignature{"", 0, function_pointer_signature(field_it->second)};
                    }
                }
            }
        }
    }
    if (!call_expr.lhs && !call_expr.explicit_global_qualification && body.local_types.contains(call_expr.name) &&
        is_function_pointer(body.local_types.at(call_expr.name))) {
        return CalleeSignature{"", 0, function_pointer_signature(body.local_types.at(call_expr.name))};
    }
    if (call_expr.lhs) {
        std::string class_name;
        if (call_expr.lhs->kind == ExprKind::Identifier) {
            auto type_it = body.local_types.find(call_expr.lhs->name);
            if (type_it != body.local_types.end()) class_name = named_type_name(type_it->second);
        } else if (is_explicit_star_this(*call_expr.lhs)) {
            auto type_it = body.local_types.find("this");
            if (type_it != body.local_types.end()) class_name = named_type_name(type_it->second);
        } else if (call_expr.lhs->kind == ExprKind::Lambda && !call_expr.lhs->name.empty()) {
            class_name = call_expr.lhs->name;
        } else if (class_field_types != nullptr && call_expr.lhs->kind == ExprKind::Member &&
                   call_expr.lhs->lhs && call_expr.lhs->lhs->kind == ExprKind::Identifier) {
            // ch05 §5.14: needed for check_generic_type_methods_once's
            // own synthesized check functions -- a generic type's method
            // calling another method *on one of its own fields*
            // (`this.item.doubled()`) must still be resolved (and, when
            // `item`'s substituted type turns out to guarantee no such
            // method, correctly left unresolvable) even though the
            // receiver is a Member, not a bare Identifier -- otherwise
            // this falls back to an unqualified, unmangled lookup
            // ("doubled") that (almost) never matches anything real,
            // silently deferring an unresolvable call entirely to
            // codegen -- which never runs at all for a synthetic,
            // check-only function (ClassDef::is_synthetic_check_only),
            // the exact gap this closes.
            auto base_it = body.local_types.find(call_expr.lhs->lhs->name);
            if (base_it != body.local_types.end()) {
                const Type& base_type =
                    base_it->second.kind == TypeKind::Reference ? *base_it->second.pointee : base_it->second;
                if (base_type.kind == TypeKind::Named) {
                    auto fields_it = class_field_types->find(base_type.name);
                    if (fields_it != class_field_types->end()) {
                        auto field_it = fields_it->second.find(call_expr.lhs->name);
                        if (field_it != fields_it->second.end()) class_name = named_type_name(field_it->second);
                    }
                }
            }
        }
        if (class_name.empty()) {
            std::optional<Type> receiver_type = infer_expr_type(*call_expr.lhs, body, signatures);
            if (receiver_type.has_value()) class_name = named_type_name(*receiver_type);
        }
        if (!class_name.empty()) return CalleeSignature{class_name + "_" + call_expr.name, 1, std::nullopt};
    }
    return CalleeSignature{call_expr.name, 0, std::nullopt};
}


// Forward declarations for a small mutually-recursive group implementing
// ch05 §5.10's function-overload resolution:
//  - infer_expr_type needs resolve_overload for a nested Call argument's
//    own return type.
//  - resolve_overload needs argument_matches_parameter to test each
//    candidate, which in turn needs infer_expr_type (to compare argument/
//    parameter types), produces_rvalue_of_type (defined below), and
//    is_read_only_reachable (defined much further below, for the
//    T&-beats-const-T&-for-a-mutable-lvalue tie-break).
// All of this always terminates: every recursive step is into a strictly
// smaller sub-expression.
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] const FunctionSignature* resolve_overload(const Expr& call_expr, const CalleeSignature& callee,
                                                          const Body& body, const Signatures& signatures);
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                            const Signatures& signatures);
// spec §6.5: forward-declared since apply_expr's own Binary/Assign case
// (defined well before is_bare_same_type_copy_source's own definition,
// near is_move_construction_shape) needs it for the Member-target copy-
// assignment eligibility check.
[[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type, const Body& body,
                                                 const Signatures& signatures);
[[nodiscard]] bool is_named_class_type(const Type& type, const Body& body) {
    if (type.kind != TypeKind::Named || body.program == nullptr) return false;
    for (const ClassDef& def : body.program->classes) {
        if (def.name == type.name) return !def.is_concept_witness;
    }
    return false;
}

[[nodiscard]] bool is_named_record_type_for_call_binding(const Type& type, const Body& body) {
    if (is_named_class_type(type, body)) return true;
    if (type.kind != TypeKind::Named || body.program == nullptr) return false;
    for (const StructDef& def : body.program->structs) {
        if (def.name == type.name) return true;
    }
    return false;
}

[[nodiscard]] bool compile_time_dependency_visible_in_body(const FunctionSignature& candidate, const Body& body) {
    if (!candidate.is_compile_time_dependency) return true;
    if (!candidate.owning_module.empty() && candidate.owning_module == body.function_visibility_module) return true;
    return !body.function_source_path.empty() && body.function_source_path == candidate.loc.source_path_text();
}

[[nodiscard]] const NodiscardInfo* nodiscard_info_for_named_type(const Type& type, const Body& body) {
    if (type.kind != TypeKind::Named || body.program == nullptr) return nullptr;
    for (const ClassDef& def : body.program->classes) {
        if (def.name == type.name && def.is_nodiscard) {
            static thread_local NodiscardInfo info;
            info.subject = "type '" + def.name + "'";
            info.reason = def.nodiscard_reason;
            return &info;
        }
    }
    for (const StructDef& def : body.program->structs) {
        if (def.name == type.name && def.is_nodiscard) {
            static thread_local NodiscardInfo info;
            info.subject = "type '" + def.name + "'";
            info.reason = def.nodiscard_reason;
            return &info;
        }
    }
    return nullptr;
}

[[nodiscard]] const NodiscardInfo* nodiscard_info_for_discarded_call(const Expr& expr, const Body& body,
                                                                     const Signatures& signatures) {
    if (expr.kind != ExprKind::Call) return nullptr;
    CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
    if (const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures)) {
        if (sig->is_nodiscard) {
            static thread_local NodiscardInfo info;
            info.subject = "function '" + (callee.key.empty() ? expr.name : callee.key) + "'";
            info.reason = sig->nodiscard_reason;
            return &info;
        }
        if (sig->return_type.kind == TypeKind::Named) return nodiscard_info_for_named_type(sig->return_type, body);
        return nullptr;
    }
    std::optional<Type> inferred = infer_expr_type(expr, body, signatures);
    if (!inferred.has_value() || inferred->kind != TypeKind::Named) return nullptr;
    return nodiscard_info_for_named_type(*inferred, body);
}

[[nodiscard]] bool is_copyable_class_lvalue_boundary_source(const Expr& expr, const Type& target_type, const Body& body,
                                                            const Signatures& signatures) {
    return body.program != nullptr && is_named_record_type_for_call_binding(target_type, body) &&
           is_bare_same_type_copy_source(expr, target_type, body, signatures) &&
           is_copy_constructible(target_type.name, *body.program);
}

[[nodiscard]] bool is_implicit_move_return_source(const Expr& expr, const Type& target_type, const Body& body) {
    if (expr.kind != ExprKind::Identifier || expr.explicit_global_qualification) return false;
    auto it = body.local_types.find(expr.name);
    return it != body.local_types.end() && types_equal(it->second, target_type);
}

// Whether `arg` is a legitimate argument for a candidate overload's
// parameter declared as `param_type`, for exact-type-match overload
// resolution (ch05 §5.10) -- not a full validity check (that's
// check_call_arguments/apply_reference_argument's job, once a specific
// overload has already been picked); this only needs to decide which of
// several candidates is *the* match.
[[nodiscard]] const FunctionSignature* find_single_argument_converting_constructor_signature(
    const Type& class_type, const Expr& arg, const Body& body, const Signatures& signatures);

[[nodiscard]] bool argument_type_matches_parameter(const Type& arg_type, const Type& param_type, const Body& body) {
    if (is_reference(param_type)) {
        if (arg_type.kind == TypeKind::Reference) {
            if (arg_type.pointee == nullptr || param_type.pointee == nullptr) return false;
            if (types_equal(*arg_type.pointee, *param_type.pointee)) {
                return !param_type.is_mutable_ref || arg_type.is_mutable_ref;
            }
            return body.program != nullptr &&
                   types_compatible_with_base_conversion(arg_type, param_type, *body.program, enclosing_class_name(body));
        }
        return param_type.pointee != nullptr &&
               (types_equal(arg_type, *param_type.pointee) ||
                (body.program != nullptr &&
                 types_compatible_with_base_conversion(arg_type, param_type, *body.program, enclosing_class_name(body))));
    }
    if (arg_type.kind == TypeKind::Reference) {
        return (arg_type.pointee != nullptr && types_equal(*arg_type.pointee, param_type)) ||
               (body.program != nullptr &&
                types_compatible_with_base_conversion(arg_type, param_type, *body.program, enclosing_class_name(body)));
    }
    return types_equal(arg_type, param_type) ||
           (body.program != nullptr &&
            types_compatible_with_base_conversion(arg_type, param_type, *body.program, enclosing_class_name(body)));
}

[[nodiscard]] bool const_reference_binds_materialized_temporary(const Expr& arg, const Type& param_type,
                                                                const Body& body, const Signatures& signatures) {
    if (!is_reference(param_type) || param_type.is_rvalue_ref || param_type.is_mutable_ref || param_type.pointee == nullptr) {
        return false;
    }
    if (produces_rvalue_of_type(arg, *param_type.pointee, body, signatures)) return true;
    return is_named_record_type_for_call_binding(*param_type.pointee, body) &&
           find_single_argument_converting_constructor_signature(*param_type.pointee, arg, body, signatures) != nullptr;
}

[[nodiscard]] bool argument_matches_parameter(const Expr& arg, const Type& param_type, const Body& body,
                                                const Signatures& signatures) {
    if (is_reference(param_type) && param_type.is_rvalue_ref) {
        // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- the mirror image of
        // the ordinary-reference case just below: needs a genuine
        // rvalue-producing argument, never a bare place.
        return produces_rvalue_of_type(arg, *param_type.pointee, body, signatures);
    }
    if (is_reference(param_type)) {
        // ch05 §5.x: a *const* reference may bind either to a genuine
        // rvalue of the exact pointee type, or to a freshly
        // materialized temporary built from a converting constructor
        // such as `std::string{"..."}` from a string literal.
        if (const_reference_binds_materialized_temporary(arg, param_type, body, signatures)) {
            return true;
        }
        // A bare lvalue-like place (Identifier/Member/Subscript/a
        // unique_ptr or raw pointer's Deref -- the same shapes
        // resolve_borrow_source_root accepts as a borrow source) is
        // viable against a T&/const T& parameter; std::move/MakeUnique/a
        // literal never is (there's no place to borrow from) unless the
        // rvalue-binding case just above already accepted it.
        if (arg.kind == ExprKind::Move ||
            arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::FloatLiteral ||
            arg.kind == ExprKind::BoolLiteral || arg.kind == ExprKind::CharLiteral ||
            arg.kind == ExprKind::StringLiteral) {
            return false;
        }
        std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
        return arg_type.has_value() && argument_type_matches_parameter(*arg_type, param_type, body);
    }
    std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
    if (!arg_type.has_value()) return false;
    if (!argument_type_matches_parameter(*arg_type, param_type, body)) {
        if (is_named_record_type_for_call_binding(param_type, body) &&
            find_single_argument_converting_constructor_signature(param_type, arg, body, signatures) != nullptr) {
            return true;
        }
        return false;
    }
    if (is_named_record_type_for_call_binding(param_type, body)) {
        return is_copyable_class_lvalue_boundary_source(arg, param_type, body, signatures) ||
               produces_rvalue_of_type(arg, param_type, body, signatures);
    }
    return true;
}

[[nodiscard]] bool constructor_parameter_accepts_argument_directly(const Expr& arg, const Type& param_type,
                                                                   const Body& body, const Signatures& signatures) {
    if (is_reference(param_type) && param_type.is_rvalue_ref) {
        return produces_rvalue_of_type(arg, *param_type.pointee, body, signatures);
    }
    if (is_reference(param_type)) {
        if (!param_type.is_mutable_ref && param_type.pointee != nullptr &&
            produces_rvalue_of_type(arg, *param_type.pointee, body, signatures)) {
            return true;
        }
        if (arg.kind == ExprKind::Move ||
            arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::FloatLiteral ||
            arg.kind == ExprKind::BoolLiteral || arg.kind == ExprKind::CharLiteral ||
            arg.kind == ExprKind::StringLiteral) {
            return false;
        }
        std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
        return arg_type.has_value() && argument_type_matches_parameter(*arg_type, param_type, body);
    }
    std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
    if (!arg_type.has_value() || !argument_type_matches_parameter(*arg_type, param_type, body)) return false;
    if (is_named_record_type_for_call_binding(param_type, body)) {
        return is_copyable_class_lvalue_boundary_source(arg, param_type, body, signatures) ||
               produces_rvalue_of_type(arg, param_type, body, signatures);
    }
    return true;
}

[[nodiscard]] bool argument_matches_parameter_for_constructor_selection(const Expr& arg, const Type& param_type,
                                                                       const Body& body, const Signatures& signatures) {
    return constructor_parameter_accepts_argument_directly(arg, param_type, body, signatures);
}

[[nodiscard]] const FunctionSignature* find_single_argument_converting_constructor_signature(
    const Type& class_type, const Expr& arg, const Body& body, const Signatures& signatures) {
    if (class_type.kind != TypeKind::Named) return nullptr;
    auto it = signatures.find(class_type.name + "_new");
    if (it == signatures.end()) return nullptr;
    for (const FunctionSignature& candidate : it->second) {
        if (!compile_time_dependency_visible_in_body(candidate, body)) continue;
        if (candidate.param_types.size() != 2) continue;
        const Type& ctor_param_type = candidate.param_types[1];
        if (types_equal(ctor_param_type, class_type) ||
            (is_reference(ctor_param_type) && ctor_param_type.pointee != nullptr &&
             types_equal(*ctor_param_type.pointee, class_type))) {
            continue;
        }
        if (constructor_parameter_accepts_argument_directly(arg, candidate.param_types[1], body, signatures)) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] bool receiver_matches_method_qualifier(const Expr& receiver_expr, const FunctionSignature& candidate,
                                                     const Body& body, const Signatures& signatures) {
    if (candidate.param_types.empty() || candidate.param_types[0].kind != TypeKind::Reference ||
        candidate.param_types[0].pointee == nullptr) {
        return true;
    }
    bool receiver_is_rvalue =
        produces_rvalue_of_type(receiver_expr, *candidate.param_types[0].pointee, body, signatures);
    switch (candidate.receiver_ref_qualifier) {
        case ReceiverRefQualifier::None: return true;
        case ReceiverRefQualifier::LValue: return !receiver_is_rvalue;
        case ReceiverRefQualifier::RValue: return receiver_is_rvalue;
    }
    return true;
}

// Resolves `call_expr` to the single FunctionSignature (among possibly
// several overloads sharing `callee.key`'s name) whose parameters match
// this call's actual arguments (ch05 §5.10) -- exact type match only, so
// resolution never needs a conversion-ranking algorithm. Returns nullptr
// when no candidate matches (the caller reports a clear "no matching
// overload" diagnostic) or when `callee.key` names nothing at all.
//
// When strictly more than one candidate matches (only possible via the
// by-value/by-reference axis -- two overloads can never share an
// identical parameter-type list, ch05 §5.10), applies the "T& beats
// const T& for a mutable lvalue" tie-break (reused from real C++,
// resolving the const/non-const method-overloading case, e.g.
// get()/get() const) across every reference-typed parameter position
// (including an implicit `this`, ch05 §5.9) where the matches disagree
// on mutability. If that still doesn't produce a unique winner, this is
// a genuine ambiguity this version has no further tie-break for --
// falls back to the first match found (in declaration order) rather
// than crashing, since v0.1's scalar-only overload sets make actually
// reaching this exceedingly rare in a real, well-formed program.
[[nodiscard]] const FunctionSignature* resolve_overload(const Expr& call_expr, const CalleeSignature& callee,
                                                          const Body& body, const Signatures& signatures) {
    if (callee.direct_signature.has_value()) {
        return signature_accepts_argument_count(*callee.direct_signature, call_expr.args.size(), callee.param_offset)
                   ? &*callee.direct_signature
                   : nullptr;
    }
    auto it = signatures.find(callee.key);
    if (it == signatures.end()) return nullptr;
    // The overwhelmingly common case: exactly one function has ever been
    // declared under this name, so there's nothing to *disambiguate*
    // between -- return it unconditionally, without running any of the
    // exact-type-match/this-mutability machinery below at all. This
    // matters beyond just being a harmless shortcut: infer_expr_type
    // can't resolve every expression shape (Member/Subscript chains,
    // notably -- movecheck has no Program access to their field/element
    // types), so *requiring* a successful match here would wrongly break
    // an ordinary, non-overloaded call whose argument happens to be one
    // of those shapes (e.g. `f(obj.field)`) purely because overload
    // resolution can't prove a match, not because one doesn't exist.
    // Whether this one candidate's parameters actually fit the call's
    // arguments is left to the checks that already existed before
    // overloading (apply_reference_argument, codegen's own type
    // checking, ...), exactly as before this feature.
    if (it->second.size() == 1) {
        const FunctionSignature& only = it->second[0];
        if (!compile_time_dependency_visible_in_body(only, body)) return nullptr;
        if (callee.param_offset == 1 && call_expr.lhs) {
            if (!only.param_types.empty() && is_reference(only.param_types[0]) && only.param_types[0].is_mutable_ref &&
                is_read_only_reachable(*call_expr.lhs, body, signatures)) {
                return nullptr;
            }
            if (!receiver_matches_method_qualifier(*call_expr.lhs, only, body, signatures)) return nullptr;
        }
        if (!signature_accepts_argument_count(only, call_expr.args.size(), callee.param_offset)) return nullptr;
        return &only;
    }

    std::vector<const FunctionSignature*> matches;
    for (const FunctionSignature& candidate : it->second) {
        if (!compile_time_dependency_visible_in_body(candidate, body)) continue;
        if (!signature_accepts_argument_count(candidate, call_expr.args.size(), callee.param_offset)) continue;
        bool all_match = true;
        // The receiver (`this`), for a method call: viable only if the
        // candidate's own `this` mutability doesn't demand more than the
        // receiver place can actually provide (mirrors
        // apply_reference_argument's identical mutable-vs-read-only-
        // reachable check, applied here purely for resolution purposes).
        if (callee.param_offset == 1 && call_expr.lhs && candidate.param_types[0].is_mutable_ref &&
            is_read_only_reachable(*call_expr.lhs, body, signatures)) {
            all_match = false;
        }
        if (all_match && callee.param_offset == 1 && call_expr.lhs &&
            !receiver_matches_method_qualifier(*call_expr.lhs, candidate, body, signatures)) {
            all_match = false;
        }
        std::size_t fixed_param_count = candidate.param_types.size() - callee.param_offset;
        for (std::size_t i = 0; all_match && i < call_expr.args.size() && i < fixed_param_count; i++) {
            all_match = argument_matches_parameter(*call_expr.args[i], candidate.param_types[i + callee.param_offset],
                                                     body, signatures);
        }
        if (all_match) matches.push_back(&candidate);
    }

    if (matches.size() <= 1) return matches.empty() ? nullptr : matches[0];

    // Tie-break: prefer whichever match has the most mutable-reference
    // parameters among positions where the argument is itself a mutable
    // place (including `this`, checked the same way as above) -- the
    // higher-scoring candidate is the more "specific" one a mutable
    // argument licenses, exactly like real C++'s own T&-over-const-T&
    // preference.
    auto mutable_ref_score = [&](const FunctionSignature& candidate) {
        int score = 0;
        if (callee.param_offset == 1 && call_expr.lhs && candidate.param_types[0].is_mutable_ref &&
            !is_read_only_reachable(*call_expr.lhs, body, signatures)) {
            score++;
        }
        if (callee.param_offset == 1 && call_expr.lhs) {
            bool receiver_is_rvalue =
                candidate.param_types[0].kind == TypeKind::Reference && candidate.param_types[0].pointee != nullptr &&
                produces_rvalue_of_type(*call_expr.lhs, *candidate.param_types[0].pointee, body, signatures);
            if ((receiver_is_rvalue && candidate.receiver_ref_qualifier == ReceiverRefQualifier::RValue) ||
                (!receiver_is_rvalue && candidate.receiver_ref_qualifier == ReceiverRefQualifier::LValue)) {
                score += 2;
            }
        }
        std::size_t fixed_param_count = candidate.param_types.size() - callee.param_offset;
        for (std::size_t i = 0; i < call_expr.args.size() && i < fixed_param_count; i++) {
            const Type& param_type = candidate.param_types[i + callee.param_offset];
            if (is_reference(param_type) && param_type.is_mutable_ref &&
                !is_read_only_reachable(*call_expr.args[i], body, signatures)) {
                score++;
            }
        }
        return score;
    };
    const FunctionSignature* best = matches[0];
    int best_score = mutable_ref_score(*best);
    bool unique_best = true;
    for (std::size_t i = 1; i < matches.size(); i++) {
        int score = mutable_ref_score(*matches[i]);
        if (score > best_score) {
            best = matches[i];
            best_score = score;
            unique_best = true;
        } else if (score == best_score) {
            unique_best = false;
        }
    }
    return unique_best ? best : matches[0];
}
[[nodiscard]] const FunctionSignature* find_const_blocked_method_candidate(const Expr& call_expr,
                                                                           const CalleeSignature& callee,
                                                                           const Body& body,
                                                                           const Signatures& signatures) {
    if (callee.param_offset != 1 || !call_expr.lhs || !is_read_only_reachable(*call_expr.lhs, body, signatures)) {
        return nullptr;
    }
    auto it = signatures.find(callee.key);
    if (it == signatures.end()) return nullptr;
    for (const FunctionSignature& candidate : it->second) {
        if (!compile_time_dependency_visible_in_body(candidate, body)) continue;
        if (!signature_accepts_argument_count(candidate, call_expr.args.size(), 1)) continue;
        if (!is_reference(candidate.param_types[0]) || candidate.param_types[0].is_rvalue_ref ||
            !candidate.param_types[0].is_mutable_ref) {
            continue;
        }
        if (!receiver_matches_method_qualifier(*call_expr.lhs, candidate, body, signatures)) continue;
        bool all_match = true;
        std::size_t fixed_param_count = candidate.param_types.size() - 1;
        for (std::size_t i = 0; all_match && i < call_expr.args.size() && i < fixed_param_count; i++) {
            all_match = argument_matches_parameter(*call_expr.args[i], candidate.param_types[i + 1], body, signatures);
        }
        if (all_match) return &candidate;
    }
    return nullptr;
}

[[nodiscard]] Type function_pointer_type_from_signature(const FunctionSignature& sig) {
    Type type;
    type.kind = TypeKind::FunctionPointer;
    type.function_return = std::make_shared<Type>(sig.return_type);
    type.function_params = sig.param_types;
    type.is_unsafe_function_pointer = sig.is_unsafe || sig.is_extern_c_declaration_only;
    return type;
}

[[nodiscard]] bool same_function_pointer_shape_ignoring_unsafe(const Type& a, const Type& b) {
    if (!is_function_pointer(a) || !is_function_pointer(b) || a.function_params.size() != b.function_params.size() ||
        !types_equal(*a.function_return, *b.function_return)) {
        return false;
    }
    for (std::size_t i = 0; i < a.function_params.size(); i++) {
        if (!types_equal(a.function_params[i], b.function_params[i])) return false;
    }
    return true;
}

[[nodiscard]] std::optional<Type> resolve_function_designator_type(const Expr& expr, const Type& target_type,
                                                                   const Body& body, const Signatures& signatures) {
    auto signature_set_for_name = [&](std::string_view name) -> const std::vector<FunctionSignature>* {
        auto it = signatures.find(std::string(name));
        return it == signatures.end() ? nullptr : &it->second;
    };
    auto lookup_name = [&](std::string_view name) -> const std::vector<FunctionSignature>* {
        if (const auto* direct = signature_set_for_name(name)) return direct;
        std::size_t pos = name.rfind("::");
        return pos == std::string_view::npos ? nullptr : signature_set_for_name(name.substr(pos + 2));
    };
    auto same_lookup_name = [](std::string_view lhs, std::string_view rhs) {
        auto tail = [](std::string_view name) {
            std::size_t pos = name.rfind("::");
            return pos == std::string_view::npos ? name : name.substr(pos + 2);
        };
        return lhs == rhs || tail(lhs) == tail(rhs);
    };
    const Expr* source = &expr;
    if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::AddressOf && expr.lhs) source = expr.lhs.get();
    if (source->kind != ExprKind::Identifier || body.local_types.contains(source->name)) return std::nullopt;
    if (find_visible_global(body.program, body.function_namespace_path, source->name, source->explicit_global_qualification) !=
        nullptr) {
        return std::nullopt;
    }
    const auto* candidates = lookup_name(source->name);
    if (candidates == nullptr) return std::nullopt;
    for (const FunctionSignature& sig : *candidates) {
        if (!compile_time_dependency_visible_in_body(sig, body)) continue;
        Type candidate = function_pointer_type_from_signature(sig);
        if (same_function_pointer_shape_ignoring_unsafe(candidate, target_type)) return candidate;
    }
    if (body.program != nullptr && !source->explicit_template_args.empty()) {
        bool saw_visible_generic_template = false;
        auto substitute_type = [&](const auto& self, Type type,
                                   const std::unordered_map<std::string, Type>& type_bindings) -> Type {
            if (type.kind == TypeKind::Named) {
                auto bound = type_bindings.find(type.name);
                if (bound != type_bindings.end()) return bound->second;
                for (Type& arg : type.template_args) arg = self(self, arg, type_bindings);
            }
            if (type.pointee) type.pointee = std::make_shared<Type>(self(self, *type.pointee, type_bindings));
            if (type.element) type.element = std::make_shared<Type>(self(self, *type.element, type_bindings));
            if (type.function_return) {
                type.function_return = std::make_shared<Type>(self(self, *type.function_return, type_bindings));
            }
            for (Type& param : type.function_params) param = self(self, param, type_bindings);
            return type;
        };
        for (const Function& fn : body.program->functions) {
            if (!same_lookup_name(fn.name, source->name) || fn.template_params.empty()) continue;
            std::string fn_visibility_module = fn.visibility_module.empty() ? fn.owning_module : fn.visibility_module;
            bool same_module = !fn_visibility_module.empty() && fn_visibility_module == body.function_visibility_module;
            bool same_source = !body.function_source_path.empty() && body.function_source_path == fn.loc.source_path_text();
            if (!fn.is_exported && !same_module && !same_source) continue;
            saw_visible_generic_template = true;
            std::unordered_map<std::string, Type> type_bindings;
            std::unordered_map<std::string, std::vector<Type>> pack_bindings;
            std::size_t explicit_index = 0;
            bool ok = true;
            for (const GenericTypeParam& tp : fn.template_params) {
                if (tp.is_pack) {
                    if (tp.is_non_type) {
                        ok = false;
                        break;
                    }
                    while (explicit_index < source->explicit_template_args.size()) {
                        const ExplicitTemplateArg& arg = source->explicit_template_args[explicit_index++];
                        if (!arg.is_type) {
                            ok = false;
                            break;
                        }
                        pack_bindings[tp.name].push_back(arg.type);
                    }
                    break;
                }
                if (explicit_index >= source->explicit_template_args.size()) {
                    ok = false;
                    break;
                }
                const ExplicitTemplateArg& arg = source->explicit_template_args[explicit_index++];
                if (tp.is_non_type || !arg.is_type) {
                    ok = false;
                    break;
                }
                type_bindings[tp.name] = arg.type;
            }
            if (!ok || explicit_index != source->explicit_template_args.size()) continue;
            Type candidate;
            candidate.kind = TypeKind::FunctionPointer;
            candidate.function_return = std::make_shared<Type>(substitute_type(substitute_type, fn.return_type, type_bindings));
            candidate.is_unsafe_function_pointer = fn.is_unsafe || fn.is_extern_c;
            for (const Param& param : fn.params) {
                if (param.is_parameter_pack && param.type.kind == TypeKind::Named) {
                    auto pack_it = pack_bindings.find(param.type.name);
                    if (pack_it == pack_bindings.end()) {
                        ok = false;
                        break;
                    }
                    for (const Type& bound : pack_it->second) candidate.function_params.push_back(bound);
                    continue;
                }
                candidate.function_params.push_back(substitute_type(substitute_type, param.type, type_bindings));
            }
            if (ok && same_function_pointer_shape_ignoring_unsafe(candidate, target_type)) return candidate;
        }
        if (saw_visible_generic_template) return target_type;
    }
    return std::nullopt;
}

void check_function_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                       const Signatures& signatures, SourceLocation loc, const std::string& target_name,
                                       bool report_errors) {
    if (!report_errors || !is_function_pointer(target_type)) return;
    std::optional<Type> source_type = resolve_function_designator_type(expr, target_type, body, signatures);
    if (!source_type) source_type = infer_expr_type(expr, body, signatures);
    if (!source_type || !is_function_pointer(*source_type)) {
        throw DataflowError("cannot initialize function pointer '" + target_name +
                             "' from this expression: expected a function or function pointer with matching "
                             "signature",
            loc);
    }
    if (types_equal(target_type, *source_type)) return;
    if (same_function_pointer_shape_ignoring_unsafe(target_type, *source_type) && target_type.is_unsafe_function_pointer &&
        !source_type->is_unsafe_function_pointer) {
        return;
    }
    if (same_function_pointer_shape_ignoring_unsafe(target_type, *source_type) && !target_type.is_unsafe_function_pointer &&
        source_type->is_unsafe_function_pointer) {
        throw DataflowError("cannot assign an unsafe-qualified function pointer to plain function pointer '" +
                                 target_name + "'",
            loc);
    }
}

void check_raw_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                   const Signatures& signatures, SourceLocation loc, const std::string& target_name,
                                   bool report_errors) {
    if (!report_errors || target_type.kind != TypeKind::Pointer) return;
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    if (!source_type || source_type->kind != TypeKind::Pointer) return;
    if (raw_pointer_implicitly_convertible(*source_type, target_type)) return;
    if (body.program != nullptr &&
        types_compatible_with_base_conversion(*source_type, target_type, *body.program, enclosing_class_name(body))) {
        return;
    }
    throw DataflowError("cannot initialize or assign raw pointer '" + target_name +
                            "' from an incompatible pointer type without an explicit cast",
                        loc);
}

// Structurally validates and resolves spec ch05.3's elision rule for a
[[nodiscard]] bool assignment_target_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            if (it != body.local_types.end()) {
                return body.const_locals.contains(expr.name) ||
                       ((is_reference(it->second) || is_span(it->second)) && !it->second.is_mutable_ref);
            }
            if (const GlobalVar* global = find_visible_global_for_expr(expr, body); global != nullptr && global->decl != nullptr) {
                const Type& type = global->decl->type;
                return global->decl->is_const || global->decl->is_constexpr ||
                       ((is_reference(type) || is_span(type)) && !type.is_mutable_ref);
            }
            return false;
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
            return assignment_target_is_read_only(*expr.lhs, body, signatures);
        case ExprKind::Unary: {
            if (expr.unary_op != UnaryOp::Deref) return false;
            if (is_explicit_star_this(expr)) return is_read_only_reachable(*expr.lhs, body, signatures);
            std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures);
            if (!operand_type.has_value()) return false;
            if (operand_type->kind == TypeKind::Pointer) {
                return !operand_type->is_mutable_pointee;
            }
            if (operand_type->kind == TypeKind::Reference) return !operand_type->is_mutable_ref;
            return false;
        }
        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            return sig != nullptr && is_reference(sig->return_type) && !sig->return_type.is_mutable_ref;
        }
        default:
            return false;
    }
}
// ch03/ch05 §5.11: the expressions allowed to bind to a `T&&` (rvalue-
// reference/move) parameter, checked against a specific `expected_type`.
// Reused, via the same Type::is_rvalue_ref flag, for a `Concept auto&&`
// generic parameter's own witness-typed slot (ch05 §5.11) and for
// passing a lambda expression literal to one (ch05 §5.12, once
// ExprKind::Lambda exists -- add it to the switch below at that point; a
// lambda literal is a fresh prvalue exactly like the cases already
// handled here). `std::move(x)` is allowed here when apply_expr's own
// Move-processing rules already license it for `x`; this helper only
// decides which *expression shapes* count as rvalues once that semantic
// check is separately satisfied.
// A bare place (Identifier/Member/Subscript/a pointer Deref) is never
// legitimate here: passing an existing lvalue directly into a by-move
// parameter without an explicit std::move would be exactly the
// unmarked implicit move ch05 §5.1 forbids -- the mirror image of
// argument_matches_parameter's ordinary-reference case, which rejects
// these same expression shapes for the opposite reason (there's no
// borrowable place to speak of).
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                            const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Move:
        case ExprKind::New:
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
        case ExprKind::Lambda:
            // ch05 §5.12: a (by now resolved) lambda literal is a fresh
            // prvalue exactly like a literal or std::make_unique<T>(...)
            // -- the primary motivating case for a `Concept auto&&`
            // parameter (ch05 §5.11), e.g. passing a closure directly to
            // a generic function.
            break;
        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig == nullptr) {
                std::optional<Type> call_type = infer_expr_type(expr, body, signatures);
                if (!expr.lhs && call_type.has_value() && types_equal(*call_type, expected_type)) break;
                return false;
            }
            // A reference-returning call yields a place/alias, not a
            // fresh value (see resolve_borrow_source_root's own Call
            // case) -- legitimate as a T&/const T& source elsewhere, but
            // not here.
            if (is_reference(sig->return_type)) return false;
            break;
        }
        default:
            return false;
    }
    std::optional<Type> actual_type = infer_expr_type(expr, body, signatures);
    if (!actual_type.has_value()) return false;
    if (types_equal(*actual_type, expected_type)) return true;
    if (expr.kind == ExprKind::Move && actual_type->kind == TypeKind::Reference && actual_type->pointee != nullptr) {
        return types_equal(*actual_type->pointee, expected_type);
    }
    return false;
}

// Infers `expr`'s scpp type, for function-overload resolution purposes
// only (ch05 §5.10) -- a best-effort, non-exhaustive type inference
// (movecheck has no general type-checking pass at all, by design: see
// e.g. produces_rvalue_of_type's similarly-scoped Call handling just
// above). Covers every expression shape that can legally appear as a
// call argument in this version: literals, a plain local (via
// body.local_types), std::move/std::make_unique, a nested call's own
// (resolved) return type, and the common unary/binary operators.
// Returns nullopt for anything it can't determine -- notably Member/
// Subscript chains, since movecheck has no access to Program's struct/
// class field-type info here, only Body's per-local types (the same
// scope limitation named_type_name/resolve_callee_signature already
// accept elsewhere). A nullopt argument type makes every candidate
// overload's corresponding parameter fail to match (see
// argument_matches_parameter) -- conservatively rejecting the call with
// a clear diagnostic rather than silently guessing an overload.
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::IntegerLiteral: return named_type("int");
        case ExprKind::FloatLiteral: return named_type("double");
        case ExprKind::BoolLiteral: return named_type("bool");
        case ExprKind::CharLiteral: return named_type("char");
        case ExprKind::Sizeof:
        case ExprKind::Alignof:
            return named_type("size_t");
        case ExprKind::StringLiteral: {
            Type result;
            result.kind = TypeKind::Pointer;
            result.pointee = std::make_shared<Type>(named_type("char"));
            result.is_mutable_pointee = false;
            return result;
        }

        case ExprKind::Identifier: {
            auto it = expr.explicit_global_qualification ? body.local_types.end() : body.local_types.find(expr.name);
            if (it != body.local_types.end()) return it->second;
            if (const GlobalVar* global = find_visible_global_for_expr(expr, body); global != nullptr && global->decl != nullptr) {
                return global->decl->type;
            }
            if (const EnumDef* def = [&]() {
                    const EnumDef* enum_def = nullptr;
                    [[maybe_unused]] const EnumVariant* variant = find_enum_variant(body.program, expr.name, &enum_def);
                    return enum_def;
                }()) {
                return named_type(def->name);
            }
            auto sig_it = signatures.find(expr.name);
            if (sig_it != signatures.end() && sig_it->second.size() == 1) {
                const FunctionSignature& sig = sig_it->second[0];
                if (!compile_time_dependency_visible_in_body(sig, body)) return std::nullopt;
                Type result;
                result.kind = TypeKind::FunctionPointer;
                result.function_return = std::make_shared<Type>(sig.return_type);
                result.function_params = sig.param_types;
                result.is_unsafe_function_pointer = sig.is_unsafe || sig.is_extern_c_declaration_only;
                return result;
            }
            return std::nullopt;
        }

        case ExprKind::Move: {
            // std::move doesn't change the static type -- still whatever
            // std::unique_ptr<T> the moved-from local was declared as.
            if (expr.lhs->kind != ExprKind::Identifier) return std::nullopt;
            auto it = body.local_types.find(expr.lhs->name);
            if (it == body.local_types.end()) {
                if (const GlobalVar* global = find_visible_global_for_expr(*expr.lhs, body);
                    global != nullptr && global->decl != nullptr) {
                    return global->decl->type;
                }
            }
            return it == body.local_types.end() ? std::nullopt : std::optional<Type>(it->second);
        }

        case ExprKind::New: {
            Type result;
            result.kind = TypeKind::Pointer;
            result.pointee = std::make_shared<Type>(expr.type);
            result.is_mutable_pointee = true;
            return result;
        }

        case ExprKind::Delete:
        case ExprKind::Destroy:
            return named_type("void");

        case ExprKind::TypeTrait:
            return named_type("bool");

        // `static_cast<T>(expr)`/`(T)expr` (ch06 §6): the cast's own
        // declared target type, unconditionally -- see codegen's
        // identical infer_type case.
        case ExprKind::Cast: return expr.type;

        case ExprKind::Lambda: {
            // ch05 §5.12: once resolved (movecheck's closure-resolution
            // pass, which runs before check_moves -- see
            // monomorphize_generics), `expr.name` holds the synthesized
            // closure class's own name; its type is exactly that class,
            // by value (matching MakeUnique's identical shape just
            // above: a fresh, concretely-typed value, not a reference).
            if (expr.name.empty()) return std::nullopt;
            return named_type(expr.name);
        }

        case ExprKind::Unary:
            switch (expr.unary_op) {
                case UnaryOp::Not: return named_type("bool");
                case UnaryOp::Neg: return infer_expr_type(*expr.lhs, body, signatures);
                case UnaryOp::AddressOf: {
                    if (expr.lhs->kind == ExprKind::Identifier && !body.local_types.contains(expr.lhs->name) &&
                        find_visible_global_for_expr(*expr.lhs, body) == nullptr) {
                        auto it = signatures.find(expr.lhs->name);
                        if (it != signatures.end() && it->second.size() == 1) {
                            const FunctionSignature& sig = it->second[0];
                            Type result;
                            result.kind = TypeKind::FunctionPointer;
                            result.function_return = std::make_shared<Type>(sig.return_type);
                            result.function_params = sig.param_types;
                            result.is_unsafe_function_pointer = sig.is_unsafe || sig.is_extern_c_declaration_only;
                            return result;
                        }
                    }
                    std::optional<Type> operand = infer_expr_type(*expr.lhs, body, signatures);
                    if (!operand) return std::nullopt;
                    Type result;
                    result.kind = TypeKind::Pointer;
                    result.pointee = std::make_shared<Type>(std::move(*operand));
                    // `&expr` always yields a mutable T* (ch05 §5.7) --
                    // whether the place itself is read-only-reachable is
                    // a separate check (is_read_only_reachable), not part
                    // of `&expr`'s own static type.
                    result.is_mutable_pointee = true;
                    return result;
                }
                case UnaryOp::Deref: {
                    std::optional<Type> operand = infer_expr_type(*expr.lhs, body, signatures);
                    if (!operand) return std::nullopt;
                    if (is_explicit_star_this(expr) && operand->kind == TypeKind::Reference && operand->pointee) {
                        return *operand->pointee;
                    }
                    if (is_function_pointer(*operand)) return *operand;
                    const Type& underlying =
                        operand->kind == TypeKind::Reference && operand->pointee ? *operand->pointee : *operand;
                    if (underlying.kind == TypeKind::Named) {
                        auto sig_it = signatures.find(underlying.name + "_operator_deref");
                        if (sig_it != signatures.end()) {
                            for (const FunctionSignature& sig : sig_it->second) {
                                if (!compile_time_dependency_visible_in_body(sig, body)) continue;
                                if (sig.param_types.empty()) continue;
                                return sig.return_type.kind == TypeKind::Reference
                                           ? std::optional<Type>(*sig.return_type.pointee)
                                           : std::optional<Type>(sig.return_type);
                            }
                        }

                    }
                    if (operand->kind != TypeKind::Pointer) return std::nullopt;
                    return *operand->pointee;
                }
            }
            return std::nullopt;

        case ExprKind::Binary:
            switch (expr.binary_op) {
                case BinaryOp::Add:
                    if (std::optional<Type> lhs = infer_expr_type(*expr.lhs, body, signatures),
                        rhs = infer_expr_type(*expr.rhs, body, signatures);
                        lhs.has_value() && rhs.has_value()) {
                        if (std::optional<Type> result = pointer_arithmetic_result_type(expr.binary_op, *lhs, *rhs)) {
                            return result;
                        }
                    }
                    [[fallthrough]];
                case BinaryOp::Sub:
                    if (expr.binary_op == BinaryOp::Sub) {
                        if (std::optional<Type> lhs = infer_expr_type(*expr.lhs, body, signatures),
                            rhs = infer_expr_type(*expr.rhs, body, signatures);
                            lhs.has_value() && rhs.has_value()) {
                            if (std::optional<Type> result = pointer_arithmetic_result_type(expr.binary_op, *lhs, *rhs)) {
                                return result;
                            }
                        }
                    }
                    [[fallthrough]];
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Assign:
                    return infer_expr_type(*expr.lhs, body, signatures);
                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Gt:
                case BinaryOp::Le:
                case BinaryOp::Ge:
                case BinaryOp::And:
                case BinaryOp::Or:
                    return named_type("bool");
            }
            return std::nullopt;

        case ExprKind::Conditional: {
            std::optional<Type> then_type = infer_expr_type(*expr.rhs, body, signatures);
            std::optional<Type> else_type = infer_expr_type(*expr.third, body, signatures);
            if (!then_type.has_value() || !else_type.has_value()) return std::nullopt;
            return types_equal(*then_type, *else_type) ? then_type : std::nullopt;
        }

        case ExprKind::Fold:
            if (expr.rhs) return infer_expr_type(*expr.rhs, body, signatures);
            return infer_expr_type(*expr.lhs, body, signatures);

        case ExprKind::Call: {
            if (is_for_range_size_builtin(expr)) {
                std::optional<Type> range_type = infer_expr_type(*expr.args[0], body, signatures);
                if (!range_type.has_value()) return std::nullopt;
                const Type& unwrapped = range_type->kind == TypeKind::Reference && range_type->pointee != nullptr
                                            ? *range_type->pointee
                                            : *range_type;
                if (unwrapped.kind == TypeKind::Array || unwrapped.kind == TypeKind::Span) return named_type("int");
                return std::nullopt;
            }
            CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig != nullptr) return sig->return_type;
            if (expr.lhs == nullptr && body.program != nullptr) {
                for (const ClassDef& def : body.program->classes) {
                    if (def.name == expr.name) return named_type(expr.name);
                }
                for (const StructDef& def : body.program->structs) {
                    if (def.name == expr.name) return named_type(expr.name);
                }
            }
            return std::nullopt;
        }

        case ExprKind::PackExpansion:
            return std::nullopt;

        case ExprKind::Member: {
            std::optional<Type> base = infer_expr_type(*expr.lhs, body, signatures);
            if (!base) return std::nullopt;
            const Type& base_named = base->kind == TypeKind::Reference ? *base->pointee : *base;
            if (base_named.kind != TypeKind::Named || body.program == nullptr) return std::nullopt;
            for (const ClassDef& def : body.program->classes) {
                if (def.name != base_named.name) continue;
                for (const ClassField& field : def.fields) {
                    if (field.name == expr.name) {
                        return field.type.kind == TypeKind::Reference ? std::optional<Type>(*field.type.pointee)
                                                                      : std::optional<Type>(field.type);
                    }
                }
                return std::nullopt;
            }
            for (const StructDef& def : body.program->structs) {
                if (def.name != base_named.name) continue;
                for (const StructField& field : def.fields) {
                    if (field.name == expr.name) {
                        return field.type.kind == TypeKind::Reference ? std::optional<Type>(*field.type.pointee)
                                                                      : std::optional<Type>(field.type);
                    }
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        case ExprKind::Subscript: {
            std::optional<Type> base = infer_expr_type(*expr.lhs, body, signatures);
            if (!base) return std::nullopt;
            const Type& effective = base->kind == TypeKind::Reference && base->pointee ? *base->pointee : *base;
            if (effective.kind == TypeKind::Array) return *effective.element;
            if (effective.kind == TypeKind::Span) return *effective.pointee;
            if (effective.kind == TypeKind::Pointer) return *effective.pointee;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void validate_sizeof_operand(const Expr& expr, const Body& body, const Signatures& signatures,
                             const SourceLocation& loc) {
    Type queried_type;
    if (expr.sizeof_operand_is_type) {
        queried_type = expr.type;
    } else {
        std::optional<Type> inferred = infer_expr_type(*expr.lhs, body, signatures);
        if (!inferred.has_value()) {
            throw DataflowError("cannot apply 'sizeof' to this expression: its type could not be inferred", loc);
        }
        queried_type = *inferred;
    }
    if (body.program == nullptr) {
        throw DataflowError("internal error: sizeof requires program type information", loc);
    }
    if (!layout_of_type(*body.program, queried_type).has_value()) {
        throw DataflowError("cannot apply 'sizeof' to this type in this version", loc);
    }
}

void validate_alignof_operand(const Expr& expr, const Body& body, const SourceLocation& loc) {
    if (body.program == nullptr) {
        throw DataflowError("internal error: alignof requires program type information", loc);
    }
    if (!layout_of_type(*body.program, expr.type).has_value()) {
        throw DataflowError("cannot apply 'alignof' to this type in this version", loc);
    }
}

} // namespace scpp
