module;

module scpp.compiler.codegen:semantics;

import std;
import :api;

namespace scpp {

namespace {

[[nodiscard]] bool same_named_record_type_ignoring_top_level_const(const Type& source_type, const Type& target_type) {
    if (source_type.kind != TypeKind::Named || target_type.kind != TypeKind::Named) return false;
    return source_type.name == target_type.name;
}

[[nodiscard]] bool pointer_to_void_parameter_accepts_pointer_in_unsafe_context(const Type& source_type,
                                                                               const Type& target_type,
                                                                               int unsafe_depth)
{
    if (unsafe_depth <= 0 || source_type.kind != TypeKind::Pointer || target_type.kind != TypeKind::Pointer ||
        source_type.pointee == nullptr || target_type.pointee == nullptr) {
        return false;
    }
    if (target_type.pointee->kind != TypeKind::Named || target_type.pointee->name != "void") return false;
    if (target_type.is_mutable_pointee && !source_type.is_mutable_pointee) return false;
    return true;
}

// Both derive from `scpp.ast`'s scalar type model -- see
// `scalar_type_info` for why that is the only place the twenty names of
// ch06 §6 are listed.
[[nodiscard]] bool is_float_scalar_name(std::string_view name) { return scpp::is_float_scalar_type_name(name); }

[[nodiscard]] bool is_integral_scalar_name(std::string_view name) { return scpp::is_integral_scalar_type_name(name); }

[[nodiscard]] bool function_accepts_argument_count(const Function& fn, std::size_t arg_count, std::size_t param_offset) {
    if (fn.params.size() < param_offset) return false;
    std::size_t fixed_param_count = fn.params.size() - param_offset;
    std::size_t min_required = fixed_param_count;
    while (min_required > 0 && fn.params[param_offset + min_required - 1].default_expr != nullptr) {
        min_required--;
    }
    if (arg_count < min_required) return false;
    if (!fn.has_varargs && arg_count > fixed_param_count) return false;
    return fn.has_varargs || arg_count <= fixed_param_count;
}

[[nodiscard]] bool literal_matches_scalar_parameter(const Expr& arg, const Type& target_type) {
    auto is_negative_literal = [&](ExprKind kind) {
        return arg.kind == ExprKind::Unary && arg.unary_op == UnaryOp::Neg && arg.lhs != nullptr && arg.lhs->kind == kind;
    };
    if (target_type.kind != TypeKind::Named) return false;
    if ((arg.kind == ExprKind::IntegerLiteral || is_negative_literal(ExprKind::IntegerLiteral)) &&
        (is_float_scalar_name(target_type.name) ||
         (is_integral_scalar_name(target_type.name) && target_type.name != "bool" &&
          target_type.name != "char"))) {
        return true;
    }
    return (arg.kind == ExprKind::FloatLiteral || is_negative_literal(ExprKind::FloatLiteral)) &&
           is_float_scalar_name(target_type.name);
}

[[nodiscard]] bool is_named_record_type_for_call_binding(const Type& type, const Program& program) {
    if (type.kind != TypeKind::Named) return false;
    if (std::ranges::any_of(program.classes, [&](const ClassDef& def) { return def.name == type.name; })) return true;
    return std::ranges::any_of(program.structs, [&](const StructDef& def) { return def.name == type.name; });
}

[[nodiscard]] bool is_nullptr_literal(const Expr& expr) {
    return expr.kind == ExprKind::NullptrLiteral;
}

} // namespace

    const StructDef* Codegen::find_struct_def(const std::string& name) const {
        const StructDef* forward_decl = nullptr;
        for (const StructDef& def : program_->structs) {
            if (def.name != name) continue;
            if (!def.is_forward_declaration) return &def;
            if (forward_decl == nullptr) forward_decl = &def;
        }
        return forward_decl;
    }


    const ClassDef* Codegen::find_class_def(const std::string& name) const
{
        for (const ClassDef& def : program_->classes) {
            if (def.name == name) return &def;
        }
        return nullptr;
    }


    [[nodiscard]] bool Codegen::is_named_record_type(const Type& type) const
{
        return type.kind == TypeKind::Named && (find_class_def(type.name) != nullptr || find_struct_def(type.name) != nullptr);
    }


    const Function* Codegen::find_function_def(const std::string& name) const
{
        for (const Function& fn : program_->functions) {
            if (fn.name == name) return &fn;
        }
        return nullptr;
    }


    [[nodiscard]] const Function* Codegen::resolve_converting_constructor_by_type(const std::string& class_name, const Expr& arg)
{
        return find_single_argument_converting_constructor(class_name, arg);
    }


    [[nodiscard]] bool Codegen::is_for_range_size_builtin(const Expr& expr) const
{
        return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.name == "$for_range_size" && expr.args.size() == 1;
    }


    std::optional<Type> Codegen::infer_type(const Expr& expr)
{
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return named_type("int");
            case ExprKind::FloatLiteral: return named_type("double");
            case ExprKind::BoolLiteral: return named_type("bool");
            case ExprKind::NullptrLiteral: return nullptr_named_type();
            case ExprKind::Alignof:
            case ExprKind::Sizeof:
                return named_type("size_t");
            case ExprKind::ValueInit:
                return expr.type;
            case ExprKind::TypeTrait: return named_type("bool");
            case ExprKind::CharLiteral: return named_type("char");
            case ExprKind::StringLiteral: {
                Type result;
                result.kind = TypeKind::Pointer;
                result.pointee = std::make_shared<Type>(named_type("char"));
                result.is_mutable_pointee = false;
                return result;
            }

            case ExprKind::Identifier: {
                if (const LocalSlot* local = find_local(expr)) return local->type;
                if (const GlobalSlot* global = find_visible_global_slot(expr.name, expr.explicit_global_qualification)) {
                    return global->type;
                }
                if (const EnumDef* def = [&, this]() {
                        const EnumDef* enum_def = nullptr;
                        [[maybe_unused]] const EnumVariant* variant = find_enum_variant(program_, expr.name, &enum_def);
                        return enum_def;
                    }()) {
                    return named_type(def->name);
                }
                return resolve_function_designator_type(expr);
            }

            case ExprKind::Move: {
                if (expr.lhs->kind != ExprKind::Identifier) return std::nullopt;
                const LocalSlot* local = find_local(*expr.lhs);
                if (local == nullptr) {
                    if (const GlobalSlot* global =
                            find_visible_global_slot(expr.lhs->name, expr.lhs->explicit_global_qualification)) {
                        return global->type;
                    }
                    return std::nullopt;
                }
                return std::optional<Type>(local->type);
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

            case ExprKind::Lambda: {
                // ch05 §5.12: once resolved (movecheck's closure-
                // resolution pass), `expr.name` holds the synthesized
                // closure class's own name -- its type is exactly that
                // class, by value (matching MakeUnique's identical shape
                // just above: a fresh, concretely-typed value).
                if (expr.name.empty()) return std::nullopt;
                return named_type(expr.name);
            }

            case ExprKind::Member: {
                std::optional<Type> base = infer_type(*expr.lhs);
                if (!base) return std::nullopt;
                // See codegen_lvalue's Identifier case: a Reference-typed
                // base (e.g. `this`) auto-dereferences to its pointee.
                const Type& base_named =
                    base->kind == TypeKind::Reference && base->pointee != nullptr ? *base->pointee : *base;
                if (base_named.kind != TypeKind::Named) return std::nullopt;
                auto struct_it = structs_.find(base_named.name);
                if (struct_it == structs_.end()) return std::nullopt;
                const StructInfo& info = struct_it->second;
                std::optional<std::size_t> field_index = info.find_field_index(expr.name);
                if (!field_index.has_value()) {
                    return resolve_function_designator_type(expr);
                }
                const Type& field_type = info.field_types[*field_index];
                // ch05 §5.12: a Reference-typed field (e.g. a closure's
                // own by-reference capture) auto-dereferences to its
                // pointee too, exactly like codegen_lvalue's own
                // (matching) Member-case fix -- `this.b`'s *type* is the
                // referent's type, not "a reference to it".
                return field_type.kind == TypeKind::Reference ? *field_type.pointee : field_type;
            }

            case ExprKind::Subscript: {
                std::optional<Type> base = infer_type(*expr.lhs);
                if (!base) return std::nullopt;
                const Type& effective = base->kind == TypeKind::Reference && base->pointee ? *base->pointee : *base;
                if (effective.kind == TypeKind::Array) return *effective.element;
                if (effective.kind == TypeKind::Span) return *effective.pointee;
                if (effective.kind == TypeKind::Pointer) return *effective.pointee;
                if (effective.kind == TypeKind::Named &&
                    (effective.name == "std::vector" || effective.name == "vector" ||
                     effective.name.starts_with("std::vector.") || effective.name.starts_with("vector."))) {
                    if (effective.template_args.size() == 1) return effective.template_args[0];
                    auto struct_it = structs_.find(effective.name);
                    if (struct_it != structs_.end()) {
                        const StructInfo& info = struct_it->second;
                        if (std::optional<std::size_t> data_index_opt = info.find_field_index("data_"); data_index_opt.has_value()) {
                            const Type& field_type = info.field_types[*data_index_opt];
                            if (field_type.kind == TypeKind::Pointer && field_type.pointee) return *field_type.pointee;
                        }
                    }
                }
                return std::nullopt;
            }

            case ExprKind::Unary:
                switch (expr.unary_op) {
                    case UnaryOp::Not: return named_type("bool");
                    case UnaryOp::Neg: return infer_type(*expr.lhs);
                    case UnaryOp::PreInc:
                    case UnaryOp::PreDec:
                    case UnaryOp::PostInc:
                    case UnaryOp::PostDec:
                        return infer_type(*expr.lhs);
                    case UnaryOp::AddressOf: {
                        if (std::optional<Type> fn_ptr = resolve_function_designator_type(expr)) return fn_ptr;
                        std::optional<Type> operand = infer_type(*expr.lhs);
                        if (!operand) return std::nullopt;
                        Type result;
                        result.kind = TypeKind::Pointer;
                        if (operand->kind == TypeKind::Reference && operand->pointee) {
                            result.pointee = std::make_shared<Type>(*operand->pointee);
                            result.is_mutable_pointee = operand->is_mutable_ref;
                        } else {
                            result.pointee = std::make_shared<Type>(std::move(*operand));
                            result.is_mutable_pointee = true; // &expr always yields a mutable T* (ch05 §5.7)
                        }
                        return result;
                    }
                    case UnaryOp::Deref: {
                        std::optional<Type> operand = infer_type(*expr.lhs);
                        if (!operand) return std::nullopt;
                        if (expr.lhs->kind == ExprKind::Identifier && expr.lhs->name == "this" &&
                            operand->kind == TypeKind::Reference && operand->pointee) {
                            return *operand->pointee;
                        }
                        if (operand->kind == TypeKind::FunctionPointer) return *operand;
                        const Type& underlying =
                            operand->kind == TypeKind::Reference && operand->pointee ? *operand->pointee : *operand;
                        if (underlying.kind == TypeKind::Named) {
                            std::vector<ExprPtr> no_args;
                            bool receiver_is_mutable = !(operand->kind == TypeKind::Reference && !operand->is_mutable_ref);
                            if (const Function* callee =
                                    resolve_overload_by_type(underlying.name + "_operator_deref", no_args, 1,
                                                         receiver_is_mutable, expr.lhs.get())) {
                                return callee->return_type.kind == TypeKind::Reference
                                           ? std::optional<Type>(*callee->return_type.pointee)
                                           : std::optional<Type>(callee->return_type);
                            }
                        }
                        if (operand->kind != TypeKind::Pointer) {
                            return std::nullopt;
                        }
                        return *operand->pointee;
                    }
                }
                return std::nullopt;

            // `static_cast<T>(expr)`/`(T)expr` (ch06 §6): the cast's own
            // declared target type, unconditionally -- that *is* the
            // whole point of an explicit cast (movecheck's own Cast
            // handling is what actually validates the source/target
            // pairing is legal in the first place).
            case ExprKind::Cast: return expr.type;

            case ExprKind::Binary:
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
                        // Each operand is inferred at most once, and the
                        // answer reused. Inferring the left operand for the
                        // pointer-arithmetic test and then inferring it
                        // again to produce the result made an n-term
                        // left-leaning `a + b + c + ...` cost 2^n, because
                        // each level re-walked its entire prefix -- 4.2
                        // million Type constructions for 22 terms, which is
                        // 2^22. movecheck's infer_expr_type had the same
                        // shape and is corrected the same way.
                        if (expr.binary_op != BinaryOp::Add && expr.binary_op != BinaryOp::Sub) {
                            return infer_type(*expr.lhs);
                        }
                        std::optional<Type> lhs = infer_type(*expr.lhs);
                        std::optional<Type> rhs = infer_type(*expr.rhs);
                        if (lhs.has_value() && rhs.has_value()) {
                            if (std::optional<Type> result = pointer_arithmetic_result_type(expr.binary_op, *lhs, *rhs)) {
                                return result;
                            }
                        }
                        return lhs;
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

            case ExprKind::Conditional: {
                std::optional<Type> then_type = infer_type(*expr.rhs);
                std::optional<Type> else_type = infer_type(*expr.third);
                if (!then_type.has_value() || !else_type.has_value()) return std::nullopt;
                return types_equal(*then_type, *else_type) ? then_type : std::nullopt;
            }

            case ExprKind::Fold:
            case ExprKind::PackExpansion:
                // Fold expressions are expanded away during generic-call
                // monomorphization; no concrete codegen path should ever
                // see one. Same for a raw `args...` pack expansion.
                return std::nullopt;

            case ExprKind::Call: {
                if (is_for_range_size_builtin(expr)) return named_type("int");
                if (expr.lhs == nullptr) {
                    if (structs_.contains(expr.name)) return named_type(expr.name);
                    if (find_class_def(expr.name) != nullptr) return named_type(expr.name);
                }
                if (expr.lhs != nullptr && expr.name.empty()) {
                    const Expr* callee_expr = expr.lhs.get();
                    if (callee_expr->kind == ExprKind::Unary && callee_expr->unary_op == UnaryOp::Deref &&
                        callee_expr->lhs != nullptr) {
                        callee_expr = callee_expr->lhs.get();
                    }
                    std::optional<Type> callee_type = infer_type(*callee_expr);
                    if (callee_type.has_value() && callee_type->kind == TypeKind::FunctionPointer) {
                        return *callee_type->function_return;
                    }
                    return std::nullopt;
                }
                if (expr.lhs == nullptr) {
                    if (const LocalSlot* callee_local = find_local(expr);
                        callee_local != nullptr && callee_local->type.kind == TypeKind::FunctionPointer) {
                        return *callee_local->type.function_return;
                    }
                }
                auto inferred_call_argument_type = [&, this](const Expr& arg, const Type& param_type) -> std::optional<Type> {
                    std::optional<Type> inferred = infer_type(arg);
                    if (inferred.has_value()) return inferred;
                    if (arg.kind == ExprKind::Identifier && param_type.kind == TypeKind::Reference &&
                        !param_type.is_mutable_ref && !param_type.is_rvalue_ref && param_type.pointee != nullptr) {
                        const LocalSlot* arg_local = find_local(arg);
                        if (arg_local != nullptr && arg_local->type.kind == TypeKind::Reference &&
                            arg_local->type.is_rvalue_ref && arg_local->type.pointee != nullptr &&
                            types_equal(*arg_local->type.pointee, *param_type.pointee)) {
                            Type fallback = arg_local->type;
                            fallback.name = arg_local->type.pointee->name;
                            return fallback;
                        }
                    }
                    return std::nullopt;
                };
                std::string callee_name = expr.name;
                std::size_t param_offset = 0;
                bool receiver_is_mutable = true;
                if (expr.lhs != nullptr) {
                    std::optional<Type> receiver = infer_type(*expr.lhs);
                    if (!receiver) return std::nullopt;
                    const Type& receiver_named =
                        receiver->kind == TypeKind::Reference && receiver->pointee != nullptr ? *receiver->pointee : *receiver;
                    if (receiver_named.kind != TypeKind::Named) return std::nullopt;
                    callee_name = receiver_named.name + "_" + expr.name;
                    param_offset = 1;
                    receiver_is_mutable = !is_read_only_place(*expr.lhs);
                }
                const Function* callee =
                    resolve_overload_by_type(callee_name, expr.args, param_offset, receiver_is_mutable, expr.lhs.get());
                if (callee == nullptr && expr.lhs != nullptr) {
                    for (const Function& fn : program_->functions) {
                        if (fn.name != callee_name || fn.is_generic_template) continue;
                        if (!function_accepts_argument_count(fn, expr.args.size(), param_offset)) continue;
                        if (param_offset == 1 && !receiver_matches_method_qualifier(*expr.lhs, fn)) continue;
                        bool all_match = true;
                        std::size_t fixed_param_count = fn.params.size() - param_offset;
                        for (std::size_t i = 0; all_match && i < expr.args.size() && i < fixed_param_count; i++) {
                            const Type& param_type = fn.params[i + param_offset].type;
                            std::optional<Type> arg_type = inferred_call_argument_type(*expr.args[i], param_type);
                            if (!arg_type.has_value()) {
                                all_match = false;
                                break;
                            }
                            if (param_type.kind == TypeKind::Reference && !param_type.is_mutable_ref &&
                                !param_type.is_rvalue_ref && param_type.pointee != nullptr &&
                                arg_type->kind == TypeKind::Reference && arg_type->is_rvalue_ref &&
                                arg_type->pointee != nullptr) {
                                all_match = types_equal(*arg_type->pointee, *param_type.pointee);
                            } else {
                                all_match = argument_type_matches_parameter(*arg_type, param_type) ||
                                            literal_matches_scalar_parameter(*expr.args[i], param_type);
                            }
                        }
                        if (all_match) {
                            callee = &fn;
                            break;
                        }
                    }
                }
                return callee == nullptr ? std::nullopt : std::optional<Type>(callee->return_type);
            }
        }
        return std::nullopt;
    }


    bool Codegen::produces_rvalue_of_type(const Expr& arg, const Type& expected_type)
{
        switch (arg.kind) {
            case ExprKind::Move:
            case ExprKind::New:
            case ExprKind::IntegerLiteral:
            case ExprKind::FloatLiteral:
            case ExprKind::BoolLiteral:
            case ExprKind::CharLiteral:
            case ExprKind::StringLiteral:
            case ExprKind::Alignof:
            case ExprKind::Sizeof:
            case ExprKind::Lambda:
            case ExprKind::ValueInit:
                break;
            case ExprKind::Call: {
                std::optional<Type> t = infer_type(arg);
                if (!t.has_value() || t->kind == TypeKind::Reference) return false;
                break;
            }
            default:
                return false;
        }
        std::optional<Type> arg_type = infer_type(arg);
        if (!arg_type.has_value()) return false;
        if (types_equal(*arg_type, expected_type)) return true;
        if (arg.kind == ExprKind::Move && arg_type->kind == TypeKind::Reference && arg_type->pointee != nullptr) {
            return types_equal(*arg_type->pointee, expected_type);
        }
        return false;
    }


    bool Codegen::const_reference_binds_materialized_temporary(const Expr& arg, const Type& param_type)
{
        if (param_type.kind != TypeKind::Reference || param_type.is_rvalue_ref || param_type.is_mutable_ref ||
            param_type.pointee == nullptr) {
            return false;
        }
        if (arg.kind == ExprKind::StringLiteral && param_type.pointee->kind == TypeKind::Named &&
            param_type.pointee->name == "std::string_view") {
            return true;
        }
        if (produces_rvalue_of_type(arg, *param_type.pointee)) return true;
        return is_named_record_type(*param_type.pointee) &&
               find_single_argument_converting_constructor(param_type.pointee->name, arg) != nullptr;
    }


    [[nodiscard]] bool Codegen::is_lvalue_copy_source_shape(const Expr& expr)
{
        switch (expr.kind) {
            case ExprKind::Identifier:
                return true;
            case ExprKind::Member:
            case ExprKind::Subscript:
                return expr.lhs != nullptr && is_lvalue_copy_source_shape(*expr.lhs);
            case ExprKind::Unary:
                // `*p` (ch05 §5.7) is unconditionally a genuine lvalue
                // "place" -- dereferencing any pointer yields an alias to
                // already-existing storage, regardless of whether the
                // pointer expression itself is an lvalue or a fresh
                // prvalue (e.g. the result of an arrow-access rewritten
                // by movecheck's rewrite_arrow_receiver into
                // `*(recv.operator_arrow())`). Matches codegen_lvalue's
                // own Unary case, which likewise treats Deref (and only
                // Deref, alongside prefix ++/--) as addressable with no
                // further recursion into the operand's own shape.
                return expr.unary_op == UnaryOp::Deref;
            case ExprKind::Call: {
                // A call expression is only a genuine lvalue "place"
                // (an alias to an already-existing object, e.g.
                // `container.at(i)`) when it returns by reference --
                // mirrors produces_rvalue_of_type's own Call handling,
                // which likewise treats a reference-returning call as
                // *not* a fresh rvalue, for the same reason. A call that
                // returns by value is a fresh prvalue and must stay
                // excluded here so it is (already correctly) handled by
                // that produces_rvalue_of_type path instead -- see the
                // branch order in try_initialize_class_storage_from_
                // same_type_source -- rather than being miscategorized
                // as a copyable place (which codegen_lvalue's own Call
                // case would then reject as "not assignable").
                std::optional<Type> call_type = infer_type(expr);
                return call_type.has_value() && call_type->kind == TypeKind::Reference;
            }
            default:
                return false;
        }
    }


    [[nodiscard]] bool Codegen::is_bare_same_type_copy_source(const Expr& expr, const Type& target_type)
{
        if (!is_lvalue_copy_source_shape(expr)) return false;
        std::optional<Type> expr_type = infer_type(expr);
        if (!expr_type.has_value()) return false;
        if (same_named_record_type_ignoring_top_level_const(*expr_type, target_type) ||
            types_equal(*expr_type, target_type)) {
            return true;
        }
        return expr_type->kind == TypeKind::Reference && !expr_type->is_rvalue_ref &&
               expr_type->pointee != nullptr &&
               (same_named_record_type_ignoring_top_level_const(*expr_type->pointee, target_type) ||
                types_equal(*expr_type->pointee, target_type));
    }


    [[nodiscard]] bool Codegen::is_implicit_move_return_source(const Expr& expr, const Type& target_type)
{
        if (expr.kind != ExprKind::Identifier) return false;
        const LocalSlot* local = find_local(expr);
        return local != nullptr && types_equal(local->type, target_type);
    }


    const Function* Codegen::find_single_argument_converting_constructor(const std::string& class_name, const Expr& arg)
{
        auto is_constructor_clone = [&](const Function& fn) {
            return fn.name == class_name + "_new" ||
                   (!fn.member_owner_class.empty() && fn.member_owner_class == class_name &&
                    fn.name.starts_with(class_name + "_new."));
        };
        std::vector<const Function*> matches;
        for (const Function& fn : program_->functions) {
            if (!is_constructor_clone(fn)) continue;
            if (fn.member_owner_class != class_name || fn.params.size() != 2) continue;
            const Type& ctor_param_type = fn.params[1].type;
            if (types_equal(ctor_param_type, named_type(class_name)) ||
                (ctor_param_type.kind == TypeKind::Reference && ctor_param_type.pointee != nullptr &&
                 types_equal(*ctor_param_type.pointee, named_type(class_name)))) {
                continue;
            }
            if (constructor_parameter_accepts_argument_directly(arg, fn.params[1].type)) matches.push_back(&fn);
        }
        if (matches.empty()) return nullptr;
        return matches[0];
    }


    bool Codegen::argument_type_matches_parameter(const Type& arg_type, const Type& candidate_param_type)
{
        if (candidate_param_type.kind == TypeKind::Pointer && arg_type.kind == TypeKind::Array &&
            candidate_param_type.pointee != nullptr && arg_type.element != nullptr) {
            return (!candidate_param_type.is_mutable_pointee || !arg_type.element->is_const_qualified) &&
                   types_equal(*arg_type.element, *candidate_param_type.pointee);
        }
        if (candidate_param_type.kind == TypeKind::Span && arg_type.kind == TypeKind::Array &&
            candidate_param_type.pointee != nullptr && arg_type.element != nullptr) {
            return (!candidate_param_type.is_mutable_ref || !arg_type.element->is_const_qualified) &&
                   types_equal(*arg_type.element, *candidate_param_type.pointee);
        }
        if (candidate_param_type.kind == TypeKind::Reference) {
            if (arg_type.kind == TypeKind::Reference) {
                if (arg_type.pointee == nullptr || candidate_param_type.pointee == nullptr) return false;
                // A *mutable* lvalue reference parameter (`T&`) cannot
                // bind to an rvalue argument -- but a *const* lvalue
                // reference (`const T&`) can bind to either an lvalue or
                // an rvalue (this is ordinary, legal C++-like reference-
                // binding; e.g. passing `std::move(x)` to a `const T&`
                // parameter is valid). `candidate_param_type.is_rvalue_ref`
                // is already guaranteed false here -- callers special-
                // case genuine `T&&` parameters separately before ever
                // reaching this comparison -- so only the mutable-ref
                // case needs this rejection.
                if (arg_type.is_rvalue_ref && candidate_param_type.is_mutable_ref) return false;
                bool pointee_same_base = types_equal(*arg_type.pointee, *candidate_param_type.pointee);
                bool const_compatible_pointee =
                    arg_type.pointee->kind == candidate_param_type.pointee->kind &&
                    arg_type.pointee->name == candidate_param_type.pointee->name &&
                    arg_type.pointee->template_args.size() == candidate_param_type.pointee->template_args.size() &&
                    (!arg_type.pointee->is_const_qualified || candidate_param_type.pointee->is_const_qualified);
                if (const_compatible_pointee) {
                    for (std::size_t i = 0; i < arg_type.pointee->template_args.size(); i++) {
                        if (!types_equal(arg_type.pointee->template_args[i], candidate_param_type.pointee->template_args[i])) {
                            const_compatible_pointee = false;
                            break;
                        }
                    }
                }
                if (!(pointee_same_base || const_compatible_pointee)) return false;
                if (candidate_param_type.pointee->is_const_qualified) return true;
                // ch05 §5.10 (pre-existing rule, preserved from before
                // this function's stricter-pointee-matching rewrite): a
                // *const* reference parameter accepts any argument
                // reference regardless of the argument's own mutability
                // (widening a mutable ref to a const one is always
                // legal); a *mutable* reference parameter additionally
                // requires the argument itself to be mutable.
                return !candidate_param_type.is_mutable_ref || arg_type.is_mutable_ref;
            }
            return candidate_param_type.pointee != nullptr && types_equal(arg_type, *candidate_param_type.pointee);
        }
        if (arg_type.kind == TypeKind::Reference) {
            return arg_type.pointee != nullptr &&
                   (types_equal(*arg_type.pointee, candidate_param_type) ||
                    types_compatible_with_base_conversion(*arg_type.pointee, candidate_param_type,
                                                          current_enclosing_class_name()));
        }
        return types_equal(arg_type, candidate_param_type);
    }


    bool Codegen::argument_matches_parameter(const Expr& arg, const Type& param_type)
{
        if (is_nullptr_literal(arg) && param_type.kind == TypeKind::Pointer) return true;
        auto argument_type_matches_or_converts = [&, this](const Type& arg_type, const Type& candidate_param_type) {
            return argument_type_matches_parameter(arg_type, candidate_param_type) ||
                   pointer_to_void_parameter_accepts_pointer_in_unsafe_context(arg_type, candidate_param_type,
                                                                               unsafe_depth_) ||
                   types_compatible_with_base_conversion(arg_type, candidate_param_type, current_enclosing_class_name());
        };
        if (param_type.kind == TypeKind::Reference && param_type.is_rvalue_ref) {
            // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- mirror image of
            // the ordinary-reference case just below.
            return produces_rvalue_of_type(arg, *param_type.pointee);
        }
        if (param_type.kind == TypeKind::Reference) {
            // ch05 §5.x: a *const* reference may bind either to a
            // genuine rvalue of the exact pointee type, or to a freshly
            // materialized temporary built through a converting
            // constructor such as `std::string{"..."}` from a string
            // literal.
            if (const_reference_binds_materialized_temporary(arg, param_type)) {
                return true;
            }
            if (arg.kind == ExprKind::Move || arg.kind == ExprKind::New ||
                arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::FloatLiteral ||
                arg.kind == ExprKind::BoolLiteral ||
                arg.kind == ExprKind::CharLiteral || arg.kind == ExprKind::StringLiteral) {
                return false;
            }
            std::optional<Type> arg_type = infer_type(arg);
            return arg_type.has_value() && argument_type_matches_or_converts(*arg_type, param_type);
        }
        if (literal_matches_scalar_parameter(arg, param_type)) return true;
        std::optional<Type> arg_type = infer_type(arg);
        if (!arg_type.has_value()) return false;
        if (!argument_type_matches_or_converts(*arg_type, param_type)) {
            if (is_named_record_type(param_type) &&
                find_single_argument_converting_constructor(param_type.name, arg) != nullptr) {
                return true;
            }
            return false;
        }
        if (is_named_record_type_for_call_binding(param_type, *program_)) {
            return (is_bare_same_type_copy_source(arg, param_type) && is_copy_constructible(param_type.name)) ||
                   produces_rvalue_of_type(arg, param_type);
        }
        return true;
    }


    bool Codegen::constructor_parameter_accepts_argument_directly(const Expr& arg, const Type& param_type)
{
        auto inferred_argument_type = [&, this]() -> std::optional<Type> {
            std::optional<Type> inferred = infer_type(arg);
            if (inferred.has_value()) return inferred;
            if (arg.kind == ExprKind::Identifier) {
                const LocalSlot* local = find_local(arg);
                if (local != nullptr && local->type.kind == TypeKind::Reference && local->type.is_rvalue_ref &&
                    local->type.pointee != nullptr) {
                    return *local->type.pointee;
                }
            }
            return std::nullopt;
        };
        auto normalized_param_type = [&](Type type) {
            return type;
        };
        if (is_nullptr_literal(arg) && param_type.kind == TypeKind::Pointer) return true;
        Type effective_param_type = normalized_param_type(param_type);
        if (effective_param_type.kind == TypeKind::Reference && effective_param_type.is_rvalue_ref) {
            return produces_rvalue_of_type(arg, *effective_param_type.pointee);
        }
        if (effective_param_type.kind == TypeKind::Reference) {
            if (!effective_param_type.is_mutable_ref && effective_param_type.pointee != nullptr &&
                produces_rvalue_of_type(arg, *effective_param_type.pointee)) {
                return true;
            }
            if (arg.kind == ExprKind::Move || arg.kind == ExprKind::New ||
                arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::FloatLiteral ||
                arg.kind == ExprKind::BoolLiteral ||
                arg.kind == ExprKind::CharLiteral || arg.kind == ExprKind::StringLiteral) {
                return false;
            }
            std::optional<Type> arg_type = inferred_argument_type();
            return arg_type.has_value() && argument_type_matches_parameter(*arg_type, effective_param_type);
        }
        std::optional<Type> arg_type = inferred_argument_type();
        if (!arg_type.has_value() || !argument_type_matches_parameter(*arg_type, effective_param_type)) return false;
        if (is_named_record_type(effective_param_type)) {
            return (is_bare_same_type_copy_source(arg, effective_param_type) && is_copy_constructible(effective_param_type.name)) ||
                   produces_rvalue_of_type(arg, effective_param_type);
        }
        return true;
    }


    bool Codegen::is_read_only_place(const Expr& expr)
{
        switch (expr.kind) {
            case ExprKind::Identifier: {
                const LocalSlot* local = find_local(expr);
                if (local == nullptr) return false;
                return local->is_const || (local->type.kind == TypeKind::Reference && !local->type.is_mutable_ref);
            }
            case ExprKind::Member:
            case ExprKind::Subscript:
                return is_read_only_place(*expr.lhs);
            case ExprKind::Unary:
                if (expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PreDec) {
                    return is_read_only_place(*expr.lhs);
                }
                if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) return false;
                {
                    const LocalSlot* local = find_local(*expr.lhs);
                    return local != nullptr && local->type.kind == TypeKind::Pointer && !local->type.is_mutable_pointee;
                }
            case ExprKind::Call: {
                std::optional<Type> t = infer_type(expr);
                return t.has_value() && t->kind == TypeKind::Reference && !t->is_mutable_ref;
            }
            default:
                return false;
        }
    }


    bool Codegen::receiver_matches_method_qualifier(const Expr& receiver_expr, const Function& fn)
{
        if (fn.params.empty() || fn.params[0].type.kind != TypeKind::Reference || fn.params[0].type.pointee == nullptr) {
            return true;
        }
        Type receiver_expected = *fn.params[0].type.pointee;
        receiver_expected.is_const_qualified = false;
        bool receiver_is_rvalue = produces_rvalue_of_type(receiver_expr, receiver_expected);
        switch (fn.receiver_ref_qualifier) {
            case ReceiverRefQualifier::None: return true;
            case ReceiverRefQualifier::LValue: return !receiver_is_rvalue;
            case ReceiverRefQualifier::RValue: return receiver_is_rvalue;
        }
        return true;
    }


    bool Codegen::rvalue_ref_collapses_to_value(const Function& fn, std::size_t param_index, const Expr& arg,
                                                std::size_t param_offset)
{
        if (param_offset == 0) return false;
        if (!fn.member_owner_class.empty()) return false;
        if (param_index >= fn.params.size()) return false;
        const Type& param_type = fn.params[param_index].type;
        return param_type.kind == TypeKind::Reference && param_type.is_rvalue_ref && param_type.pointee != nullptr &&
               produces_rvalue_of_type(arg, *param_type.pointee);
    }


    Type Codegen::normalized_param_type(const Expr& arg, Type type)
{
        if (type.kind == TypeKind::Named && !type.name.empty() && type.template_args.empty() &&
            std::isupper(static_cast<unsigned char>(type.name[0]))) {
            if (std::optional<Type> inferred = infer_type(arg); inferred.has_value()) return *inferred;
        }
        return type;
    }


    // Every name whose declaration could conceivably be what this call
    // targets, before any argument is looked at -- monomorphized clones
    // and inherited-method helpers included (see the four shape tests
    // below). Split out of resolve_overload_by_type so that
    // describe_call_resolution_failure can ask the same question: a
    // diagnostic that decides "this name doesn't exist" by a *different*
    // rule than the resolver used is exactly how the misleading "call to
    // unknown function" message survived this long.
    std::vector<const Function*> Codegen::collect_call_candidates(const std::string& callee_name,
                                                                  std::size_t param_offset, const Expr* receiver_expr)
{
        auto is_constructor_clone = [&](const Function& fn) {
            return callee_name.ends_with("_new") &&
                   fn.name.starts_with(callee_name + ".");
        };
        auto is_dotted_clone = [&](const Function& fn) {
            if (!fn.name.ends_with(callee_name) || fn.name.size() <= callee_name.size()) return false;
            std::size_t separator = fn.name.size() - callee_name.size() - 1;
            if (fn.name[separator] != '.') return false;
            // ch05 §5.14: the text before that '.' has to be this
            // function's own owner class -- the shape
            // matches_receiver_method_name below validates
            // ("Owner.Owner_member"). Without the check, a *different*
            // class's monomorphized instantiation is mistaken for a clone
            // of `callee_name` whenever its own mangled type argument
            // happens to end with it: `std::shared_ptr<Bar>`'s own
            // constructor is named `std::shared_ptr.Bar_new`, which ends
            // with ".Bar_new" and so used to satisfy a plain `new Bar()`
            // looking for `Bar_new` -- silently running shared_ptr's
            // constructor over a Bar-sized allocation.
            return fn.name.compare(0, separator, fn.member_owner_class) == 0;
        };
        auto matches_receiver_method_name = [&](const Function& fn) {
            if (!receiver_expr || param_offset != 1) return true;
            if (fn.name == callee_name) return true;
            if (is_dotted_clone(fn)) {
                std::size_t sep = callee_name.rfind('_');
                if (sep == std::string::npos) return false;
                std::string owner = callee_name.substr(0, sep);
                if (fn.member_owner_class.empty() || fn.member_owner_class != owner) return false;
                std::string member = callee_name.substr(sep + 1);
                return fn.name == owner + "." + callee_name || fn.name == owner + "." + member;
            }
            std::size_t sep = callee_name.rfind('_');
            return sep != std::string::npos && !fn.member_owner_class.empty() &&
                   fn.member_owner_class == callee_name.substr(0, sep);
        };
        auto is_concrete_receiver_helper = [&](const Function& fn) {
            if (!receiver_expr || param_offset != 1) return false;
            if (fn.member_owner_class.empty()) return false;
            if (fn.name == callee_name || is_dotted_clone(fn)) return false;
            std::size_t sep = callee_name.rfind('_');
            if (sep == std::string::npos) return false;
            std::string_view owner = std::string_view(callee_name).substr(0, sep);
            std::string_view member = std::string_view(callee_name).substr(sep + 1);
            return fn.member_owner_class == owner && fn.name == std::string(member) &&
                   owner.find('.') != std::string::npos;
        };
        std::vector<const Function*> candidates;
        for (const Function& fn : program_->functions) {
            bool name_eq = fn.name == callee_name;
            bool concrete_helper = is_concrete_receiver_helper(fn);
            bool ctor_clone = is_constructor_clone(fn);
            bool dotted = is_dotted_clone(fn);
            bool receiver_ok = matches_receiver_method_name(fn);
            if ((name_eq || concrete_helper || ctor_clone || dotted) &&
                receiver_ok) {
                candidates.push_back(&fn);
            }
        }
        return candidates;
    }


    // The single viability test for one candidate against one call --
    // used both to *resolve* (any non-None reason means "not this one")
    // and to *explain* (the reason itself is what the diagnostic
    // reports). Previously the single-candidate and multi-candidate arms
    // of resolve_overload_by_type each ran their own copy of these four
    // checks, in different orders and with slightly different guards.
    CallCandidateRejection Codegen::classify_call_candidate(const Function& fn, const std::vector<ExprPtr>& args,
                                                            std::size_t param_offset, bool receiver_is_mutable,
                                                            const Expr* receiver_expr)
{
        if (param_offset == 1 && receiver_expr != nullptr) {
            if (!fn.params.empty() && fn.params[0].type.kind == TypeKind::Reference &&
                fn.params[0].type.is_mutable_ref && !receiver_is_mutable) {
                return CallCandidateRejection{CallRejectionReason::ReceiverIsReadOnly, 0, Type{}};
            }
            if (!receiver_matches_method_qualifier(*receiver_expr, fn)) {
                return CallCandidateRejection{CallRejectionReason::ReceiverRefQualifier, 0, Type{}};
            }
        }
        if (!function_accepts_argument_count(fn, args.size(), param_offset)) {
            return CallCandidateRejection{CallRejectionReason::ArgumentCount, 0, Type{}};
        }
        std::size_t fixed_param_count = fn.params.size() - param_offset;
        for (std::size_t i = 0; i < args.size() && i < fixed_param_count; i++) {
            Type candidate_param_type = normalized_param_type(*args[i], fn.params[i + param_offset].type);
            if (rvalue_ref_collapses_to_value(fn, i + param_offset, *args[i], param_offset)) {
                candidate_param_type = *candidate_param_type.pointee;
            }
            if (!argument_matches_parameter(*args[i], candidate_param_type) &&
                !literal_matches_scalar_parameter(*args[i], candidate_param_type)) {
                return CallCandidateRejection{CallRejectionReason::ArgumentType, i, candidate_param_type};
            }
        }
        return CallCandidateRejection{CallRejectionReason::None, 0, Type{}};
    }


    // A call named the way the user wrote it. codegen mangles a method
    // call to `Class_method` for lookup, but that spelling appears
    // nowhere in the source, so it must never reach a diagnostic.
    std::string Codegen::call_display_name(const Expr& expr, const std::string& receiver_class)
{
        if (!receiver_class.empty()) return receiver_class + "::" + expr.name;
        if (!expr.name.empty()) return expr.name;
        if (expr.lhs != nullptr && expr.lhs->kind == ExprKind::Identifier) return expr.lhs->name;
        return "<function pointer>";
    }


    // Turns a failed resolve_overload_by_type into a message that names
    // the actual problem.
    //
    // Every one of these failures used to be reported as "call to unknown
    // function 'X' (resolve)" -- a name-lookup message for a question that
    // is not about name lookup at all. It sent the reader looking for a
    // missing declaration when the declaration was right there and only
    // an argument type differed, and for a method it printed codegen's own
    // mangled `Class_method` spelling, a name that appears nowhere in the
    // user's source. (Same defect shape as the ch06 cast diagnostic fixed
    // earlier: a message that describes a different problem than the one
    // that occurred is worse than no message, because it is actively
    // misleading.)
    //
    // Note this path only runs for calls the *frontend* let through.
    // movecheck's resolve_overload deliberately accepts a single-candidate
    // name without matching argument types at all -- documented there,
    // because infer_expr_type cannot type Member/Subscript chains and
    // requiring a match would reject valid calls -- and explicitly defers
    // to "codegen's own type checking". That deferral is sound; codegen
    // does the checking. It was only ever the *report* that was wrong.
    std::string Codegen::describe_call_resolution_failure(const std::string& callee_name,
                                                          const std::string& display_name,
                                                          const std::vector<ExprPtr>& args, std::size_t param_offset,
                                                          bool receiver_is_mutable, const Expr* receiver_expr)
{
        std::vector<const Function*> candidates = collect_call_candidates(callee_name, param_offset, receiver_expr);
        if (candidates.empty()) {
            return "call to unknown function '" + display_name + "': no function with that name is declared here";
        }

        auto describe_signature = [&](const Function& fn) {
            std::string result = display_name + "(";
            for (std::size_t i = param_offset; i < fn.params.size(); i++) {
                if (i != param_offset) result += ", ";
                result += describe_type_brief(fn.params[i].type);
            }
            result += ")";
            if (fn.is_generic_template) result += " [generic]";
            return result;
        };
        auto candidate_list = [&]() {
            std::string result;
            for (const Function* fn : candidates) {
                result += "\n  candidate: " + describe_signature(*fn);
            }
            return result;
        };

        // Report the most specific cause available: a candidate rejected
        // purely on an argument type says more than one rejected on
        // arity, which in turn says more than a receiver-shape mismatch.
        const Function* type_mismatch_fn = nullptr;
        CallCandidateRejection type_mismatch{CallRejectionReason::None, 0, Type{}};
        bool any_read_only_receiver = false;
        bool any_ref_qualifier = false;
        for (const Function* fn : candidates) {
            CallCandidateRejection rejection =
                classify_call_candidate(*fn, args, param_offset, receiver_is_mutable, receiver_expr);
            switch (rejection.reason) {
                case CallRejectionReason::ArgumentType:
                    if (type_mismatch_fn == nullptr) {
                        type_mismatch_fn = fn;
                        type_mismatch = rejection;
                    }
                    break;
                case CallRejectionReason::ReceiverIsReadOnly: any_read_only_receiver = true; break;
                case CallRejectionReason::ReceiverRefQualifier: any_ref_qualifier = true; break;
                default: break;
            }
        }

        if (type_mismatch_fn != nullptr) {
            std::optional<Type> actual = infer_type(*args[type_mismatch.argument_index]);
            std::string actual_text =
                actual.has_value() ? "'" + describe_type_brief(*actual) + "'" : "a different type";
            return "no overload of '" + display_name + "' matches these argument types: argument " +
                   std::to_string(type_mismatch.argument_index + 1) + " is " + actual_text + ", but '" +
                   describe_signature(*type_mismatch_fn) + "' expects '" +
                   describe_type_brief(type_mismatch.expected_param_type) +
                   "' (spec ch05.10 -- overload resolution is exact type match only; an explicit "
                   "static_cast<T> may be required)" +
                   (candidates.size() > 1 ? candidate_list() : std::string());
        }
        if (any_read_only_receiver) {
            return "cannot call non-const member function '" + display_name +
                   "' through a read-only (const) receiver";
        }
        if (any_ref_qualifier) {
            return "no overload of '" + display_name +
                   "' accepts this receiver: its '&'/'&&' ref-qualifier does not match the receiver "
                   "expression (spec ch05.9)" +
                   candidate_list();
        }
        return "no overload of '" + display_name + "' takes " + std::to_string(args.size()) +
               (args.size() == 1 ? " argument" : " arguments") + candidate_list();
    }


    const Function* Codegen::resolve_overload_by_type(const std::string& callee_name, const std::vector<ExprPtr>& args,
                                              std::size_t param_offset, bool receiver_is_mutable ,
                                              const Expr* receiver_expr)
{
        auto scalar_exact_match_score = [&, this](const Function* fn) {
            int score = 0;
            std::size_t fixed_param_count = fn->params.size() - param_offset;
            for (std::size_t i = 0; i < args.size() && i < fixed_param_count; i++) {
                std::optional<Type> arg_type = infer_type(*args[i]);
                if (!arg_type.has_value()) continue;
                Type candidate_param_type = normalized_param_type(*args[i], fn->params[i + param_offset].type);
                if (candidate_param_type.kind == TypeKind::Reference) continue;
                if (types_equal(*arg_type, candidate_param_type)) score += 4;
                else if (literal_matches_scalar_parameter(*args[i], candidate_param_type)) score += 1;
            }
            return score;
        };
        std::vector<const Function*> candidates = collect_call_candidates(callee_name, param_offset, receiver_expr);
        if (candidates.empty()) return nullptr;
        if (candidates.size() == 1 && !candidates[0]->is_generic_template) {
            return classify_call_candidate(*candidates[0], args, param_offset, receiver_is_mutable, receiver_expr).reason ==
                           CallRejectionReason::None
                       ? candidates[0]
                       : nullptr;
        }

        std::vector<const Function*> matches;
        for (const Function* fn : candidates) {
            if (classify_call_candidate(*fn, args, param_offset, receiver_is_mutable, receiver_expr).reason ==
                CallRejectionReason::None) {
                matches.push_back(fn);
            }
        }
        if (matches.empty()) return nullptr;
        if (matches.size() == 1) return matches[0];
        auto exact_non_reference_match = [&, this](const Function* fn) {
            std::size_t fixed_param_count = fn->params.size() - param_offset;
            for (std::size_t i = 0; i < args.size() && i < fixed_param_count; i++) {
                std::optional<Type> arg_type = infer_type(*args[i]);
                if (!arg_type.has_value()) return false;
                Type candidate_param_type = normalized_param_type(*args[i], fn->params[i + param_offset].type);
                if (!types_equal(*arg_type, candidate_param_type)) return false;
            }
            return true;
        };
        if (callee_name.ends_with("_new")) {
            for (const Function* fn : matches) {
                if (!fn->is_generic_template && exact_non_reference_match(fn) && fn->name.starts_with(callee_name + ".")) {
                    return fn;
                }
            }
            for (const Function* fn : matches) {
                if (!fn->is_generic_template && fn->name.starts_with(callee_name + ".")) return fn;
            }
            for (const Function* fn : matches) {
                if (!fn->is_generic_template) return fn;
            }
        }
        const Function* best_exact = nullptr;
        int best_exact_score = -1;
        bool unique_exact = true;
        for (const Function* fn : matches) {
            int score = exact_non_reference_match(fn) ? 1000 + scalar_exact_match_score(fn) : scalar_exact_match_score(fn);
            if (score > best_exact_score) {
                best_exact = fn;
                best_exact_score = score;
                unique_exact = true;
            } else if (score == best_exact_score) {
                unique_exact = false;
            }
        }
        if (best_exact != nullptr && unique_exact && best_exact_score > 0) {
            bool exact_path_has_reference_axis_difference = false;
            for (std::size_t i = 0; i < matches.size() && !exact_path_has_reference_axis_difference; i++) {
                for (std::size_t j = i + 1; j < matches.size() && !exact_path_has_reference_axis_difference; j++) {
                    std::size_t fixed_param_count =
                        std::min(matches[i]->params.size(), matches[j]->params.size()) - param_offset;
                    for (std::size_t k = 0; k < args.size() && k < fixed_param_count; k++) {
                        bool lhs_ref = matches[i]->params[k + param_offset].type.kind == TypeKind::Reference;
                        bool rhs_ref = matches[j]->params[k + param_offset].type.kind == TypeKind::Reference;
                        if (lhs_ref != rhs_ref) {
                            exact_path_has_reference_axis_difference = true;
                            break;
                        }
                    }
                }
            }
            if (!exact_path_has_reference_axis_difference) return best_exact;
        }
        // Tie-break ("T& beats const T& for a mutable lvalue", ch05
        // §5.10): prefer whichever match has the most mutable-reference
        // parameters (including `this`) among positions where the
        // argument/receiver is itself a mutable place. Falls back to the
        // first match if that still doesn't produce a unique winner.
        auto mutable_ref_score = [&, this](const Function* fn) {
            int score = 0;
            if (param_offset == 1 && fn->params[0].type.is_mutable_ref && receiver_is_mutable) score++;
            if (param_offset == 1 && receiver_expr != nullptr && fn->params[0].type.pointee != nullptr) {
                Type receiver_expected = *fn->params[0].type.pointee;
                receiver_expected.is_const_qualified = false;
                bool receiver_is_rvalue = produces_rvalue_of_type(*receiver_expr, receiver_expected);
                if ((receiver_is_rvalue && fn->receiver_ref_qualifier == ReceiverRefQualifier::RValue) ||
                    (!receiver_is_rvalue && fn->receiver_ref_qualifier == ReceiverRefQualifier::LValue)) {
                    score += 2;
                }
            }
            std::size_t fixed_param_count = fn->params.size() - param_offset;
            for (std::size_t i = 0; i < args.size() && i < fixed_param_count; i++) {
                const Type& param_type = fn->params[i + param_offset].type;
                if (param_type.kind == TypeKind::Reference && param_type.is_mutable_ref && !is_read_only_place(*args[i])) {
                    score++;
                }
            }
            return score;
        };
        auto value_vs_reference_axis_score = [&, this](const Function* fn) {
            int score = 0;
            std::size_t fixed_param_count = fn->params.size() - param_offset;
            for (std::size_t i = 0; i < args.size() && i < fixed_param_count; i++) {
                const Type& param_type = fn->params[i + param_offset].type;
                const LocalSlot* arg_local = find_local(*args[i]);
                bool arg_is_bare_name = args[i]->kind == ExprKind::Identifier &&
                                        !args[i]->explicit_global_qualification && arg_local == nullptr;
                bool arg_is_local_rvalue_ref =
                    arg_local != nullptr && arg_local->type.kind == TypeKind::Reference && arg_local->type.is_rvalue_ref;
                bool arg_is_lvalue_like = args[i]->kind == ExprKind::Identifier || args[i]->kind == ExprKind::Member ||
                                          args[i]->kind == ExprKind::Subscript;
                bool arg_is_prvalue_like = args[i]->kind == ExprKind::Move || args[i]->kind == ExprKind::New ||
                                           args[i]->kind == ExprKind::IntegerLiteral || args[i]->kind == ExprKind::FloatLiteral ||
                                           args[i]->kind == ExprKind::BoolLiteral || args[i]->kind == ExprKind::CharLiteral ||
                                           args[i]->kind == ExprKind::StringLiteral;
                if (arg_is_bare_name) continue;
                if (arg_is_local_rvalue_ref) continue;
                if (arg_is_lvalue_like) {
                    if (param_type.kind == TypeKind::Reference) score += 4;
                    else score -= 4;
                } else if (arg_is_prvalue_like) {
                    if (param_type.kind != TypeKind::Reference) score += 4;
                    else score -= 4;
                }
            }
            return score;
        };
        bool has_reference_axis_difference = false;
        for (std::size_t i = 0; i < matches.size() && !has_reference_axis_difference; i++) {
            for (std::size_t j = i + 1; j < matches.size() && !has_reference_axis_difference; j++) {
                std::size_t fixed_param_count = std::min(matches[i]->params.size(), matches[j]->params.size()) - param_offset;
                for (std::size_t k = 0; k < args.size() && k < fixed_param_count; k++) {
                    bool lhs_ref = matches[i]->params[k + param_offset].type.kind == TypeKind::Reference;
                    bool rhs_ref = matches[j]->params[k + param_offset].type.kind == TypeKind::Reference;
                    if (lhs_ref != rhs_ref) {
                        has_reference_axis_difference = true;
                        break;
                    }
                }
            }
        }
        const Function* best = matches[0];
        int best_score = (has_reference_axis_difference ? value_vs_reference_axis_score(best) * 100 : 0) +
                         mutable_ref_score(best);
        bool unique_best = true;
        for (std::size_t i = 1; i < matches.size(); i++) {
            int score = (has_reference_axis_difference ? value_vs_reference_axis_score(matches[i]) * 100 : 0) +
                        mutable_ref_score(matches[i]);
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


    const Function* Codegen::resolve_constructor_overload_exact(const std::string& class_name, const std::vector<ExprPtr>& args)
{
        auto is_constructor_clone = [&](const Function& fn) {
            return fn.name == class_name + "_new" ||
                   (!fn.member_owner_class.empty() && fn.member_owner_class == class_name &&
                    fn.name.starts_with(class_name + "_new."));
        };
        std::vector<const Function*> matches;
        for (const Function& fn : program_->functions) {
            if (!is_constructor_clone(fn)) continue;
            if (!function_accepts_argument_count(fn, args.size(), 1)) continue;
            bool all_match = true;
            for (std::size_t i = 0; all_match && i < args.size(); i++) {
                all_match = argument_matches_parameter(*args[i], fn.params[i + 1].type);
            }
            if (all_match) matches.push_back(&fn);
        }
        if (matches.empty()) return nullptr;
        auto exact_type_match = [&, this](const Function* fn) {
            for (std::size_t i = 0; i < args.size(); i++) {
                std::optional<Type> inferred = infer_type(*args[i]);
                if (!inferred.has_value()) return false;
                if (!types_equal(*inferred, fn->params[i + 1].type)) return false;
            }
            return true;
        };
        for (const Function* fn : matches) {
            if (exact_type_match(fn)) return fn;
        }
        if (matches.size() == 1) return matches[0];
        auto mutable_ref_score = [&, this](const Function* fn) {
            int score = 0;
            for (std::size_t i = 0; i < args.size(); i++) {
                const Type& param_type = fn->params[i + 1].type;
                if (param_type.kind == TypeKind::Reference && param_type.is_mutable_ref && !is_read_only_place(*args[i])) {
                    score++;
                }
            }
            return score;
        };
        const Function* best = matches[0];
        int best_score = mutable_ref_score(best);
        bool unique_best = true;
        for (std::size_t i = 1; i < matches.size(); i++) {
            int score = mutable_ref_score(matches[i]);
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


    [[nodiscard]] const Type& Codegen::binary_operand_type(const Type& type)
{
        return type.kind == TypeKind::Reference ? *type.pointee : type;
    }

    [[nodiscard]] const Expr& Codegen::unwrap_negated_numeric_literal(const Expr& expr)
{
        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Neg && expr.lhs != nullptr &&
            (expr.lhs->kind == ExprKind::IntegerLiteral || expr.lhs->kind == ExprKind::FloatLiteral)) {
            return *expr.lhs;
        }
        return expr;
    }


    [[nodiscard]] bool Codegen::is_pointer_arithmetic_offset_type(const Type& type)
{
        return type.kind == TypeKind::Named && type.name != "bool" && is_integral_scalar_type_name(type.name);
    }


    [[nodiscard]] bool Codegen::pointer_supports_arithmetic(const Type& type) const
{
        return type.kind == TypeKind::Pointer && type.pointee != nullptr && !is_interface_pointer_type(type) &&
               !(type.pointee->kind == TypeKind::Named && type.pointee->name == "void");
    }


    [[nodiscard]] std::optional<Type> Codegen::pointer_arithmetic_result_type(BinaryOp op, const Type& lhs, const Type& rhs) const
{
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

} // namespace scpp
