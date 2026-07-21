module;

module scpp.compiler.codegen:semantics;

import std;
import :api;

namespace scpp {

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


    ExprPtr Codegen::clone_expr(const Expr& expr) const
{
        auto clone = std::make_unique<Expr>();
        clone->kind = expr.kind;
        clone->loc = expr.loc;
        clone->int_value = expr.int_value;
        clone->float_value = expr.float_value;
        clone->bool_value = expr.bool_value;
        clone->name = expr.name;
        clone->explicit_global_qualification = expr.explicit_global_qualification;
        clone->binary_op = expr.binary_op;
        clone->unary_op = expr.unary_op;
        clone->fold_ellipsis_on_left = expr.fold_ellipsis_on_left;
        if (expr.lhs) clone->lhs = clone_expr(*expr.lhs);
        if (expr.rhs) clone->rhs = clone_expr(*expr.rhs);
        if (expr.third) clone->third = clone_expr(*expr.third);
        for (const ExprPtr& arg : expr.args) clone->args.push_back(clone_expr(*arg));
        clone->explicit_template_args.reserve(expr.explicit_template_args.size());
        for (const ExplicitTemplateArg& arg : expr.explicit_template_args) {
            ExplicitTemplateArg cloned_arg;
            cloned_arg.is_type = arg.is_type;
            cloned_arg.type = arg.type;
            if (arg.value) cloned_arg.value = std::shared_ptr<Expr>(clone_expr(*arg.value).release());
            clone->explicit_template_args.push_back(std::move(cloned_arg));
        }
        clone->type = expr.type;
        clone->has_paren_init = expr.has_paren_init;
        clone->destroy_through_pointer = expr.destroy_through_pointer;
        clone->through_arrow = expr.through_arrow;
        clone->implicit_arrow_deref = expr.implicit_arrow_deref;
        clone->implicit_arrow_chain_safe = expr.implicit_arrow_chain_safe;
        return clone;
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
            case ExprKind::Alignof:
            case ExprKind::Sizeof:
                return named_type("size_t");
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
                auto it = expr.explicit_global_qualification ? locals_.end() : locals_.find(expr.name);
                if (it != locals_.end()) return it->second.type;
                if (const GlobalSlot* global = find_visible_global_slot(expr.name, expr.explicit_global_qualification)) {
                    return global->type;
                }
                if (const EnumDef* def = [&]() {
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
                auto it = locals_.find(expr.lhs->name);
                if (it == locals_.end()) {
                    if (const GlobalSlot* global =
                            find_visible_global_slot(expr.lhs->name, expr.lhs->explicit_global_qualification)) {
                        return global->type;
                    }
                }
                return it == locals_.end() ? std::nullopt : std::optional<Type>(it->second.type);
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
                const Type& base_named = base->kind == TypeKind::Reference ? *base->pointee : *base;
                if (base_named.kind != TypeKind::Named) return std::nullopt;
                auto struct_it = structs_.find(base_named.name);
                if (struct_it == structs_.end()) return std::nullopt;
                const StructInfo& info = struct_it->second;
                std::optional<std::size_t> field_index = info.find_field_index(expr.name);
                if (!field_index.has_value()) return std::nullopt;
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
                return std::nullopt;
            }

            case ExprKind::Unary:
                switch (expr.unary_op) {
                    case UnaryOp::Not: return named_type("bool");
                    case UnaryOp::Neg: return infer_type(*expr.lhs);
                    case UnaryOp::AddressOf: {
                        if (std::optional<Type> fn_ptr = resolve_function_designator_type(expr)) return fn_ptr;
                        std::optional<Type> operand = infer_type(*expr.lhs);
                        if (!operand) return std::nullopt;
                        Type result;
                        result.kind = TypeKind::Pointer;
                        result.pointee = std::make_shared<Type>(std::move(*operand));
                        result.is_mutable_pointee = true; // &expr always yields a mutable T* (ch05 §5.7)
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
                        if (std::optional<Type> lhs = infer_type(*expr.lhs), rhs = infer_type(*expr.rhs);
                            lhs.has_value() && rhs.has_value()) {
                            if (std::optional<Type> result = pointer_arithmetic_result_type(expr.binary_op, *lhs, *rhs)) {
                                return result;
                            }
                        }
                        [[fallthrough]];
                    case BinaryOp::Sub:
                        if (expr.binary_op == BinaryOp::Sub) {
                            if (std::optional<Type> lhs = infer_type(*expr.lhs), rhs = infer_type(*expr.rhs);
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
                        return infer_type(*expr.lhs);
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
                if (expr.lhs == nullptr && !expr.explicit_global_qualification && locals_.contains(expr.name) &&
                    locals_.at(expr.name).type.kind == TypeKind::FunctionPointer) {
                    return *locals_.at(expr.name).type.function_return;
                }
                std::string callee_name = expr.name;
                std::size_t param_offset = 0;
                bool receiver_is_mutable = true;
                if (expr.lhs != nullptr) {
                    std::optional<Type> receiver = infer_type(*expr.lhs);
                    if (!receiver) return std::nullopt;
                    const Type& receiver_named =
                        receiver->kind == TypeKind::Reference ? *receiver->pointee : *receiver;
                    if (receiver_named.kind != TypeKind::Named) return std::nullopt;
                    callee_name = receiver_named.name + "_" + expr.name;
                    param_offset = 1;
                    receiver_is_mutable = !is_read_only_place(*expr.lhs);
                }
                const Function* callee =
                    resolve_overload_by_type(callee_name, expr.args, param_offset, receiver_is_mutable, expr.lhs.get());
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
            default:
                return false;
        }
    }


    [[nodiscard]] bool Codegen::is_bare_same_type_copy_source(const Expr& expr, const Type& target_type)
{
        if (!is_lvalue_copy_source_shape(expr)) return false;
        std::optional<Type> expr_type = infer_type(expr);
        if (!expr_type.has_value()) return false;
        if (types_equal(*expr_type, target_type)) return true;
        return expr_type->kind == TypeKind::Reference && !expr_type->is_rvalue_ref &&
               expr_type->pointee != nullptr && types_equal(*expr_type->pointee, target_type);
    }


    [[nodiscard]] bool Codegen::is_implicit_move_return_source(const Expr& expr, const Type& target_type)
{
        if (expr.kind != ExprKind::Identifier || expr.explicit_global_qualification) return false;
        auto it = locals_.find(expr.name);
        return it != locals_.end() && types_equal(it->second.type, target_type);
    }


    const Function* Codegen::find_single_argument_converting_constructor(const std::string& class_name, const Expr& arg)
{
        std::vector<const Function*> matches;
        for (const Function& fn : program_->functions) {
            if (fn.name != class_name + "_new" || fn.params.size() != 2) continue;
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
        if (candidate_param_type.kind == TypeKind::Reference) {
            if (arg_type.kind == TypeKind::Reference) {
                if (arg_type.pointee == nullptr || candidate_param_type.pointee == nullptr) return false;
                return types_equal(*arg_type.pointee, *candidate_param_type.pointee) &&
                       (!candidate_param_type.is_mutable_ref || arg_type.is_mutable_ref);
            }
            return candidate_param_type.pointee != nullptr && types_equal(arg_type, *candidate_param_type.pointee);
        }
        if (arg_type.kind == TypeKind::Reference) {
            return arg_type.pointee != nullptr && types_equal(*arg_type.pointee, candidate_param_type);
        }
        return types_equal(arg_type, candidate_param_type);
    }


    bool Codegen::argument_matches_parameter(const Expr& arg, const Type& param_type)
{
        auto argument_type_matches_or_converts = [&](const Type& arg_type, const Type& candidate_param_type) {
            return argument_type_matches_parameter(arg_type, candidate_param_type) ||
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
        std::optional<Type> arg_type = infer_type(arg);
        if (!arg_type.has_value()) return false;
        if (!argument_type_matches_or_converts(*arg_type, param_type)) {
            if (is_named_record_type(param_type) &&
                find_single_argument_converting_constructor(param_type.name, arg) != nullptr) {
                return true;
            }
            return false;
        }
        if (is_named_record_type(param_type)) {
            return (is_bare_same_type_copy_source(arg, param_type) && is_copy_constructible(param_type.name)) ||
                   produces_rvalue_of_type(arg, param_type);
        }
        return true;
    }


    bool Codegen::constructor_parameter_accepts_argument_directly(const Expr& arg, const Type& param_type)
{
        if (param_type.kind == TypeKind::Reference && param_type.is_rvalue_ref) {
            return produces_rvalue_of_type(arg, *param_type.pointee);
        }
        if (param_type.kind == TypeKind::Reference) {
            if (!param_type.is_mutable_ref && param_type.pointee != nullptr &&
                produces_rvalue_of_type(arg, *param_type.pointee)) {
                return true;
            }
            if (arg.kind == ExprKind::Move || arg.kind == ExprKind::New ||
                arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::FloatLiteral ||
                arg.kind == ExprKind::BoolLiteral ||
                arg.kind == ExprKind::CharLiteral || arg.kind == ExprKind::StringLiteral) {
                return false;
            }
            std::optional<Type> arg_type = infer_type(arg);
            return arg_type.has_value() && argument_type_matches_parameter(*arg_type, param_type);
        }
        std::optional<Type> arg_type = infer_type(arg);
        if (!arg_type.has_value() || !argument_type_matches_parameter(*arg_type, param_type)) return false;
        if (is_named_record_type(param_type)) {
            return (is_bare_same_type_copy_source(arg, param_type) && is_copy_constructible(param_type.name)) ||
                   produces_rvalue_of_type(arg, param_type);
        }
        return true;
    }


    bool Codegen::is_read_only_place(const Expr& expr)
{
        switch (expr.kind) {
            case ExprKind::Identifier: {
                auto it = locals_.find(expr.name);
                if (it == locals_.end()) return false;
                return it->second.is_const || (it->second.type.kind == TypeKind::Reference && !it->second.type.is_mutable_ref);
            }
            case ExprKind::Member:
            case ExprKind::Subscript:
                return is_read_only_place(*expr.lhs);
            case ExprKind::Unary:
                if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) return false;
                {
                    auto it = locals_.find(expr.lhs->name);
                    return it != locals_.end() && it->second.type.kind == TypeKind::Pointer &&
                           !it->second.type.is_mutable_pointee;
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
        bool receiver_is_rvalue = produces_rvalue_of_type(receiver_expr, *fn.params[0].type.pointee);
        switch (fn.receiver_ref_qualifier) {
            case ReceiverRefQualifier::None: return true;
            case ReceiverRefQualifier::LValue: return !receiver_is_rvalue;
            case ReceiverRefQualifier::RValue: return receiver_is_rvalue;
        }
        return true;
    }


    const Function* Codegen::resolve_overload_by_type(const std::string& callee_name, const std::vector<ExprPtr>& args,
                                              std::size_t param_offset, bool receiver_is_mutable ,
                                              const Expr* receiver_expr)
{
        std::vector<const Function*> candidates;
        for (const Function& fn : program_->functions) {
            if (fn.name == callee_name) candidates.push_back(&fn);
        }
        if (candidates.empty()) return nullptr;
        if (candidates.size() == 1) {
            if (param_offset == 1 && receiver_expr != nullptr) {
                if (candidates[0]->params.size() > 0 && candidates[0]->params[0].type.kind == TypeKind::Reference &&
                    candidates[0]->params[0].type.is_mutable_ref && !receiver_is_mutable) {
                    return nullptr;
                }
                if (!receiver_matches_method_qualifier(*receiver_expr, *candidates[0])) return nullptr;
            }
            return candidates[0];
        }

        std::vector<const Function*> matches;
        for (const Function* fn : candidates) {
            if (fn->params.size() != args.size() + param_offset) continue;
            // The receiver (`this`): viable only if the candidate's own
            // `this` mutability doesn't demand more than the receiver
            // place can actually provide.
            if (param_offset == 1 && fn->params[0].type.is_mutable_ref && !receiver_is_mutable) continue;
            if (param_offset == 1 && receiver_expr != nullptr &&
                !receiver_matches_method_qualifier(*receiver_expr, *fn)) {
                continue;
            }
            bool all_match = true;
            for (std::size_t i = 0; all_match && i < args.size(); i++) {
                all_match = argument_matches_parameter(*args[i], fn->params[i + param_offset].type);
            }
            if (all_match) matches.push_back(fn);
        }
        if (matches.empty()) return nullptr;
        if (matches.size() == 1) return matches[0];

        // Tie-break ("T& beats const T& for a mutable lvalue", ch05
        // §5.10): prefer whichever match has the most mutable-reference
        // parameters (including `this`) among positions where the
        // argument/receiver is itself a mutable place. Falls back to the
        // first match if that still doesn't produce a unique winner.
        auto mutable_ref_score = [&](const Function* fn) {
            int score = 0;
            if (param_offset == 1 && fn->params[0].type.is_mutable_ref && receiver_is_mutable) score++;
            if (param_offset == 1 && receiver_expr != nullptr && fn->params[0].type.pointee != nullptr) {
                bool receiver_is_rvalue = produces_rvalue_of_type(*receiver_expr, *fn->params[0].type.pointee);
                if ((receiver_is_rvalue && fn->receiver_ref_qualifier == ReceiverRefQualifier::RValue) ||
                    (!receiver_is_rvalue && fn->receiver_ref_qualifier == ReceiverRefQualifier::LValue)) {
                    score += 2;
                }
            }
            for (std::size_t i = 0; i < args.size(); i++) {
                const Type& param_type = fn->params[i + param_offset].type;
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


    const Function* Codegen::resolve_constructor_overload_exact(const std::string& class_name, const std::vector<ExprPtr>& args)
{
        std::vector<const Function*> matches;
        for (const Function& fn : program_->functions) {
            if (fn.name != class_name + "_new") continue;
            if (fn.params.size() != args.size() + 1) continue;
            bool all_match = true;
            for (std::size_t i = 0; all_match && i < args.size(); i++) {
                all_match = argument_matches_parameter(*args[i], fn.params[i + 1].type);
            }
            if (all_match) matches.push_back(&fn);
        }
        if (matches.empty()) return nullptr;
        if (matches.size() == 1) return matches[0];
        auto mutable_ref_score = [&](const Function* fn) {
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


    [[nodiscard]] bool Codegen::types_equal(const Type& a, const Type& b)
{
        if (a.kind != b.kind) return false;
        switch (a.kind) {
            case TypeKind::Named:
                if (a.name != b.name || a.template_args.size() != b.template_args.size()) return false;
                for (std::size_t i = 0; i < a.template_args.size(); i++) {
                    if (!types_equal(a.template_args[i], b.template_args[i])) return false;
                }
                return true;
            case TypeKind::Pointer: return a.is_mutable_pointee == b.is_mutable_pointee && types_equal(*a.pointee, *b.pointee);
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
            case TypeKind::Array: return a.array_size == b.array_size && types_equal(*a.element, *b.element);
        }
        return false;
    }


    [[nodiscard]] const Type& Codegen::binary_operand_type(const Type& type)
{
        return type.kind == TypeKind::Reference ? *type.pointee : type;
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
