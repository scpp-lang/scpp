module;

module scpp.compiler.movecheck:signatures;

import std;
import scpp.ast;
import :errors;
import scpp.mir;
import :state;
import :types;

namespace scpp {

struct FunctionSignature {
    std::vector<Type> param_types;
    std::vector<std::string> param_names;
    std::vector<std::shared_ptr<Expr>> param_default_exprs;
    std::vector<bool> param_require_thread_movable;
    std::vector<bool> param_require_thread_shareable;
    std::vector<LifetimeAnnotation> param_lifetimes;
    Type return_type;
    LifetimeAnnotation return_lifetime;
    std::vector<std::size_t> returned_lifetime_param_indices;
    std::optional<std::size_t> elided_param_index;
    bool is_extern_c_declaration_only = false;
    bool is_unsafe = false;
    bool is_nodiscard = false;
    std::string nodiscard_reason;
    bool is_compile_time_dependency = false;
    std::string owning_module;
    std::string member_owner_class;
    bool is_static = false;
    bool has_varargs = false;
    AccessSpecifier access = AccessSpecifier::Public;
    SourceLocation loc;
    ReceiverRefQualifier receiver_ref_qualifier = ReceiverRefQualifier::None;
};

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

using Signatures = std::unordered_map<std::string, std::vector<FunctionSignature>>;

[[nodiscard]] bool compile_time_dependency_visible_in_body(const FunctionSignature& candidate, const Body& body);
[[nodiscard]] bool argument_matches_parameter_for_constructor_selection(const Expr& arg, const Type& param_type,
                                                                       const Body& body,
                                                                       const Signatures& signatures);
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures);

[[nodiscard]] bool has_user_declared_copy_ctor(const std::string& class_name, const Program& program);
[[nodiscard]] bool has_user_declared_copy_assign(const std::string& class_name, const Program& program);
[[nodiscard]] bool has_user_declared_dtor(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_field_copy_constructible(const Type& type, const Program& program);
[[nodiscard]] bool is_field_copy_assignable(const Type& type, const Program& program);
[[nodiscard]] bool class_has_any_constructor(const std::string& class_name, const Program& program);
[[nodiscard]] std::string unqualified_template_base_name(std::string_view class_name);
[[nodiscard]] bool names_direct_base(const std::string& member_name, const ClassDef& def);
void collect_virtual_interface_bases_in_construction_order(const Program& program, const ClassDef& def,
                                                           std::vector<const ClassDef*>& out,
                                                           std::unordered_set<std::string>& seen);
[[nodiscard]] std::vector<const ClassDef*> collect_virtual_interface_bases_in_construction_order(
    const Program& program, const ClassDef& def);
[[nodiscard]] const MemberInitializer* find_explicit_interface_initializer(const Function& ctor,
                                                                           const ClassDef& interface_def);
[[nodiscard]] const MemberInitializer* find_explicit_base_initializer(const Function& ctor, const ClassDef& def);
void validate_constructor_member_initialization(const Function& ctor, const ClassDef& def, const Program& program);
[[nodiscard]] bool is_copy_constructible(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_copy_assignable(const std::string& class_name, const Program& program);

[[nodiscard]] const FunctionSignature* resolve_constructor_signature(const std::string& class_name,
                                                                     const std::vector<ExprPtr>& ctor_args,
                                                                     const Body& body,
                                                                     const Signatures& signatures);
void ensure_implicit_default_construction_is_valid(const std::string& class_name,
                                                   std::string_view current_class,
                                                   const Body& body,
                                                   const Signatures& signatures,
                                                   const SourceLocation& loc,
                                                   std::string_view context_message);
void validate_constructor_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
                                              const Signatures& signatures);
void validate_constructor_virtual_interface_base_initialization(const Function& ctor, const ClassDef& def,
                                                                const Body& body,
                                                                const Signatures& signatures);
[[nodiscard]] std::optional<std::size_t> resolve_elided_param_index(const Function& fn);
[[nodiscard]] bool param_can_outlive_call_for_lifetime_return(const Param& param);
void validate_lifetime_annotation_placement(const Function& fn);
[[nodiscard]] std::vector<std::size_t> resolve_returned_lifetime_param_indices(const Function& fn);
[[nodiscard]] Signatures build_signatures(const Program& program);

// spec §6.5: whether `class_name` has declared its own copy constructor
// -- a function named "class_name_new" (see parse_class_def) whose sole
// non-`this` parameter is `const class_name&` (an ordinary, non-rvalue,
// read-only reference to the class's own type -- the shape spec §6.5's
// own worked example, and the overwhelmingly common real-world one,
// uses; a mutable-reference-parameter copy constructor, while legal
// real C++, is out of scope for this recognition).
[[nodiscard]] bool has_user_declared_copy_ctor(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (is_defaulted_special_member_equivalent_to_implicit_omission(fn)) continue;
        if (is_copy_constructor_function(fn) && fn.member_owner_class == class_name) {
            return true;
        }
    }
    return false;
}

// spec §6.5: whether `class_name` has declared its own copy assignment
// operator -- a function named "class_name_operator_assign" (see
// parse_class_body_into's operator= parsing) whose sole non-`this`
// parameter is `const class_name&`, mirroring has_user_declared_copy_ctor
// exactly (an operator= overload taking any other shape is simply an
// ordinary, unrelated overload of the name -- not *the* copy assignment
// operator this recognizes).
[[nodiscard]] bool has_user_declared_copy_assign(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (is_defaulted_special_member_equivalent_to_implicit_omission(fn)) continue;
        if (is_copy_assignment_function(fn) && fn.member_owner_class == class_name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_user_declared_dtor(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (!fn.name.ends_with("_delete") || fn.params.size() != 1) continue;
        const Type& this_param = fn.params[0].type;
        if (this_param.kind == TypeKind::Reference && this_param.is_mutable_ref && this_param.pointee &&
            this_param.pointee->kind == TypeKind::Named && this_param.pointee->name == class_name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_copy_constructible(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_copy_assignable(const std::string& class_name, const Program& program);

// spec §6.5(5)'s own note: a field's own copy-constructibility -- a
// reference always is (bound once, from the source's own referent,
// exactly like move construction's identical carve-out, spec §6.4); a
// nested class recurses; everything else (scalar, struct -- always
// bitwise-copyable per ch04 §4.1 regardless of its own fields, raw
// pointer, array of any of these) is unconditionally
// copy-constructible.
[[nodiscard]] bool is_field_copy_constructible(const Type& type, const Program& program) {
    if (type.kind == TypeKind::Reference) return true;
    if (type.kind == TypeKind::Array) return is_field_copy_constructible(*type.element, program);
    if (type.kind == TypeKind::Named) {
        for (const ClassDef& def : program.classes) {
            if (def.name == type.name) return is_copy_constructible(type.name, program);
        }
        return true; // scalar, struct, or an unrecognized/generic-witness name
    }
    return true; // Pointer, Span, ...: always bitwise-copyable, no restriction
}

// Same as is_field_copy_constructible, but for assignment -- a reference
// field is the one case that differs (never assignable, spec §6.4/§6.5's
// identical "can't be re-seated" carve-out); is_copy_assignable's own
// direct-field loop already rejects a *directly* reference-typed field
// before ever consulting this helper, but nested recursion still needs
// its own answer for one reachable transitively (impossible in the
// current v0.1 subset, since a struct/class field can never itself be
// reference-typed except via this exact direct case -- kept anyway for
// symmetry and defensiveness).
[[nodiscard]] bool is_field_copy_assignable(const Type& type, const Program& program) {
    if (type.kind == TypeKind::Reference) return false;
    if (type.kind == TypeKind::Array) return is_field_copy_assignable(*type.element, program);
    if (type.kind == TypeKind::Named) {
        for (const ClassDef& def : program.classes) {
            if (def.name == type.name) return is_copy_assignable(type.name, program);
        }
        return true;
    }
    return true;
}

[[nodiscard]] bool class_has_any_constructor(const std::string& class_name, const Program& program) {
    return std::any_of(program.functions.begin(), program.functions.end(), [&](const Function& fn) {
        return is_constructor_function(fn) && fn.member_owner_class == class_name &&
               !is_defaulted_special_member_equivalent_to_implicit_omission(fn);
    });
}

[[nodiscard]] std::string unqualified_template_base_name(std::string_view class_name) {
    std::size_t scope = class_name.rfind("::");
    std::string_view tail = scope == std::string_view::npos ? class_name : class_name.substr(scope + 2);
    std::size_t dot = tail.find('.');
    if (dot != std::string_view::npos) tail = tail.substr(0, dot);
    return std::string(tail);
}

[[nodiscard]] bool names_direct_base(const std::string& member_name, const ClassDef& def) {
    const BaseSpecifier* base = def.direct_ordinary_base();
    if (base == nullptr || base->base_type.name.empty()) return false;
    return member_name == base->base_type.name || member_name == unqualified_template_base_name(base->base_type.name);
}

void collect_virtual_interface_bases_in_construction_order(const Program& program, const ClassDef& def,
                                                           std::vector<const ClassDef*>& out,
                                                           std::unordered_set<std::string>& seen) {
    for (const BaseSpecifier& base : def.base_specifiers) {
        const ClassDef* base_def = find_class_def(program, base.base_type.name);
        if (base_def == nullptr || base_def->is_forward_declaration) continue;
        collect_virtual_interface_bases_in_construction_order(program, *base_def, out, seen);
        if (base.kind == BaseClassKind::Interface && seen.insert(base_def->name).second) out.push_back(base_def);
    }
}

[[nodiscard]] std::vector<const ClassDef*> collect_virtual_interface_bases_in_construction_order(const Program& program,
                                                                                                 const ClassDef& def) {
    std::vector<const ClassDef*> out;
    std::unordered_set<std::string> seen;
    collect_virtual_interface_bases_in_construction_order(program, def, out, seen);
    return out;
}

[[nodiscard]] const MemberInitializer* find_explicit_interface_initializer(const Function& ctor, const ClassDef& interface_def) {
    for (const MemberInitializer& init : ctor.member_initializers) {
        if (init.member_name == interface_def.name ||
            init.member_name == unqualified_template_base_name(interface_def.name)) {
            return &init;
        }
    }
    return nullptr;
}

[[nodiscard]] const MemberInitializer* find_explicit_base_initializer(const Function& ctor, const ClassDef& def) {
    const BaseSpecifier* base = def.direct_ordinary_base();
    if (base == nullptr) return nullptr;
    for (const MemberInitializer& init : ctor.member_initializers) {
        if (names_direct_base(init.member_name, def)) return &init;
    }
    return nullptr;
}

void validate_constructor_member_initialization(const Function& ctor, const ClassDef& def, const Program& program) {
    if (!is_constructor_function(ctor) || ctor.member_owner_class != def.name || def.is_forward_declaration ||
        is_defaulted_special_member_equivalent_to_implicit_omission(ctor)) {
        return;
    }
    if (!ctor.generic_method_owner_id.empty() && ctor.generic_method_owner_id != def.template_owner_id) return;
    std::unordered_set<std::string> direct_field_names;
    for (const ClassField& field : def.fields) direct_field_names.insert(field.name);
    std::vector<const ClassDef*> interface_bases = collect_virtual_interface_bases_in_construction_order(program, def);
    const MemberInitializer* explicit_base_init = find_explicit_base_initializer(ctor, def);
    const BaseSpecifier* base = def.direct_ordinary_base();
    if (explicit_base_init != nullptr && base != nullptr && direct_field_names.contains(base->base_type.name)) {
        throw DataflowError("constructor for class '" + def.name + "' cannot disambiguate '" + base->base_type.name +
                                "' in its member-initializer-list because that name matches both a direct field and "
                                "the direct base class",
                            explicit_base_init->loc.is_known() ? explicit_base_init->loc : ctor.loc);
    }
    for (const MemberInitializer& init : ctor.member_initializers) {
        if (&init == explicit_base_init) continue;
        bool names_interface_base = false;
        for (const ClassDef* interface_def : interface_bases) {
            if (interface_def == nullptr) continue;
            if (init.member_name == interface_def->name ||
                init.member_name == unqualified_template_base_name(interface_def->name)) {
                names_interface_base = true;
                break;
            }
        }
        if (names_interface_base) continue;
        if (!direct_field_names.contains(init.member_name)) {
            throw DataflowError("constructor for class '" + def.name + "' names unknown member '" + init.member_name +
                                    "' in its member-initializer-list",
                                init.loc.is_known() ? init.loc : ctor.loc);
        }
    }
    std::vector<std::string> missing;
    for (const ClassField& field : def.fields) {
        if (field.type.kind == TypeKind::Array) continue;
        bool covered_by_ctor = std::any_of(ctor.member_initializers.begin(), ctor.member_initializers.end(),
                                           [&](const MemberInitializer& init) { return init.member_name == field.name; });
        if (!covered_by_ctor && !field.default_initializer.has_value()) missing.push_back(field.name);
    }
    if (!missing.empty()) {
        std::string names;
        for (std::size_t i = 0; i < missing.size(); i++) {
            if (i > 0) names += ", ";
            names += "'" + missing[i] + "'";
        }
        throw DataflowError("constructor for class '" + def.name + "' leaves member(s) " + names +
                                " uninitialized; each constructor must initialize every non-static data member "
                                "either via its own member-initializer-list or an in-class default member "
                                "initializer (spec §6.1(4))",
                            ctor.loc);
    }
}

[[nodiscard]] const FunctionSignature* resolve_constructor_signature(const std::string& class_name,
                                                                     const std::vector<ExprPtr>& ctor_args,
                                                                     const Body& body, const Signatures& signatures);
void ensure_implicit_default_construction_is_valid(const std::string& class_name, std::string_view current_class,
                                                   const Body& body, const Signatures& signatures,
                                                   const SourceLocation& loc, std::string_view context_message);
void validate_constructor_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
                                              const Signatures& signatures);

// spec §6.5(2): a class has an implicitly-defined copy constructor iff
// it declares none of {copy constructor, copy assignment operator,
// destructor} itself (ch08 Q15's "no mixed state" tightening) *and*
// every field is itself copy-constructible (spec §6.5(5)'s own
// recursive note) -- a user-declared copy constructor always wins
// regardless of fields (it's the user's own code, not compiler-derived).
[[nodiscard]] bool is_copy_constructible(const std::string& class_name, const Program& program) {
    auto has_direct_reference_field = [&](const auto& fields) {
        for (const auto& field : fields) {
            if (field.type.kind == TypeKind::Reference) return true;
        }
        return false;
    };
    if (has_user_declared_copy_ctor(class_name, program)) return true;
    if (has_user_declared_copy_assign(class_name, program)) {
        return false;
    }
    for (const ClassDef& def : program.classes) {
        if (def.name != class_name) continue;
        if (has_user_declared_dtor(class_name, program) && !has_direct_reference_field(def.fields)) return false;
        for (const ClassField& f : def.fields) {
            if (!is_field_copy_constructible(f.type, program)) return false;
        }
        return true;
    }
    for (const StructDef& def : program.structs) {
        if (def.name != class_name) continue;
        if (has_user_declared_dtor(class_name, program) && !has_direct_reference_field(def.fields)) return false;
        for (const StructField& f : def.fields) {
            if (!is_field_copy_constructible(f.type, program)) return false;
        }
        return true;
    }
    return false; // not a recognized class at all
}

// spec §6.5(3): symmetric to is_copy_constructible, plus the reference-
// member carve-out (a class with a directly reference-typed field has
// no compiler-provided copy assignment operator at all, exactly
// mirroring move assignment's identical spec §6.4(3) rule).
[[nodiscard]] bool is_copy_assignable(const std::string& class_name, const Program& program) {
    if (has_user_declared_copy_assign(class_name, program)) return true;
    if (has_user_declared_dtor(class_name, program) || has_user_declared_copy_ctor(class_name, program)) {
        return false;
    }
    for (const ClassDef& def : program.classes) {
        if (def.name != class_name) continue;
        for (const ClassField& f : def.fields) {
            if (is_reference(f.type)) return false;
            if (!is_field_copy_assignable(f.type, program)) return false;
        }
        return true;
    }
    for (const StructDef& def : program.structs) {
        if (def.name != class_name) continue;
        for (const StructField& f : def.fields) {
            if (is_reference(f.type)) return false;
            if (!is_field_copy_assignable(f.type, program)) return false;
        }
        return true;
    }
    return false;
}

// Returns the class/struct name `type` resolves to as a Named type,
// seeing through a Reference (e.g. `this`'s own declared type, ch05
// §5.9) -- or empty if `type` isn't (possibly a reference to) a Named
// type at all. Used only by apply_expr's Member case, to tell a
// class-typed base (access-controlled, ch04 §4.2) apart from a
// struct-typed one (never access-controlled, ch04 §4.1).
[[nodiscard]] const FunctionSignature* resolve_constructor_signature(const std::string& class_name,
                                                                     const std::vector<ExprPtr>& ctor_args,
                                                                     const Body& body, const Signatures& signatures) {
    auto it = signatures.find(class_name + "_new");
    if (it == signatures.end()) return nullptr;
    std::vector<const FunctionSignature*> matches;
    for (const FunctionSignature& candidate : it->second) {
        if (!compile_time_dependency_visible_in_body(candidate, body)) continue;
        if (!signature_accepts_argument_count(candidate, ctor_args.size(), 1)) continue;
        bool all_match = true;
        for (std::size_t i = 0; all_match && i < ctor_args.size(); i++) {
            all_match = argument_matches_parameter_for_constructor_selection(*ctor_args[i],
                                                                             candidate.param_types[i + 1], body,
                                                                             signatures);
        }
        if (all_match) matches.push_back(&candidate);
    }
    if (matches.empty()) return nullptr;
    if (matches.size() == 1) return matches[0];
    auto mutable_ref_score = [&](const FunctionSignature& candidate) {
        int score = 0;
        for (std::size_t i = 0; i < ctor_args.size(); i++) {
            const Type& param_type = candidate.param_types[i + 1];
            if (is_reference(param_type) && param_type.is_mutable_ref &&
                !is_read_only_reachable(*ctor_args[i], body, signatures)) {
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

void ensure_implicit_default_construction_is_valid(const std::string& class_name, std::string_view current_class,
                                                   const Body& body, const Signatures& signatures,
                                                   const SourceLocation& loc, std::string_view context_message) {
    if (body.program == nullptr) return;
    const ClassDef* class_def = find_class_def(*body.program, class_name);
    if (class_def == nullptr) return;
    if (class_has_any_constructor(class_name, *body.program)) {
        static const std::vector<ExprPtr> no_ctor_args;
        const FunctionSignature* sig = resolve_constructor_signature(class_name, no_ctor_args, body, signatures);
        if (sig == nullptr) {
            throw DataflowError(std::string(context_message) + ": base class '" + class_name +
                                    "' has no default constructor; write an explicit base-class initializer",
                                loc);
        }
        if (sig->access == AccessSpecifier::Private && !sig->member_owner_class.empty() &&
            current_class != sig->member_owner_class) {
            throw DataflowError(std::string(context_message) + ": base class '" + class_name +
                                    "' default constructor is private; write an explicit base-class initializer "
                                    "calling an accessible constructor",
                                loc);
        }
        if (sig->is_unsafe) {
            throw DataflowError(std::string(context_message) + ": base class '" + class_name +
                                    "' default constructor is [[scpp::unsafe]]",
                                loc);
        }
        return;
    }
    if (const BaseSpecifier* base = class_def->direct_ordinary_base()) {
        ensure_implicit_default_construction_is_valid(base->base_type.name, current_class, body, signatures, loc,
                                                      context_message);
    }
}

void validate_constructor_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
                                              const Signatures& signatures) {
    const BaseSpecifier* base = def.direct_ordinary_base();
    if (!is_constructor_function(ctor) || ctor.member_owner_class != def.name || base == nullptr ||
        is_defaulted_special_member_equivalent_to_implicit_omission(ctor)) {
        return;
    }
    if (!ctor.generic_method_owner_id.empty() && ctor.generic_method_owner_id != def.template_owner_id) return;
    const MemberInitializer* explicit_base_init = find_explicit_base_initializer(ctor, def);
    std::string context_message =
        "constructor for class '" + def.name + "' must initialize its direct base class '" + base->base_type.name + "'";
    if (explicit_base_init == nullptr) {
        ensure_implicit_default_construction_is_valid(base->base_type.name, def.name, body, signatures, ctor.loc,
                                                      context_message);
        return;
    }
    const FunctionSignature* sig =
        resolve_constructor_signature(base->base_type.name, explicit_base_init->initializer.brace_args, body, signatures);
    if (sig == nullptr) {
        if (body.program != nullptr && !class_has_any_constructor(base->base_type.name, *body.program) &&
            explicit_base_init->initializer.brace_args.empty()) {
            ensure_implicit_default_construction_is_valid(base->base_type.name, def.name, body, signatures,
                                                          explicit_base_init->loc, context_message);
            return;
        }
        throw DataflowError("base-class initializer for '" + base->base_type.name +
                                "' does not match any constructor of that class",
                            explicit_base_init->loc.is_known() ? explicit_base_init->loc : ctor.loc);
    }
    if (sig->access == AccessSpecifier::Private && !sig->member_owner_class.empty() && def.name != sig->member_owner_class) {
        throw DataflowError("cannot call private constructor of base class '" + base->base_type.name +
                                "' from derived class '" + def.name + "'",
                            explicit_base_init->loc.is_known() ? explicit_base_init->loc : ctor.loc);
    }
    if (sig->is_unsafe) {
        throw DataflowError("cannot call base class '" + base->base_type.name +
                                "' constructor outside '[[scpp::unsafe]] { }': its own declaration is marked "
                                "'[[scpp::unsafe]]'",
                            explicit_base_init->loc.is_known() ? explicit_base_init->loc : ctor.loc);
    }
}

void validate_constructor_virtual_interface_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
                                                                const Signatures& signatures) {
    if (!is_constructor_function(ctor) || ctor.member_owner_class != def.name || body.program == nullptr ||
        is_defaulted_special_member_equivalent_to_implicit_omission(ctor)) {
        return;
    }
    if (!ctor.generic_method_owner_id.empty() && ctor.generic_method_owner_id != def.template_owner_id) return;
    std::vector<const ClassDef*> interface_bases = collect_virtual_interface_bases_in_construction_order(*body.program, def);
    for (const ClassDef* interface_def : interface_bases) {
        if (interface_def == nullptr) continue;
        const MemberInitializer* explicit_init = find_explicit_interface_initializer(ctor, *interface_def);
        std::string context_message =
            "constructor for class '" + def.name + "' must initialize virtual interface base '" + interface_def->name + "'";
        if (explicit_init == nullptr) {
            if (!def.is_interface) {
                ensure_implicit_default_construction_is_valid(interface_def->name, def.name, body, signatures, ctor.loc,
                                                              context_message);
            }
            continue;
        }
        const FunctionSignature* sig = resolve_constructor_signature(interface_def->name, explicit_init->initializer.brace_args,
                                                                     body, signatures);
        if (sig == nullptr) {
            if (!class_has_any_constructor(interface_def->name, *body.program) &&
                explicit_init->initializer.brace_args.empty()) {
                ensure_implicit_default_construction_is_valid(interface_def->name, def.name, body, signatures,
                                                              explicit_init->loc.is_known() ? explicit_init->loc : ctor.loc,
                                                              context_message);
                continue;
            }
            throw DataflowError("base-class initializer for '" + interface_def->name +
                                    "' does not match any constructor of that class",
                                explicit_init->loc.is_known() ? explicit_init->loc : ctor.loc);
        }
        if (sig->access == AccessSpecifier::Private && !sig->member_owner_class.empty() &&
            def.name != sig->member_owner_class) {
            throw DataflowError("cannot call private constructor of base class '" + interface_def->name +
                                    "' from derived class '" + def.name + "'",
                                explicit_init->loc.is_known() ? explicit_init->loc : ctor.loc);
        }
        if (sig->is_unsafe) {
            throw DataflowError("cannot call base class '" + interface_def->name +
                                    "' constructor outside '[[scpp::unsafe]] { }': its own declaration is marked "
                                    "'[[scpp::unsafe]]'",
                                explicit_init->loc.is_known() ? explicit_init->loc : ctor.loc);
        }
    }
}
[[nodiscard]] std::optional<std::size_t> resolve_elided_param_index(const Function& fn) {
    if (fn.return_lifetime.present()) return std::nullopt;
    if (!is_reference(fn.return_type)) return std::nullopt;

    // ch04 §4.2/ch05 §5.9/spec §6.5: a method's own `this` (always
    // params[0], see make_this_param) is *always* the elision source
    // when present, regardless of how many other reference parameters
    // the method also takes -- the "this-elision rule" other comments
    // in this file already reference by name. This isn't a general
    // multiple-reference-parameter lifetime-group solution (ch05 §5.3's
    // own `[[scpp::lifetime(name)]]` design remains unimplemented,
    // tracked past v0.1) -- it's a narrow, specifically-justified
    // special case for exactly the shape a user-declared copy
    // assignment operator needs (spec §6.5's own worked example,
    // `RefCounted& operator=(const RefCounted& other) { ...; return
    // *this; }`): real C++'s own universal convention is that an
    // assignment operator always returns `*this`, never its argument,
    // so `this` is the only sound choice regardless of what other
    // reference parameters are also in scope -- exactly like the
    // single-reference-parameter case below, this is a structural,
    // signature-level inference (never verified against what the body
    // actually returns), just extended to cover this one additional,
    // well-understood shape.
    if (!fn.params.empty() && fn.params[0].name == "this" && is_reference(fn.params[0].type)) {
        if (fn.return_type.is_mutable_ref && !fn.params[0].type.is_mutable_ref) {
            throw DataflowError("function '" + fn.name +
                                 "' returns a mutable reference ('T&') but its 'this' is a read-only ('const') "
                                 "receiver; a mutable reference cannot be manufactured from a shared one",
                fn.loc);
        }
        return 0;
    }

    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < fn.params.size(); i++) {
        // ch03/ch05 §5.11: an rvalue-reference (`T&&`) parameter is
        // never an eligible elision source -- its argument may be a
        // fresh temporary (a literal, a std::make_unique<T>(...)/call
        // result) whose storage the caller never promises to keep alive
        // past the call, unlike an ordinary T&/const T& argument (always
        // a real place the caller keeps borrowed for the call's
        // duration). Returning a reference derived from it would be a
        // dangling reference in exactly the cases elision is supposed to
        // rule out.
        if (!is_reference(fn.params[i].type) || fn.params[i].type.is_rvalue_ref) continue;
        if (found.has_value()) {
            throw DataflowError(
                "function '" + fn.name +
                "' returns a reference but has more than one reference parameter; scpp v0.1 can only infer a "
                "returned reference's lifetime when there is exactly one (spec ch05.3) -- refactor to take a "
                "single reference parameter, or return by value/std::unique_ptr instead",
                fn.loc);
        }
        found = i;
    }
    if (!found.has_value()) {
        throw DataflowError(
            "function '" + fn.name +
            "' returns a reference but has no reference parameter to infer its lifetime from (spec ch05.3) -- "
            "refactor to take a single reference parameter, or return by value/std::unique_ptr instead",
            fn.loc);
    }
    if (fn.return_type.is_mutable_ref && !fn.params[*found].type.is_mutable_ref) {
        throw DataflowError("function '" + fn.name +
                             "' returns a mutable reference ('T&') but its sole reference parameter '" +
                             fn.params[*found].name +
                             "' is a shared reference ('const T&'); a mutable reference cannot be manufactured "
                             "from a shared one",
            fn.loc);
    }
    return found;
}

[[nodiscard]] bool param_can_outlive_call_for_lifetime_return(const Param& param) {
    if (!is_lifetime_eligible_type(param.type)) return false;
    return !(is_reference(param.type) && param.type.is_rvalue_ref);
}

void validate_lifetime_annotation_placement(const Function& fn) {
    for (const Param& param : fn.params) {
        if (!param.lifetime.present()) continue;
        if (!is_lifetime_eligible_type(param.type)) {
            throw DataflowError("parameter '" + param.name +
                                    "' bears '[[scpp::lifetime(name)]]' but is not a reference, pointer, or span",
                                fn.loc);
        }
    }
    if (fn.return_lifetime.present() && !is_lifetime_eligible_type(fn.return_type)) {
        throw DataflowError("function '" + fn.name +
                                "' bears '[[scpp::lifetime(name)]]' on its declarator, but its return type is not "
                                "a reference, pointer, or span",
                            fn.loc);
    }
}

[[nodiscard]] bool is_operator_arrow_function(const Function& fn) {
    return !fn.member_owner_class.empty() && fn.name.ends_with("_operator_arrow") && !fn.params.empty() &&
           fn.params[0].name == "this";
}

void validate_operator_arrow_signature(const Function& fn) {
    if (!is_operator_arrow_function(fn)) return;
    if (fn.params.size() != 1) {
        throw DataflowError("operator-> of class '" + fn.member_owner_class + "' shall have no parameters", fn.loc);
    }
    const Type& ret = fn.return_type;
    bool returns_pointer = ret.kind == TypeKind::Pointer;
    bool returns_class = ret.kind == TypeKind::Named;
    bool returns_class_ref = ret.kind == TypeKind::Reference && ret.pointee != nullptr && ret.pointee->kind == TypeKind::Named;
    if (!returns_pointer && !returns_class && !returns_class_ref) {
        throw DataflowError("operator-> of class '" + fn.member_owner_class +
                                "' shall return a pointer, a class, or a reference to class",
                            fn.loc);
    }
}

[[nodiscard]] std::vector<std::size_t> resolve_returned_lifetime_param_indices(const Function& fn) {
    validate_lifetime_annotation_placement(fn);
    if (fn.return_lifetime.present()) {
        if (fn.return_lifetime.is_any()) {
            throw DataflowError("function '" + fn.name +
                                    "' cannot name the reserved lifetime group 'any' in its return annotation",
                                fn.loc);
        }
        std::vector<std::size_t> indices;
        for (std::size_t i = 0; i < fn.params.size(); i++) {
            if (fn.params[i].lifetime.name != fn.return_lifetime.name) continue;
            if (!param_can_outlive_call_for_lifetime_return(fn.params[i])) {
                throw DataflowError("function '" + fn.name + "' ties its return to parameter '" + fn.params[i].name +
                                        "', but that parameter cannot outlive the call",
                                    fn.loc);
            }
            indices.push_back(i);
        }
        if (indices.empty() && is_operator_arrow_function(fn) && !fn.params.empty() && fn.params[0].name == "this") {
            indices.push_back(0);
        }
        if (indices.empty()) {
            throw DataflowError("function '" + fn.name + "' names lifetime group '" + fn.return_lifetime.name +
                                    "' in its return annotation, but no parameter belongs to that group",
                                fn.loc);
        }
        return indices;
    }
    if (!is_reference(fn.return_type)) return {};
    std::optional<std::size_t> elided = resolve_elided_param_index(fn);
    if (!elided.has_value()) return {};
    const Param& param = fn.params[*elided];
    if (param.lifetime.is_any()) {
        throw DataflowError("function '" + fn.name +
                                "' returns a value derived from reserved lifetime group 'any', which may not "
                                "escape the call",
                            fn.loc);
    }
    return {*elided};
}

// Whether assigning through `expr` (used as an assignment's *target* --
// see apply_expr's Binary/Assign case) would write through a read-only
// (`const T&` reference, `std::span<const T>`, or `const T*` raw
// pointer) somewhere in its chain -- Reference/Span reuse the same
// `is_mutable_ref` flag for "is this view/borrow read-only", Pointer has
// its own analogous `is_mutable_pointee` flag (see ast.cppm's Type; ch05
// §5.7 -- writing through a `const T*` is an ordinary type error,
// unconditionally, even inside `unsafe { }`, so this is never gated by
// `state.unsafe_depth`). A `.field`/`[index]` projection's constness
// always comes from its outermost base (struct fields can never
// themselves be references or spans -- ch04.1 permanently forbids that
// -- so there's never a *nested* one to find deeper in the chain); a
// call's constness comes from its own return type. A plain local (not
// itself a reference/span) is always writable here -- move/
// initialization-state legality is checked separately, this is purely
// about const-ness.
[[nodiscard]] Signatures build_signatures(const Program& program) {
    Signatures signatures;
    for (const Function& fn : program.functions) {
        if (is_defaulted_special_member_equivalent_to_implicit_omission(fn)) continue;
        validate_operator_arrow_signature(fn);
        FunctionSignature sig;
        sig.param_types.reserve(fn.params.size());
        sig.param_names.reserve(fn.params.size());
        sig.param_default_exprs.reserve(fn.params.size());
        sig.param_require_thread_movable.reserve(fn.params.size());
        sig.param_require_thread_shareable.reserve(fn.params.size());
        for (const Param& param : fn.params) {
            sig.param_types.push_back(param.type);
            sig.param_names.push_back(param.name);
            sig.param_default_exprs.push_back(param.default_expr);
            sig.param_require_thread_movable.push_back(param.require_thread_movable);
            sig.param_require_thread_shareable.push_back(param.require_thread_shareable);
            sig.param_lifetimes.push_back(param.lifetime);
        }
        sig.return_type = fn.return_type;
        sig.return_lifetime = fn.return_lifetime;
        sig.returned_lifetime_param_indices = resolve_returned_lifetime_param_indices(fn);
        sig.elided_param_index = fn.return_lifetime.present() ? std::nullopt : resolve_elided_param_index(fn);
        sig.is_extern_c_declaration_only = fn.is_extern_c && fn.body == nullptr;
        sig.is_unsafe = fn.is_unsafe;
        sig.is_nodiscard = fn.is_nodiscard;
        sig.nodiscard_reason = fn.nodiscard_reason;
        sig.is_compile_time_dependency = fn.is_compile_time_dependency;
        sig.owning_module = fn.visibility_module.empty() ? fn.owning_module : fn.visibility_module;
        sig.member_owner_class = fn.member_owner_class;
        sig.is_static = fn.is_static;
        sig.has_varargs = fn.has_varargs;
        sig.access = fn.access;
        sig.loc = fn.loc;
        sig.receiver_ref_qualifier = fn.receiver_ref_qualifier;
        std::vector<FunctionSignature>& overloads = signatures[fn.name];
        for (const FunctionSignature& existing : overloads) {
            bool same_params = existing.param_types.size() == sig.param_types.size();
            for (std::size_t i = 0; same_params && i < sig.param_types.size(); i++) {
                same_params = types_equal(existing.param_types[i], sig.param_types[i]);
            }
            if (same_params && existing.receiver_ref_qualifier == sig.receiver_ref_qualifier) {
                throw DataflowError("redefinition of '" + fn.name +
                                     "': a previous declaration with an identical parameter list already "
                                     "exists (ch05 §5.10 -- functions can only be overloaded by parameter "
                                     "list, return type alone doesn't count as a difference)",
                    fn.loc);
            }
        }
        overloads.push_back(std::move(sig));
    }
    return signatures;
}

} // namespace scpp
