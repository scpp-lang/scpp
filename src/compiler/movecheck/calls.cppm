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
    return expr.kind == ExprKind::NullptrLiteral;
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
[[nodiscard]] bool is_named_record_type(const Type& type, const Body& body);
[[nodiscard]] bool compile_time_dependency_visible_in_body(const FunctionSignature& candidate, const Body& body);

[[nodiscard]] bool is_copyable_class_lvalue_boundary_source(const Expr& expr, const Type& target_type,
                                                            const Body& body,
                                                            const Signatures& signatures);
[[nodiscard]] bool is_freely_copyable_class_value_source(const Expr& expr, const Type& target_type, const Body& body,
                                                         const Signatures& signatures);
[[nodiscard]] bool is_implicit_move_return_source(const Expr& expr, const Type& target_type, const Body& body);
[[nodiscard]] std::vector<ArgumentConversion> constructor_argument_conversions(const FunctionSignature& candidate,
                                                                               const std::vector<ExprPtr>& ctor_args,
                                                                               const Body& body,
                                                                               const Signatures& signatures);
[[nodiscard]] const FunctionSignature* find_single_argument_converting_constructor_signature(
            const Type& class_type, const Expr& arg, const Body& body, const Signatures& signatures);
        [[nodiscard]] bool argument_type_matches_parameter(const Type& arg_type, const Type& param_type, const Body& body);
[[nodiscard]] bool const_reference_binds_materialized_temporary(const Expr& arg, const Type& param_type,
                                                                const Body& body,
                                                                const Signatures& signatures,
                                                                bool allow_user_defined_conversion = true);
[[nodiscard]] bool argument_matches_parameter(const Expr& arg, const Type& param_type, const Body& body,
                                              const Signatures& signatures,
                                              bool allow_user_defined_conversion = true,
                                              bool require_usable_class_value_source = true);
[[nodiscard]] bool receiver_matches_method_qualifier(const Expr& receiver_expr,
                                                     const FunctionSignature& candidate,
                                                     const Body& body,
                                                     const Signatures& signatures);
[[nodiscard]] const FunctionSignature* resolve_overload(const Expr& call_expr, const CalleeSignature& callee,
                                                        const Body& body, const Signatures& signatures,
                                                          std::vector<const FunctionSignature*>* out_ambiguous = nullptr);
[[nodiscard]] const FunctionSignature* find_const_blocked_method_candidate(const Expr& call_expr,
                                                                           const CalleeSignature& callee,
                                                                           const Body& body,
                                                                           const Signatures& signatures);
// [over.match.oper]/2-3: an operator expression with a class operand is
// resolved as a call to an operator function, chosen from the member
// operator functions of the left operand's class plus the non-member
// ones found for the operands. This is the *one* place that answers
// "which operator function does `a @ b` call?" -- movecheck and codegen
// both ask it, so neither can select a function the other did not.
struct SelectedOperator {
    const FunctionSignature* signature = nullptr;
    ExprPtr call;
    std::size_t param_offset = 0;
    std::string method_name;
};

// `lhs_type`/`rhs_type` are passed in rather than inferred here: the
// callers already have them, and inferring an operand twice at one level
// of a left-leaning `a + b + c + ...` chain costs 2^n (see the
// ExprKind::Binary arm of infer_expr_type for the measurement).
[[nodiscard]] SelectedOperator resolve_binary_operator_call(const Expr& expr, const std::optional<Type>& lhs_type,
                                                            const std::optional<Type>& rhs_type, const Body& body,
                                                            const Signatures& signatures);
[[nodiscard]] SelectedOperator resolve_subscript_operator_call(const Expr& expr, const std::optional<Type>& base_type,
                                                               const Body& body, const Signatures& signatures);
[[nodiscard]] SelectedOperator resolve_unary_operator_call(const Expr& expr, const std::optional<Type>& operand_type,
                                                           const Body& body, const Signatures& signatures);
// [over.match.conv]/1: the conversion function of `operand`'s class that
// yields `destination`. `allow_explicit` is direct-initialization's
// answer ([dcl.init]/16.6, [expr.static.cast]/4) and copy-initialization's
// is false ([over.match.copy]/1).
//
// SCPP26 needs no ranking over a candidate *set* here the way C++26 does:
// [over.ics.user]'s second standard conversion sequence would be the only
// thing that could relate a conversion function's return type to a
// different destination, and §16.3(1)/(3) leave the identity conversion as
// the only one between scalar types. So exactly one conversion function
// can ever apply -- the one whose return type is the destination -- and
// looking it up by name is the whole of [over.match.conv] here.
[[nodiscard]] SelectedOperator resolve_conversion_function_call(const Expr& operand,
                                                               const std::optional<Type>& operand_type,
                                                               const Type& destination, bool allow_explicit,
                                                               const Body& body, const Signatures& signatures);
// The same lookup, answering only "does this class declare a conversion
// function to `destination` that copy-initialization is not allowed to
// use?" -- so a diagnostic can say `explicit` rather than "no conversion".
[[nodiscard]] bool has_explicit_only_conversion_function(const std::optional<Type>& operand_type,
                                                         const Type& destination, const Body& body,
                                                         const Signatures& signatures);
[[nodiscard]] const FunctionSignature* find_conversion_function_signature(const std::optional<Type>& operand_type,
                                                                          const Type& destination, const Body& body,
                                                                          const Signatures& signatures,
                                                                          std::string& key_out);
[[nodiscard]] bool operand_type_needs_an_operator_function(const Type& type, const Body& body);
[[nodiscard]] Type function_pointer_type_from_signature(const FunctionSignature& sig);
[[nodiscard]] bool same_function_pointer_shape_ignoring_unsafe(const Type& a, const Type& b);
[[nodiscard]] std::optional<Type> resolve_function_designator_type(const Expr& expr, const Type& target_type,
                                                                   const Body& body,
                                                                   const Signatures& signatures,
                                                                   const FunctionSignature** out_selected = nullptr);
[[nodiscard]] std::expected<void, DataflowError> check_function_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                       const Signatures& signatures, SourceLocation loc,
                                       const std::string& target_name, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_raw_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                  const Signatures& signatures, SourceLocation loc,
                                  const std::string& target_name, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_nullptr_assignment(const Type& target_type, const Expr& expr,
                                  SourceLocation loc, const std::string& target_name, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_scalar_conversion(const Type& target_type, const Expr& expr,
                                  const Body& body, const Signatures& signatures, SourceLocation loc,
                                  const std::string& target_name, bool report_errors);
[[nodiscard]] std::expected<void, DataflowError> check_expression_yields_a_value(const Expr& expr, const Body& body,
                                  const Signatures& signatures, SourceLocation loc, const std::string& role,
                                  bool report_errors);
[[nodiscard]] bool assignment_target_is_read_only(const Expr& expr, const Body& body,
                                                  const Signatures& signatures);
[[nodiscard]] bool place_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] std::string describe_const_source(const Expr& expr, const Body& body, const Signatures& signatures);
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
    // An enum value bound to a target that is neither an enum nor a scalar
    // is not this rule's question. The rule is about enum<->integer and
    // enum<->enum conversions (ch06 §6); binding `Mode` to a
    // `std::expected<Mode, E>` return type, or to any other class/struct
    // parameter, is constructor selection's question and is answered there
    // (check_constructor_arguments' exact-type match). Firing here would
    // reject `return mode;` from a function declared
    // `std::expected<FunctionEvalMode, ParseError>` -- which parser.cppm
    // does -- with a diagnostic about integers that names nothing in the
    // program. The target-is-enum direction stays unconditional: nothing
    // else answers "what may initialize an enum".
    if (!target_is_enum && !is_scalar_named_type(target_operand)) return {};
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
                                                          const Body& body, const Signatures& signatures,
                                                          std::vector<const FunctionSignature*>* out_ambiguous);
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

// Whether `type` names a `class` specifically -- *not* a struct. Only
// three questions are genuinely class-only: access control (a struct's
// members are never access-controlled), virtual/interface membership
// ([spec §11.1(2.3)]: a struct shall not declare a virtual member), and
// the mandatory virtual destructor ([spec §11.5(1)]). Every
// copy/move/ownership rule (§6.4/§6.5/§6.6) is about *record* types and
// must ask is_named_record_type instead.
[[nodiscard]] bool is_named_class_type(const Type& type, const Body& body) {
    if (type.kind != TypeKind::Named || body.program == nullptr) return false;
    for (const ClassDef& def : body.program->classes) {
        if (def.name == type.name) return !def.is_concept_witness;
    }
    return false;
}

// The struct sibling of is_named_class_type: a plain aggregate record,
// which a brace-enclosed initializer list may initialize directly.
[[nodiscard]] const StructDef* find_struct_def_for_brace_binding(const Type& type, const Body& body) {
    if (type.kind != TypeKind::Named || body.program == nullptr) return nullptr;
    if (is_named_class_type(type, body)) return nullptr;
    for (const StructDef& def : body.program->structs) {
        if (def.name == type.name) {
            for (const StructField& field : def.fields) {
                if (field.access != AccessSpecifier::Public) return nullptr;
            }
            return &def;
        }
    }
    return nullptr;
}

[[nodiscard]] bool type_absorbs_brace_initializer_run(const Type& type, const Body& body) {
    if (type.kind == TypeKind::Array && type.element != nullptr) return true;
    return find_struct_def_for_brace_binding(type, body) != nullptr;
}

void count_braced_init_list_fill(const Type& type, const std::vector<ExprPtr>& args, std::size_t& index,
                                 const Body& body, const Signatures& signatures);

// Movecheck's copy of codegen's cursor walk (see
// Codegen::count_braced_init_list_cursor). It answers the same question
// -- can this braced list initialize this parameter type? -- because
// movecheck resolves overloads independently, and a different answer
// here would move-check a call against a signature codegen does not
// select.
void count_braced_init_list_cursor(const Type& type, const std::vector<ExprPtr>& args, std::size_t& index,
                                   const Body& body, const Signatures& signatures) {
    if (index >= args.size()) return;
    const Expr& next = *args[index];
    if (next.kind == ExprKind::BracedInitList) {
        ++index;
        return;
    }
    if (!type_absorbs_brace_initializer_run(type, body)) {
        ++index;
        return;
    }
    if (type.kind == TypeKind::Named) {
        std::optional<Type> source_type = infer_expr_type(next, body, signatures);
        if (source_type.has_value() && types_equal(*source_type, type)) {
            ++index;
            return;
        }
    }
    count_braced_init_list_fill(type, args, index, body, signatures);
}

void count_braced_init_list_fill(const Type& type, const std::vector<ExprPtr>& args, std::size_t& index,
                                 const Body& body, const Signatures& signatures) {
    if (type.kind == TypeKind::Array && type.element != nullptr) {
        for (std::int64_t covered = 0; covered < type.array_size && index < args.size(); ++covered) {
            count_braced_init_list_cursor(*type.element, args, index, body, signatures);
        }
        return;
    }
    const StructDef* def = find_struct_def_for_brace_binding(type, body);
    if (def == nullptr) return;
    for (std::size_t field = 0; field < def->fields.size() && index < args.size(); ++field) {
        count_braced_init_list_cursor(def->fields[field].type, args, index, body, signatures);
    }
}

[[nodiscard]] bool braced_init_list_can_initialize(const Type& type, const std::vector<ExprPtr>& args, const Body& body,
                                                   const Signatures& signatures);

[[nodiscard]] bool braced_init_list_can_initialize(const Type& type, const std::vector<ExprPtr>& args, const Body& body,
                                                   const Signatures& signatures) {
    // Mirrors codegen's identically named rule: a reference target binds
    // to the temporary the list materializes (lifetime of the full call
    // expression), except a *mutable* reference, which has no observable
    // place for the callee's writes to reach.
    if (is_reference(type)) {
        if (type.pointee == nullptr) return false;
        if (type.is_mutable_ref && !type.is_rvalue_ref) return false;
        return braced_init_list_can_initialize(*type.pointee, args, body, signatures);
    }
    if (type_absorbs_brace_initializer_run(type, body)) {
        std::size_t index = 0;
        count_braced_init_list_fill(type, args, index, body, signatures);
        return index == args.size();
    }
    if (is_named_class_type(type, body)) {
        // A class-typed target makes the list a constructor call, whose
        // arity is a real filter even before argument types are
        // considered; codegen's own braced_init_list_can_initialize
        // resolves the overload exactly and rejects what does not match.
        std::string ctor_name = type.name;
        ctor_name += "_new";
        for (const auto& [name, overloads] : signatures) {
            if (name != ctor_name && !(!name.empty() && name.starts_with(ctor_name + "."))) continue;
            for (const FunctionSignature& candidate : overloads) {
                if (candidate.member_owner_class != type.name) continue;
                if (signature_accepts_argument_count(candidate, args.size(), /*param_offset=*/1)) return true;
            }
        }
        return args.empty();
    }
    return args.size() <= 1;
}

// Whether `type` names a user-declared *record* type -- a `class` or a
// `struct`. This is the question spec §6.4/§6.5/§6.6 ask: those clauses
// govern "a class type", which in [class.pre]/[dcl.type] terms is any
// class, struct or union, and §6.5's own worked example is spelled
// `struct RefCounted`. Deliberately *not* is_named_class_type, which
// answers the narrower `class`-vs-`struct` question access control (and
// only access control) needs -- asking that one for an ownership rule is
// what left every struct outside §6.4/§6.5 entirely.
[[nodiscard]] bool is_named_record_type(const Type& type, const Body& body) {
    if (is_named_class_type(type, body)) return true;
    if (type.kind != TypeKind::Named || body.program == nullptr) return false;
    for (const StructDef& def : body.program->structs) {
        if (def.name == type.name) return !def.is_concept_witness;
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
    return body.program != nullptr && is_named_record_type(target_type, body) &&
           is_bare_same_type_copy_source(expr, target_type, body, signatures) &&
           is_copy_constructible(target_type.name, *body.program);
}

[[nodiscard]] bool is_freely_copyable_class_value_source(const Expr& expr, const Type& target_type, const Body& body,
                                                         const Signatures& signatures) {
    if (body.program == nullptr || !is_named_record_type(target_type, body) ||
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
    // An array argument decays to a pointer to its first element. An
    // array is read-only if either it or its element carries the
    // qualifier: `const char w[3]` records it on the element, a string
    // literal on the array itself (see string_literal_type) -- either
    // way it may not bind to a pointer whose pointee is mutable.
    if (param_type.kind == TypeKind::Pointer && arg_type.kind == TypeKind::Array &&
        param_type.pointee != nullptr && arg_type.element != nullptr) {
        return (!param_type.is_mutable_pointee ||
                !(arg_type.is_const_qualified || arg_type.element->is_const_qualified)) &&
               types_equal(*arg_type.element, *param_type.pointee);
    }
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
                                                                const Body& body, const Signatures& signatures,
                                                                bool allow_user_defined_conversion) {
    if (!is_reference(param_type) || param_type.is_rvalue_ref || param_type.is_mutable_ref || param_type.pointee == nullptr) {
        return false;
    }
    if (produces_rvalue_of_type(arg, *param_type.pointee, body, signatures)) return true;
    return allow_user_defined_conversion && is_named_record_type(*param_type.pointee, body) &&
           find_single_argument_converting_constructor_signature(*param_type.pointee, arg, body, signatures) != nullptr;
}

// [over.best.ics]/4: when the argument being converted is itself the
// argument of a user-defined conversion -- a converting constructor's own
// parameter -- only standard conversion sequences are considered. That is
// what `allow_user_defined_conversion` spells: user-defined conversions do
// not chain. It is also what terminates the recursion below, since
// find_single_argument_converting_constructor_signature asks this same
// question of each candidate constructor's parameter.
//
// [dcl.init]/17.6.1: whether a by-value class argument can actually be
// *copied* into its parameter is decided after a candidate is chosen, not
// while choosing one -- clang selects `C(S)` for `C c{s}` and only then
// reports S's deleted copy constructor. `require_usable_class_value_source`
// spells that: constructor selection passes false so that a class-typed
// parameter is matched on its type alone and the by-value/§6.5(2) rule
// keeps its own, far more specific diagnostic downstream. Folding it into
// viability instead makes the rejection contradict itself -- "no
// constructor of 'C' matches: argument type is 'S'; candidate: C(S)".
[[nodiscard]] bool argument_matches_parameter(const Expr& arg, const Type& param_type, const Body& body,
                                                const Signatures& signatures,
                                                bool allow_user_defined_conversion,
                                                bool require_usable_class_value_source) {
    if (arg.kind == ExprKind::BracedInitList) {
        return braced_init_list_can_initialize(param_type, arg.args, body, signatures);
    }
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
        if (const_reference_binds_materialized_temporary(arg, param_type, body, signatures,
                                                         allow_user_defined_conversion)) {
            return true;
        }
        // [over.ics.ref]/1: a `T&` binds only to an lvalue. A
        // prvalue -- spec §6.6(1)'s "fresh value": a call's returned
        // value, a `new`, a constructed temporary -- has no place to
        // borrow from, so no conversion sequence exists and the
        // candidate is not viable. The Move/literal list just below
        // was a hand-enumerated subset of the same rule that left
        // `Call` out, which is why `f(g())` with `f(int)` and
        // `f(int&)` declared did not simply pick `f(int)`: `f(int&)`
        // stayed viable, tied, and the call was rejected for trying
        // to borrow `g()`'s result.
        if (param_type.is_mutable_ref && param_type.pointee != nullptr &&
            produces_rvalue_of_type(arg, *param_type.pointee, body, signatures)) {
            return false;
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
    if (literal_argument_adopts_parameter_type(arg, param_type)) return true;
    std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
    if (!arg_type.has_value()) return false;
    if (!argument_type_matches_parameter(*arg_type, param_type, body)) {
        if (allow_user_defined_conversion && is_named_record_type(param_type, body) &&
            find_single_argument_converting_constructor_signature(param_type, arg, body, signatures) != nullptr) {
            return true;
        }
        // [over.ics.user]'s other half: the *argument* has class type
        // and reaches the parameter through a conversion function it
        // declares. Guarded by the same `allow_user_defined_conversion`
        // as the converting-constructor route above, so [over.best.ics]/4
        // keeps holding -- neither may chain onto the other.
        if (allow_user_defined_conversion) {
            std::string key;
            const FunctionSignature* conversion =
                find_conversion_function_signature(arg_type, param_type, body, signatures, key);
            if (conversion != nullptr && !conversion->is_explicit) return true;
        }
        return false;
    }
    if (require_usable_class_value_source && is_named_record_type(param_type, body)) {
        return is_copyable_class_lvalue_boundary_source(arg, param_type, body, signatures) ||
               is_freely_copyable_class_value_source(arg, param_type, body, signatures) ||
               produces_rvalue_of_type(arg, param_type, body, signatures);
    }
    return true;
}

// A parameter that names a type which does not exist at this phase is a
// monomorphization placeholder -- the `T` of `template<typename T> C(T)`.
// Movecheck runs before those are substituted, so matching an argument
// against one can only succeed: whatever the argument's type is, that is
// what `T` will become. Every *other* named type does exist, and matching
// against it is the check.
//
// Drawing that line is the whole of constructor type checking here. Without
// it -- and the predicate below drew no line at all, substituting the
// argument's own type for *every* bare named parameter type -- the
// comparison compared a type with itself and could not fail: `C(int)`
// "accepted" a double, a bool, a pointer and an unrelated record alike.
// Codegen's copy of that predicate, under the same name, still carries the
// same lambda with an empty body (semantics.cppm), so the two passes
// answered "does this argument match this parameter?" differently and only
// codegen's answer was a check -- which is why the front end appeared to
// reject these calls while movecheck was silently selecting a constructor
// for them.
[[nodiscard]] bool named_type_is_monomorphization_placeholder(const Type& type, const Body& body) {
    if (type.kind != TypeKind::Named || type.name.empty()) return false;
    if (!type.template_args.empty() || !type.non_type_args.empty()) return false;
    if (is_scalar_named_type(type) || is_void_named_type(type)) return false;
    // spec §16.4(5): `nullptr_t` "is not a scalar type: it is not named in
    // Table 1". It is nonetheless a real, spellable fundamental type, so it
    // is not a placeholder -- and treating it as one makes
    // `std::unique_ptr(nullptr_t)` accept every pointer argument that
    // `unique_ptr(T*)` should take, the very hazard codegen's own
    // resolver already carries a comment about.
    if (is_nullptr_type(type)) return false;
    if (is_enum_type(type, body.program)) return false;
    return !is_named_record_type(type, body);
}

// "Does this argument bind to this parameter?" -- for a *constructor's*
// parameter. Which is the same question argument_matches_parameter answers
// for a function's, so it is answered by calling it, with the one thing that
// genuinely differs applied first: a constructor may still carry an
// unsubstituted template parameter as its parameter type.
//
// It used to be a second, hand-copied answer, and it had drifted from the
// first in four ways, each an under-rejection or an over-rejection of its
// own: it never let a `const std::string&` parameter bind a string literal
// through a converting constructor ([over.ics.user], the case
// const_reference_binds_materialized_temporary exists for); it never applied
// [over.ics.ref]/1 to a `T&` parameter given a prvalue; it could not see a
// nested braced-init-list argument at all; and a by-value record parameter
// got no converting-constructor route either. A constructor call and a
// function call were being judged by two different rules.
[[nodiscard]] bool constructor_parameter_accepts_argument_directly(const Expr& arg, const Type& param_type,
                                                                   const Body& body, const Signatures& signatures,
                                                                   bool allow_user_defined_conversion,
                                                                   bool require_usable_class_value_source) {
    // A parameter naming a type that does not exist at this phase is a
    // monomorphization placeholder -- the `T` of `template<typename T> C(T)`.
    // Movecheck runs before those are substituted, so whatever the argument's
    // type is, that is what `T` will become: substituting it is what makes the
    // *type* half of the match trivially true. Nothing else about the match is
    // -- a bare `std::unique_ptr` lvalue is no more a legitimate by-value
    // argument for a `T` than for a spelled-out one -- so the substituted type
    // goes through the same check as any other. Every *other* named type does
    // exist, and matching against it is the check.
    Type effective_param_type = param_type;
    if (named_type_is_monomorphization_placeholder(param_type, body)) {
        std::optional<Type> inferred = infer_expr_type(arg, body, signatures);
        if (!inferred.has_value()) return true;
        effective_param_type = *inferred;
    }
    return argument_matches_parameter(arg, effective_param_type, body, signatures,
                                      allow_user_defined_conversion, require_usable_class_value_source);
}

// Selection proper. A class-typed parameter is matched on its type alone
// here: the by-value copy rule ([dcl.init]/17.6.1, spec §6.5(2)) is applied
// to the *selected* constructor's arguments downstream, where it has its own
// diagnostic. Every other user of this predicate is answering "is this call
// viable at all?" with no such downstream re-check, and keeps it.
[[nodiscard]] bool argument_matches_parameter_for_constructor_selection(const Expr& arg, const Type& param_type,
                                                                       const Body& body, const Signatures& signatures,
                                                                       bool allow_user_defined_conversion) {
    return constructor_parameter_accepts_argument_directly(arg, param_type, body, signatures,
                                                           allow_user_defined_conversion,
                                                           /*require_usable_class_value_source=*/false);
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
    std::vector<const FunctionSignature*> matches;
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
            if (constructor_parameter_accepts_argument_directly(arg, ctor_param_type, body, signatures,
                                                               /*allow_user_defined_conversion=*/false,
                                                               /*require_usable_class_value_source=*/true)) {
                matches.push_back(&candidate);
            }
        }
    }
    if (matches.empty()) return nullptr;
    if (matches.size() == 1) return matches[0];
    // [over.ics.rank] through the shared algebra, exactly as codegen's
    // find_single_argument_converting_constructor now does. This returned
    // the first candidate the signature map happened to enumerate, which
    // is not even a stable order -- so movecheck could decide a
    // conversion existed via one constructor while codegen, ranking,
    // found the pair ambiguous and emitted nothing.
    std::vector<ExprPtr> single_arg;
    single_arg.push_back(deep_clone_expr(arg));
    std::vector<std::vector<ArgumentConversion>> conversions;
    for (const FunctionSignature* candidate : matches) {
        conversions.push_back(constructor_argument_conversions(*candidate, single_arg, body, signatures));
    }
    std::vector<std::size_t> best = best_viable_candidates(conversions);
    if (best.size() != 1) return nullptr;
    return matches[best[0]];
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
// When strictly more than one candidate is viable, ranks them with the
// shared [over.ics.rank] algebra in ast.cppm -- the same one codegen and
// the constant evaluator use, so that all three passes name the same
// function. A call with no single best viable candidate is ambiguous
// ([over.match.best]) and resolves to nothing; describe_overload_failure
// reports it by name.
// Explains a resolve_overload failure, for the case where the *name*
// exists and only the call doesn't fit it.
//
// One message -- "no overload of 'f' matches these argument types" --
// used to stand in for every cause here, including the one it is
// factually wrong about: passing the wrong *number* of arguments is not
// an argument-type problem, and telling the reader to look at their
// types sends them to the wrong place. The same generic-message-for-
// several-distinct-causes shape as codegen's "call to unknown function";
// both are fixed the same way, by asking the resolver's own predicates
// which stage rejected each candidate.
[[nodiscard]] std::string describe_overload_failure(const Expr& call_expr, const CalleeSignature& callee,
                                                     const std::string& display_name,
                                                     const std::vector<FunctionSignature>& candidates, const Body& body,
                                                     const Signatures& signatures) {
    auto describe_signature = [&](const FunctionSignature& sig) {
        std::string result = display_name + "(";
        for (std::size_t i = callee.param_offset; i < sig.param_types.size(); i++) {
            if (i != callee.param_offset) result += ", ";
            result += describe_type_brief(sig.param_types[i]);
        }
        result += ")";
        if (sig.is_generic_template) result += " [generic]";
        return result;
    };
    std::string candidate_list;
    for (const FunctionSignature& sig : candidates) {
        candidate_list += "\n  candidate: " + describe_signature(sig);
    }

    // Ambiguity is reported before any per-candidate rejection, because
    // there is no rejection to report: every tied candidate matched.
    // Worded identically to codegen's describe_call_resolution_failure --
    // one question, one message, whichever pass happens to reach it
    // first. This used to fall through to "no overload of 'f' matches
    // these argument types", which is factually the opposite of what
    // happened: they all matched.
    {
        std::vector<const FunctionSignature*> tied;
        if (resolve_overload(call_expr, callee, body, signatures, &tied) == nullptr && tied.size() > 1) {
            std::string result = "ambiguous call to '" + display_name + "': " + std::to_string(tied.size()) +
                                 " overloads match these argument types equally well and none is better than the others ([over.match.best])";
            for (const FunctionSignature* sig : tied) result += "\n  candidate: " + describe_signature(*sig);
            return result;
        }
    }

    std::vector<const FunctionSignature*> right_arity;
    for (const FunctionSignature& sig : candidates) {
        if (signature_accepts_argument_count(sig, call_expr.args.size(), callee.param_offset)) right_arity.push_back(&sig);
    }
    if (right_arity.empty()) {
        return "no overload of '" + display_name + "' takes " + std::to_string(call_expr.args.size()) +
               (call_expr.args.size() == 1 ? " argument" : " arguments") + candidate_list;
    }

    for (const FunctionSignature* sig : right_arity) {
        std::size_t fixed_param_count = sig->param_types.size() - callee.param_offset;
        for (std::size_t i = 0; i < call_expr.args.size() && i < fixed_param_count; i++) {
            const Type& param_type = sig->param_types[i + callee.param_offset];
            if (argument_matches_parameter(*call_expr.args[i], param_type, body, signatures)) continue;
            // Mirrors codegen's identical special case: a braced list has
            // no type to name and no static_cast to suggest.
            if (call_expr.args[i]->kind == ExprKind::BracedInitList) {
                return "no overload of '" + display_name + "' matches these argument types: argument " +
                       std::to_string(i + 1) + " is a brace-enclosed initializer list of " +
                       std::to_string(call_expr.args[i]->args.size()) +
                       (call_expr.args[i]->args.size() == 1 ? " initializer" : " initializers") +
                       ", which does not initialize parameter type '" + describe_type_brief(param_type) +
                       "': check that its elements match that type's members in number and type" +
                       (candidates.size() > 1 ? candidate_list : std::string());
            }
            std::optional<Type> actual = infer_expr_type(*call_expr.args[i], body, signatures);
            // [over.match.copy]/1 excluded the one conversion that
            // would have made this argument viable. Saying so is not the
            // same as saying no conversion exists, and the §16.3(3)
            // message below describes a *scalar* argument, which this
            // one is not.
            if (has_explicit_only_conversion_function(actual, param_type, body, signatures)) {
                return "no overload of '" + display_name + "' matches these argument types: argument " +
                       std::to_string(i + 1) + ": " +
                       explicit_only_conversion_function_message(describe_type_brief(*actual),
                                                                 describe_type_brief(param_type)) +
                       (candidates.size() > 1 ? candidate_list : std::string());
            }
            return "no overload of '" + display_name + "' matches these argument types: argument " +
                   std::to_string(i + 1) + " is " +
                   (actual.has_value() ? "'" + describe_type_brief(*actual) + "'" : "a different type") + ", but '" +
                   describe_signature(*sig) + "' expects '" + describe_type_brief(param_type) +
                   "' (spec §16.3(3) -- an argument of scalar type matches a parameter of scalar "
                   "type only if the two types are the same; an explicit static_cast<T> may be required)" +
                   (candidates.size() > 1 ? candidate_list : std::string());
        }
    }
    return "no overload of '" + display_name +
           "' matches these argument types (spec §16.3(3) -- an argument of scalar type matches a "
           "parameter of scalar type only if the two types are the same; an explicit static_cast<T> may be required)" +
           candidate_list;
}


// The movecheck half of the shared [over.ics.rank] vocabulary: one entry
// per argument, with the implicit object parameter as entry zero when
// there is a receiver. Deliberately the same shape codegen's
// argument_conversions_for produces, because the two passes must not
// answer "which function does this call name?" differently -- a call
// move-checked against one signature and emitted against another is the
// hazard already flagged where apply_reference_argument reasons about
// resolution.
[[nodiscard]] std::vector<ArgumentConversion> argument_conversions_for(const Expr& call_expr,
                                                                       const FunctionSignature& candidate,
                                                                       const CalleeSignature& callee, const Body& body,
                                                                       const Signatures& signatures) {
    auto strip_to_value = [](Type type) {
        if (type.kind == TypeKind::Reference && type.pointee != nullptr) type = *type.pointee;
        type.is_const_qualified = false;
        return type;
    };
    auto reference_facts = [](const Type& type, ArgumentConversion& conversion) {
        if (type.kind != TypeKind::Reference) return;
        conversion.binds_reference = true;
        conversion.reference_is_mutable = type.is_mutable_ref && !type.is_rvalue_ref;
        conversion.reference_is_rvalue = type.is_rvalue_ref;
    };
    std::vector<ArgumentConversion> result;
    if (callee.param_offset == 1 && call_expr.lhs != nullptr && !candidate.param_types.empty()) {
        ArgumentConversion receiver_conversion{};
        receiver_conversion.rank = ConversionRank::Identity;
        reference_facts(candidate.param_types[0], receiver_conversion);
        if (candidate.param_types[0].pointee != nullptr) {
            receiver_conversion.argument_is_rvalue =
                produces_rvalue_of_type(*call_expr.lhs, *candidate.param_types[0].pointee, body, signatures);
            if (!candidate.param_types[0].is_mutable_ref && is_read_only_reachable(*call_expr.lhs, body, signatures)) {
                receiver_conversion.unknown = true;
            }
        }
        result.push_back(receiver_conversion);
    }
    std::size_t fixed_param_count = candidate.param_types.size() - callee.param_offset;
    for (std::size_t i = 0; i < call_expr.args.size(); i++) {
        ArgumentConversion conversion{};
        conversion.rank = ConversionRank::Identity;
        if (i >= fixed_param_count) {
            conversion.unknown = true;
            result.push_back(conversion);
            continue;
        }
        const Type& param_type = candidate.param_types[i + callee.param_offset];
        reference_facts(param_type, conversion);
        Type target = param_type;
        if (param_type.kind == TypeKind::Reference && param_type.pointee != nullptr) target = *param_type.pointee;
        Type target_value = target;
        target_value.is_const_qualified = false;
        conversion.argument_is_rvalue = produces_rvalue_of_type(*call_expr.args[i], target_value, body, signatures);
        // spec §16.2(1): a literal argument *has* the parameter's scalar
        // type, so the sequence is the identity, not a conversion --
        // but only for the type §16.2(3) gives it absent a context; see
        // literal_argument_ranks_as_identity.
        if (literal_argument_adopts_parameter_type(*call_expr.args[i], param_type)) {
            if (!literal_argument_ranks_as_identity(*call_expr.args[i], param_type))
                conversion.rank = ConversionRank::Conversion;
            result.push_back(conversion);
            continue;
        }
        std::optional<Type> arg_type = infer_expr_type(*call_expr.args[i], body, signatures);
        if (!arg_type.has_value()) {
            // movecheck cannot type every expression shape -- Member and
            // Subscript chains in particular, since it has no Program
            // access to their field types. An unknown sequence compares
            // equal to every other, so a shape movecheck cannot see
            // never invents a preference codegen would not agree with.
            conversion.unknown = true;
        } else {
            Type from = decay_array_to_pointer(*arg_type);
            if (types_equal(strip_to_value(from), strip_to_value(target))) {
                conversion.rank = ConversionRank::Identity;
            } else if (is_qualification_conversion(from, target)) {
                conversion.rank = ConversionRank::Qualification;
            } else {
                conversion.rank = ConversionRank::Conversion;
            }
        }
        result.push_back(conversion);
    }
    return result;
}

[[nodiscard]] const FunctionSignature* resolve_overload(const Expr& call_expr, const CalleeSignature& callee,
                                                          const Body& body, const Signatures& signatures,
                                                          std::vector<const FunctionSignature*>* out_ambiguous) {
    if (out_ambiguous != nullptr) out_ambiguous->clear();
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
            const Type& param_type = candidate.param_types[i + callee.param_offset];
            all_match = argument_matches_parameter(*call_expr.args[i], param_type, body, signatures);
            // [over.ics.ref]: a read-only argument forms no conversion
            // sequence to a `T&` parameter, so the candidate is not
            // viable -- the same rule codegen's classify_call_candidate
            // applies, asked here so that the two passes select the same
            // function.
            if (all_match && is_reference(param_type) && param_type.is_mutable_ref && !param_type.is_rvalue_ref &&
                is_read_only_reachable(*call_expr.args[i], body, signatures)) {
                all_match = false;
            }
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

    // The implicit object parameter's ref-qualifier, when the class
    // declares both a qualified and an unqualified overload of the same
    // name -- C++ makes that pair ill-formed to declare, so there is no
    // [over.ics.rank] rule for it and it stays a narrowing preference
    // rather than part of the ranking below. Kept identical to codegen's
    // resolve_overload_by_type so the two passes select the same
    // function for `std::move(x).error()`.
    if (callee.param_offset == 1 && call_expr.lhs != nullptr) {
        std::vector<const FunctionSignature*> ref_qualified;
        for (const FunctionSignature* candidate : matches) {
            if (candidate->receiver_ref_qualifier == ReceiverRefQualifier::None) continue;
            if (candidate->param_types.empty() || candidate->param_types[0].pointee == nullptr) continue;
            bool receiver_is_rvalue =
                produces_rvalue_of_type(*call_expr.lhs, *candidate->param_types[0].pointee, body, signatures);
            if (receiver_is_rvalue ? candidate->receiver_ref_qualifier == ReceiverRefQualifier::RValue
                                   : candidate->receiver_ref_qualifier == ReceiverRefQualifier::LValue) {
                ref_qualified.push_back(candidate);
            }
        }
        if (!ref_qualified.empty()) matches = std::move(ref_qualified);
        if (matches.size() == 1) return matches[0];
    }

    // [over.match.best] over [over.ics.rank], using the same shared
    // algebra codegen and the constant evaluator use.
    //
    // This used to be a scalar `mutable_ref_score` followed by
    // `return unique_best ? best : matches[0];` -- on a tie, the
    // candidate that happened to be declared first, with no diagnostic.
    // Two things were wrong with that. A scalar score cannot represent
    // [over.ics.rank]'s "neither is better", which is the whole content
    // of an ambiguous call; and the fallback made movecheck select a
    // signature codegen does not select, so a call could be
    // move-checked against `f(int)` while `f(int&)` was emitted.
    // A genuinely ambiguous call has no selection to make: codegen
    // rejects the program with the ambiguity message, so there is no
    // emitted code for the checks below to have missed.
    std::vector<std::vector<ArgumentConversion>> conversions;
    for (const FunctionSignature* candidate : matches) {
        conversions.push_back(argument_conversions_for(call_expr, *candidate, callee, body, signatures));
    }
    std::vector<std::size_t> best = best_viable_candidates(conversions);
    if (best.size() == 1) return matches[best[0]];
    if (out_ambiguous != nullptr) {
        for (std::size_t index : best) out_ambiguous->push_back(matches[index]);
    }
    return nullptr;
}
// [over.built]: the built-in candidates for the operators exist for the
// arithmetic, enumeration and pointer types only. An operand of class
// type therefore has no built-in candidate at all, which is what makes
// "no operator function" ill-formed rather than a fall-through to
// integer arithmetic.
[[nodiscard]] bool operand_type_needs_an_operator_function(const Type& type, const Body& body) {
    const Type& effective = type.kind == TypeKind::Reference && type.pointee != nullptr ? *type.pointee : type;
    return is_named_record_type(effective, body);
}

[[nodiscard]] const Type& operator_operand_type(const Type& type) {
    return type.kind == TypeKind::Reference && type.pointee != nullptr ? *type.pointee : type;
}

// [basic.lookup.argdep]: a non-member operator function is looked up in
// the namespaces associated with the operand types as well as at the
// point of use. Without this, `std::operator+(const char*, const
// std::string&)` would be invisible to `"a" + s` written outside
// namespace std.
void collect_operator_lookup_keys(const std::string& method_name, const std::optional<Type>& lhs_type,
                                  const std::optional<Type>& rhs_type, const Body& body,
                                  std::vector<std::string>& keys) {
    keys.push_back(method_name);
    if (!body.function_namespace_path.empty()) {
        std::string qualified;
        for (std::size_t i = 0; i < body.function_namespace_path.size(); i++) {
            qualified += body.function_namespace_path[i];
            qualified += "::";
        }
        qualified += method_name;
        keys.push_back(qualified);
    }
    auto add_associated = [&](const std::optional<Type>& type) {
        if (!type.has_value()) return;
        const Type& effective = operator_operand_type(*type);
        if (effective.kind != TypeKind::Named) return;
        std::size_t pos = effective.name.rfind("::");
        if (pos == std::string::npos) return;
        std::string qualified = effective.name.substr(0, pos + 2);
        qualified += method_name;
        keys.push_back(qualified);
    };
    add_associated(lhs_type);
    add_associated(rhs_type);
}

[[nodiscard]] ExprPtr make_free_operator_call_expr(const Expr& lhs, const Expr& rhs, const std::string& method_name,
                                                   const SourceLocation& loc) {
    ExprPtr call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->loc = loc;
    call->name = method_name;
    call->args.push_back(deep_clone_expr_with_loc(lhs, loc));
    call->args.push_back(deep_clone_expr_with_loc(rhs, loc));
    return call;
}

[[nodiscard]] SelectedOperator resolve_binary_operator_call(const Expr& expr, const std::optional<Type>& lhs_type,
                                                            const std::optional<Type>& rhs_type, const Body& body,
                                                            const Signatures& signatures) {
    SelectedOperator selected{};
    if (expr.kind != ExprKind::Binary || expr.lhs == nullptr || expr.rhs == nullptr) return selected;
    // Nothing to select unless an operand has class type: [over.built]
    // covers every other operand pair, and asking further would search
    // the signature table on every scalar `+` in the program.
    bool class_operand_present = (lhs_type.has_value() && operand_type_needs_an_operator_function(*lhs_type, body)) ||
                                 (rhs_type.has_value() && operand_type_needs_an_operator_function(*rhs_type, body));
    if (!class_operand_present) return selected;
    std::string method_name = binary_operator_method_name(expr.binary_op);
    if (method_name.empty()) return selected;
    // The member candidates of the left operand's class ([over.match.oper]/2.1).
    if (lhs_type.has_value()) {
        const Type& lhs_operand = operator_operand_type(*lhs_type);
        if (lhs_operand.kind == TypeKind::Named) {
            std::string key = lhs_operand.name;
            key += "_";
            key += method_name;
            if (signatures.contains(key)) {
                ExprPtr call = make_operator_call_expr(*expr.lhs, *expr.rhs, method_name, expr.loc);
                CalleeSignature callee{key, 1, std::nullopt};
                if (const FunctionSignature* sig = resolve_overload(*call, callee, body, signatures); sig != nullptr) {
                    selected.signature = sig;
                    selected.call = std::move(call);
                    selected.param_offset = 1;
                    selected.method_name = std::move(key);
                    return selected;
                }
            }
        }
    }
    // The non-member candidates ([over.match.oper]/2.2).
    std::vector<std::string> keys;
    collect_operator_lookup_keys(method_name, lhs_type, rhs_type, body, keys);
    for (const std::string& key : keys) {
        if (!signatures.contains(key)) continue;
        ExprPtr call = make_free_operator_call_expr(*expr.lhs, *expr.rhs, key, expr.loc);
        CalleeSignature callee{key, 0, std::nullopt};
        if (const FunctionSignature* sig = resolve_overload(*call, callee, body, signatures); sig != nullptr) {
            selected.signature = sig;
            selected.call = std::move(call);
            selected.param_offset = 0;
            selected.method_name = key;
            return selected;
        }
    }
    // [over.match.oper]/3.4.3's rewritten candidate, for `==`/`!=` only:
    // `a == b` also considers `b == a`.
    if ((expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne) && rhs_type.has_value()) {
        const Type& rhs_operand = operator_operand_type(*rhs_type);
        if (rhs_operand.kind == TypeKind::Named) {
            std::string key = rhs_operand.name;
            key += "_";
            key += method_name;
            if (signatures.contains(key)) {
                ExprPtr call = make_operator_call_expr(*expr.rhs, *expr.lhs, method_name, expr.loc);
                CalleeSignature callee{key, 1, std::nullopt};
                if (const FunctionSignature* sig = resolve_overload(*call, callee, body, signatures); sig != nullptr) {
                    selected.signature = sig;
                    selected.call = std::move(call);
                    selected.param_offset = 1;
                    selected.method_name = std::move(key);
                    return selected;
                }
            }
        }
    }
    return selected;
}

[[nodiscard]] SelectedOperator resolve_unary_operator_call(const Expr& expr, const std::optional<Type>& operand_type,
                                                           const Body& body, const Signatures& signatures) {
    SelectedOperator selected{};
    if (expr.kind != ExprKind::Unary || expr.lhs == nullptr) return selected;
    std::string method_name = unary_operator_method_name(expr.unary_op);
    if (method_name.empty()) return selected;
    if (!operand_type.has_value()) return selected;
    const Type& operand = operator_operand_type(*operand_type);
    if (operand.kind != TypeKind::Named) return selected;
    std::string key = operand.name;
    key += "_";
    key += method_name;
    if (!signatures.contains(key)) return selected;
    ExprPtr call = make_unary_operator_call_expr(*expr.lhs, method_name, expr.loc);
    CalleeSignature callee{key, 1, std::nullopt};
    if (const FunctionSignature* sig = resolve_overload(*call, callee, body, signatures); sig != nullptr) {
        selected.signature = sig;
        selected.call = std::move(call);
        selected.param_offset = 1;
        selected.method_name = std::move(key);
    }
    return selected;
}

// [over.match.conv]/1: see the declaration in this file's forward
// declarations for why a name lookup is the whole of the rule here.
[[nodiscard]] const FunctionSignature* find_conversion_function_signature(const std::optional<Type>& operand_type,
                                                                          const Type& destination, const Body& body,
                                                                          const Signatures& signatures,
                                                                          std::string& key_out) {
    if (!operand_type.has_value()) return nullptr;
    const Type& operand = operator_operand_type(*operand_type);
    if (operand.kind != TypeKind::Named) return nullptr;
    if (!is_named_record_type(operand, body)) return nullptr;
    key_out = conversion_function_key(operand.name, destination);
    auto it = signatures.find(key_out);
    if (it == signatures.end()) return nullptr;
    for (const FunctionSignature& candidate : it->second) {
        if (!compile_time_dependency_visible_in_body(candidate, body)) continue;
        if (candidate.param_types.size() != 1) continue;
        return &candidate;
    }
    return nullptr;
}

[[nodiscard]] SelectedOperator resolve_conversion_function_call(const Expr& operand,
                                                               const std::optional<Type>& operand_type,
                                                               const Type& destination, bool allow_explicit,
                                                               const Body& body, const Signatures& signatures) {
    SelectedOperator selected{};
    std::string key;
    const FunctionSignature* candidate =
        find_conversion_function_signature(operand_type, destination, body, signatures, key);
    if (candidate == nullptr) return selected;
    if (candidate->is_explicit && !allow_explicit) return selected;
    ExprPtr call = make_unary_operator_call_expr(operand, conversion_function_method_name(destination), operand.loc);
    CalleeSignature callee{key, 1, std::nullopt};
    if (const FunctionSignature* sig = resolve_overload(*call, callee, body, signatures); sig != nullptr) {
        selected.signature = sig;
        selected.call = std::move(call);
        selected.param_offset = 1;
        selected.method_name = std::move(key);
    }
    return selected;
}

[[nodiscard]] bool has_explicit_only_conversion_function(const std::optional<Type>& operand_type,
                                                         const Type& destination, const Body& body,
                                                         const Signatures& signatures) {
    std::string key;
    const FunctionSignature* candidate =
        find_conversion_function_signature(operand_type, destination, body, signatures, key);
    return candidate != nullptr && candidate->is_explicit;
}

// [over.sub]: `a[i]` with a class operand is `a.operator[](i)`. The
// declaration syntax exists, so the call has to resolve as well --
// accepting a declaration nothing can ever reach is the same hole
// one level down.
[[nodiscard]] SelectedOperator resolve_subscript_operator_call(const Expr& expr, const std::optional<Type>& base_type,
                                                               const Body& body, const Signatures& signatures) {
    SelectedOperator selected{};
    if (expr.kind != ExprKind::Subscript || expr.lhs == nullptr || expr.rhs == nullptr) return selected;
    if (!base_type.has_value()) return selected;
    const Type& base = operator_operand_type(*base_type);
    if (base.kind != TypeKind::Named || !is_named_record_type(base, body)) return selected;
    std::string key = base.name;
    key += "_operator_subscript";
    if (!signatures.contains(key)) return selected;
    ExprPtr call = make_operator_call_expr(*expr.lhs, *expr.rhs, std::string("operator_subscript"), expr.loc);
    CalleeSignature callee{key, 1, std::nullopt};
    if (const FunctionSignature* sig = resolve_overload(*call, callee, body, signatures); sig != nullptr) {
        selected.signature = sig;
        selected.call = std::move(call);
        selected.param_offset = 1;
        selected.method_name = std::move(key);
    }
    return selected;
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
                                                                   const Body& body, const Signatures& signatures,
                                                                   const FunctionSignature** out_selected) {
    if (out_selected != nullptr) *out_selected = nullptr;
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
        if (same_function_pointer_shape_ignoring_unsafe(candidate, target_type)) {
            if (out_selected != nullptr) *out_selected = &sig;
            return candidate;
        }
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
    // ch06 §6: `nullptr` initializes a function pointer just as it does
    // any other pointer -- a function pointer *is* a pointer type, so
    // excluding it here would have made `nullptr_t` convert to every
    // pointer type but one.
    if (is_nullptr_literal(expr)) return {};
    const FunctionSignature* designated = nullptr;
    std::optional<Type> source_type = resolve_function_designator_type(expr, target_type, body, signatures, &designated);
    // [dcl.fct.def.delete]/2: forming a pointer to a deleted function
    // *names* it just as calling it does. Nothing else would ever report
    // it -- the pointer's own signature check passes, and no source text
    // ever calls the deleted body.
    if (designated != nullptr && designated->is_deleted) {
        return std::unexpected(DataflowError(
            deleted_function_error_message("'" + designated->display_name + "'", designated->loc), loc));
    }
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
    // Everything reaching here has a source signature that differs from
    // the target's in the return type or in some parameter type -- the
    // two cases above are the only ones where a *difference* is
    // permitted, and both of them are a difference in the unsafe
    // qualifier alone. Falling through to acceptance let `int (*fp)(int)
    // = &wrong;` (with `int wrong(bool)`) through, and then called
    // `wrong` with an `int` argument through a pointer typed to promise
    // a `bool` one: a type confusion the language's whole no-implicit-
    // conversion rule exists to prevent, just spelled indirectly. The
    // comparison is `types_equal` per component, i.e. exact match, for
    // the same reason overload resolution is exact match (ch05 §5.10):
    // ch06 §6 gives scpp no implicit conversion between any two distinct
    // scalar types, so there is no signature a call through this pointer
    // could satisfy other than the one it names.
    //
    // Nothing about lifetimes participates: `[[scpp::lifetime]]` and the
    // reference-return inference live on the Function/FunctionSignature,
    // never on the FunctionPointer Type, so a signature cannot carry
    // them. That is not an omission here -- an indirect call through a
    // function pointer is already refused a borrow of its result
    // ("doesn't return a reference with an inferrable lifetime"), which
    // is the same answer #412 reached for a type-erased `std::function`.
    return std::unexpected(DataflowError(
        "cannot initialize or assign function pointer '" + target_name + "' of type '" +
            describe_type_brief(target_type) + "' from a function of type '" + describe_type_brief(*source_type) +
            "': a function pointer's return type and every parameter type must match exactly (spec ch05 §5.16/ch06 §6)",
        loc));
}

// ch06 §6: `nullptr` (type `nullptr_t`) converts to any raw pointer
// type, to any function pointer type, to `nullptr_t` itself, and -- via
// an ordinary converting constructor -- to a class type that declares
// one taking it. It converts to nothing else: not to `bool`, and not to
// any integer type.
//
// Checked here, where both scpp types are still in hand, rather than
// left to codegen's check_store_type -- that one sees only the lowered
// LLVM types, so it could say no more than "no implicit conversion
// between distinct scalar types ... use an explicit static_cast<T>",
// which names a category `nullptr_t` is not in and advises a cast the
// language does not accept for it either.
std::expected<void, DataflowError> check_nullptr_assignment(const Type& target_type, const Expr& expr,
                                                            SourceLocation loc, const std::string& target_name,
                                                            bool report_errors) {
    if (!report_errors || !is_nullptr_literal(expr)) return {};
    if (target_type.kind == TypeKind::Pointer || target_type.kind == TypeKind::FunctionPointer) return {};
    if (is_nullptr_type(target_type)) return {};
    // A class destination is decided by constructor overload resolution,
    // not here -- `std::unique_ptr<T> u = nullptr;` is a converting-
    // constructor call, and reporting it as an invalid conversion would
    // pre-empt the better message the constructor machinery already
    // produces when no such constructor exists.
    if (target_type.kind != TypeKind::Named || !is_scalar_type_name(target_type.name)) return {};
    return std::unexpected(DataflowError("cannot initialize or assign '" + target_name + "' of type '" +
                                             describe_type_brief(target_type) +
                                             "' from 'nullptr': 'nullptr_t' converts only to a pointer type, to a "
                                             "function pointer type, or to a class type declaring a constructor that "
                                             "takes it -- never to 'bool' and never to an integer type (spec ch16 "
                                             "§16.4(5), §16.5(1)-(2))",
                                         loc));
}

// [basic.types.general], which spec §1(2) applies unchanged: an
// expression of type `void` produces no value, and may appear only where
// no value is wanted -- as a discarded expression statement, or as the
// operand of `return` in a function that itself returns `void`.
//
// scpp had this rule nowhere. Every check that judges an expression used
// as a value asks a question of the form "are these two *scalar* types
// the same?", and each one declines when it does not recognise a type:
// check_scalar_conversion returns early on a non-scalar source,
// binary_expr_has_valid_arithmetic_types returns `true` the moment
// either operand is not a scalar, and binary_expr_has_compatible_types
// asks types_equal, which cheerfully answers "yes" for `void` against
// `void`. `void` is the one type no such list contains, so every one of
// them abstained and the question went to codegen -- which caught it
// where it happened to store through a checked path (a local
// declaration, an assignment, a `return`) and did not where it did not.
// A namespace-scope initializer and a unary minus reached LLVM, where
// `void` finds an llvm_unreachable that a release build compiles to
// nothing: `int g = h();` became unbounded recursion in
// llvm::DataLayout::getAlignment, and `void v;` a jump to an address on
// the stack. Those crash sites correctly assume a first-class type; the
// fault was that nothing upstream had ever asked whether the expression
// was a value.
//
// So it is asked here, once, by the checks that already stand at each of
// the positions spec §16.3(1) enumerates -- and asked *before* they
// decide whether they recognise the type, because "there is no value
// here" is not a conversion question and has to be answered first.
//
// `void*` is untouched: that is a pointer, and a perfectly ordinary
// value.
std::expected<void, DataflowError> check_expression_yields_a_value(const Expr& expr, const Body& body,
                                                                   const Signatures& signatures, SourceLocation loc,
                                                                   const std::string& role, bool report_errors) {
    if (!report_errors) return {};
    // See Body::function_is_generic_template (mir.cppm): inside an
    // uninstantiated template a `void` answer can mean "unconstrained",
    // and this rule rejects rather than abstains.
    if (body.function_is_generic_template) return {};
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    if (!source_type.has_value() || !is_void_named_type(*source_type)) return {};
    return std::unexpected(DataflowError(
        "cannot use a 'void' value as " + role +
            ": an expression of type 'void' produces no value, so it may appear only as a discarded expression "
            "statement or as the operand of a 'return' in a function returning 'void' ([basic.types.general], "
            "applied unchanged by spec §1(2))",
        loc));
}

// spec §6: scpp has no implicit conversion between *any* two distinct
// scalar types. That rule is the language's headline guarantee, but it
// was only half enforced: comparison, overload resolution and `?:`
// compared type *names*, while an initializer, an assignment and a
// `return` were only ever checked by codegen's check_store_type /
// check_return_type, which compare the *lowered LLVM* types. Two names
// that lower alike were therefore interconvertible in silence --
// `bool` into `char`, `int8_t` into `uint8_t`, `int` into
// `unsigned int`, `size_t` into `ptrdiff_t`, `int` into `int32_t`.
// Signedness, and the distinction between a fixed-width name and its
// same-width counterpart, were simply lost.
//
// So this checks by name, at the one phase that still has the scpp
// types. It is deliberately *additive*: codegen's representation checks
// stay exactly as they are, because they are the backstop for
// everything movecheck cannot type -- infer_expr_type gives up on
// Member/Subscript chains (it has no Program-level field-type
// information), which is precisely the `return value->tag;` shape
// check_return_type was introduced for. Replacing one with the other
// would trade a false-accept for a false-reject; the two together
// cover the boundary.
//
// Only scalar-to-scalar is judged here. A non-scalar destination, an
// unknown source type, or a literal source is left to the machinery
// that already handles it -- a literal has no type of its own to
// convert *from* (`int8_t x = 1;` is a valid initialization of an
// `int8_t`, not an `int`-to-`int8_t` conversion), and that judgment
// belongs to literal_compatible_with_type.
std::expected<void, DataflowError> check_scalar_conversion(const Type& target_type, const Expr& expr, const Body& body,
                                                           const Signatures& signatures, SourceLocation loc,
                                                           const std::string& target_name, bool report_errors) {
    if (!report_errors) return {};
    const Type& target_operand = binary_operand_type(target_type);
    if (target_operand.kind != TypeKind::Named || !is_scalar_type_name(target_operand.name)) return {};
    // spec §6: an untyped integer literal adopts the type of the place
    // it initializes, but only when the value it spells is one of that
    // type's values. `int8_t x = 300;` and `unsigned int x = -5;` used
    // to be accepted for the same reason every other conversion hole on
    // this path was: the literal was matched on *shape* and never on
    // value. Reported here rather than left to fall through to the
    // conversion diagnostic below, because a literal has no source type
    // to name and "cannot convert an int to int8_t" would be a
    // misleading description of what is wrong.
    const Expr* integer_literal = &expr;
    std::int64_t literal_sign = 1;
    if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Neg && expr.lhs != nullptr) {
        integer_literal = expr.lhs.get();
        literal_sign = -1;
    }
    if (integer_literal->kind == ExprKind::IntegerLiteral && integer_literal_compatible_with_type(target_operand) &&
        !integer_literal_value_fits(literal_sign * integer_literal->int_value, target_operand.name)) {
        return std::unexpected(DataflowError(
            "integer literal " + std::to_string(literal_sign * integer_literal->int_value) +
                " is out of range for " + target_name + " of type '" + target_operand.name + "' (spec §6)",
            loc));
    }
    if (integer_literal->kind == ExprKind::IntegerLiteral && integer_literal_compatible_with_type(target_operand)) {
        return {};
    }
    if (literal_compatible_with_type(expr, target_operand)) return {};
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    if (!source_type.has_value()) return {};
    const Type& source_operand = binary_operand_type(*source_type);
    if (source_operand.kind != TypeKind::Named || !is_scalar_type_name(source_operand.name)) return {};
    if (source_operand.name == target_operand.name) return {};
    return std::unexpected(DataflowError(
        "cannot convert a '" + source_operand.name + "' value to '" + target_operand.name + "' for " + target_name +
            ": scpp has no implicit conversion between distinct scalar types, not even between two of the same width "
            "(spec §6) -- use an explicit 'static_cast<" +
            target_operand.name + ">(...)' if the conversion is intended",
        loc));
}

// Where the field a Member expression names was declared, so a
// read-only diagnostic about `s.x` can point at the `const` on `x`
// rather than only asserting that it exists.
[[nodiscard]] std::optional<SourceLocation> find_field_decl_loc(const Expr& member, const Body& body,
                                                                const Signatures& signatures) {
    if (body.program == nullptr || member.lhs == nullptr) return std::nullopt;
    std::optional<Type> base = infer_expr_type(*member.lhs, body, signatures);
    if (!base.has_value()) return std::nullopt;
    const Type* named = &*base;
    if (named->kind == TypeKind::Reference && named->pointee != nullptr) named = named->pointee.get();
    if (named->kind != TypeKind::Named) return std::nullopt;
    if (const ClassDef* def = find_class_def(*body.program, named->name)) {
        for (const ClassField& field : def->fields) {
            if (field.name == member.name) return field.loc;
        }
    }
    if (const StructDef* def = find_struct_def(*body.program, named->name)) {
        for (const StructField& field : def->fields) {
            if (field.name == member.name) return field.loc;
        }
    }
    return std::nullopt;
}

// Names the object whose constness a rejected binding would have
// dropped, and where it was made const -- because "cannot convert
// 'const int*' to 'int*'" leaves the reader looking for a `const` that
// is nowhere near the line being rejected. `&s.arr[i]` for a `const S s`
// is reported against `s`, so the walk here is to the *root* of the
// place chain: that is the declaration the user would have to change.
//
// Empty when the root isn't a declaration this can point at (a call
// result, a `const T&` parameter's referent, a string literal) -- the
// caller then prints the types alone rather than an invented location.
[[nodiscard]] std::string describe_const_source(const Expr& expr, const Body& body, const Signatures& signatures) {
    const Expr* place = &expr;
    while (place != nullptr) {
        if (place->kind == ExprKind::Unary &&
            (place->unary_op == UnaryOp::AddressOf || place->unary_op == UnaryOp::Deref) && place->lhs != nullptr) {
            place = place->lhs.get();
            continue;
        }
        if ((place->kind == ExprKind::Member || place->kind == ExprKind::Subscript) && place->lhs != nullptr) {
            // Stop at a projection that is itself the read-only step, so
            // `c.ref_field` is blamed on the field, not on `c`.
            if (place_is_read_only(*place, body, signatures) &&
                !place_is_read_only(*place->lhs, body, signatures)) {
                if (place->kind != ExprKind::Member) return "the element is read-only";
                std::string result{"the field '"};
                result += place->name;
                result += "' is read-only";
                if (std::optional<SourceLocation> field_loc = find_field_decl_loc(*place, body, signatures);
                    field_loc.has_value() && field_loc->line != 0) {
                    result += ", declared at line ";
                    result += std::to_string(static_cast<std::int64_t>(field_loc->line));
                }
                return result;
            }
            place = place->lhs.get();
            continue;
        }
        break;
    }
    if (place == nullptr || place->kind != ExprKind::Identifier) return "";
    if (std::optional<LocalId> local = body.local_of(*place); local.has_value()) {
        const LocalDecl& decl = body.decl(*local);
        const Type& type = body.type_of(*local);
        if (!decl.is_const && !type.is_const_qualified) {
            // A shared borrow is read-only without any of its *own*
            // declaration being `const`-qualified -- the qualifier is on
            // the referent, spelled `const T&`. Naming it matters most
            // for a `const T&` parameter, where the reader otherwise
            // gets no location at all.
            if (!(is_reference(type) || is_span(type)) || type.is_mutable_ref) return "";
            std::string result{"'"};
            result += decl.source_name;
            result += "' is a read-only ('const') ";
            result += is_span(type) ? "view" : "reference";
            if (decl.decl_loc.line != 0) {
                result += ", declared at line ";
                result += std::to_string(static_cast<std::int64_t>(decl.decl_loc.line));
            }
            return result;
        }
        std::string result{"'"};
        result += decl.source_name;
        result += "' is declared ";
        result += decl.is_constexpr ? "constexpr" : "const";
        if (decl.decl_loc.line != 0) {
            result += " at line ";
            result += std::to_string(static_cast<std::int64_t>(decl.decl_loc.line));
        }
        return result;
    }
    if (const GlobalVar* global = find_visible_global_for_expr(*place, body);
        global != nullptr && global->decl != nullptr) {
        if (!global->decl->is_const && !global->decl->is_constexpr && !global->decl->type.is_const_qualified) return "";
        std::string result{"the global '"};
        result += global->decl->var_name;
        result += "' is declared ";
        result += global->decl->is_constexpr ? "constexpr" : "const";
        result += " at line ";
        result += std::to_string(static_cast<std::int64_t>(global->decl->loc.line));
        return result;
    }
    return "";
}

// Renders a place expression back into something close to its source
// spelling, so a diagnostic about `s.arr[i] = 1` can say which place it
// means instead of "this place". Falls back to an empty string for
// anything that is not a plain place chain; the caller then says "this
// place".
[[nodiscard]] std::string describe_place_expr(const Expr& expr) {
    if (expr.kind == ExprKind::Identifier) return expr.name;
    if (expr.kind == ExprKind::Member && expr.lhs != nullptr) {
        std::string base = describe_place_expr(*expr.lhs);
        if (base.empty()) return "";
        base += expr.implicit_arrow_chain_safe ? "->" : ".";
        base += expr.name;
        return base;
    }
    if (expr.kind == ExprKind::Subscript && expr.lhs != nullptr) {
        std::string base = describe_place_expr(*expr.lhs);
        if (base.empty()) return "";
        base += "[...]";
        return base;
    }
    if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr) {
        std::string base = describe_place_expr(*expr.lhs);
        if (base.empty()) return "";
        return "*" + base;
    }
    return "";
}

// The single wording of "you may not write here, because the place is
// read-only". Every modifying operation asks the same question -- plain
// `=`, a compound `+=`, `++`/`--` -- and before this they answered it
// with three different sentences ("cannot reassign 'c' after
// initialization", "cannot assign to this place: it is reached through a
// read-only (const) reference", "cannot apply '++' to this place: ...").
// The last two also asserted a *reference* that a `const` local, global
// or by-value parameter does not have.
//
// `operator_spelling` is the operator the user actually wrote, so the
// message stays specific without becoming a separate sentence per
// operator.
[[nodiscard]] DataflowError read_only_write_error(const Expr& place, const Body& body, const Signatures& signatures,
                                                  const std::string& operator_spelling, SourceLocation loc) {
    std::string message{"cannot modify "};
    std::string spelling = describe_place_expr(place);
    if (spelling.empty()) {
        message += "this place";
    } else {
        message += "'";
        message += spelling;
        message += "'";
    }
    message += " through '";
    message += operator_spelling;
    message += "': it is read-only";
    std::string const_source = describe_const_source(place, body, signatures);
    if (!const_source.empty()) {
        message += " (";
        message += const_source;
        message += ")";
    }
    return DataflowError(message, loc);
}

std::expected<void, DataflowError> check_raw_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                   const Signatures& signatures, SourceLocation loc, const std::string& target_name,
                                   bool report_errors) {
    if (!report_errors || target_type.kind != TypeKind::Pointer) return {};
    // A mismatch only means something once both pointees are real types.
    // Inside a still-uninstantiated generic they are not: the class's own
    // type parameters are placeholders, and the bare-witness pass
    // substitutes them into declared member types while leaving the types
    // inferred for expressions spelled as the parameter itself -- so
    // `shared_ptr<T>`'s `this->ptr_` reads as `__generic_bare_witness*`
    // against a `T*` source that means the very same type. Per Body's
    // function_is_generic_template contract a rule that *rejects* must not
    // be founded on those stand-in types, and nothing is lost by declining:
    // every generic body is checked again at each instantiation, where the
    // pointees are real.
    //
    // This previously asked instead whether each pointee named a type the
    // program defines, which approximated "am I looking at a placeholder?"
    // by a flat, unscoped name lookup -- so any program that declared its
    // own global `T` (or `class T`, or `enum class T`) made the library's
    // unrelated parameter `T` look resolved and unmasked the mismatch,
    // failing to compile inside std_memory.scpp.
    if (body.function_is_generic_template) return {};
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    // An array source decays to a pointer to its first element
    // ([conv.array]), so the same compatibility question applies to the
    // decayed type. Without this the early return below would skip every
    // array source, and since a string literal's type is now an array of
    // `const char` ([lex.string]/1) rather than a `const char*`,
    // `char* p = "abcd";` would have silently started compiling.
    //
    if (source_type && source_type->kind == TypeKind::Array) source_type = decay_array_to_pointer(*source_type);
    if (!source_type || source_type->kind != TypeKind::Pointer) return {};
    if (raw_pointer_implicitly_convertible(*source_type, target_type)) return {};
    if (body.program != nullptr &&
        types_compatible_with_base_conversion(*source_type, target_type, *body.program, enclosing_class_name(body))) {
        return {};
    }
    // [conv.qual]/3 permits a qualification conversion only in the
    // direction that *adds* const, so dropping it is its own failure and
    // gets its own message. Told apart from an unrelated-pointee mismatch
    // because the two have completely different fixes: this one is
    // always spelled by qualifying the destination, and saying "without
    // an explicit cast" here would be naming a fix the next diagnostic
    // forbids -- ch05 §5.1(5.2) rejects a raw-pointer cast outside
    // `[[scpp::unsafe]] { }`, and reaching for `unsafe` to defeat a
    // const check is not a fix at all.
    std::string source_brief = describe_type_brief(*source_type);
    std::string target_brief = describe_type_brief(target_type);
    if (!source_type->is_mutable_pointee && target_type.is_mutable_pointee && source_type->pointee != nullptr &&
        target_type.pointee != nullptr && types_equal(*source_type->pointee, *target_type.pointee)) {
        std::string const_source = describe_const_source(expr, body, signatures);
        std::string message{"cannot initialize or assign raw pointer '"};
        message += target_name;
        message += "' (of type '";
        message += target_brief;
        message += "') from '";
        message += source_brief;
        message += "': that would drop 'const' and hand out a mutable handle to a read-only object";
        if (!const_source.empty()) {
            message += " (";
            message += const_source;
            message += ")";
        }
        message += " -- to read through it, declare it '";
        message += source_brief;
        message += "'";
        return std::unexpected(DataflowError(message, loc));
    }
    std::string message{"cannot initialize or assign raw pointer '"};
    message += target_name;
    message += "' (of type '";
    message += target_brief;
    message += "') from an incompatible pointer type '";
    message += source_brief;
    message += "' without an explicit cast";
    return std::unexpected(DataflowError(message, loc));
}

// spec §6.2(10): "A shared reborrow does not make the program more
// permissive than the binding from which it is formed: it may not be
// used to mutate an object or range that is reachable only through a
// shared or `const` binding." This is that predicate -- "is the place
// this expression denotes reachable only read-only?" -- and there is
// exactly one of it.
//
// There were two, and they disagreed in five places. `assignment_target_
// is_read_only` answered it for "may I write here?" and `is_read_only_
// reachable` (borrows.cppm) for "may I derive a mutable handle here?",
// which are the same question about the same place. Only the first knew
// about globals, so `const int g = 5;` rejected `g = 9;` and accepted
// `int* p = &g;`, `int& r = g;`, `auto& r = g;`, `take_ref(g)`,
// `take_ptr(&g)` and `std::span<int> s = ga;` -- every one of which then
// wrote through the const object at runtime. Only the first knew that a
// lambda's captured `const T&` field and a `std::span<const T>` base
// stay read-only through a projection; only the first treated `*r` for a
// `const T&` r as read-only. Only the second gave a span return type its
// read-only answer.
//
// So this is the union of the two, and both names now denote it.
//
// The rule itself now lives in scpp.ast (`place_is_read_only(const
// Expr&, const ReadOnlyPlaceQuery&)`), because codegen was asking the
// very same question with its own third copy; this function is just
// movecheck's name resolution for it. See that function for the rule and
// for what the two copies disagreed about.
[[nodiscard]] bool place_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures) {
    ReadOnlyPlaceQuery query;
    query.declared_variable =
        [&](const Expr& name_expr) -> std::optional<std::pair<bool, Type>> {
        if (std::optional<LocalId> local = body.local_of(name_expr); local.has_value()) {
            return std::pair<bool, Type>(body.decl(*local).is_const, body.type_of(*local));
        }
        if (const GlobalVar* global = find_visible_global_for_expr(name_expr, body);
            global != nullptr && global->decl != nullptr) {
            return std::pair<bool, Type>(global->decl->is_const || global->decl->is_constexpr, global->decl->type);
        }
        return std::nullopt;
    };
    query.inferred_type = [&](const Expr& sub) { return infer_expr_type(sub, body, signatures); };
    query.call_return_type = [&](const Expr& call) -> std::optional<Type> {
        CalleeSignature callee = resolve_callee_signature(call, body, signatures);
        const FunctionSignature* sig = resolve_overload(call, callee, body, signatures);
        if (sig == nullptr) return std::nullopt;
        return sig->return_type;
    };
    query.class_def = [&](const std::string& name) -> const ClassDef* {
        return body.program != nullptr ? find_class_def(*body.program, name) : nullptr;
    };
    query.struct_def = [&](const std::string& name) -> const StructDef* {
        return body.program != nullptr ? find_struct_def(*body.program, name) : nullptr;
    };
    return place_is_read_only(expr, query);
}

// The same question under the name the assignment path asks it by.
[[nodiscard]] bool assignment_target_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures) {
    return place_is_read_only(expr, body, signatures);
}

// [basic.lval]/1 + [expr.ass]/1: does this expression designate an
// object at all, i.e. is it a candidate for being written to? This is a
// separate question from `place_is_read_only`, which asks whether the
// object it designates may be *modified*.
//
// `++`/`--`/`+=` used to answer it with `resolve_borrow_source_root(...)
// .empty()`, which actually means "no *local* owns this place" -- true
// of every namespace-scope variable, so `int g = 5; g++;` was rejected
// as "operand of '++' must be an assignable place" while the plain
// `g = 7;` beside it compiled. Two code paths, one question, and the
// borrow-tracking answer standing in for the language one; that also put
// the wrong message on `const int g = 5; ++g;`, which is a const error,
// not a not-a-place error.
[[nodiscard]] bool expr_is_assignable_place(const Expr& expr, const Body& body) {
    switch (expr.kind) {
        case ExprKind::Identifier:
            if (body.local_of(expr).has_value()) return true;
            return find_visible_global_for_expr(expr, body) != nullptr;
        case ExprKind::Member:
        case ExprKind::Subscript:
            return expr.lhs != nullptr && expr_is_assignable_place(*expr.lhs, body);
        case ExprKind::Unary:
            if (is_explicit_star_this(expr)) return true;
            return expr.unary_op == UnaryOp::Deref;
        case ExprKind::Call:
            // `operator_deref` is how an overloaded `*p` arrives here;
            // any other call yields a prvalue, which is not a place.
            return expr.name == "operator_deref";
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
        case ExprKind::BracedInitList:
            // A brace-enclosed initializer list is the same situation as
            // the ExprKind::ValueInit case just below -- a brand new,
            // alias-free temporary carrying no type of its own, adapting
            // to whatever `expected_type` the context supplies (which is
            // why infer_expr_type deliberately returns nullopt for it) --
            // but unlike `{}` it has elements, so the question "can this
            // list initialize that type?" has a real answer and is worth
            // asking rather than assuming. Answering it here with the
            // same walk codegen uses keeps movecheck's independent
            // overload resolution in step with codegen's: a more
            // permissive answer here would move-check a call against a
            // signature codegen does not select.
            return braced_init_list_can_initialize(expected_type, expr.args, body, signatures);
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
        case ExprKind::Unary: {
            // `&x` (address-of) always yields a brand new pointer prvalue,
            // independent of whatever move/borrow state `x` itself has --
            // exactly as fresh as a literal or std::make_unique<T>(...),
            // regardless of whether `x` is a plain local, a field access
            // (e.g. `&type.lifetime`), or any other place expression.
            if (expr.unary_op == UnaryOp::AddressOf) break;
            // A unary operator function's result is a prvalue of its
            // declared return type, like any other call -- `V b = -a;`
            // otherwise looked for a converting constructor `V(V)`.
            std::optional<Type> operand = infer_expr_type(*expr.lhs, body, signatures);
            SelectedOperator selected = resolve_unary_operator_call(expr, operand, body, signatures);
            if (selected.signature == nullptr || is_reference(selected.signature->return_type)) return false;
            if (!types_equal(selected.signature->return_type, expected_type)) return false;
            break;
        }
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
        case ExprKind::Binary: {
            // [over.match.oper]/2: an operator expression with a class
            // operand *is* a call, so it is exactly as fresh as the
            // operator function's return -- `a + b` returning `V` by
            // value initializes a `V` for the same reason
            // `a.plus(b)` does. Without this arm, an operator function's
            // result was not a usable initializer at all: the very first
            // thing `V c = a + b;` reported.
            if (expr.lhs == nullptr || expr.rhs == nullptr) return false;
            std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
            std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
            SelectedOperator selected = resolve_binary_operator_call(expr, lhs_type, rhs_type, body, signatures);
            if (selected.signature == nullptr) return false;
            if (is_reference(selected.signature->return_type)) return false;
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
//
// [conv.lval]/1: reading an lvalue yields a prvalue whose type is the
// *cv-unqualified* version of the object's type -- which is why
// `const int c = 5; int v = c;`, `f(c)` for `f(int)`, and `auto v = c;`
// all work in C++ and why the qualifier only survives where no such
// conversion happens: `&c` ([expr.unary.op]/3 keeps it), array-to-pointer
// decay ([conv.array] -- `const char w[6]` yields `const char*`), and
// reference binding (which never reads the object at all).
//
// So this wrapper is the lvalue-to-rvalue conversion, and the split it
// draws is the whole reason the const representation could move onto the
// type at all: infer_expr_type answers "what is this expression's
// *value* type", place_is_read_only answers "is the object this
// expression names writable". Arrays are exempt because they have no
// lvalue-to-rvalue conversion -- they decay instead, and decay carries
// the qualifier into the pointee.
[[nodiscard]] std::optional<Type> infer_expr_lvalue_type(const Expr& expr, const Body& body, const Signatures& signatures);

[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures) {
    std::optional<Type> type = infer_expr_lvalue_type(expr, body, signatures);
    if (type.has_value() && type->kind != TypeKind::Array) type->is_const_qualified = false;
    return type;
}

[[nodiscard]] std::optional<Type> infer_expr_lvalue_type(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        // A brace-enclosed initializer list has no type of its own; only
        // the initialization boundary that consumes it knows what it
        // means, so it is exactly the nullopt case described above.
        case ExprKind::BracedInitList: return std::nullopt;
        case ExprKind::IntegerLiteral: return named_type("int");
        case ExprKind::FloatLiteral: return named_type("double");
        case ExprKind::BoolLiteral: return named_type("bool");
        // ch06 §6: `nullptr` finally has a type of its own. Before
        // `nullptr_t` existed this fell through to the Identifier case
        // and produced no type at all, which is why every consumer
        // needing one either special-cased the spelling or reported the
        // literal as an undeclared variable.
        case ExprKind::NullptrLiteral: return nullptr_named_type();
        case ExprKind::CharLiteral: return named_type("char");
        case ExprKind::Sizeof:
        case ExprKind::Alignof:
            return named_type("size_t");
        case ExprKind::ValueInit:
            return expr.type;
        case ExprKind::StringLiteral: return string_literal_type(expr.name.size());

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

        case ExprKind::Move:
            // std::move doesn't change the static type: `std::move(E)`
            // designates the very object `E` does, so its type is `E`'s
            // own -- for any place expression `E`, not a list of the
            // shapes that happened to be needed.
            //
            // It was that list: a bare identifier, then `.back()`/
            // `.front()`, then a subscript, each added when a caller hit
            // the nullopt. `E.field` was never on it, so
            // `std::move(s.ps)` had no type, produces_rvalue_of_type
            // answered "not a fresh value", and all four class-value
            // boundaries rejected it -- behind a bespoke message
            // ("std::move currently only supports a plain local
            // variable") that reported the missing shape list as if it
            // were the language rule. spec §6.2(3) names no such
            // restriction; see apply_expr's ExprKind::Move case.
            return infer_expr_lvalue_type(*expr.lhs, body, signatures);

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
                case UnaryOp::Not:
                case UnaryOp::Neg:
                case UnaryOp::PreInc:
                case UnaryOp::PreDec:
                case UnaryOp::PostInc:
                case UnaryOp::PostDec: {
                    // [over.match.oper]/2: a unary operator with a class
                    // operand names an operator function, exactly as a
                    // binary one does. Only `operator*` and `operator->`
                    // were ever looked up, so `-v` reported `V` (the
                    // operand's own type) and `!v` reported `bool`, both
                    // without asking whether the class had the operator.
                    std::optional<Type> operand = infer_expr_type(*expr.lhs, body, signatures);
                    SelectedOperator selected = resolve_unary_operator_call(expr, operand, body, signatures);
                    if (selected.signature != nullptr) return selected.signature->return_type;
                    // No operator function and a class operand: [over.built]
                    // offers no candidate either, so the expression has no
                    // type at all. Reporting the operand's own type here is
                    // what made `V b = -a;` say "no constructor of 'V'
                    // matches this call ... argument type is 'V'" -- a
                    // consequence of the lie, not the defect.
                    if (operand.has_value() && operand_type_needs_an_operator_function(*operand, body)) {
                        // [expr.unary.op]/9: `!`'s operand is
                        // contextually converted to `bool` first, so a
                        // class operand that declares `operator bool`
                        // has a built-in candidate after all, and the
                        // expression's type is `bool` -- reporting "no
                        // type" for it made `!h ? a : b` say the
                        // conditional needed a `bool` condition when the
                        // `!` already produced one.
                        if (expr.unary_op == UnaryOp::Not) {
                            std::string key;
                            if (find_conversion_function_signature(operand, named_type("bool"), body, signatures,
                                                                   key) != nullptr) {
                                return named_type("bool");
                            }
                        }
                        return std::nullopt;
                    }
                    if (expr.unary_op == UnaryOp::Not) return named_type("bool");
                    return operand;
                }
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
                    if (operand->kind == TypeKind::Reference && operand->pointee != nullptr) {
                        Type referent = *operand->pointee;
                        if (!operand->is_mutable_ref) referent.is_const_qualified = true;
                        return pointer_to(std::move(referent));
                    }
                    // [expr.unary.op]/3: `&E` has type "pointer to T"
                    // where T is E's type *including its cv-qualifiers*.
                    // This used to hand back a mutable `T*` for every
                    // non-reference operand and leave the constness
                    // question to a separate guard scoped to the
                    // syntactic `&expr` initializer/argument shapes --
                    // so `H h{&c};`, `int* p = cond ? &c : &v;` and
                    // `return &c;` were never asked at all. A place
                    // reached read-only through a projection
                    // (`&s.field` of a const `s`) has no const on its
                    // own inferred type -- movecheck's Member/Subscript
                    // inference yields no type for those at all -- so
                    // the place predicate answers for those, in the same
                    // expression, rather than in a second guard
                    // somewhere else.
                    Type referent = std::move(*operand);
                    if (place_is_read_only(*expr.lhs, body, signatures)) referent.is_const_qualified = true;
                    return pointer_to(std::move(referent));
                }
                case UnaryOp::Deref: {
                    std::optional<Type> operand = infer_expr_type(*expr.lhs, body, signatures);
                    if (!operand) return std::nullopt;
                    // [expr.unary.op]/1 requires a pointer operand, which an
                    // array reaches through [conv.array]'s array-to-pointer
                    // conversion -- `*"abcd"` has type `const char`.
                    if (operand->kind == TypeKind::Array) operand = decay_array_to_pointer(*operand);
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

        case ExprKind::Binary: {
            // [over.match.oper]/2: when an operand has class type the
            // expression *is* a call, so its type is the selected
            // operator function's return type -- not the left operand's,
            // and not `bool` for a relational one. Asked before the
            // built-in arms below, because [over.built] provides no
            // built-in candidate for a class operand at all.
            //
            // The two operand types are inferred exactly once here and
            // handed to everything that needs them, for the 2^n reason
            // spelled out in the arithmetic block below.
            std::optional<Type> binary_lhs_type = infer_expr_type(*expr.lhs, body, signatures);
            std::optional<Type> binary_rhs_type = infer_expr_type(*expr.rhs, body, signatures);
            SelectedOperator operator_call =
                resolve_binary_operator_call(expr, binary_lhs_type, binary_rhs_type, body, signatures);
            if (operator_call.signature != nullptr) return operator_call.signature->return_type;
            switch (expr.binary_op) {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::AddAssign:
                case BinaryOp::SubAssign:
                case BinaryOp::MulAssign:
                case BinaryOp::DivAssign:
                case BinaryOp::Assign: {
                    // Each operand is inferred at most once here, and the
                    // answer reused, which is the whole reason this arm is
                    // written as one block rather than as a fallthrough
                    // chain. `a + b + c + ...` is left-leaning, so asking
                    // for the left operand's type a second time re-walks
                    // the entire prefix: inferring lhs and rhs for the
                    // pointer-arithmetic test below and then inferring lhs
                    // *again* to produce the result cost 2^n for an n-term
                    // chain -- measured at exactly that, the worst node
                    // visited 892 times for 8 terms and 229,372 times for
                    // 16, with 22 terms taking half a minute to compile.
                    //
                    // Note what is deliberately not done: the result is not
                    // cached on the Expr. This function's answer depends on
                    // the Body and Signatures it is asked about, and
                    // monomorphize asks about the same expression in more
                    // than one instantiation, so a per-node cache would
                    // hold one instantiation's answer and hand it to
                    // another. Not recomputing is what is needed, not
                    // remembering.
                    const bool additive = expr.binary_op == BinaryOp::Add || expr.binary_op == BinaryOp::Sub;
                    const bool multiplicative = expr.binary_op == BinaryOp::Mul || expr.binary_op == BinaryOp::Div;
                    // spec §6: in `2 * len`, the `2` has no type of its
                    // own -- it adopts `len`'s, so the product is a
                    // `size_t`, not an `int`. Taking the lhs type
                    // unconditionally reported `int` for exactly that
                    // shape, which was invisible while nothing compared
                    // the result against its destination and became a
                    // spurious "cannot convert 'int' to 'size_t'" the
                    // moment something did. The compound-assignment and
                    // plain-assignment forms are excluded deliberately:
                    // their type is the type of the place being written,
                    // whatever the right-hand side spells.
                    const bool adopts_rhs_type = (additive || multiplicative) &&
                                                 is_untyped_numeric_literal(*expr.lhs) &&
                                                 !is_untyped_numeric_literal(*expr.rhs);
                    if (additive) {
                        if (binary_lhs_type.has_value() && binary_rhs_type.has_value()) {
                            if (std::optional<Type> result = pointer_arithmetic_result_type(
                                    expr.binary_op, *binary_lhs_type, *binary_rhs_type)) {
                                return result;
                            }
                        }
                        return adopts_rhs_type ? binary_rhs_type : binary_lhs_type;
                    }
                    if (adopts_rhs_type) return binary_rhs_type;
                    return binary_lhs_type;
                }
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
        }

        case ExprKind::Conditional: {
            std::optional<Type> then_type = infer_expr_type(*expr.rhs, body, signatures);
            std::optional<Type> else_type = infer_expr_type(*expr.third, body, signatures);
            if (!then_type.has_value() || !else_type.has_value()) return std::nullopt;
            // [expr.cond]/4 applies the array-to-pointer conversion to both
            // operands before the composite type is determined.
            then_type = decay_array_to_pointer(*then_type);
            else_type = decay_array_to_pointer(*else_type);
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
            // [expr.ref]/4: the member access's type is the field's type
            // "combined with the cv-qualification of the object
            // expression" -- so a field of a `const S` is itself const,
            // which is what made `const S s{"hello"}; char* p = s.a;`
            // hand out a mutable pointer into a const object. The base is
            // asked for its *lvalue* type here, not its prvalue type: the
            // qualifier this rule propagates is precisely the one
            // [conv.lval] would have stripped.
            std::optional<Type> base = infer_expr_lvalue_type(*expr.lhs, body, signatures);
            if (!base) return std::nullopt;
            const bool base_is_const = base->kind == TypeKind::Reference ? !base->is_mutable_ref : base->is_const_qualified;
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
                    if (field.name == expr.name) return member_access_type(field.type, base_is_const);
                }
                return {};
            }
            if (const StructDef* def = find_struct_def(*body.program, base_named.name)) {
                for (const StructField& field : def->fields) {
                    if (field.name == expr.name) return member_access_type(field.type, base_is_const);
                }
                return {};
            }
            return std::nullopt;
        }

        case ExprKind::Subscript: {
            // [expr.sub]/1 defines `E1[E2]` as `*(E1 + E2)`, so the
            // element of a `const T[N]` is a `const T` -- the same
            // qualification-propagation rule as [expr.ref]/4 above, and
            // the reason the base is asked for its lvalue type.
            std::optional<Type> base = infer_expr_lvalue_type(*expr.lhs, body, signatures);
            if (!base) return std::nullopt;
            const bool base_is_const = base->kind == TypeKind::Reference ? !base->is_mutable_ref : base->is_const_qualified;
            const Type& effective = base->kind == TypeKind::Reference && base->pointee ? *base->pointee : *base;
            if (effective.kind == TypeKind::Array) return member_access_type(*effective.element, base_is_const || effective.is_const_qualified);
            if (effective.kind == TypeKind::Span) return member_access_type(*effective.pointee, !effective.is_mutable_ref);
            if (effective.kind == TypeKind::Pointer) return member_access_type(*effective.pointee, !effective.is_mutable_pointee);
            if (std::optional<Type> element = infer_vector_element_type(effective, body); element.has_value()) {
                return *element;
            }
            SelectedOperator selected = resolve_subscript_operator_call(expr, base, body, signatures);
            if (selected.signature != nullptr) {
                return is_reference(selected.signature->return_type) && selected.signature->return_type.pointee
                           ? member_access_type(*selected.signature->return_type.pointee,
                                                !selected.signature->return_type.is_mutable_ref)
                           : selected.signature->return_type;
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
