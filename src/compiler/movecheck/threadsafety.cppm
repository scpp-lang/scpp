module;

module scpp.compiler.movecheck:threadsafety;

import std;
import scpp.ast;
import :errors;
import :types;
import :signatures;
import :calls;

namespace scpp {

void collect_interfaces_for_thread_safety(const Program& program, const std::string& class_name,
                                          std::unordered_set<std::string>& out) {
    const ClassDef* def = find_class_def(program, class_name);
    if (def == nullptr) return;
    for (const BaseSpecifier& base : def->base_specifiers) {
        const ClassDef* base_def = find_class_def(program, base.base_type.name);
        if (base_def == nullptr) continue;
        if (base_def->is_interface) out.insert(base_def->name);
        collect_interfaces_for_thread_safety(program, base.base_type.name, out);
    }
}

[[nodiscard]] std::expected<bool, DataflowError> evaluate_thread_bool_constant_expr_for_program(const Expr& expr, const Program& program,
                                                                  std::unordered_set<std::string> visiting = {});
[[nodiscard]] std::expected<bool, DataflowError> thread_movable_of(const Type& type, const Program& program,
                                     std::unordered_set<std::string> visiting = {});
[[nodiscard]] std::expected<bool, DataflowError> thread_shareable_of(const Type& type, const Program& program,
                                       std::unordered_set<std::string> visiting = {});
[[nodiscard]] bool parameter_requires_thread_safety_constraint(const FunctionSignature& sig, std::size_t param_index);
[[nodiscard]] std::string parameter_display_name(const FunctionSignature& sig, std::size_t param_index);
[[nodiscard]] bool parameter_names_interface_type(const Type& param_type, const Body& body);
[[nodiscard]] Type thread_safety_constraint_subject_type(const Expr& arg, const Type& param_type,
                                                         const Body& body, const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> enforce_thread_safety_constraints_for_argument(const Expr& arg, const FunctionSignature& sig,
                                                    std::size_t param_index, std::string_view callee_kind,
                                                    const std::string& callee_name, const Body& body,
                                                    const Signatures& signatures, SourceLocation loc);


[[nodiscard]] bool parameter_requires_thread_safety_constraint(const FunctionSignature& sig, std::size_t param_index) {
    return param_index < sig.param_require_thread_movable.size() &&
           (sig.param_require_thread_movable[param_index] || sig.param_require_thread_shareable[param_index]);
}

[[nodiscard]] std::string parameter_display_name(const FunctionSignature& sig, std::size_t param_index) {
    if (param_index < sig.param_names.size() && !sig.param_names[param_index].empty()) return sig.param_names[param_index];
    return "#" + std::to_string(param_index + 1);
}

[[nodiscard]] bool parameter_names_interface_type(const Type& param_type, const Body& body) {
    if (body.program == nullptr || param_type.pointee == nullptr || param_type.pointee->kind != TypeKind::Named) return false;
    const ClassDef* param_interface = find_class_def(*body.program, param_type.pointee->name);
    return param_interface != nullptr && param_interface->is_interface;
}

[[nodiscard]] std::optional<Type> concrete_interface_argument_type(const Expr& arg, const Type& param_type, const Body& body,
                                                                   const Signatures& signatures) {
    if (body.program == nullptr || !parameter_names_interface_type(param_type, body)) return std::nullopt;
    std::optional<Type> source_type = infer_expr_type(arg, body, signatures);
    if (!source_type.has_value()) return std::nullopt;
    const Type& source = *source_type;
    if (!argument_type_matches_parameter(source, param_type, body)) return std::nullopt;
    if (source.kind == TypeKind::Reference && source.pointee != nullptr) return *source.pointee;
    if (source.kind == TypeKind::Pointer && source.pointee != nullptr) return *source.pointee;
    if (source.kind == TypeKind::Named) return source;
    return std::nullopt;
}

[[nodiscard]] Type thread_safety_constraint_subject_type(const Expr& arg, const Type& param_type, const Body& body,
                                                         const Signatures& signatures) {
    if (param_type.kind == TypeKind::Named && !param_type.name.empty()) {
        std::optional<Type> source_type = infer_expr_type(arg, body, signatures);
        if (source_type.has_value()) return *source_type;
    }
    if (std::optional<Type> concrete = concrete_interface_argument_type(arg, param_type, body, signatures);
        concrete.has_value()) {
        return *concrete;
    }
    return param_type;
}

[[nodiscard]] std::expected<void, DataflowError> enforce_thread_safety_constraints_for_argument(const Expr& arg, const FunctionSignature& sig, std::size_t param_index,
                                                    std::string_view callee_kind, const std::string& callee_name,
                                                    const Body& body, const Signatures& signatures, SourceLocation loc) {
    if (body.program == nullptr || !parameter_requires_thread_safety_constraint(sig, param_index) ||
        param_index >= sig.param_types.size()) {
        return {};
    }
    Type subject = thread_safety_constraint_subject_type(arg, sig.param_types[param_index], body, signatures);
    if (subject.kind != TypeKind::Reference && sig.param_types[param_index].kind == TypeKind::Reference &&
        sig.param_types[param_index].pointee != nullptr) {
        Type wrapped = sig.param_types[param_index];
        wrapped.pointee = std::make_shared<Type>(subject);
        subject = std::move(wrapped);
    }
    std::string param_name = parameter_display_name(sig, param_index);
    if (sig.param_require_thread_movable[param_index]) {
        auto _r = thread_movable_of(subject, *body.program);
        if (!_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!_r.value()) {
            return std::unexpected(DataflowError("argument for parameter '" + param_name + "' of " + std::string(callee_kind) + " '" +
                                    callee_name + "' does not satisfy '[[scpp::thread_movable]]' (spec §8.1/§8.5(6))",
                loc));
        }
    }
    bool shareable_ok = true;
    if (sig.param_require_thread_shareable[param_index]) {
        auto _r = thread_shareable_of(subject, *body.program);
        if (!_r.has_value()) return std::unexpected(std::move(_r).error());
        shareable_ok = _r.value();
    }
    if (!shareable_ok) {
        if (std::optional<Type> concrete = concrete_interface_argument_type(arg, sig.param_types[param_index], body, signatures);
            concrete.has_value()) {
            auto _r = thread_shareable_of(*concrete, *body.program);
            if (!_r.has_value()) return std::unexpected(std::move(_r).error());
            shareable_ok = _r.value();
        }
    }
    if (!shareable_ok) {
        return std::unexpected(DataflowError("argument for parameter '" + param_name + "' of " + std::string(callee_kind) + " '" +
                                callee_name + "' does not satisfy '[[scpp::thread_shareable]]' (spec §8.1/§8.5(6))",
            loc));
    }
    return {};
}

[[nodiscard]] std::expected<bool, DataflowError> evaluate_thread_bool_constant_expr_for_program(const Expr& expr, const Program& program,
                                                                  std::unordered_set<std::string> visiting) {
    switch (expr.kind) {
        case ExprKind::BoolLiteral: return expr.bool_value;
        case ExprKind::TypeTrait:
            return expr.name == "is_thread_movable" ? thread_movable_of(expr.type, program, visiting)
                                                    : thread_shareable_of(expr.type, program, visiting);
        case ExprKind::Unary:
            if (expr.unary_op == UnaryOp::Not && expr.lhs) {
                auto _r = evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting);
                if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                return !_r.value();
            }
            break;
        case ExprKind::Binary:
            if (!expr.lhs || !expr.rhs) break;
            if (expr.binary_op == BinaryOp::And) {
                auto _lhs = evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting);
                if (!_lhs.has_value()) return std::unexpected(std::move(_lhs).error());
                if (!_lhs.value()) return false;
                return evaluate_thread_bool_constant_expr_for_program(*expr.rhs, program, visiting);
            }
            if (expr.binary_op == BinaryOp::Or) {
                auto _lhs = evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting);
                if (!_lhs.has_value()) return std::unexpected(std::move(_lhs).error());
                if (_lhs.value()) return true;
                return evaluate_thread_bool_constant_expr_for_program(*expr.rhs, program, visiting);
            }
            if (expr.binary_op == BinaryOp::Eq) {
                auto _lhs = evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting);
                if (!_lhs.has_value()) return std::unexpected(std::move(_lhs).error());
                auto _rhs = evaluate_thread_bool_constant_expr_for_program(*expr.rhs, program, visiting);
                if (!_rhs.has_value()) return std::unexpected(std::move(_rhs).error());
                return _lhs.value() == _rhs.value();
            }
            if (expr.binary_op == BinaryOp::Ne) {
                auto _lhs = evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting);
                if (!_lhs.has_value()) return std::unexpected(std::move(_lhs).error());
                auto _rhs = evaluate_thread_bool_constant_expr_for_program(*expr.rhs, program, visiting);
                if (!_rhs.has_value()) return std::unexpected(std::move(_rhs).error());
                return _lhs.value() != _rhs.value();
            }
            break;
        default: break;
    }
    return std::unexpected(DataflowError("thread-trait override expressions must be boolean constant expressions built from "
                        "bool literals, !, &&, ||, ==, !=, and scpp::is_thread_movable/shareable(T)",
                        expr.loc));
}

[[nodiscard]] std::expected<bool, DataflowError> thread_movable_of(const Type& type, const Program& program,
                                     std::unordered_set<std::string> visiting) {
    switch (type.kind) {
        case TypeKind::Named: {
            if (is_scalar_type_name(type.name)) return true;
            if (find_enum_def(&program, type.name) != nullptr) return true;
            if (visiting.contains(type.name)) return true;
            visiting.insert(type.name);
            for (const ClassDef& c : program.classes) {
                if (c.name != type.name) continue;
                if (c.thread_movable_override) return true;
                if (c.thread_movable_if_movable_expr) {
                    return evaluate_thread_bool_constant_expr_for_program(*c.thread_movable_if_movable_expr, program,
                                                                          visiting);
                }
                for (const ClassField& f : c.fields) {
                    auto _r = thread_movable_of(f.type, program, visiting);
                    if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                    if (!_r.value()) return false;
                }
                return true;
            }
            for (const StructDef& s : program.structs) {
                if (s.name != type.name) continue;
                if (s.thread_movable_override) return true;
                for (const StructField& f : s.fields) {
                    auto _r = thread_movable_of(f.type, program, visiting);
                    if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                    if (!_r.value()) return false;
                }
                return true;
            }
            return false;
        }
        case TypeKind::Pointer: return false;
        case TypeKind::Function:
        case TypeKind::FunctionPointer: return true;
        case TypeKind::Array:
            if (!type.element) return false;
            return thread_movable_of(*type.element, program, visiting);
        case TypeKind::Reference:
            if (!type.is_rvalue_ref) return false;
            if (!type.pointee) return false;
            return thread_movable_of(*type.pointee, program, visiting);
        case TypeKind::Span: return false;
    }
    return false;
}

[[nodiscard]] std::expected<bool, DataflowError> thread_shareable_of(const Type& type, const Program& program,
                                       std::unordered_set<std::string> visiting) {
    switch (type.kind) {
        case TypeKind::Named: {
            if (is_scalar_type_name(type.name)) return true;
            if (visiting.contains(type.name)) return true;
            visiting.insert(type.name);
            for (const ClassDef& c : program.classes) {
                if (c.name != type.name) continue;
                if (c.is_interface) return true;
                if (c.thread_shareable_override) return true;
                if (c.thread_movable_if_shareable_expr) {
                    return evaluate_thread_bool_constant_expr_for_program(*c.thread_movable_if_shareable_expr, program,
                                                                          visiting);
                }
                std::unordered_set<std::string> interfaces;
                collect_interfaces_for_thread_safety(program, c.name, interfaces);
                for (const std::string& interface_name : interfaces) {
                    const ClassDef* iface = find_class_def(program, interface_name);
                    if (iface != nullptr && iface->thread_shareable_override && c.fields.empty()) return true;
                }
                for (const ClassField& f : c.fields) {
                    auto _r = thread_shareable_of(f.type, program, visiting);
                    if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                    if (!_r.value()) return false;
                }
                return true;
            }
            for (const StructDef& s : program.structs) {
                if (s.name != type.name) continue;
                if (s.thread_shareable_override) return true;
                for (const StructField& f : s.fields) {
                    auto _r = thread_shareable_of(f.type, program, visiting);
                    if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                    if (!_r.value()) return false;
                }
                return true;
            }
            return false;
        }
        case TypeKind::Pointer: return false;
        case TypeKind::Function:
        case TypeKind::FunctionPointer: return true;
        case TypeKind::Array:
            if (!type.element) return false;
            return thread_shareable_of(*type.element, program, visiting);
        case TypeKind::Reference:
            if (type.is_rvalue_ref) {
                if (!type.pointee) return false;
                return thread_shareable_of(*type.pointee, program, visiting);
            }
            if (!type.pointee || type.is_mutable_ref) return false;
            return thread_shareable_of(*type.pointee, program, visiting);
        case TypeKind::Span:
            if (!type.pointee || type.is_mutable_ref) return false;
            return thread_shareable_of(*type.pointee, program, visiting);
    }
    return false;
}

} // namespace scpp
