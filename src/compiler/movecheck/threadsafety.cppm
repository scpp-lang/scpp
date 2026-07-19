module;

module scpp.compiler.movecheck:threadsafety;

import std;
import scpp.ast;
import :errors;
import :types;
import :signatures;
import :calls;

namespace scpp {

[[nodiscard]] bool evaluate_thread_bool_constant_expr_for_program(const Expr& expr, const Program& program,
                                                                  std::unordered_set<std::string> visiting = {});
[[nodiscard]] bool thread_movable_of(const Type& type, const Program& program,
                                     std::unordered_set<std::string> visiting = {});
[[nodiscard]] bool thread_shareable_of(const Type& type, const Program& program,
                                       std::unordered_set<std::string> visiting = {});
[[nodiscard]] bool parameter_requires_thread_safety_constraint(const FunctionSignature& sig, std::size_t param_index);
[[nodiscard]] std::string parameter_display_name(const FunctionSignature& sig, std::size_t param_index);
[[nodiscard]] bool parameter_names_interface_type(const Type& param_type, const Body& body);
[[nodiscard]] Type thread_safety_constraint_subject_type(const Expr& arg, const Type& param_type,
                                                         const Body& body, const Signatures& signatures);
void enforce_thread_safety_constraints_for_argument(const Expr& arg, const FunctionSignature& sig,
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

[[nodiscard]] Type thread_safety_constraint_subject_type(const Expr& arg, const Type& param_type, const Body& body,
                                                         const Signatures& signatures) {
    if ((param_type.kind == TypeKind::Reference || param_type.kind == TypeKind::Pointer) &&
        parameter_names_interface_type(param_type, body)) {
        std::optional<Type> source_type = infer_expr_type(arg, body, signatures);
        if (source_type.has_value()) {
            Type source = *source_type;
            if (source.kind == TypeKind::Reference && source.pointee != nullptr) return *source.pointee;
            if (source.kind == TypeKind::Pointer && source.pointee != nullptr) return *source.pointee;
            return source;
        }
    }
    return param_type;
}

void enforce_thread_safety_constraints_for_argument(const Expr& arg, const FunctionSignature& sig, std::size_t param_index,
                                                    std::string_view callee_kind, const std::string& callee_name,
                                                    const Body& body, const Signatures& signatures, SourceLocation loc) {
    if (body.program == nullptr || !parameter_requires_thread_safety_constraint(sig, param_index) ||
        param_index >= sig.param_types.size()) {
        return;
    }
    Type subject = thread_safety_constraint_subject_type(arg, sig.param_types[param_index], body, signatures);
    std::string param_name = parameter_display_name(sig, param_index);
    if (sig.param_require_thread_movable[param_index] && !thread_movable_of(subject, *body.program)) {
        throw DataflowError("argument for parameter '" + param_name + "' of " + std::string(callee_kind) + " '" +
                                callee_name + "' does not satisfy '[[scpp::thread_movable]]' (spec §8.1/§8.5(6))",
            loc);
    }
    if (sig.param_require_thread_shareable[param_index] && !thread_shareable_of(subject, *body.program)) {
        throw DataflowError("argument for parameter '" + param_name + "' of " + std::string(callee_kind) + " '" +
                                callee_name + "' does not satisfy '[[scpp::thread_shareable]]' (spec §8.1/§8.5(6))",
            loc);
    }
}

[[nodiscard]] bool evaluate_thread_bool_constant_expr_for_program(const Expr& expr, const Program& program,
                                                                  std::unordered_set<std::string> visiting) {
    switch (expr.kind) {
        case ExprKind::BoolLiteral: return expr.bool_value;
        case ExprKind::TypeTrait:
            return expr.name == "is_thread_movable" ? thread_movable_of(expr.type, program, visiting)
                                                    : thread_shareable_of(expr.type, program, visiting);
        case ExprKind::Unary:
            if (expr.unary_op == UnaryOp::Not && expr.lhs) {
                return !evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting);
            }
            break;
        case ExprKind::Binary:
            if (!expr.lhs || !expr.rhs) break;
            if (expr.binary_op == BinaryOp::And) {
                return evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting) &&
                       evaluate_thread_bool_constant_expr_for_program(*expr.rhs, program, visiting);
            }
            if (expr.binary_op == BinaryOp::Or) {
                return evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting) ||
                       evaluate_thread_bool_constant_expr_for_program(*expr.rhs, program, visiting);
            }
            if (expr.binary_op == BinaryOp::Eq) {
                return evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting) ==
                       evaluate_thread_bool_constant_expr_for_program(*expr.rhs, program, visiting);
            }
            if (expr.binary_op == BinaryOp::Ne) {
                return evaluate_thread_bool_constant_expr_for_program(*expr.lhs, program, visiting) !=
                       evaluate_thread_bool_constant_expr_for_program(*expr.rhs, program, visiting);
            }
            break;
        default: break;
    }
    throw DataflowError("thread-trait override expressions must be boolean constant expressions built from "
                        "bool literals, !, &&, ||, ==, !=, and scpp::is_thread_movable/shareable(T)",
                        expr.loc);
}

[[nodiscard]] bool thread_movable_of(const Type& type, const Program& program,
                                     std::unordered_set<std::string> visiting) {
    switch (type.kind) {
        case TypeKind::Named: {
            if (is_scalar_type_name(type.name)) return true;
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
                    if (!thread_movable_of(f.type, program, visiting)) return false;
                }
                return true;
            }
            for (const StructDef& s : program.structs) {
                if (s.name != type.name) continue;
                if (s.thread_movable_override) return true;
                for (const StructField& f : s.fields) {
                    if (!thread_movable_of(f.type, program, visiting)) return false;
                }
                return true;
            }
            return false;
        }
        case TypeKind::Pointer: return false;
        case TypeKind::Function:
        case TypeKind::FunctionPointer: return true;
        case TypeKind::Array: return type.element && thread_movable_of(*type.element, program, visiting);
        case TypeKind::Reference:
            return type.is_rvalue_ref ? (type.pointee && thread_movable_of(*type.pointee, program, visiting)) : false;
        case TypeKind::Span: return false;
    }
    return false;
}

[[nodiscard]] bool thread_shareable_of(const Type& type, const Program& program,
                                       std::unordered_set<std::string> visiting) {
    switch (type.kind) {
        case TypeKind::Named: {
            if (is_scalar_type_name(type.name)) return true;
            if (visiting.contains(type.name)) return true;
            visiting.insert(type.name);
            for (const ClassDef& c : program.classes) {
                if (c.name != type.name) continue;
                if (c.thread_shareable_override) return true;
                if (c.thread_movable_if_shareable_expr) {
                    return evaluate_thread_bool_constant_expr_for_program(*c.thread_movable_if_shareable_expr, program,
                                                                          visiting);
                }
                for (const ClassField& f : c.fields) {
                    if (!thread_shareable_of(f.type, program, visiting)) return false;
                }
                return true;
            }
            for (const StructDef& s : program.structs) {
                if (s.name != type.name) continue;
                if (s.thread_shareable_override) return true;
                for (const StructField& f : s.fields) {
                    if (!thread_shareable_of(f.type, program, visiting)) return false;
                }
                return true;
            }
            return false;
        }
        case TypeKind::Pointer: return false;
        case TypeKind::Function:
        case TypeKind::FunctionPointer: return true;
        case TypeKind::Array: return type.element && thread_shareable_of(*type.element, program, visiting);
        case TypeKind::Reference:
            if (type.is_rvalue_ref) return type.pointee && thread_shareable_of(*type.pointee, program, visiting);
            return type.pointee && !type.is_mutable_ref && thread_shareable_of(*type.pointee, program, visiting);
        case TypeKind::Span:
            return type.pointee && !type.is_mutable_ref && thread_shareable_of(*type.pointee, program, visiting);
    }
    return false;
}

} // namespace scpp
