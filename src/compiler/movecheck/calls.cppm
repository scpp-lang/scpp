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
    if (body.program == nullptr) {
        return find_visible_global(OptionalProgramRef{}, body.function_namespace_path, expr.name,
                                   expr.explicit_global_qualification);
    }
    std::reference_wrapper<const Program> program_ref{*body.program};
    return find_visible_global(OptionalProgramRef{program_ref}, body.function_namespace_path, expr.name,
                               expr.explicit_global_qualification);
}

// Mirrors find_visible_global's identical progressive-namespace-prefix
// search (ch11 §11.x), but for a bare (no-receiver) constructor call's
// own class/struct name -- needed because, unlike a `Type::name` (always
// stamped fully-qualified by the parser's own resolve_visible_type_name,
// e.g. ch05's every variable declaration/parameter/return type), a Call
// expression's own `.name` is *never* namespace-qualified by the parser
// (see parse_postfix's plain `node->name = expr->name;`, copied straight
// from the spelled identifier token) -- only a `ClassName::member(...)`-
// shaped static-member call gets any owner-name resolution at all. A
// bare `Widget(1, 2)` called from *within* `namespace scpp { ... }`
// therefore carries `expr.name == "Widget"`, never "scpp::Widget", even
// though every ClassDef/StructDef's own `.name` *is* stored fully-
// qualified (e.g. "scpp::Widget") -- so a naive `def.name == expr.name`
// comparison (as infer_expr_type's bare-call fallback used to do) can
// never match a single namespace-scoped class, the overwhelmingly
// common case in practice. Exercised for the first time by self-hosting
// parser.cppm's std::expected/std::unexpected-heavy style, e.g.
// `return std::unexpected(ParseError(...));`: ParseError's own
// constructor call is the argument being type-inferred here, and
// ParseError is declared inside `namespace scpp { ... }` just like the
// function calling it.
[[nodiscard]] std::optional<std::string> resolve_visible_class_or_struct_name(
    const Program& program, const std::vector<std::string>& namespace_path, const std::string& name,
    bool explicit_global_qualification) {
    auto matches_name = [&](std::string_view candidate) {
        for (const ClassDef& def : program.classes) {
            if (std::string_view{def.name} == candidate) return true;
        }
        for (const StructDef& def : program.structs) {
            if (std::string_view{def.name} == candidate) return true;
        }
        return false;
    };
    if (explicit_global_qualification) {
        return matches_name(name) ? std::optional<std::string>(name) : std::nullopt;
    }
    for (std::size_t depth = namespace_path.size(); depth > 0; depth--) {
        std::string candidate{};
        for (std::size_t i = 0; i < depth; i++) {
            if (candidate.size() != 0) candidate += "::";
            candidate += namespace_path[i];
        }
        candidate += "::";
        candidate += name;
        if (matches_name(candidate)) return candidate;
    }
    return matches_name(name) ? std::optional<std::string>(name) : std::nullopt;
}

struct CalleeSignature {
    std::string key;
    std::size_t param_offset = 0;
    std::optional<FunctionSignature> direct_signature;
};

[[nodiscard]] std::optional<Type> resolve_direct_type_alias_call_type(const Expr& call_expr, const Body& body) {
    if (body.program == nullptr || call_expr.lhs != nullptr || call_expr.name.empty()) return std::nullopt;
    auto matches_name = [&](std::string_view alias_name) {
        if (alias_name == call_expr.name) return true;
        if (!call_expr.explicit_global_qualification && !body.function_namespace_path.empty()) {
            std::string qualified;
            for (std::size_t i = 0; i < body.function_namespace_path.size(); i++) {
                if (i != 0) qualified += "::";
                qualified += body.function_namespace_path[i];
            }
            qualified += "::";
            qualified += call_expr.name;
            return alias_name == qualified;
        }
        return false;
    };
    for (const TypeAliasDecl& alias : body.program->type_aliases) {
        if (matches_name(alias.name)) return alias.underlying_type;
    }
    return std::nullopt;
}

void rewrite_type_alias_constructor_call(Expr& expr, const Body& body) {
    std::optional<Type> alias_type = resolve_direct_type_alias_call_type(expr, body);
    if (!alias_type.has_value() || alias_type->kind != TypeKind::Named) return;
    expr.name = alias_type->name;
    expr.explicit_global_qualification = true;
}

[[nodiscard]] bool is_nullptr_literal(const Expr& expr) {
    return expr.kind == ExprKind::Identifier && expr.name == "nullptr" && !expr.explicit_global_qualification;
}

// `std::nullopt` -- parsed as a plain (non globally-qualified)
// Identifier (there is no dedicated ExprKind for it, unlike
// std::move/scpp::is_thread_movable; see parse_primary's fallback
// qualified-name path), exactly mirroring is_nullptr_literal just above
// for the pointer-world equivalent, `nullptr`.
[[nodiscard]] bool is_nullopt_literal(const Expr& expr) {
    return expr.kind == ExprKind::Identifier && (expr.name == "std::nullopt" || expr.name == "nullopt") &&
           !expr.explicit_global_qualification;
}

// An optional-type parameter's stored name is "std::optional"/"optional"
// pre-monomorphization (template_args populated separately), but
// post-monomorphization it may instead already be flattened to
// "std::optional.<Element>" with no separate template_args -- mirrors
// is_vector_like_named_type's identical dual-form handling just below.
[[nodiscard]] bool is_optional_named_type(const Type& type) {
    return type.kind == TypeKind::Named &&
           (type.name == "std::optional" || type.name == "optional" || type.name.starts_with("std::optional.") ||
            type.name.starts_with("optional."));
}

[[nodiscard]] FunctionSignature function_pointer_signature(const Type& type);
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool is_reference_wrapper_constructor_call(const Expr& expr);
[[nodiscard]] bool is_optional_constructor_call(const Expr& expr);
        [[nodiscard]] std::expected<void, DataflowError> check_enum_conversion_compatibility(const Type& target_type, const Expr& source_expr, const Body& body,
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
[[nodiscard]] bool is_freely_copyable_class_value_source(const Expr& expr, const Type& target_type, const Body& body,
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
[[nodiscard]] std::expected<void, DataflowError> check_function_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                       const Signatures& signatures, SourceLocation loc,
                                       const std::string& target_name, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_raw_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                  const Signatures& signatures, SourceLocation loc,
                                  const std::string& target_name, bool report_errors);
[[nodiscard]] bool assignment_target_is_read_only(const Expr& expr, const Body& body,
                                                  const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> validate_sizeof_operand(const Expr& expr, const Body& body, const Signatures& signatures,
                                    const SourceLocation& loc);
[[nodiscard]] std::expected<void, DataflowError> validate_alignof_operand(const Expr& expr, const Body& body, const SourceLocation& loc);
[[nodiscard]] std::optional<LocalId> direct_write_root(const Expr& expr, const Body& body);
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                           const Signatures& signatures);

std::expected<void, DataflowError> check_enum_conversion_compatibility(const Type& target_type, const Expr& source_expr, const Body& body,
                                         const Signatures& signatures, const SourceLocation& loc) {
    const Type& target_operand = binary_operand_type(target_type);
    std::optional<Type> source_type = infer_expr_type(source_expr, body, signatures);
    if (!source_type.has_value()) return {};
    const Type& source_operand = binary_operand_type(*source_type);
    bool target_is_enum = is_enum_type(target_operand, body.program);
    bool source_is_enum = is_enum_type(source_operand, body.program);
    if (!(target_is_enum || source_is_enum)) return {};
    if (types_equal(target_operand, source_operand)) return {};
    return std::unexpected(DataflowError("enum class values do not implicitly convert to or from integers (or other enum types) in "
                        "this version; use an explicit cast to the enum's underlying type",
                        loc));
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

[[nodiscard]] bool is_vector_like_named_type(const Type& type) {
    return type.kind == TypeKind::Named &&
           (type.name == "std::vector" || type.name == "vector" || type.name.starts_with("std::vector.") ||
            type.name.starts_with("vector."));
}

[[nodiscard]] std::optional<Type> infer_vector_element_type(const Type& type, const Body& body) {
    if (!is_vector_like_named_type(type)) return std::nullopt;
    if (type.template_args.size() == 1) return type.template_args[0];
    if (body.program == nullptr) return std::nullopt;
    if (const ClassDef* def = find_class_def(*body.program, type.name)) {
        for (const ClassField& field : def->fields) {
            if (field.name == "data_" && field.type.kind == TypeKind::Pointer && field.type.pointee) {
                return *field.type.pointee;
            }
        }
        return std::nullopt;
    }
    if (const StructDef* def = find_struct_def(*body.program, type.name)) {
        for (const StructField& field : def->fields) {
            if (field.name == "data_" && field.type.kind == TypeKind::Pointer && field.type.pointee) {
                return *field.type.pointee;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
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
            const Type* callee_type = body.type_if_local(*callee_expr);
            if (callee_type != nullptr && is_function_pointer(*callee_type)) {
                return CalleeSignature{"", 0, function_pointer_signature(*callee_type)};
            }
        } else if (class_field_types != nullptr && callee_expr->kind == ExprKind::Member && callee_expr->lhs &&
                   callee_expr->lhs->kind == ExprKind::Identifier) {
            const Type* base = body.type_if_local(*callee_expr->lhs);
            if (base != nullptr) {
                const Type& base_type = base->kind == TypeKind::Reference ? *base->pointee : *base;
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
        const Type* base = body.type_if_local(*call_expr.lhs);
        if (base != nullptr) {
            const Type& base_type = base->kind == TypeKind::Reference ? *base->pointee : *base;
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
    if (const Type* bare_local_type = body.type_if_local(call_expr); bare_local_type != nullptr) {
        const Type& local_type = *bare_local_type;
        const Type& callee_type =
            local_type.kind == TypeKind::Reference && local_type.pointee != nullptr ? *local_type.pointee : local_type;
        if (is_function_pointer(callee_type)) {
            return CalleeSignature{"", 0, function_pointer_signature(callee_type)};
        }
        // A local variable holding a callable object -- in practice
        // always a resolved lambda closure (ExprKind::Lambda's own
        // infer_expr_type case, above, returns exactly this shape: a
        // TypeKind::Named type whose name is the synthesized closure
        // class) -- is called the same way a receiver's own method call
        // would be: forward to <ClassName>_call with an implicit `this`
        // (param_offset 1), mirroring resolve_lambda's own identical
        // name-mangling convention for the synthesized call method
        // (monomorphize.cppm: `call_method.name = class_name +
        // "_call";`). In practice monomorphize.cppm's own bare-call-
        // redirect already rewrites a resolved lambda variable's call
        // to this shape (`f(args)` -> `f.call(args)`) before this point,
        // so this is reached only as a defensive fallback for any other
        // caller of resolve_callee_signature that hasn't gone through
        // that rewrite.
        if (callee_type.kind == TypeKind::Named && signatures.contains(callee_type.name + "_call")) {
            return CalleeSignature{callee_type.name + "_call", 1, std::nullopt};
        }
    }
    if (call_expr.lhs) {
        std::string class_name;
        if (call_expr.lhs->kind == ExprKind::Identifier) {
            const Type* receiver = body.type_if_local(*call_expr.lhs);
            if (receiver != nullptr) class_name = named_type_name(*receiver);
        } else if (is_explicit_star_this(*call_expr.lhs)) {
            std::optional<LocalId> self = body.this_local();
            if (self.has_value()) class_name = named_type_name(body.type_of(*self));
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
            const Type* member_base = body.type_if_local(*call_expr.lhs->lhs);
            if (member_base != nullptr) {
                const Type& base_type =
                    member_base->kind == TypeKind::Reference ? *member_base->pointee : *member_base;
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
[[nodiscard]] bool is_reference_wrapper_constructor_call(const Expr& expr) {
    return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.args.size() == 1 &&
           (expr.name == "std::reference_wrapper" || expr.name == "reference_wrapper");
}

[[nodiscard]] bool is_optional_constructor_call(const Expr& expr) {
    return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.args.size() == 1 &&
           (expr.name == "std::optional" || expr.name == "optional");
}

[[nodiscard]] bool is_zero_arg_optional_constructor_call(const Expr& expr) {
    return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.args.empty() &&
           (expr.name == "std::optional" || expr.name == "optional");
}

// A defaulted-away vector-typed parameter's own `= {}` default
// expression is synthesized (by the same default-argument machinery
// check_call_arguments' caller uses) as an explicit generic-constructor
// call whose own `.name` is the fully bracketed instantiation (e.g.
// "std::vector<scpp::AlignmentSpecifier>()"), unlike an ordinary
// explicit `std::optional<T>(...)` call's bare, bracket-free name (see
// is_optional_constructor_call just above -- that one's explicit
// template argument travels via a separate field, not baked into
// `.name`) -- so both forms are accepted here.
[[nodiscard]] bool is_zero_arg_vector_constructor_call(const Expr& expr) {
    return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.args.empty() &&
           (expr.name == "std::vector" || expr.name == "vector" || expr.name.starts_with("std::vector<") ||
            expr.name.starts_with("vector<"));
}

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

[[nodiscard]] bool is_freely_copyable_class_value_source(const Expr& expr, const Type& target_type, const Body& body,
                                                         const Signatures& signatures) {
    if (body.program == nullptr || !is_named_record_type_for_call_binding(target_type, body) ||
        !is_freely_copyable_value_type(target_type, *body.program)) {
        return false;
    }
    if (expr.kind == ExprKind::Lambda) return true;
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    if (!source_type.has_value()) return false;
    if (types_equal(*source_type, target_type)) return true;
    return source_type->kind == TypeKind::Reference && source_type->pointee != nullptr &&
           !source_type->is_rvalue_ref && types_equal(*source_type->pointee, target_type);
}

[[nodiscard]] bool is_implicit_move_return_source(const Expr& expr, const Type& target_type, const Body& body) {
    if (expr.kind != ExprKind::Identifier || expr.explicit_global_qualification) return false;
    const Type* type = body.type_if_local(expr);
    return type != nullptr && types_equal(*type, target_type);
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
                if (param_type.is_mutable_ref) return arg_type.is_mutable_ref;
                return true;
            }
            if (!param_type.is_mutable_ref && !arg_type.is_mutable_ref &&
                types_equal(arg_type, param_type)) {
                return true;
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
        return (arg_type.pointee != nullptr &&
                (types_equal(*arg_type.pointee, param_type) ||
                 (body.program != nullptr &&
                  types_compatible_with_base_conversion(*arg_type.pointee, param_type, *body.program,
                                                        enclosing_class_name(body))))) ||
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
    if (is_nullptr_literal(arg) && param_type.kind == TypeKind::Pointer) return true;
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
               is_freely_copyable_class_value_source(arg, param_type, body, signatures) ||
               produces_rvalue_of_type(arg, param_type, body, signatures);
    }
    return true;
}

[[nodiscard]] bool constructor_parameter_accepts_argument_directly(const Expr& arg, const Type& param_type,
                                                                   const Body& body, const Signatures& signatures) {
    auto normalized_param_type = [&](Type type) {
        if (type.kind == TypeKind::Named && !type.name.empty() && type.template_args.empty()) {
            if (std::optional<Type> inferred = infer_expr_type(arg, body, signatures); inferred.has_value()) return *inferred;
        }
        return type;
    };
    if (is_nullptr_literal(arg) && param_type.kind == TypeKind::Pointer) return true;
    Type effective_param_type = normalized_param_type(param_type);
    if (is_reference(effective_param_type) && effective_param_type.is_rvalue_ref) {
        return produces_rvalue_of_type(arg, *effective_param_type.pointee, body, signatures);
    }
    if (is_reference(effective_param_type)) {
        if (!effective_param_type.is_mutable_ref && effective_param_type.pointee != nullptr &&
            produces_rvalue_of_type(arg, *effective_param_type.pointee, body, signatures)) {
            return true;
        }
        if (arg.kind == ExprKind::Move ||
            arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::FloatLiteral ||
            arg.kind == ExprKind::BoolLiteral || arg.kind == ExprKind::CharLiteral ||
            arg.kind == ExprKind::StringLiteral) {
            return false;
        }
        std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
        return arg_type.has_value() && argument_type_matches_parameter(*arg_type, effective_param_type, body);
    }
    std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
    if (!arg_type.has_value() || !argument_type_matches_parameter(*arg_type, effective_param_type, body)) return false;
    if (is_named_record_type_for_call_binding(effective_param_type, body)) {
        return is_copyable_class_lvalue_boundary_source(arg, effective_param_type, body, signatures) ||
               is_freely_copyable_class_value_source(arg, effective_param_type, body, signatures) ||
               produces_rvalue_of_type(arg, effective_param_type, body, signatures);
    }
    return true;
}

[[nodiscard]] bool argument_matches_parameter_for_constructor_selection(const Expr& arg, const Type& param_type,
                                                                       const Body& body, const Signatures& signatures) {
    return constructor_parameter_accepts_argument_directly(arg, param_type, body, signatures);
}

[[nodiscard]] const FunctionSignature* find_single_argument_converting_constructor_signature(
    const Type& class_type, const Expr& arg, const Body& body, const Signatures& signatures) {
    auto normalized_ctor_param_type = [&](const FunctionSignature& candidate) {
        Type type = candidate.param_types[1];
        if (!candidate.is_generic_template && type.kind == TypeKind::Reference && type.is_rvalue_ref &&
            type.pointee != nullptr && produces_rvalue_of_type(arg, *type.pointee, body, signatures)) {
            return *type.pointee;
        }
        return type;
    };
    if (class_type.kind != TypeKind::Named) return nullptr;
    std::string ctor_name = class_type.name + "_new";
    auto is_constructor_clone_name = [&](std::string_view name) {
        return name == ctor_name || (!name.empty() && name.starts_with(ctor_name + "."));
    };
    for (const auto& [name, overloads] : signatures) {
        if (!is_constructor_clone_name(name)) continue;
        for (const FunctionSignature& candidate : overloads) {
            if (candidate.member_owner_class != class_type.name) continue;
            if (!compile_time_dependency_visible_in_body(candidate, body)) continue;
            if (candidate.param_types.size() != 2) continue;
            Type ctor_param_type = normalized_ctor_param_type(candidate);
            if (types_equal(ctor_param_type, class_type) ||
                (is_reference(ctor_param_type) && ctor_param_type.pointee != nullptr &&
                 types_equal(*ctor_param_type.pointee, class_type))) {
                continue;
            }
            if (constructor_parameter_accepts_argument_directly(arg, ctor_param_type, body, signatures)) {
                return &candidate;
            }
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
    Type receiver_expected = *candidate.param_types[0].pointee;
    receiver_expected.is_const_qualified = false;
    bool receiver_is_rvalue = produces_rvalue_of_type(receiver_expr, receiver_expected, body, signatures);
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

    bool have_non_generic_match = std::any_of(matches.begin(), matches.end(),
                                              [](const FunctionSignature* sig) { return !sig->is_generic_template; });
    if (have_non_generic_match) {
        matches.erase(std::remove_if(matches.begin(), matches.end(),
                                     [](const FunctionSignature* sig) { return sig->is_generic_template; }),
                      matches.end());
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
    if (source->kind != ExprKind::Identifier || body.local_of(*source).has_value()) return std::nullopt;
    const GlobalVar* visible_global = nullptr;
    if (body.program != nullptr) {
        std::reference_wrapper<const Program> program_ref{*body.program};
        visible_global = find_visible_global(OptionalProgramRef{program_ref}, body.function_namespace_path, source->name,
                                             source->explicit_global_qualification);
    } else {
        visible_global =
            find_visible_global(OptionalProgramRef{}, body.function_namespace_path, source->name,
                                source->explicit_global_qualification);
    }
    if (visible_global != nullptr) {
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

std::expected<void, DataflowError> check_function_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                       const Signatures& signatures, SourceLocation loc, const std::string& target_name,
                                       bool report_errors) {
    if (!report_errors || !is_function_pointer(target_type)) return {};
    std::optional<Type> source_type = resolve_function_designator_type(expr, target_type, body, signatures);
    if (!source_type) source_type = infer_expr_type(expr, body, signatures);
    if (!source_type || !is_function_pointer(*source_type)) {
        return std::unexpected(DataflowError("cannot initialize function pointer '" + target_name +
                             "' from this expression: expected a function or function pointer with matching "
                             "signature",
            loc));
    }
    if (types_equal(target_type, *source_type)) return {};
    if (same_function_pointer_shape_ignoring_unsafe(target_type, *source_type) && target_type.is_unsafe_function_pointer &&
        !source_type->is_unsafe_function_pointer) {
        return {};
    }
    if (same_function_pointer_shape_ignoring_unsafe(target_type, *source_type) && !target_type.is_unsafe_function_pointer &&
        source_type->is_unsafe_function_pointer) {
        return std::unexpected(DataflowError("cannot assign an unsafe-qualified function pointer to plain function pointer '" +
                                 target_name + "'",
            loc));
    }
    return {};
}

std::expected<void, DataflowError> check_raw_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                   const Signatures& signatures, SourceLocation loc, const std::string& target_name,
                                   bool report_errors) {
    if (!report_errors || target_type.kind != TypeKind::Pointer) return {};
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    if (!source_type || source_type->kind != TypeKind::Pointer) return {};
    if (raw_pointer_implicitly_convertible(*source_type, target_type)) return {};
    if (body.program != nullptr &&
        types_compatible_with_base_conversion(*source_type, target_type, *body.program, enclosing_class_name(body))) {
        return {};
    }
    return std::unexpected(DataflowError("cannot initialize or assign raw pointer '" + target_name +
                            "' from an incompatible pointer type without an explicit cast",
                        loc));
}

// Structurally validates and resolves spec ch05.3's elision rule for a
[[nodiscard]] bool assignment_target_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            if (std::optional<LocalId> local = body.local_of(expr); local.has_value()) {
                const Type& type = body.type_of(*local);
                return body.decl(*local).is_const ||
                       ((is_reference(type) || is_span(type)) && !type.is_mutable_ref);
            }
            if (const GlobalVar* global = find_visible_global_for_expr(expr, body); global != nullptr && global->decl != nullptr) {
                const Type& type = global->decl->type;
                return global->decl->is_const || global->decl->is_constexpr ||
                       ((is_reference(type) || is_span(type)) && !type.is_mutable_ref);
            }
            return false;
        }
        case ExprKind::Member:
        case ExprKind::Subscript: {
            // Most projections inherit writeability from their base (a
            // field of a const object, an element of a const/span<const>
            // view, ...), but some expressions themselves *are* a
            // read-only reference-like view even when the base object is
            // otherwise mutable -- most notably lambda capture fields that
            // preserve an outer `const T&`. Check the projection's own
            // declared view type first so `this.capture = ...` is rejected
            // when the field denotes a shared borrow/span, then fall back
            // to the base-place const-reachability rule for ordinary value
            // fields.
            if (expr.kind == ExprKind::Member && body.program != nullptr) {
                std::optional<Type> base = infer_expr_type(*expr.lhs, body, signatures);
                const Type* base_named = base.has_value() ? &*base : nullptr;
                if (base_named != nullptr && base_named->kind == TypeKind::Reference && base_named->pointee != nullptr) {
                    base_named = base_named->pointee.get();
                }
                if (base_named != nullptr && base_named->kind == TypeKind::Named) {
                    if (const ClassDef* def = find_class_def(*body.program, base_named->name)) {
                        for (const ClassField& field : def->fields) {
                            if (field.name == expr.name) {
                                if ((is_reference(field.type) || is_span(field.type)) && !field.type.is_mutable_ref) return true;
                                break;
                            }
                        }
                    }
                    if (const StructDef* def = find_struct_def(*body.program, base_named->name)) {
                        for (const StructField& field : def->fields) {
                            if (field.name == expr.name) {
                                if ((is_reference(field.type) || is_span(field.type)) && !field.type.is_mutable_ref) return true;
                                break;
                            }
                        }
                    }
                }
            } else if (expr.kind == ExprKind::Subscript) {
                std::optional<Type> base = infer_expr_type(*expr.lhs, body, signatures);
                const Type* effective = base.has_value() ? &*base : nullptr;
                if (effective != nullptr && effective->kind == TypeKind::Reference && effective->pointee != nullptr) {
                    effective = effective->pointee.get();
                }
                if (effective != nullptr && effective->kind == TypeKind::Span && !effective->is_mutable_ref) return true;
            }
            return assignment_target_is_read_only(*expr.lhs, body, signatures);
        }
        case ExprKind::Unary: {
            if (expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PreDec) {
                return assignment_target_is_read_only(*expr.lhs, body, signatures);
            }
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
// A call through a function-pointer-typed field -- `receiver.field_
// (args)`/`this->field_(args)`, parsed identically to an ordinary named-
// method call (parse_member_or_method_call fuses the member access and
// the call into one node: expr.name holds the field's name, expr.lhs the
// receiver) -- e.g. std::function<R(Args...)>'s own `call()`: `return
// this->invoke_(this->object_, args...);`. resolve_callee_signature
// already recognizes this exact shape for argument validation, but only
// when given a precomputed ClassFieldTypes cache that most of this
// file's callers (infer_expr_type/produces_rvalue_of_type below) don't
// have access to; look the field up directly off the receiver's own
// ClassDef instead, mirroring the ExprKind::Member case's identical
// find_class_def-based field lookup (this file, further below). Shared
// by both of this file's callers so they agree on exactly which call
// shapes qualify, rather than maintaining two independent copies of the
// same lookup.
[[nodiscard]] std::optional<Type> function_pointer_field_call_return_type(const Expr& expr, const Body& body) {
    if (expr.lhs == nullptr || expr.name.empty() || expr.lhs->kind != ExprKind::Identifier || body.program == nullptr) {
        return std::nullopt;
    }
    const Type* base = body.type_if_local(*expr.lhs);
    if (base == nullptr) return std::nullopt;
    std::string class_name = named_type_name(*base);
    if (class_name.empty()) return std::nullopt;
    const ClassDef* def = find_class_def(*body.program, class_name);
    if (def == nullptr) return std::nullopt;
    for (const ClassField& field : def->fields) {
        if (field.name == expr.name && is_function_pointer(field.type)) {
            return function_pointer_signature(field.type).return_type;
        }
    }
    return std::nullopt;
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
    bool call_receiver_is_move = false;
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
        case ExprKind::ValueInit:
            // A bare `{}` always value-initializes a brand new,
            // alias-free temporary -- but unlike every other case in
            // this switch, it carries no fixed type of its own: it
            // adapts to whatever `expected_type` the calling context
            // demands (exactly the real-C++ meaning of value-
            // initialization). monomorphize's walk_expr only ever
            // stamps expr.type for the one narrow case it exists to
            // serve (a `return {};` statement, from the enclosing
            // function's own return type -- see its own
            // ExprKind::ValueInit case); a `{}` reached here as a call
            // argument -- including a defaulted-away parameter's own
            // `= {}` default expression, cloned fresh per call site by
            // check_call_arguments -- was never walked at all, so
            // expr.type is left unset/stale. Falling through to the
            // generic infer_expr_type/types_equal check below would
            // then spuriously fail against the *correct* expected_type,
            // so this returns true unconditionally instead: whatever
            // type is being asked for is always exactly as fresh and
            // legitimate as an explicit `Type{}` constructor call (the
            // ExprKind::Call case just below, which this otherwise
            // mirrors) -- dataflow.cppm's own ExprKind::ValueInit case
            // separately rejects any type with no valid zero-argument
            // construction path, so soundness doesn't depend on the
            // type-match performed here.
            return true;
        case ExprKind::Unary:
            // `&x` (address-of) always yields a brand new pointer prvalue,
            // independent of whatever move/borrow state `x` itself has --
            // exactly as fresh as a literal or std::make_unique<T>(...),
            // regardless of whether `x` is a plain local, a field access
            // (e.g. `&type.lifetime`), or any other place expression.
            if (expr.unary_op != UnaryOp::AddressOf) return false;
            break;
        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig == nullptr) {
                std::optional<Type> call_type = infer_expr_type(expr, body, signatures);
                if (!expr.lhs && call_type.has_value() && types_equal(*call_type, expected_type)) break;
                // `s.substr(...)` (a host-library std::string method with
                // no scpp-side FunctionSignature, per infer_expr_type's
                // own identical special case just above it) is a narrow,
                // explicitly-named exception to the general "unresolvable
                // method call" rejection just below: unlike an arbitrary
                // receiver-having call whose value-vs-reference return
                // category can't be verified without a signature, this
                // one specific host method is known to always construct
                // a brand new std::string by value, never alias an
                // existing one.
                if (expr.lhs != nullptr && expr.name == "substr" && call_type.has_value() &&
                    types_equal(*call_type, expected_type)) {
                    break;
                }
                // A call through a function-pointer-typed field (e.g.
                // std::function<R(Args...)>::call()'s own `return this->
                // invoke_(this->object_, args...);`) is, like `.substr()`
                // just above, resolvable only via infer_expr_type's own
                // function_pointer_field_call_return_type fallback (no
                // FunctionSignature exists for it), but is unconditionally
                // just as fresh as any other function call: invoking a
                // stored callable always produces a brand new by-value
                // result (or a fresh reference, licensed elsewhere), never
                // an alias to storage this call's own receiver already
                // owned -- there is no way for a *call* to hand back
                // something it didn't just construct or that its own
                // callee didn't already prove fresh.
                if (call_type.has_value() && types_equal(*call_type, expected_type) &&
                    function_pointer_field_call_return_type(expr, body).has_value()) {
                    break;
                }
                // A bare, zero-arg `std::vector<T>()` generic-constructor
                // call (also with no scpp-side FunctionSignature, since
                // std::vector is host-library) is likewise known to
                // always construct a brand new, empty vector by value --
                // needed because a defaulted-away vector-typed
                // parameter's own `= {}` default expression is
                // synthesized as exactly this call shape (see is_zero_
                // arg_vector_constructor_call's own doc comment) rather
                // than as ExprKind::ValueInit. Unlike the two checks
                // above, this returns true directly rather than
                // breaking: infer_expr_type can never resolve a type for
                // this call (std::vector has no scpp-side
                // FunctionSignature), so falling through to the post-
                // switch actual_type/types_equal check below would
                // always fail regardless of this condition.
                if (!expr.lhs && is_zero_arg_vector_constructor_call(expr) && is_vector_like_named_type(expected_type)) {
                    return true;
                }
                return false;
            }
            // A reference-returning call yields a place/alias, not a
            // fresh value (see resolve_borrow_source_root's own Call
            // case) -- legitimate as a T&/const T& source elsewhere, but
            // not here. Exception: a method called directly on a
            // std::move(...) receiver (`std::move(x).value()`, the
            // idiom for extracting a std::expected/std::optional's
            // payload) -- the signature database models `.value()` with
            // a single, receiver-value-category-agnostic T& return (it
            // doesn't track a separate `&&`-qualified overload), but a
            // call whose own receiver is itself freshly std::move'd is
            // exactly as fresh as std::move(x) itself: the callee is
            // handing off part of an object the caller has already
            // abandoned, not aliasing something that outlives the call.
            call_receiver_is_move = expr.lhs != nullptr && expr.lhs->kind == ExprKind::Move;
            if (is_reference(sig->return_type) && !call_receiver_is_move) return false;
            break;
        }
        case ExprKind::Identifier:
            // `std::nullopt` against a std::optional<T> target is
            // exactly as fresh as a literal -- an alias-free constant
            // that never references any existing storage -- the mirror
            // image of is_nullptr_literal's identical bare-`nullptr`
            // exception for pointer targets (see argument_matches_
            // parameter's own use of that). Every other Identifier is a
            // place expression (some existing named variable), never a
            // fresh value, so it's rejected immediately just like the
            // default case below.
            return is_nullopt_literal(expr) && is_optional_named_type(expected_type);
        default:
            return false;
    }
    std::optional<Type> actual_type = infer_expr_type(expr, body, signatures);
    if (!actual_type.has_value()) return false;
    if (types_equal(*actual_type, expected_type)) return true;
    // A resolved method call's return type is used as-is (not unwrapped)
    // by infer_expr_type's own Call case above -- so a reference return
    // just licensed as fresh by the switch's own Call case above (a
    // std::move(...)-receiver method call) still shows up here as a
    // reference type and needs the exact same pointee-unwrapping
    // std::move itself gets just below, not just an outright
    // reference-vs-value mismatch.
    bool needs_pointee_unwrap = expr.kind == ExprKind::Move || call_receiver_is_move;
    if (expected_type.is_const_qualified) {
        Type unqualified_expected = expected_type;
        unqualified_expected.is_const_qualified = false;
        if (types_equal(*actual_type, unqualified_expected)) return true;
        if (needs_pointee_unwrap && actual_type->kind == TypeKind::Reference &&
            actual_type->pointee != nullptr) {
            return types_equal(*actual_type->pointee, unqualified_expected);
        }
    }
    if (needs_pointee_unwrap && actual_type->kind == TypeKind::Reference &&
        actual_type->pointee != nullptr) {
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
// body.local_decls), std::move/std::make_unique, a nested call's own
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
        case ExprKind::ValueInit:
            return expr.type;
        case ExprKind::StringLiteral: {
            Type result;
            result.kind = TypeKind::Pointer;
            result.pointee = std::make_shared<Type>(named_type("char"));
            result.is_mutable_pointee = false;
            return result;
        }

        case ExprKind::Identifier: {
            if (const Type* local_type = body.type_if_local(expr); local_type != nullptr) return *local_type;
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
            if (expr.lhs->kind == ExprKind::Identifier) {
                const Type* moved_type = body.type_if_local(*expr.lhs);
                if (moved_type == nullptr) {
                    if (const GlobalVar* global = find_visible_global_for_expr(*expr.lhs, body);
                        global != nullptr && global->decl != nullptr) {
                        return global->decl->type;
                    }
                    return std::nullopt;
                }
                return std::optional<Type>(*moved_type);
            }
            // std::move(v.back())/std::move(v.front()) (relocating a
            // container element elsewhere, e.g. `std::string x =
            // std::move(segments.back());`) -- the moved-from expression
            // isn't a bare identifier here, but this Call case's own
            // .back()/.front() special-case (just below) already resolves
            // the exact element type, unwrapped from any reference, which
            // is exactly as valid a moved-from type as an identifier's.
            if (expr.lhs->kind == ExprKind::Call) return infer_expr_type(*expr.lhs, body, signatures);
            // std::move(program.functions[i]) (relocating a container
            // element accessed by index rather than through a named
            // reference first, e.g. reconcile_ordinary_forward_
            // declarations' early-return path) -- Subscript's own
            // handling (just below in this same function) already
            // resolves the exact element type via infer_vector_element_
            // type, exactly as valid a moved-from type as an identifier's
            // or the .back()/.front() Call case just above.
            if (expr.lhs->kind == ExprKind::Subscript) return infer_expr_type(*expr.lhs, body, signatures);
            return std::nullopt;
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
                case UnaryOp::PreInc:
                case UnaryOp::PreDec:
                case UnaryOp::PostInc:
                case UnaryOp::PostDec:
                    return infer_expr_type(*expr.lhs, body, signatures);
                case UnaryOp::AddressOf: {
                    if (expr.lhs->kind == ExprKind::Identifier && !body.local_of(*expr.lhs).has_value() &&
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
                    if (operand->kind == TypeKind::Reference && operand->pointee != nullptr) {
                        result.pointee = std::make_shared<Type>(*operand->pointee);
                        result.is_mutable_pointee = operand->is_mutable_ref;
                    } else {
                        result.pointee = std::make_shared<Type>(std::move(*operand));
                        // `&expr` of a non-reference place yields a mutable
                        // T* (ch05 §5.7) -- whether the place itself is
                        // read-only-reachable is a separate check
                        // (is_read_only_reachable), not part of `&expr`'s own
                        // static type.
                        result.is_mutable_pointee = true;
                    }
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
                case BinaryOp::AddAssign:
                case BinaryOp::SubAssign:
                case BinaryOp::MulAssign:
                case BinaryOp::DivAssign:
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
            // `v.back()`/`v.front()` on a vector-like receiver (ch04's
            // std::vector, a host-library type with no scpp-side
            // FunctionSignature entry to find via resolve_overload below,
            // unlike a user-defined method) -- special-cased the same way
            // Subscript's own operator[] case is, just below, via the same
            // infer_vector_element_type helper, so both accessors agree on
            // the exact same inferred element type. Needed for e.g. a
            // `cond ? v[i] : v.back();` ternary (ExprKind::Conditional,
            // above) to type-check at all: without this, `.back()`'s arm
            // infers to nullopt and the whole conditional does too, even
            // though both arms in fact yield the identical element type.
            if (expr.lhs != nullptr && expr.args.empty() && (expr.name == "back" || expr.name == "front")) {
                std::optional<Type> receiver_type = infer_expr_type(*expr.lhs, body, signatures);
                if (receiver_type.has_value()) {
                    const Type& effective = receiver_type->kind == TypeKind::Reference && receiver_type->pointee != nullptr
                                                ? *receiver_type->pointee
                                                : *receiver_type;
                    if (std::optional<Type> element = infer_vector_element_type(effective, body); element.has_value()) {
                        return *element;
                    }
                }
            }
            // `s.substr(...)` on a std::string receiver (likewise a
            // host-library type with no scpp-side FunctionSignature
            // entry to find via resolve_overload below) always yields a
            // brand new std::string by value -- unlike .back()/.front()
            // just above, which alias an existing element rather than
            // freshly constructing one.
            if (expr.lhs != nullptr && expr.name == "substr") {
                std::optional<Type> receiver_type = infer_expr_type(*expr.lhs, body, signatures);
                if (receiver_type.has_value()) {
                    const Type& effective = receiver_type->kind == TypeKind::Reference && receiver_type->pointee != nullptr
                                                ? *receiver_type->pointee
                                                : *receiver_type;
                    if (effective.kind == TypeKind::Named && (effective.name == "std::string" || effective.name == "string")) {
                        return named_type("std::string");
                    }
                }
            }
            // `c.empty() ` on a host-library container receiver (std::
            // string, std::vector, or an unordered_map/unordered_set --
            // again, no scpp-side FunctionSignature entry to find via
            // resolve_overload below) always yields a plain `bool`.
            // Unlike .back()/.front()/.substr() above, this one needs no
            // element-type lookup, just recognizing the receiver's own
            // named type -- but it still needs its own case: without it,
            // a `cond.empty() ? a : b` ternary's own condition (ch06)
            // fails to resolve to 'bool' at all, even though a plain
            // `if (!cond.empty())` never required this (ExprKind::
            // Conditional, unlike TerminatorKind::Branch, strictly checks
            // its condition's inferred type -- see dataflow.cppm).
            if (expr.lhs != nullptr && expr.args.empty() && expr.name == "empty") {
                std::optional<Type> receiver_type = infer_expr_type(*expr.lhs, body, signatures);
                if (receiver_type.has_value()) {
                    const Type& effective = receiver_type->kind == TypeKind::Reference && receiver_type->pointee != nullptr
                                                ? *receiver_type->pointee
                                                : *receiver_type;
                    bool is_known_container =
                        effective.kind == TypeKind::Named &&
                        (effective.name == "std::string" || effective.name == "string" ||
                         is_vector_like_named_type(effective) || effective.name == "std::unordered_map" ||
                         effective.name == "unordered_map" || effective.name.starts_with("std::unordered_map.") ||
                         effective.name.starts_with("unordered_map.") || effective.name == "std::unordered_set" ||
                         effective.name == "unordered_set" || effective.name.starts_with("std::unordered_set.") ||
                         effective.name.starts_with("unordered_set."));
                    if (is_known_container) return named_type("bool");
                }
            }
            if (is_for_range_size_builtin(expr)) {
                std::optional<Type> range_type = infer_expr_type(*expr.args[0], body, signatures);
                if (!range_type.has_value()) return std::nullopt;
                const Type& unwrapped = range_type->kind == TypeKind::Reference && range_type->pointee != nullptr
                                            ? *range_type->pointee
                                            : *range_type;
                if (unwrapped.kind == TypeKind::Array || unwrapped.kind == TypeKind::Span) return named_type("int");
                if (is_vector_like_named_type(unwrapped)) return named_type("int");
                return std::nullopt;
            }
            if (is_reference_wrapper_constructor_call(expr)) {
                std::optional<Type> arg_type = infer_expr_type(*expr.args[0], body, signatures);
                if (arg_type.has_value() && arg_type->kind == TypeKind::Reference && arg_type->pointee != nullptr) {
                    Type wrapped;
                    wrapped.kind = TypeKind::Named;
                    wrapped.name = expr.name.starts_with("std::") ? "std::reference_wrapper" : "reference_wrapper";
                    Type referent = *arg_type->pointee;
                    referent.is_const_qualified = referent.is_const_qualified || !arg_type->is_mutable_ref;
                    wrapped.template_args.push_back(referent);
                    wrapped.is_reference_wrapper_lifetime_source = true;
                    return wrapped;
                }
            }
            if (is_optional_constructor_call(expr)) {
                std::optional<Type> arg_type = infer_expr_type(*expr.args[0], body, signatures);
                if (arg_type.has_value() && arg_type->is_reference_wrapper_lifetime_source) {
                    Type opt;
                    opt.kind = TypeKind::Named;
                    opt.name = expr.name.starts_with("std::") ? "std::optional" : "optional";
                    opt.template_args.push_back(*arg_type);
                    opt.is_reference_wrapper_lifetime_source = true;
                    return opt;
                }
            }
            if (std::optional<Type> alias_type = resolve_direct_type_alias_call_type(expr, body); alias_type.has_value()) {
                return *alias_type;
            }
            CalleeSignature callee = resolve_callee_signature(expr, body, signatures);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig != nullptr) return sig->return_type;
            // A call through a function-pointer-typed field --
            // `receiver.field_(args)`/`this->field_(args)`, parsed
            // identically to an ordinary named-method call
            // (parse_member_or_method_call fuses the member access and
            // the call into one node: expr.name holds the field's name,
            // expr.lhs the receiver) -- e.g. std::function<R(Args...)>'s
            // own `call()`: `return this->invoke_(this->object_,
            // args...);`. resolve_callee_signature just above already
            // recognizes this exact shape for argument validation, but
            // only when given a precomputed ClassFieldTypes cache (see
            // its own class_field_types-gated branches) that this call
            // site has no access to, so `callee`/`sig` come back empty
            // here and this call's own type would otherwise silently
            // resolve to nullopt -- which is fatal for a reference-
            // returning field (dataflow.cppm's return-type-compatibility
            // check has nothing to compare fn.return_type against) even
            // though a value-returning one tends to go unnoticed. See
            // function_pointer_field_call_return_type's own doc comment
            // (this file, above produces_rvalue_of_type) for why this is
            // factored out into a shared helper rather than kept inline.
            if (std::optional<Type> field_call_type = function_pointer_field_call_return_type(expr, body);
                field_call_type.has_value()) {
                return *field_call_type;
            }
            if (is_zero_arg_optional_constructor_call(expr)) {
                if (sig != nullptr && sig->return_type.is_reference_wrapper_lifetime_source) {
                    return sig->return_type;
                }
            }
            if (expr.lhs == nullptr && body.program != nullptr) {
                if (std::optional<std::string> resolved = resolve_visible_class_or_struct_name(
                        *body.program, body.function_namespace_path, expr.name, expr.explicit_global_qualification);
                    resolved.has_value()) {
                    return named_type(*resolved);
                }
            }
            return std::nullopt;
        }

        case ExprKind::PackExpansion:
            return std::nullopt;

        case ExprKind::Member: {
            std::optional<Type> base = infer_expr_type(*expr.lhs, body, signatures);
            if (!base) return std::nullopt;
            const Type& base_named =
                base->kind == TypeKind::Reference && base->pointee != nullptr ? *base->pointee : *base;
            if (base_named.kind != TypeKind::Named || body.program == nullptr) return std::nullopt;
            // `program.functions` (`Program`, ast.cppm's own compiler-
            // internal AST root type, is never itself parsed as a scpp
            // ClassDef/StructDef -- it's a host type merely referenced
            // by parser.cppm's own self-hosting source, so find_class_def/
            // find_struct_def below can never find a definition for it)
            // -- needed so a move/copy out of a `program.functions[i]`
            // element (e.g. `Function moved = std::move(program.functions[i]);`,
            // or as a call argument to an overloaded function like
            // push_back) can have its type verified at all, the same way
            // .back()/.front()/.substr() are special-cased just below for
            // the analogous host-library-receiver gap.
            if (base_named.name == "Program" && expr.name == "functions") {
                Type functions_type = named_type("std::vector");
                functions_type.template_args.push_back(named_type("Function"));
                return functions_type;
            }
            if (const ClassDef* def = find_class_def(*body.program, base_named.name)) {
                for (const ClassField& field : def->fields) {
                    if (field.name == expr.name) {
                        return field.type.kind == TypeKind::Reference ? std::optional<Type>(*field.type.pointee)
                                                                      : std::optional<Type>(field.type);
                    }
                }
                return {};
            }
            if (const StructDef* def = find_struct_def(*body.program, base_named.name)) {
                for (const StructField& field : def->fields) {
                    if (field.name == expr.name) {
                        return field.type.kind == TypeKind::Reference ? std::optional<Type>(*field.type.pointee)
                                                                      : std::optional<Type>(field.type);
                    }
                }
                return {};
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
            if (std::optional<Type> element = infer_vector_element_type(effective, body); element.has_value()) {
                return *element;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::expected<void, DataflowError> validate_sizeof_operand(const Expr& expr, const Body& body, const Signatures& signatures,
                             const SourceLocation& loc) {
    Type queried_type;
    if (expr.sizeof_operand_is_type) {
        queried_type = expr.type;
    } else {
        std::optional<Type> inferred = infer_expr_type(*expr.lhs, body, signatures);
        if (!inferred.has_value()) {
            return std::unexpected(DataflowError("cannot apply 'sizeof' to this expression: its type could not be inferred", loc));
        }
        queried_type = *inferred;
    }
    if (body.program == nullptr) {
        return std::unexpected(DataflowError("internal error: sizeof requires program type information", loc));
    }
    if (!layout_of_type(*body.program, queried_type).has_value()) {
        return std::unexpected(DataflowError("cannot apply 'sizeof' to this type in this version", loc));
    }
    return {};
}

std::expected<void, DataflowError> validate_alignof_operand(const Expr& expr, const Body& body, const SourceLocation& loc) {
    if (body.program == nullptr) {
        return std::unexpected(DataflowError("internal error: alignof requires program type information", loc));
    }
    if (!layout_of_type(*body.program, expr.type).has_value()) {
        return std::unexpected(DataflowError("cannot apply 'alignof' to this type in this version", loc));
    }
    return {};
}

} // namespace scpp
