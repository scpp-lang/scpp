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
[[nodiscard]] bool is_pointer_return_lifetime_source_type(const Type& type);
[[nodiscard]] bool is_function_pointer(const Type& type);
[[nodiscard]] bool is_for_range_size_builtin(const Expr& expr);
[[nodiscard]] bool is_synthesized_for_range_storage(std::string_view name);
// Writes an `auto` declaration's now-concrete type back onto the
// declaration it belongs to, so later statements in the same function
// see the real type instead of the "auto" placeholder build_mir first
// recorded. Addressed by declaration, not by name -- two `auto` locals
// in sibling scopes can share a spelling and must not overwrite each
// other.
void refine_declared_type(const Stmt& stmt, Body& body, const Type& inferred);
[[nodiscard]] bool is_reborrowable_local_type(const Type& type);
[[nodiscard]] bool local_is_suspended_for_reborrow(std::string_view name, const DataflowState& state);
[[nodiscard]] bool local_has_mutable_reborrow_suspended(std::string_view name, const DataflowState& state);
[[nodiscard]] bool is_explicit_star_this(const Expr& expr);
[[nodiscard]] Type by_reference_capture_type(const Type& captured_type, bool source_is_const);
[[nodiscard]] Type by_value_capture_type(const LambdaCapture& capture, const Type& captured_type);

[[nodiscard]] bool is_scalar_type_name(const std::string& name);
[[nodiscard]] bool is_integral_scalar_type_name(const std::string& name);
[[nodiscard]] const EnumDef* find_enum_def(const Program* program, const std::string& name);
[[nodiscard]] const EnumVariant* find_enum_variant(const Program* program, const std::string& name,
                                                  const EnumDef** owning_enum = nullptr);
[[nodiscard]] bool is_enum_type(const Type& type, const Program* program);
[[nodiscard]] const Type* enum_underlying_type(const Type& type, const Program* program);

[[nodiscard]] const ClassDef* find_class_def(const Program& program, const std::string& class_name);
[[nodiscard]] const StructDef* find_struct_def(const Program& program, const std::string& struct_name);
[[nodiscard]] bool type_contains_lifetime_carrying_state(const Type& type, const Program& program,
                                                         std::unordered_set<std::string> visiting = {});
[[nodiscard]] std::string named_type_name(const Type& type);
[[nodiscard]] bool raw_pointer_implicitly_convertible(const Type& source, const Type& target);
[[nodiscard]] bool is_scalar_named_type(const Type& type);
[[nodiscard]] bool is_float_named_type(const Type& type);
[[nodiscard]] bool is_void_named_type(const Type& type);
[[nodiscard]] bool integer_literal_compatible_with_type(const Type& type);
[[nodiscard]] const Type& binary_operand_type(const Type& type);
[[nodiscard]] bool is_pointer_arithmetic_offset_type(const Type& type);
[[nodiscard]] bool pointer_supports_arithmetic(const Type& type);
[[nodiscard]] std::optional<Type> pointer_arithmetic_result_type(BinaryOp op, const Type& lhs, const Type& rhs);
[[nodiscard]] bool literal_compatible_with_type(const Expr& literal, const Type& type);
[[nodiscard]] bool conditional_arm_types_agree(const Expr& then_arm, const Type& then_type, const Expr& else_arm,
                                               const Type& else_type);

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
namespace {
[[nodiscard]] bool unwrap_reference_wrapper_lifetime_source(const Type& type) {
    return type.is_reference_wrapper_lifetime_source;
}
}

[[nodiscard]] bool is_pointer_return_lifetime_source_type(const Type& type) {
    return is_lifetime_eligible_type(type) || unwrap_reference_wrapper_lifetime_source(type);
}
[[nodiscard]] bool is_function_pointer(const Type& type) { return type.kind == TypeKind::FunctionPointer; }
[[nodiscard]] bool is_for_range_size_builtin(const Expr& expr) {
    return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.name == "$for_range_size" && expr.args.size() == 1;
}
[[nodiscard]] bool is_synthesized_for_range_storage(std::string_view name) { return name.rfind("$for_range_", 0) == 0; }

void refine_declared_type(const Stmt& stmt, Body& body, const Type& inferred) {
    if (!has_declared_local(stmt)) return;
    LocalId id = declared_local_of(stmt);
    if (!body.is_valid_local(id)) return;
    body.decl(id).type = inferred;
}
[[nodiscard]] bool is_reborrowable_local_type(const Type& type) { return is_reference(type) || is_span(type); }
// "Is *any* reborrow (shared or mutable) currently outstanding from
// `local`?" -- used where *every* kind of outstanding reborrow must
// block (writing directly through the lender while any reference
// derived from it is still alive would be unsound, regardless of
// whether that reference is shared or mutable). Forming a *new shared*
// reborrow is less strict than this -- see local_has_mutable_reborrow_
// suspended below, which only the mutable case actually needs.
[[nodiscard]] bool local_is_suspended_for_reborrow(LocalId local, const DataflowState& state) {
    auto it = state.suspended_reborrows.find(local);
    return it != state.suspended_reborrows.end() &&
           (it->second.shared_count > 0 || it->second.mutable_suspended);
}
// "Is a *mutable* reborrow currently outstanding from `local`?" -- the
// narrower check a *new shared* reborrow attempt needs: any number of
// simultaneous shared reborrows of the same lender coexist safely (see
// ReborrowSuspension's own comment), so only an existing *mutable* one
// (which by construction excludes every other reborrow, shared or
// mutable, while it's alive) can conflict with forming another.
[[nodiscard]] bool local_has_mutable_reborrow_suspended(LocalId local, const DataflowState& state) {
    auto it = state.suspended_reborrows.find(local);
    return it != state.suspended_reborrows.end() && it->second.mutable_suspended;
}
[[nodiscard]] bool is_explicit_star_this(const Expr& expr) {
    return expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr &&
           expr.lhs->kind == ExprKind::Identifier && expr.lhs->name == "this";
}
[[nodiscard]] Type by_reference_capture_type(const Type& captured_type, bool source_is_const) {
    if (is_reference(captured_type)) return captured_type;
    Type capture_type;
    capture_type.kind = TypeKind::Reference;
    capture_type.pointee = std::make_shared<Type>(captured_type);
    capture_type.is_mutable_ref = !source_is_const;
    return capture_type;
}

[[nodiscard]] Type by_value_capture_type(const LambdaCapture& capture, const Type& captured_type) {
    // ch05 §5.12: `[this]` is spelled without `&` but is not an object
    // capture at all -- it names the enclosing receiver, and scpp models
    // it as a reference field exactly as `this` itself is a reference
    // parameter. Copying the whole object here would silently change
    // what every member access through the closure refers to, and would
    // put a class through copy rules it never asked for.
    if (capture.name == "this") return captured_type;
    // A by-value capture is an *owned* field of the closure object
    // (docs/book ch11-01: "by-value captures become ordinary owned
    // fields"), so what it stores is the referent, copied -- never the
    // reference itself, which would make the field an alias and tie the
    // closure to a lifetime a by-value capture is specifically meant to
    // be free of. Real C++ decays the same way (`[r]` where `r` is
    // `int&` copies the `int`).
    //
    // This is the mirror of by_reference_capture_type just above, and
    // the two together are the whole answer to "what type does a
    // capture's field have": that one adds a reference where there is
    // none, this one removes one where there is.
    if (is_reference(captured_type) && captured_type.pointee != nullptr) {
        // The reference's own read-only-ness (`is_mutable_ref`) stays
        // behind with the reference: what a `const int&` yields is an
        // ordinary `int` field, which a `mutable` lambda may then write,
        // because the copy is the closure's own object and writing it
        // cannot reach the original.
        return *captured_type.pointee;
    }
    return captured_type;
}

// The scalar predicates below are thin re-exports of `scpp.ast`'s
// scalar type model (see `scalar_type_info`), which is the single place
// in the compiler that lists ch06 §6's twenty names. They exist as
// `const std::string&` overloads because every caller here already holds
// a `Type::name`, and because dropping them would churn several hundred
// call sites for no gain -- but they answer nothing of their own.

[[nodiscard]] bool is_scalar_type_name(const std::string& name) {
    return scpp::is_scalar_type_name(std::string_view{name});
}

[[nodiscard]] bool is_integral_scalar_type_name(const std::string& name) {
    return scpp::is_integral_scalar_type_name(std::string_view{name});
}

[[nodiscard]] bool is_unsigned_scalar_type_name(const std::string& name) {
    return scpp::is_unsigned_scalar_type_name(std::string_view{name});
}

// spec §6: the width, in bits, of an integral scalar type name. Only
// meaningful for names is_integral_scalar_type_name accepts. movecheck
// has no target description of its own, so `size_t`/`ptrdiff_t` get the
// host's pointer width -- the same default TargetLayoutInfo uses.
[[nodiscard]] int integral_scalar_bit_width(const std::string& name) {
    return scpp::scalar_bit_width(std::string_view{name}, scpp::host_pointer_bit_width());
}

[[nodiscard]] bool integer_literal_value_fits(std::int64_t value, const std::string& type_name) {
    return scpp::integer_literal_value_fits(value, std::string_view{type_name}, scpp::host_pointer_bit_width());
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

[[nodiscard]] const StructDef* find_struct_def(const Program& program, const std::string& struct_name) {
    const StructDef* forward_decl = nullptr;
    for (const StructDef& def : program.structs) {
        if (def.name != struct_name) continue;
        if (!def.is_forward_declaration) return &def;
        if (forward_decl == nullptr) forward_decl = &def;
    }
    return forward_decl;
}

[[nodiscard]] bool type_contains_lifetime_carrying_state(const Type& type, const Program& program,
                                                         std::unordered_set<std::string> visiting) {
    if (is_pointer_return_lifetime_source_type(type)) return true;
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

// Overload resolution (ch05 §5.10) is exact type match only -- ch06
// established that no scpp scalar type implicitly converts to any other --
// and scpp::types_equal (scpp.ast) is that "exact match" test. It is
// deliberately strict about is_mutable_ref/is_mutable_pointee (`T&` and
// `const T&`, `T*` and `const T*`, are distinct parameter types for
// overloading, not interchangeable) and about is_rvalue_ref (a borrow and
// ch03's move-parameter form `T&&` are likewise never interchangeable).
//
// This file used to carry its own copy of that comparison. So did
// codegen/semantics.cppm, constexpression.cppm, parser.cppm and
// driver.cppm, and the five had drifted apart: this one ignored
// non_type_args entirely, so `Buf<4>` and `Buf<8>` compared *equal* and
// two overloads distinguished only by a non-type template argument were
// rejected here as a redefinition with an identical parameter list.

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

// spec §6: does `type` name a type that actually exists at this phase,
// or a placeholder that only becomes one at monomorphization?
//
[[nodiscard]] bool is_scalar_named_type(const Type& type) {
    return type.kind == TypeKind::Named && scpp::is_scalar_type_name(std::string_view{type.name});
}

[[nodiscard]] bool is_float_named_type(const Type& type) {
    return type.kind == TypeKind::Named && scpp::is_float_scalar_type_name(std::string_view{type.name});
}

// `void` itself, never `void*`. Deliberately a predicate of its own
// rather than a name spelled out at each place that needs it: every
// question this file answers is asked of a *value*, and `void` is the
// one named type that is not one, so "is this a value at all?" has to be
// askable separately from "which type is it?".
[[nodiscard]] bool is_void_named_type(const Type& type) {
    return type.kind == TypeKind::Named && type.name == "void";
}

[[nodiscard]] bool integer_literal_compatible_with_type(const Type& type) {
    return integer_literal_may_adopt_type(type);
}

[[nodiscard]] const Type& binary_operand_type(const Type& type) {
    return literal_adoption_target(type);
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

// Binds `scpp.ast`'s literal-adoption rule to the host's pointer width.
// The rule lives there so that constant evaluation can ask the same
// question and get the same answer -- see `literal_adopts_type`. Its
// companion `is_untyped_numeric_literal` needs no such binding, so it
// moved to `scpp.ast` outright; both declaration and definition are gone
// from here and every caller now resolves to the `scpp.ast` one.
[[nodiscard]] bool literal_compatible_with_type(const Expr& literal, const Type& type) {
    return literal_adopts_type(literal, type, scpp::host_pointer_bit_width());
}

// ch05/ch06: `?:` yields a *value*, and its two arms have to agree on
// that value's type. They are allowed to disagree in exactly the two
// ways a binary operator's own operands already may (see
// binary_expr_has_compatible_types, which this deliberately mirrors):
//
//   - a *scalar lvalue* arm contributes its referent type, since the
//     conditional is a value rather than a place -- `packed ? size :
//     std::max(a, b)` agrees even though `std::max` returns `const
//     std::uint64_t&`. Deliberately restricted to scalars: decaying a
//     class-typed reference arm to a value would be a silent copy,
//     which scpp never performs implicitly.
//   - an untyped numeric/bool/char literal arm adopts the other arm's
//     type, the same rule literal_compatible_with_type applies to a
//     binary operand -- `packed ? 1 : alignment`.
//
// Everything else still has to match exactly: scpp has no implicit
// scalar conversions (ch06).
[[nodiscard]] bool conditional_arm_types_agree(const Expr& then_arm, const Type& then_type, const Expr& else_arm,
                                               const Type& else_type) {
    if (types_equal(then_type, else_type)) return true;
    const Type& then_value = binary_operand_type(then_type);
    const Type& else_value = binary_operand_type(else_type);
    if (is_scalar_named_type(then_value) && is_scalar_named_type(else_value) && types_equal(then_value, else_value)) {
        return true;
    }
    return literal_compatible_with_type(then_arm, else_value) || literal_compatible_with_type(else_arm, then_value);
}
[[nodiscard]] std::string enclosing_class_name(const Body& body) {
    std::optional<LocalId> self = body.this_local();
    if (!self) return "";
    return named_type_name(body.type_of(*self));
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
