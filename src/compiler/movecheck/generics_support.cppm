module;

module scpp.compiler.movecheck:generics_support;

import std;
import scpp.ast;
import :types;
import :signatures;

namespace scpp {

[[nodiscard]] Function clone_function(const Function& fn);
[[nodiscard]] bool type_satisfies_concept(const Type& type, const ConceptDef& concept_def,
                                          const Program& program);
[[nodiscard]] std::string mangle_type_for_clone_name(const Type& type);
[[nodiscard]] bool probe_lifetime_groups_match(const ConceptRequirement& req, const Function& fn);

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
    for (const Param& param : fn.params) clone.params.push_back(deep_clone_param(param));
    clone.body = fn.body ? deep_clone_stmt(*fn.body) : nullptr;
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
    clone.is_deleted = fn.is_deleted;
    clone.is_explicit = fn.is_explicit;
    clone.access = fn.access;
    clone.eval_mode = fn.eval_mode;
    clone.namespace_path = fn.namespace_path;
    clone.is_exported = fn.is_exported;
    clone.owning_module = fn.owning_module;
    clone.forwards_to = fn.forwards_to;
    return clone;
}

// spec §6.2(22)-(22.4): whether candidate declaration `fn`
// (already confirmed to match `req` on name/arg-count/arg-types/return-
// type) also satisfies the lifetime-group relation `req`'s own probe
// parameters impose. `req.arg_lifetimes` is parallel to `req.arg_types`;
// `fn.params[i + 1]` is `fn`'s corresponding real parameter (`params[0]`
// is always the implicit receiver, see make_this_param). Uses the same
// declaration-local, alpha-equivalent comparison spec §6.2(22) uses for
// an ordinary call: two probe parameters are compared only against each
// other (never by spelling against `fn`'s own group names), so `fn` may
// freely use whatever group names it likes as long as its own grouping
// relation -- which of its parameters share a group, and which don't --
// mirrors the probes'.
[[nodiscard]] bool probe_lifetime_groups_match(const ConceptRequirement& req, const Function& fn) {
    for (std::size_t i = 0; i < req.arg_lifetimes.size(); i++) {
        const LifetimeAnnotation& probe = req.arg_lifetimes[i];
        if (!probe.present()) continue; // spec §6.2(22.4): no attribute, no constraint.
        const LifetimeAnnotation& candidate = fn.params[i + 1].lifetime;
        if (probe.is_any()) {
            // spec §6.2(22.3): an `any`-tagged probe requires the
            // corresponding real parameter to also be `any`-tagged.
            if (!candidate.present() || !candidate.is_any()) return false;
            continue;
        }
        // spec §6.2(22.1) first clause: a user-written-group probe
        // requires the corresponding real parameter to belong to some
        // non-`any` group.
        if (!candidate.present() || candidate.is_any()) return false;
        // spec §6.2(22.1) second clause/(22.2): same spelling among
        // probes => same real group; different spelling => different
        // real group.
        for (std::size_t j = 0; j < i; j++) {
            const LifetimeAnnotation& other_probe = req.arg_lifetimes[j];
            if (!other_probe.present() || other_probe.is_any()) continue;
            const LifetimeAnnotation& other_candidate = fn.params[j + 1].lifetime;
            bool probes_same_group = other_probe.name == probe.name;
            bool candidates_same_group = other_candidate.name == candidate.name;
            if (probes_same_group != candidates_same_group) return false;
        }
    }
    return true;
}

// spec §13.2(5): substitutes every occurrence of the concept's own
// template_param_name (e.g. "T") appearing (possibly nested, inside a
// pointer/reference/array/template-argument position) within `type`
// with `concrete_type` -- used to turn a construction requirement's own
// argument types (spelled against the concept's abstract "T") into the
// real argument types to check against a specific candidate type's own
// constructors. Deliberately a small, self-contained duplicate of
// monomorphize.cppm's own (private, inaccessible from here)
// substitute_type_param, same existing precedent as this file's own
// independently-duplicated types_equal/mangle_type_for_clone_name.
[[nodiscard]] Type substitute_concept_template_param(Type type, const std::string& template_param_name,
                                                     const Type& concrete_type) {
    if (type.kind == TypeKind::Named && type.template_args.empty() && type.name == template_param_name) {
        return concrete_type;
    }
    if (type.pointee) {
        type.pointee =
            std::make_shared<Type>(substitute_concept_template_param(*type.pointee, template_param_name, concrete_type));
    }
    if (type.element) {
        type.element =
            std::make_shared<Type>(substitute_concept_template_param(*type.element, template_param_name, concrete_type));
    }
    for (Type& arg : type.template_args) arg = substitute_concept_template_param(arg, template_param_name, concrete_type);
    return type;
}

// spec §13.2(5)-(6): whether an argument of type `arg_type` may be
// passed to a real constructor parameter of type `param_type`,
// structurally -- exact match, or (since a construction requirement's
// own probe argument, e.g. `t` in `T{t}`, is always an ordinary named
// lvalue, never an rvalue -- there is no syntax in this v0.1 requires-
// expression grammar to spell one) binding to a same-typed lvalue
// reference parameter, const or otherwise. Never models the fuller
// overload-resolution/value-category machinery calls.cppm's own
// constructor_parameter_accepts_argument_directly needs for a *real*
// constructor call (which has an actual argument Expr, and thus an
// actual value category, to inspect) -- this concept-checking pass has
// never had access to that machinery (see type_satisfies_concept's own
// pre-existing method-call matching just below, which is equally exact-
// match-only), and a construction requirement's own probe parameters
// never carry enough information (no real expression, just a bare
// Type) to do better than this.
[[nodiscard]] bool construction_argument_type_matches(Type arg_type, Type param_type) {
    if (param_type.kind == TypeKind::Reference) {
        if (param_type.is_rvalue_ref || param_type.pointee == nullptr) return false;
        param_type = *param_type.pointee;
    }
    param_type.is_const_qualified = false;
    if (arg_type.kind == TypeKind::Reference && arg_type.pointee != nullptr) arg_type = *arg_type.pointee;
    arg_type.is_const_qualified = false;
    return types_equal(arg_type, param_type);
}

// spec §13.2(5): whether `target_type` (a concrete, ordinary type) has
// a real constructor accepting exactly `arg_types` (already substituted
// -- see substitute_concept_template_param) -- reuses is_constructor_
// function (ast.cppm), the same "ClassName_new" recognition every other
// constructor-aware pass in this compiler (calls.cppm,
// signatures.cppm's own is_copy_constructible, ...) already shares, so
// this doesn't duplicate a *second*, independent notion of "which
// function is a constructor" -- only the (necessarily different, see
// construction_argument_type_matches's own comment) argument-matching
// rule is specific to this concept-checking pass.
[[nodiscard]] bool type_has_matching_constructor(const Type& target_type, const std::vector<Type>& arg_types,
                                                 const Program& program) {
    if (target_type.kind != TypeKind::Named) return false;
    // A single argument structurally identical to target_type itself is
    // exactly real C++'s own copy-construction shape -- the spec's own
    // worked example, `requires(T t) { T{t}; }` -- and is also, by far,
    // this requirement shape's single most common real use (std::vector
    // itself, see std_vector.scpp, only ever needs this exact shape).
    // Unlike a converting/multi-argument constructor, a scalar/enum's
    // own copy-constructibility, and a class/struct's *implicit*
    // (compiler-provided, never user-declared, spec §6.5) copy
    // constructor, are never represented as a real "ClassName_new"
    // Function this pass could find by searching program.functions
    // below -- so this shape instead reuses signatures.cppm's own,
    // already-general is_copy_constructible (the exact same notion of
    // copy-constructibility this compiler's real dataflow/move-checking
    // already uses everywhere else, e.g. calls.cppm's is_copyable_class_
    // lvalue_boundary_source), which already correctly covers every
    // category this compiler recognizes.
    if (arg_types.size() == 1) {
        Type single_arg = arg_types[0];
        if (single_arg.kind == TypeKind::Reference && single_arg.pointee != nullptr) single_arg = *single_arg.pointee;
        single_arg.is_const_qualified = false;
        Type unqualified_target = target_type;
        unqualified_target.is_const_qualified = false;
        if (types_equal(single_arg, unqualified_target)) {
            if (is_scalar_named_type(target_type) || is_enum_type(target_type, &program)) return true;
            // Same named-wrapper carve-out is_field_copy_constructible
            // and is_freely_copyable_class_value_source already apply at
            // every other copy-related boundary (spec §6.5(5)): a stdlib
            // "view" wrapper such as std::string_view is spelled as a
            // class with user-declared special members in the library
            // source, but is still meant to behave like a freely
            // copyable scalar pair -- checked before the ordinary class/
            // struct is_copy_constructible path below, which would
            // otherwise see its user-declared destructor and (correctly,
            // for an ordinary class) conclude it has no implicit copy
            // constructor.
            if (is_freely_copyable_value_type(target_type, program)) return true;
            if (find_class_def(program, target_type.name) != nullptr ||
                find_struct_def(program, target_type.name) != nullptr) {
                return is_copy_constructible(target_type.name, program);
            }
        }
    }
    for (const Function& fn : program.functions) {
        if (fn.member_owner_class != target_type.name || !is_constructor_function(fn)) continue;
        if (fn.params.size() != arg_types.size() + 1) continue;
        bool args_match = true;
        for (std::size_t i = 0; args_match && i < arg_types.size(); i++) {
            args_match = construction_argument_type_matches(arg_types[i], fn.params[i + 1].type);
        }
        if (args_match) return true;
    }
    return false;
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
// unconstrained, so any return type qualifies. spec §6.2(22): a
// requirement whose probe parameters bear `[[scpp::lifetime(...)]]`
// additionally requires the candidate method's corresponding parameters
// to honor that same lifetime-grouping relation (see
// probe_lifetime_groups_match) -- this is a real constraint on concept
// satisfaction, not merely syntax the probe parameter tolerates.
// spec §13.2: a construction-shaped requirement (req.is_construct) is
// checked differently -- see type_has_matching_constructor -- rather
// than searching for a same-named method at all.
[[nodiscard]] bool type_satisfies_concept(const Type& type, const ConceptDef& concept_def, const Program& program) {
    if (type.kind != TypeKind::Named) return false;
    for (const ConceptRequirement& req : concept_def.requirements) {
        if (req.is_construct) {
            Type target_type;
            if (req.construct_type_name == concept_def.template_param_name) {
                target_type = type;
            } else {
                target_type.kind = TypeKind::Named;
                target_type.name = req.construct_type_name;
            }
            std::vector<Type> substituted_args;
            substituted_args.reserve(req.arg_types.size());
            for (const Type& arg_type : req.arg_types) {
                substituted_args.push_back(
                    substitute_concept_template_param(arg_type, concept_def.template_param_name, type));
            }
            if (!type_has_matching_constructor(target_type, substituted_args, program)) return false;
            continue;
        }
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
            for (std::size_t i = 0; args_match && i < req.arg_types.size(); i++) {
                args_match = types_equal(fn.params[i + 1].type, req.arg_types[i]);
            }
            if (!args_match) continue;
            if (req.has_return_constraint && !types_equal(fn.return_type, req.return_type)) continue;
            if (!probe_lifetime_groups_match(req, fn)) continue;
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
    std::string const_prefix = type.is_const_qualified ? "const_" : "";
    switch (type.kind) {
        case TypeKind::Named: {
            if (type.template_args.empty()) return const_prefix + type.name;
            std::string result = const_prefix + type.name;
            for (const Type& arg : type.template_args) result += "_" + mangle_type_for_clone_name(arg);
            return result;
        }
        case TypeKind::Pointer:
            return const_prefix + mangle_type_for_clone_name(*type.pointee) + (type.is_mutable_pointee ? "_ptr" : "_cptr");
        case TypeKind::Function: {
            std::string result = const_prefix + mangle_type_for_clone_name(*type.function_return) + "_fntype";
            for (const Type& param : type.function_params) result += "_" + mangle_type_for_clone_name(param);
            if (type.is_const_function) result += "_const";
            if (type.function_ref_qualifier == ReceiverRefQualifier::LValue) result += "_lrefq";
            if (type.function_ref_qualifier == ReceiverRefQualifier::RValue) result += "_rrefq";
            return result;
        }
        case TypeKind::FunctionPointer: {
            std::string result = const_prefix + mangle_type_for_clone_name(*type.function_return) +
                                 (type.is_unsafe_function_pointer ? "_ufnptr" : "_fnptr");
            for (const Type& param : type.function_params) result += "_" + mangle_type_for_clone_name(param);
            return result;
        }
        case TypeKind::Reference:
            return const_prefix + mangle_type_for_clone_name(*type.pointee) +
                   (type.is_rvalue_ref ? "_rref" : (type.is_mutable_ref ? "_ref" : "_cref"));
        case TypeKind::Span:
            return const_prefix + mangle_type_for_clone_name(*type.pointee) + (type.is_mutable_ref ? "_span" : "_cspan");
        case TypeKind::Array:
            return const_prefix + mangle_type_for_clone_name(*type.element) + "_arr" + std::to_string(type.array_size);
    }
    return "?";
}

} // namespace scpp
