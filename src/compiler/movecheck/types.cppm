module;

module scpp.compiler.movecheck:types;

import std;
import scpp.ast;
import scpp.mir;
import :state;

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

[[nodiscard]] bool is_reference(const Type& type) { return type.kind == TypeKind::Reference; }
[[nodiscard]] bool is_span(const Type& type) { return type.kind == TypeKind::Span; }
[[nodiscard]] bool is_pointer(const Type& type) { return type.kind == TypeKind::Pointer; }
[[nodiscard]] bool is_lifetime_eligible_type(const Type& type) {
    return is_reference(type) || is_pointer(type) || is_span(type);
}
[[nodiscard]] bool is_function_pointer(const Type& type) { return type.kind == TypeKind::FunctionPointer; }
[[nodiscard]] bool is_for_range_size_builtin(const Expr& expr) {
    return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.name == "$for_range_size" && expr.args.size() == 1;
}
[[nodiscard]] bool is_synthesized_for_range_storage(std::string_view name) { return name.rfind("$for_range_", 0) == 0; }
[[nodiscard]] bool is_reborrowable_local_type(const Type& type) { return is_reference(type) || is_span(type); }
[[nodiscard]] bool local_is_suspended_for_reborrow(std::string_view name, const DataflowState& state) {
    auto it = state.suspended_reborrows.find(std::string(name));
    return it != state.suspended_reborrows.end() && it->second > 0;
}
[[nodiscard]] bool is_explicit_star_this(const Expr& expr) {
    return expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr &&
           expr.lhs->kind == ExprKind::Identifier && expr.lhs->name == "this";
}

// ch06 §6: the complete scalar/numeric family a `static_cast<T>(expr)`/
// `(T)expr` may legally convert between (ExprKind::Cast's own apply_expr
// case) -- `TypeKind::Named` alone isn't enough to tell a scalar apart
// from a struct/class/witness name (all three share that TypeKind), so
// this checks against the exact, closed set ch06 documents rather than
// the type's own `kind`.
[[nodiscard]] bool is_scalar_type_name(const std::string& name) {
    static const std::unordered_set<std::string> scalar_names = {
        "bool", "char", "int", "long", "unsigned int", "unsigned long", "int8_t", "int16_t", "int32_t",
        "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "float", "double", "float32_t", "float64_t",
        "size_t", "ptrdiff_t"};
    return scalar_names.contains(name);
}

[[nodiscard]] bool is_integral_scalar_type_name(const std::string& name) {
    static const std::unordered_set<std::string> integral_scalar_names = {
        "char",      "int",          "long",         "unsigned int", "unsigned long", "int8_t",  "int16_t",
        "int32_t",   "int64_t",      "uint8_t",      "uint16_t",     "uint32_t",      "uint64_t", "size_t",
        "ptrdiff_t",
    };
    return integral_scalar_names.contains(name);
}

[[nodiscard]] const EnumDef* find_enum_def(const Program* program, const std::string& name) {
    if (program == nullptr) return nullptr;
    for (const EnumDef& def : program->enums) {
        if (def.name == name) return &def;
    }
    return nullptr;
}

[[nodiscard]] const EnumVariant* find_enum_variant(const Program* program, const std::string& name,
                                                   const EnumDef** owning_enum) {
    if (program == nullptr) return nullptr;
    for (const EnumDef& def : program->enums) {
        for (const EnumVariant& variant : def.variants) {
            if (variant.name == name) {
                if (owning_enum != nullptr) *owning_enum = &def;
                return &variant;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] bool is_enum_type(const Type& type, const Program* program) {
    return type.kind == TypeKind::Named && find_enum_def(program, type.name) != nullptr;
}

[[nodiscard]] const Type* enum_underlying_type(const Type& type, const Program* program) {
    const EnumDef* def = find_enum_def(program, type.name);
    return def == nullptr ? nullptr : &def->underlying_type;
}

[[nodiscard]] const ClassDef* find_class_def(const Program& program, const std::string& class_name) {
    const ClassDef* forward_decl = nullptr;
    for (const ClassDef& def : program.classes) {
        if (def.name != class_name) continue;
        if (!def.is_forward_declaration) return &def;
        if (forward_decl == nullptr) forward_decl = &def;
    }
    return forward_decl;
}

[[nodiscard]] bool type_contains_lifetime_carrying_state(const Type& type, const Program& program,
                                                         std::unordered_set<std::string> visiting) {
    if (is_lifetime_eligible_type(type)) return true;
    if (type.kind == TypeKind::Array && type.element != nullptr) {
        return type_contains_lifetime_carrying_state(*type.element, program, std::move(visiting));
    }
    if (type.kind != TypeKind::Named) return false;
    if (!visiting.insert(type.name).second) return false;
    if (const ClassDef* def = find_class_def(program, type.name)) {
        for (const ClassField& field : def->fields) {
            if (type_contains_lifetime_carrying_state(field.type, program, visiting)) return true;
        }
    }
    return false;
}
[[nodiscard]] std::string named_type_name(const Type& type) {
    if (type.kind == TypeKind::Named) return type.name;
    if (type.kind == TypeKind::Reference && type.pointee->kind == TypeKind::Named) return type.pointee->name;
    return "";
}

// Structural (deep) equality between two Types -- needed since Type's
// pointee/element are shared_ptr (Type's own comment: "so Type stays
// copyable"), so two independently-parsed-but-conceptually-identical
// types (e.g. two separate `int*` parameter declarations) are different
// shared_ptr instances and would compare unequal under a naively
// `=default`-ed operator==. Used only for function-overload resolution
// (ch05 §5.10): since ch06 established no scpp scalar type implicitly
// converts to any other, overload resolution is exact type match only --
// this is that "exact match" test. Deliberately requires is_mutable_ref/
// is_mutable_pointee to also match: `T&` and `const T&` (or `T*`/
// `const T*`) are distinct parameter types for overloading purposes, not
// interchangeable. Reference additionally requires is_rvalue_ref to
// match: `T&`/`const T&` (a borrow) and `T&&` (ch03's move-parameter
// form) are likewise distinct parameter types, never interchangeable --
// meaningless for Span (which has no rvalue-reference concept at all).
[[nodiscard]] bool types_equal(const Type& a, const Type& b) {
    if (a.kind != b.kind) return false;
    if (a.is_const_qualified != b.is_const_qualified) return false;
    switch (a.kind) {
        case TypeKind::Named:
            if (a.name != b.name || a.template_args.size() != b.template_args.size()) return false;
            for (std::size_t i = 0; i < a.template_args.size(); i++) {
                if (!types_equal(a.template_args[i], b.template_args[i])) return false;
            }
            return true;
        case TypeKind::Pointer:
            return a.is_mutable_pointee == b.is_mutable_pointee && types_equal(*a.pointee, *b.pointee);
        case TypeKind::Function:
        case TypeKind::FunctionPointer:
            if ((a.kind == TypeKind::FunctionPointer && a.is_unsafe_function_pointer != b.is_unsafe_function_pointer) ||
                (a.kind == TypeKind::Function &&
                 (a.is_const_function != b.is_const_function ||
                  a.function_ref_qualifier != b.function_ref_qualifier)) ||
                !types_equal(*a.function_return, *b.function_return) ||
                a.function_params.size() != b.function_params.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.function_params.size(); i++) {
                if (!types_equal(a.function_params[i], b.function_params[i])) return false;
            }
            return true;
        case TypeKind::Reference:
            return a.is_mutable_ref == b.is_mutable_ref && a.is_rvalue_ref == b.is_rvalue_ref &&
                   types_equal(*a.pointee, *b.pointee);
        case TypeKind::Span:
            return a.is_mutable_ref == b.is_mutable_ref && types_equal(*a.pointee, *b.pointee);
        case TypeKind::Array:
            return a.array_size == b.array_size && types_equal(*a.element, *b.element);
    }
    return false;
}

[[nodiscard]] bool raw_pointer_implicitly_convertible(const Type& source, const Type& target) {
    if (source.kind != TypeKind::Pointer || target.kind != TypeKind::Pointer) return false;
    if (!source.is_mutable_pointee && target.is_mutable_pointee) return false;
    const Type& source_pointee =
        source.pointee->kind == TypeKind::Reference && source.pointee->pointee ? *source.pointee->pointee : *source.pointee;
    const Type& target_pointee =
        target.pointee->kind == TypeKind::Reference && target.pointee->pointee ? *target.pointee->pointee : *target.pointee;
    if (types_equal(source_pointee, target_pointee)) return true;
    bool source_is_void = source_pointee.kind == TypeKind::Named && source_pointee.name == "void";
    bool target_is_void = target_pointee.kind == TypeKind::Named && target_pointee.name == "void";
    return source_is_void || target_is_void;
}

[[nodiscard]] bool is_scalar_named_type(const Type& type) {
    return type.kind == TypeKind::Named &&
           (type.name == "int" || type.name == "bool" || type.name == "char" || type.name == "long" ||
            type.name == "float" || type.name == "double" || type.name == "unsigned int" ||
            type.name == "unsigned long" || type.name == "size_t" || type.name == "ptrdiff_t" ||
            type.name == "int8_t" || type.name == "int16_t" || type.name == "int32_t" || type.name == "int64_t" ||
            type.name == "uint8_t" || type.name == "uint16_t" || type.name == "uint32_t" || type.name == "uint64_t" ||
            type.name == "float32_t" || type.name == "float64_t");
}

[[nodiscard]] bool is_float_named_type(const Type& type) {
    return type.kind == TypeKind::Named &&
           (type.name == "float" || type.name == "double" || type.name == "float32_t" || type.name == "float64_t");
}

[[nodiscard]] bool integer_literal_compatible_with_type(const Type& type) {
    return type.kind == TypeKind::Named && type.name != "bool" && type.name != "char" && is_scalar_named_type(type);
}

[[nodiscard]] const Type& binary_operand_type(const Type& type) {
    return type.kind == TypeKind::Reference ? *type.pointee : type;
}

[[nodiscard]] bool is_pointer_arithmetic_offset_type(const Type& type) {
    return type.kind == TypeKind::Named && type.name != "bool" && is_integral_scalar_type_name(type.name);
}

[[nodiscard]] bool pointer_supports_arithmetic(const Type& type) {
    return type.kind == TypeKind::Pointer && type.pointee != nullptr &&
           !(type.pointee->kind == TypeKind::Named && type.pointee->name == "void");
}

[[nodiscard]] std::optional<Type> pointer_arithmetic_result_type(BinaryOp op, const Type& lhs, const Type& rhs) {
    const Type& lhs_operand = binary_operand_type(lhs);
    const Type& rhs_operand = binary_operand_type(rhs);
    if (op == BinaryOp::Add) {
        if (pointer_supports_arithmetic(lhs_operand) && is_pointer_arithmetic_offset_type(rhs_operand)) {
            return lhs_operand;
        }
        if (is_pointer_arithmetic_offset_type(lhs_operand) && pointer_supports_arithmetic(rhs_operand)) {
            return rhs_operand;
        }
        return std::nullopt;
    }
    if (op == BinaryOp::Sub) {
        if (pointer_supports_arithmetic(lhs_operand) && is_pointer_arithmetic_offset_type(rhs_operand)) {
            return lhs_operand;
        }
        if (pointer_supports_arithmetic(lhs_operand) && pointer_supports_arithmetic(rhs_operand) &&
            types_equal(lhs_operand, rhs_operand)) {
            return named_type("ptrdiff_t");
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool literal_compatible_with_type(const Expr& literal, const Type& type) {
    const Type& operand_type = binary_operand_type(type);
    switch (literal.kind) {
        case ExprKind::IntegerLiteral: return integer_literal_compatible_with_type(operand_type);
        case ExprKind::FloatLiteral: return is_float_named_type(operand_type);
        case ExprKind::BoolLiteral: return operand_type.kind == TypeKind::Named && operand_type.name == "bool";
        case ExprKind::CharLiteral: return operand_type.kind == TypeKind::Named && operand_type.name == "char";
        default: return false;
    }
}
[[nodiscard]] std::string enclosing_class_name(const Body& body) {
    auto it = body.local_types.find("this");
    if (it == body.local_types.end()) return "";
    return named_type_name(it->second);
}

[[nodiscard]] bool type_names_interface(const Program& program, const std::string& name) {
    const ClassDef* def = find_class_def(program, name);
    return def != nullptr && def->is_interface;
}

[[nodiscard]] bool is_interface_representation_type(const Type& type, const Program& program) {
    if ((type.kind == TypeKind::Pointer || type.kind == TypeKind::Reference) && type.pointee &&
        type.pointee->kind == TypeKind::Named) {
        return type_names_interface(program, type.pointee->name);
    }
    return false;
}

[[nodiscard]] bool has_accessible_base_conversion(const Program& program, const std::string& source_name,
                                                  const std::string& target_name,
                                                  std::string_view current_class) {
    if (source_name == target_name) return true;
    const ClassDef* def = find_class_def(program, source_name);
    if (def == nullptr) return false;
    for (const BaseSpecifier& base : def->base_specifiers) {
        if (base.access == AccessSpecifier::Private && current_class != source_name) {
            continue;
        }
        if (base.base_type.name == target_name) return true;
        if (has_accessible_base_conversion(program, base.base_type.name, target_name, current_class)) return true;
    }
    return false;
}

[[nodiscard]] bool types_compatible_with_base_conversion(const Type& source_type, const Type& target_type,
                                                         const Program& program, std::string_view current_class) {
    if (types_equal(source_type, target_type)) return true;
    if (target_type.kind == TypeKind::Reference && source_type.kind == TypeKind::Reference &&
        !target_type.is_rvalue_ref && !source_type.is_rvalue_ref && target_type.pointee && source_type.pointee) {
        if (target_type.is_mutable_ref && !source_type.is_mutable_ref) return false;
        if (types_equal(*source_type.pointee, *target_type.pointee)) return true;
        return target_type.pointee->kind == TypeKind::Named && source_type.pointee->kind == TypeKind::Named &&
               has_accessible_base_conversion(program, source_type.pointee->name, target_type.pointee->name,
                                              current_class);
    }
    if (target_type.kind == TypeKind::Reference && source_type.kind != TypeKind::Reference && target_type.pointee) {
        if (types_equal(source_type, *target_type.pointee)) return true;
        return target_type.pointee->kind == TypeKind::Named && source_type.kind == TypeKind::Named &&
               has_accessible_base_conversion(program, source_type.name, target_type.pointee->name, current_class);
    }
    if (target_type.kind == TypeKind::Pointer && source_type.kind == TypeKind::Pointer && target_type.pointee &&
        source_type.pointee) {
        if (target_type.is_mutable_pointee && !source_type.is_mutable_pointee) return false;
        if (types_equal(*source_type.pointee, *target_type.pointee)) return true;
        return target_type.pointee->kind == TypeKind::Named && source_type.pointee->kind == TypeKind::Named &&
               has_accessible_base_conversion(program, source_type.pointee->name, target_type.pointee->name,
                                              current_class);
    }
    return false;
}

} // namespace scpp
