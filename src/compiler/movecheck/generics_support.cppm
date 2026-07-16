module;

#include <memory>
#include <string>
#include <unordered_set>

module scpp.compiler.movecheck:generics_support;

import scpp.ast;
import :types;

namespace scpp {

ExprPtr clone_expr(const Expr& expr);
StmtPtr clone_stmt(const Stmt& stmt);
[[nodiscard]] Function clone_function(const Function& fn);
[[nodiscard]] bool type_satisfies_concept(const Type& type, const ConceptDef& concept_def,
                                          const Program& program);
[[nodiscard]] std::string mangle_type_for_clone_name(const Type& type);

ExprPtr clone_expr(const Expr& expr) {
    auto clone = std::make_unique<Expr>();
    clone->kind = expr.kind;
    clone->loc = expr.loc;
    clone->int_value = expr.int_value;
    clone->bool_value = expr.bool_value;
    clone->name = expr.name;
    clone->explicit_global_qualification = expr.explicit_global_qualification;
    clone->binary_op = expr.binary_op;
    if (expr.lhs) clone->lhs = clone_expr(*expr.lhs);
    if (expr.rhs) clone->rhs = clone_expr(*expr.rhs);
    clone->unary_op = expr.unary_op;
    clone->args.reserve(expr.args.size());
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
    clone->fold_ellipsis_on_left = expr.fold_ellipsis_on_left;
    return clone;
}

StmtPtr clone_stmt(const Stmt& stmt) {
    auto clone = std::make_unique<Stmt>();
    clone->kind = stmt.kind;
    clone->loc = stmt.loc;
    clone->type = stmt.type;
    clone->var_name = stmt.var_name;
    if (stmt.init) clone->init = clone_expr(*stmt.init);
    clone->has_ctor_args = stmt.has_ctor_args;
    clone->ctor_args.reserve(stmt.ctor_args.size());
    for (const ExprPtr& arg : stmt.ctor_args) clone->ctor_args.push_back(clone_expr(*arg));
    if (stmt.expr) clone->expr = clone_expr(*stmt.expr);
    if (stmt.condition) clone->condition = clone_expr(*stmt.condition);
    if (stmt.then_branch) clone->then_branch = clone_stmt(*stmt.then_branch);
    if (stmt.else_branch) clone->else_branch = clone_stmt(*stmt.else_branch);
    clone->is_constexpr = stmt.is_constexpr;
    clone->if_mode = stmt.if_mode;
    clone->statements.reserve(stmt.statements.size());
    for (const StmtPtr& s : stmt.statements) clone->statements.push_back(clone_stmt(*s));
    clone->is_unsafe = stmt.is_unsafe;
    return clone;
}

// ch05 §5.14: `Function` has no copy constructor at all (its `body` is
// a move-only `StmtPtr`) -- this is the closest equivalent, deep-cloning
// the body (via clone_stmt) while plainly copying every other, already-
// copyable field. Used by method_templates_of to hand back independent
// copies (never references into program_.functions' own backing
// storage, which the generic-type monomorphization machinery may
// reallocate out from under a held reference -- see this file's other
// generic-type methods' identical concern).
[[nodiscard]] Function clone_function(const Function& fn) {
    Function clone;
    clone.return_type = fn.return_type;
    clone.return_lifetime = fn.return_lifetime;
    clone.name = fn.name;
    clone.loc = fn.loc;
    clone.params = fn.params;
    clone.body = fn.body ? clone_stmt(*fn.body) : nullptr;
    clone.is_extern_c = fn.is_extern_c;
    clone.is_module_extern = fn.is_module_extern;
    clone.is_unsafe = fn.is_unsafe;
    clone.is_nodiscard = fn.is_nodiscard;
    clone.nodiscard_reason = fn.nodiscard_reason;
    clone.is_compile_time_dependency = fn.is_compile_time_dependency;
    clone.has_varargs = fn.has_varargs;
    clone.method_requires_concept = fn.method_requires_concept;
    clone.is_generic_template = fn.is_generic_template;
    clone.template_params = fn.template_params;
    clone.generic_method_owner_id = fn.generic_method_owner_id;
    clone.member_owner_class = fn.member_owner_class;
    clone.member_initializers = fn.member_initializers;
    clone.receiver_ref_qualifier = fn.receiver_ref_qualifier;
    clone.is_static = fn.is_static;
    clone.is_virtual = fn.is_virtual;
    clone.is_override = fn.is_override;
    clone.is_pure = fn.is_pure;
    clone.is_defaulted = fn.is_defaulted;
    clone.access = fn.access;
    clone.eval_mode = fn.eval_mode;
    clone.namespace_path = fn.namespace_path;
    clone.is_exported = fn.is_exported;
    clone.owning_module = fn.owning_module;
    clone.forwards_to = fn.forwards_to;
    return clone;
}

// ch05 §5.11: whether `type` (a concrete, ordinary type -- never a
// witness class) structurally satisfies `concept_def`: for every
// requirement, the class named by `type` must have a real method
// matching the requirement's own shape exactly -- same synthesized name
// (`ClassName_methodName`, see ClassDef's own comment), same argument
// types (exact match, ch05 §5.10 -- no implicit conversions), and (only
// when the requirement itself constrains it) an identical return type.
// A simple requirement (no return-type constraint) only requires the
// method to exist with matching arguments -- its own return type is
// unconstrained, so any return type qualifies.
[[nodiscard]] bool type_satisfies_concept(const Type& type, const ConceptDef& concept_def, const Program& program) {
    if (type.kind != TypeKind::Named) return false;
    for (const ConceptRequirement& req : concept_def.requirements) {
        std::string method_name = type.name + "_" + req.method_name;
        bool found = false;
        for (const Function& fn : program.functions) {
            if (fn.name != method_name || fn.params.empty()) continue;
            if (fn.params.size() != req.arg_types.size() + 1) continue;
            if (concept_def.requires_param_is_const &&
                (!is_reference(fn.params[0].type) || fn.params[0].type.is_mutable_ref)) {
                continue;
            }
            bool args_match = true;
            for (size_t i = 0; args_match && i < req.arg_types.size(); i++) {
                args_match = types_equal(fn.params[i + 1].type, req.arg_types[i]);
            }
            if (!args_match) continue;
            if (req.has_return_constraint && !types_equal(fn.return_type, req.return_type)) continue;
            found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

// A short, deterministic, LLVM-identifier-safe encoding of `type` for a
// monomorphized clone's own name -- deliberately duplicated from
// codegen's own (private, inaccessible from here) mangle_type rather
// than shared across modules, same existing precedent as this file's
// own independently-duplicated types_equal.
[[nodiscard]] std::string mangle_type_for_clone_name(const Type& type) {
    switch (type.kind) {
        case TypeKind::Named: {
            if (type.template_args.empty()) return type.name;
            std::string result = type.name;
            for (const Type& arg : type.template_args) result += "_" + mangle_type_for_clone_name(arg);
            return result;
        }
        case TypeKind::Pointer:
            return mangle_type_for_clone_name(*type.pointee) + (type.is_mutable_pointee ? "_ptr" : "_cptr");
        case TypeKind::Function: {
            std::string result = mangle_type_for_clone_name(*type.function_return) + "_fntype";
            for (const Type& param : type.function_params) result += "_" + mangle_type_for_clone_name(param);
            if (type.is_const_function) result += "_const";
            if (type.function_ref_qualifier == ReceiverRefQualifier::LValue) result += "_lrefq";
            if (type.function_ref_qualifier == ReceiverRefQualifier::RValue) result += "_rrefq";
            return result;
        }
        case TypeKind::FunctionPointer: {
            std::string result = mangle_type_for_clone_name(*type.function_return) +
                                 (type.is_unsafe_function_pointer ? "_ufnptr" : "_fnptr");
            for (const Type& param : type.function_params) result += "_" + mangle_type_for_clone_name(param);
            return result;
        }
        case TypeKind::Reference:
            return mangle_type_for_clone_name(*type.pointee) +
                   (type.is_rvalue_ref ? "_rref" : (type.is_mutable_ref ? "_ref" : "_cref"));
        case TypeKind::Span: return mangle_type_for_clone_name(*type.pointee) + (type.is_mutable_ref ? "_span" : "_cspan");
        case TypeKind::Array:
            return mangle_type_for_clone_name(*type.element) + "_arr" + std::to_string(type.array_size);
    }
    return "?";
}

} // namespace scpp
