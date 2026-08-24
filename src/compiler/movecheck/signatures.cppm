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
    std::vector<bool> param_is_forwarding_reference;
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
    bool is_generic_template = false;
    // [dcl.fct.def.delete]/2: a deleted function still takes part in
    // overload resolution -- it is *naming* the winner that is
    // ill-formed -- so this must travel with the signature rather than
    // remove the candidate from the set.
    bool is_deleted = false;
    std::string display_name;
};

namespace {
[[nodiscard]] bool is_forwarding_reference_param(const Function& fn, const Param& param) {
    if (param.type.kind != TypeKind::Reference || !param.type.is_rvalue_ref || !param.type.pointee) return false;
    if (!param.generic_concept.empty()) return true;
    if (param.type.pointee->kind != TypeKind::Named || !param.type.pointee->template_args.empty() ||
        !param.type.pointee->non_type_args.empty()) {
        return false;
    }
    for (const GenericTypeParam& tp : fn.template_params) {
        if (!tp.is_non_type && !tp.is_pack && tp.name == param.type.pointee->name) return true;
    }
    return false;
}

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
[[nodiscard]] bool place_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool expr_is_assignable_place(const Expr& expr, const Body& body);
[[nodiscard]] DataflowError read_only_write_error(const Expr& place, const Body& body, const Signatures& signatures,
                                                  const std::string& operator_spelling, SourceLocation loc);
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                           const Signatures& signatures);


[[nodiscard]] std::string describe_constructor_candidate(const FunctionSignature& candidate) {
    std::string result = candidate.member_owner_class + "_new(";
    for (std::size_t i = 1; i < candidate.param_types.size(); i++) {
        if (i != 1) result += ", ";
        result += describe_type_brief(candidate.param_types[i]);
    }
    result += ")";
    if (candidate.is_generic_template) result += " [generic]";
    return result;
}

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
[[nodiscard]] std::expected<void, DataflowError> validate_constructor_member_initialization(const Function& ctor, const ClassDef& def, const Program& program);
[[nodiscard]] std::expected<void, DataflowError> validate_struct_constructor_member_initialization(const Function& ctor, const StructDef& def, const Program& program);
[[nodiscard]] bool is_copy_constructible(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_copy_assignable(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_freely_copyable_value_type(const Type& type, const Program& program);
[[nodiscard]] std::string_view record_keyword(const std::string& record_name, const Program& program);

[[nodiscard]] const FunctionSignature* resolve_constructor_signature(const std::string& class_name,
                                                                     const std::vector<ExprPtr>& ctor_args,
                                                                     const Body& body,
                                                                     const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> ensure_implicit_default_construction_is_valid(const std::string& class_name,
                                                   std::string_view current_class,
                                                   const Body& body,
                                                   const Signatures& signatures,
                                                   const SourceLocation& loc,
                                                   std::string_view context_message);
[[nodiscard]] std::expected<void, DataflowError> validate_constructor_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
                                              const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> validate_constructor_virtual_interface_base_initialization(const Function& ctor, const ClassDef& def,
                                                                const Body& body,
                                                                const Signatures& signatures);
[[nodiscard]] std::expected<std::optional<std::size_t>, DataflowError> resolve_elided_param_index(const Function& fn);
[[nodiscard]] std::vector<std::size_t> infer_pointer_return_source_param_indices(const Function& fn);
[[nodiscard]] bool param_can_outlive_call_for_lifetime_return(const Param& param);
[[nodiscard]] std::expected<void, DataflowError> validate_lifetime_annotation_placement(const Function& fn);
[[nodiscard]] std::expected<std::vector<std::size_t>, DataflowError> resolve_returned_lifetime_param_indices(const Function& fn);
[[nodiscard]] std::expected<Signatures, DataflowError> build_signatures(const Program& program);

// spec §6.4/§6.5/§6.6 govern every *class type*, which in
// [class.pre]/[dcl.type] terms includes `struct`. A diagnostic about one
// of those rules must name the keyword the user actually wrote, so that a
// struct and a class violating the same rule read as the same rule rather
// than as two different ones.
[[nodiscard]] std::string_view record_keyword(const std::string& record_name, const Program& program) {
    for (const StructDef& def : program.structs) {
        if (def.name == record_name) return "struct";
    }
    return "class";
}

// spec §6.5: whether `class_name` has declared its own copy constructor
// -- a function named "class_name_new" (see parse_class_def) whose sole
// non-`this` parameter is `const class_name&` (an ordinary, non-rvalue,
// read-only reference to the class's own type -- the shape spec §6.5's
// own worked example, and the overwhelmingly common real-world one,
// uses; a mutable-reference-parameter copy constructor, while legal
// real C++, is out of scope for this recognition).
[[nodiscard]] const Function* find_user_declared_copy_ctor(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (is_copy_constructor_function(fn) && fn.member_owner_class == class_name) {
            return &fn;
        }
    }
    return nullptr;
}

[[nodiscard]] bool has_user_declared_copy_ctor(const std::string& class_name, const Program& program) {
    return find_user_declared_copy_ctor(class_name, program) != nullptr;
}

// spec §6.5: whether `class_name` has declared its own copy assignment
// operator -- a function named "class_name_operator_assign" (see
// parse_class_body_into's operator= parsing) whose sole non-`this`
// parameter is `const class_name&`, mirroring has_user_declared_copy_ctor
// exactly (an operator= overload taking any other shape is simply an
// ordinary, unrelated overload of the name -- not *the* copy assignment
// operator this recognizes).
[[nodiscard]] const Function* find_user_declared_copy_assign(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (is_copy_assignment_function(fn) && fn.member_owner_class == class_name) {
            return &fn;
        }
    }
    return nullptr;
}

[[nodiscard]] bool has_user_declared_copy_assign(const std::string& class_name, const Program& program) {
    return find_user_declared_copy_assign(class_name, program) != nullptr;
}

[[nodiscard]] const Function* find_user_declared_dtor(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (!fn.name.ends_with("_delete") || fn.params.size() != 1) continue;
        if (fn.member_owner_class != class_name) continue;
        if (is_special_member_this_param(fn.params[0].type, class_name)) return &fn;
    }
    return nullptr;
}

[[nodiscard]] bool has_user_declared_dtor(const std::string& class_name, const Program& program) {
    return find_user_declared_dtor(class_name, program) != nullptr;
}

// [class.dtor]/14: a program is ill-formed if a potentially-invoked
// destructor is deleted. scpp destroys every class-typed object at the
// end of its scope, so *creating* one is exactly such a potential
// invocation -- the error belongs at the declaration, not at some later
// point where the destructor is implicitly named and nothing in the
// source text mentions it.
[[nodiscard]] bool has_deleted_dtor(const std::string& class_name, const Program& program) {
    const Function* dtor = find_user_declared_dtor(class_name, program);
    return dtor != nullptr && dtor->is_deleted;
}
// Certain stdlib "view" wrappers intentionally behave like scalar pairs at
// by-value boundaries even though they are spelled as classes with
// user-declared special members in the library source. Treat those named
// wrappers as freely copyable without relaxing ordinary class copy rules.
[[nodiscard]] bool is_freely_copyable_value_type(const Type& type, const Program&) {
    if (type.kind != TypeKind::Named) return false;
    std::string base_name = unqualified_template_base_name(type.name);
    return type.name == "std::string_view" || type.name == "std::format_string<>" || base_name == "format_string";
}

[[nodiscard]] bool is_copy_constructible(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_copy_assignable(const std::string& class_name, const Program& program);

// spec §6.5(5)'s own note: a field's own copy-constructibility -- a
// reference always is (bound once, from the source's own referent,
// exactly like move construction's identical carve-out, spec §6.4); a
// nested *record* -- class or struct alike, since §6.5 governs every
// "class type" and [class.pre] makes that include `struct` -- recurses
// (except the same named "freely copyable value type" stdlib
// view-wrappers is_freely_copyable_value_type already carves out at
// every other copy-related boundary -- e.g.
// is_freely_copyable_class_value_source -- a struct/class field of one
// of those types, such as std::string_view, is copy-constructible
// exactly like the wrapper itself always is, regardless of its own
// user-declared special members); everything else (scalar, raw pointer,
// array of any of these) is unconditionally copy-constructible.
[[nodiscard]] bool is_field_copy_constructible(const Type& type, const Program& program) {
    if (type.kind == TypeKind::Reference) return true;
    if (type.kind == TypeKind::Array) return is_field_copy_constructible(*type.element, program);
    if (type.kind == TypeKind::Named) {
        if (is_freely_copyable_value_type(type, program)) return true;
        for (const ClassDef& def : program.classes) {
            if (def.name == type.name) return is_copy_constructible(type.name, program);
        }
        for (const StructDef& def : program.structs) {
            if (def.name == type.name) return is_copy_constructible(type.name, program);
        }
        return true; // scalar or an unrecognized/generic-witness name
    }
    return true; // Pointer, Span, ...: always bitwise-copyable, no restriction
}

// Same as is_field_copy_constructible, but for assignment -- a reference
// field is the one case that differs (never assignable, spec §6.4/§6.5's
// identical "can't be re-seated" carve-out); is_copy_assignable's own
// direct-field loop already rejects a *directly* reference-typed field
// before ever consulting this helper, but nested recursion still needs
// its own answer for one reachable transitively.
[[nodiscard]] bool is_field_copy_assignable(const Type& type, const Program& program) {
    if (type.kind == TypeKind::Reference) return false;
    if (type.kind == TypeKind::Array) return is_field_copy_assignable(*type.element, program);
    if (type.kind == TypeKind::Named) {
        if (is_freely_copyable_value_type(type, program)) return true;
        for (const ClassDef& def : program.classes) {
            if (def.name == type.name) return is_copy_assignable(type.name, program);
        }
        for (const StructDef& def : program.structs) {
            if (def.name == type.name) return is_copy_assignable(type.name, program);
        }
        return true;
    }
    return true;
}

[[nodiscard]] bool class_has_any_constructor(const std::string& class_name, const Program& program) {
    return std::any_of(program.functions.begin(), program.functions.end(), [&](const Function& fn) {
        return is_constructor_function(fn) && fn.member_owner_class == class_name;
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
    auto base = def.direct_ordinary_base();
    if (!base.has_value() || base->get().base_type.name.empty()) return false;
    return member_name == base->get().base_type.name || member_name == unqualified_template_base_name(base->get().base_type.name);
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
    auto base = def.direct_ordinary_base();
    if (!base.has_value()) return nullptr;
    for (const MemberInitializer& init : ctor.member_initializers) {
        if (names_direct_base(init.member_name, def)) return &init;
    }
    return nullptr;
}

// The order in which a constructor's member-initializer entries actually
// run, as a rank per entry (nullopt for an entry naming nothing this
// class has -- validate_constructor_member_initialization rejects those
// separately, with a better message).
//
// Not a model invented here: it is read straight off what codegen emits.
// emit_complete_object_interface_initializers runs the virtual interface
// bases first, in collect_virtual_interface_bases_in_construction_order
// order; emit_constructor_member_initializers then runs the direct
// ordinary base, and then walks `class_def->fields` in declaration
// order, selecting each field's initializer by *name*. The order the
// entries were written in is never consulted.
[[nodiscard]] std::vector<std::optional<std::size_t>> member_initializer_execution_ranks(
    const Function& ctor, const ClassDef& def, const std::vector<const ClassDef*>& interface_bases) {
    std::vector<std::optional<std::size_t>> ranks;
    ranks.reserve(ctor.member_initializers.size());
    for (const MemberInitializer& init : ctor.member_initializers) {
        std::optional<std::size_t> rank;
        for (std::size_t i = 0; i < interface_bases.size(); i++) {
            const ClassDef* interface_def = interface_bases[i];
            if (interface_def == nullptr) continue;
            if (init.member_name == interface_def->name ||
                init.member_name == unqualified_template_base_name(interface_def->name)) {
                rank = i;
                break;
            }
        }
        if (!rank.has_value() && names_direct_base(init.member_name, def)) rank = interface_bases.size();
        if (!rank.has_value()) {
            for (std::size_t i = 0; i < def.fields.size(); i++) {
                if (def.fields[i].name == init.member_name) {
                    rank = interface_bases.size() + 1 + i;
                    break;
                }
            }
        }
        ranks.push_back(rank);
    }
    return ranks;
}

// Whether `def`, or anything it inherits from, declares a data member
// called `name`.
[[nodiscard]] bool class_or_bases_declare_field(const Program& program, const ClassDef& def, const std::string& name) {
    for (const ClassField& field : def.fields) {
        if (field.name == name) return true;
    }
    for (const BaseSpecifier& base : def.base_specifiers) {
        const ClassDef* base_def = find_class_def(program, base.base_type.name);
        if (base_def == nullptr || base_def->is_forward_declaration) continue;
        if (class_or_bases_declare_field(program, *base_def, name)) return true;
    }
    return false;
}

// The construction rank at which the data member `name` becomes alive,
// on the same scale member_initializer_execution_ranks uses: a field
// declared by `def` itself gets its own declaration rank, and a field
// inherited from a base gets *that base's* rank, because a base
// subobject is constructed all at once.
//
// nullopt means `name` is not a data member reachable from here, which
// is somebody else's diagnostic rather than a construction-order one.
[[nodiscard]] std::optional<std::size_t> member_construction_rank(const Program& program, const ClassDef& def,
                                                                 const std::vector<const ClassDef*>& interface_bases,
                                                                 const std::string& name) {
    for (std::size_t i = 0; i < def.fields.size(); i++) {
        if (def.fields[i].name == name) return interface_bases.size() + 1 + i;
    }
    for (std::size_t i = 0; i < interface_bases.size(); i++) {
        const ClassDef* interface_def = interface_bases[i];
        if (interface_def != nullptr && class_or_bases_declare_field(program, *interface_def, name)) return i;
    }
    auto base = def.direct_ordinary_base();
    if (base.has_value()) {
        const ClassDef* base_def = find_class_def(program, base->get().base_type.name);
        if (base_def != nullptr && !base_def->is_forward_declaration &&
            class_or_bases_declare_field(program, *base_def, name)) {
            return interface_bases.size();
        }
    }
    return std::nullopt;
}

// What check_this_usage_in_expr needs to know about the class or struct
// under construction, so that both get the same walker and the same
// answer to "when does this member become alive". A struct is the
// degenerate case: no bases, so a field's rank is just its declaration
// index.
struct ConstructedOwner {
    const Program* program = nullptr;
    const ClassDef* class_def = nullptr;
    const StructDef* struct_def = nullptr;
    const std::vector<const ClassDef*>* interface_bases = nullptr;

    [[nodiscard]] const std::string& name() const { return class_def != nullptr ? class_def->name : struct_def->name; }

    [[nodiscard]] std::optional<std::size_t> rank_of_member(const std::string& member_name) const {
        if (class_def != nullptr) {
            return member_construction_rank(*program, *class_def, *interface_bases, member_name);
        }
        for (std::size_t i = 0; i < struct_def->fields.size(); i++) {
            if (struct_def->fields[i].name == member_name) return i;
        }
        return std::nullopt;
    }
};

// spec §6.1: inside a member-initializer, `this` denotes an object that
// does not exist yet. Only the part of it already constructed may be
// touched, and by the time the entry at rank `rank` runs that is exactly
// the bases and fields whose own rank is lower -- a fact that is now
// positional rather than a dataflow property, because
// check_member_initializer_order forces the written order to be the
// construction order.
//
// So `this` may appear only as the immediate base of a member access
// naming an already-constructed data member. Everything else is
// rejected, and has to be: `this->compute()`, `peek(this)` and
// `peek(*this)` all hand the whole object to code free to read any
// field, including ones that are still raw storage, so permitting them
// would let the direct read be restated and trivially defeat the rule.
// There is no point inside the list at which the object is complete --
// even the last field's initializer runs before that field exists --
// so no escape can be allowed anywhere in the list rather than only
// early in it.
//
// Reading a member is not distinguished from taking its address: both
// name a field that is not yet an object, and `&this->b_` outlives the
// initializer that formed it.
[[nodiscard]] std::expected<void, DataflowError> check_this_usage_in_expr(const Expr& expr, std::size_t rank,
                                                                         const ConstructedOwner& owner,
                                                                         const std::string& entry_description) {
    if (expr.kind == ExprKind::Identifier && expr.name == "this") {
        return std::unexpected(DataflowError(
            "'this' escapes the member-initializer for " + entry_description + " of '" + owner.name() +
                "', where the object is still under construction -- inside a member-initializer-list 'this' may only "
                "be used to read an already-initialized member as 'this->member' (spec §6.1)",
            expr.loc));
    }
    if (expr.kind == ExprKind::Call && expr.lhs != nullptr && expr.lhs->kind == ExprKind::Identifier &&
        expr.lhs->name == "this") {
        // parse_member_or_method_call stores the receiver of `this->m()`
        // directly in the Call's lhs, so this is the shape a method call
        // on the object under construction actually takes -- naming that
        // rather than letting it fall through to the generic escape below.
        return std::unexpected(DataflowError(
            "member-initializer for " + entry_description + " of '" + owner.name() + "' calls method '" + expr.name +
                "' on an object that is still under construction; a method may read any member, including ones not "
                "initialized yet, so no method may be called before the last member-initializer has run (spec §6.1)",
            expr.loc));
    }
    if (expr.kind == ExprKind::Member && expr.lhs != nullptr && expr.lhs->kind == ExprKind::Identifier &&
        expr.lhs->name == "this") {
        std::optional<std::size_t> member_rank = owner.rank_of_member(expr.name);
        if (member_rank.has_value() && *member_rank >= rank) {
            return std::unexpected(DataflowError(
                "member-initializer for " + entry_description + " of '" + owner.name() + "' uses member '" +
                    expr.name + "', which is not initialized until later; members are constructed in declaration "
                    "order, so an initializer may only use members declared before the one it initializes "
                    "(spec §6.1)",
                expr.loc));
        }
        // A name that is not a data member of this class is somebody
        // else's diagnostic (a bare `this->m` naming a method is already
        // rejected as an unknown field); saying anything about
        // construction order here would name a member that does not
        // exist.
        return {};
    }
    for (const Expr* child : {expr.lhs.get(), expr.rhs.get(), expr.third.get()}) {
        if (child == nullptr) continue;
        if (auto _r = check_this_usage_in_expr(*child, rank, owner, entry_description);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    for (const ExprPtr& arg : expr.args) {
        if (arg == nullptr) continue;
        if (auto _r = check_this_usage_in_expr(*arg, rank, owner, entry_description);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    for (const LambdaCapture& capture : expr.lambda_captures) {
        if (capture.name == "this") {
            return std::unexpected(DataflowError(
                "member-initializer for " + entry_description + " of '" + owner.name() +
                    "' captures 'this' in a lambda, but the object is still under construction (spec §6.1)",
                expr.loc));
        }
        if (capture.init == nullptr) continue;
        if (auto _r = check_this_usage_in_expr(*capture.init, rank, owner, entry_description);
            !_r.has_value()) {
            return std::unexpected(std::move(_r).error());
        }
    }
    // Deliberately not descending into expr.lambda_body: a lambda body
    // can only reach an enclosing member through a capture of `this`,
    // which the loop above rejects outright, so the capture list is the
    // choke point and walking the body would be a second answer to the
    // same question rather than extra reach.
    return {};
}

[[nodiscard]] std::expected<void, DataflowError> check_member_initializer_this_usage(
    const Function& ctor, const ConstructedOwner& owner, const std::vector<std::optional<std::size_t>>& ranks) {
    // A lambda that captures `this` desugars to a closure class with a
    // field named "this" and a constructor taking a *second* parameter
    // named "this" holding the enclosing object, which shadows the
    // receiver in params[0] (monomorphize.cppm's closure synthesis).
    // In such a constructor the name `this` in a member-initializer
    // denotes that parameter, not the object being constructed, so
    // nothing here has anything to say about it -- the enclosing `this`
    // is a fully constructed object being passed in by value.
    for (std::size_t i = 1; i < ctor.params.size(); i++) {
        if (ctor.params[i].name == "this") return {};
    }
    for (std::size_t i = 0; i < ranks.size(); i++) {
        if (!ranks[i].has_value()) continue;
        const MemberInitializer& init = ctor.member_initializers[i];
        std::string description = "'" + init.member_name + "'";
        if (init.initializer.expr != nullptr) {
            if (auto _r = check_this_usage_in_expr(*init.initializer.expr, *ranks[i], owner, description);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        for (const ExprPtr& arg : init.initializer.brace_args) {
            if (arg == nullptr) continue;
            if (auto _r = check_this_usage_in_expr(*arg, *ranks[i], owner, description); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
    }
    return {};
}

// spec §6.1: a member-initializer-list must be written in the order it
// runs -- interface bases, then the direct base, then fields in
// declaration order.
//
// scpp rejects this rather than silently reordering (which is what
// codegen does today, and what C++ does with a warning) because the two
// disagree observably: for `: b_{trace(2)}, a_{trace(1)}` the program
// prints 1 then 2. A list that says one thing and does another is the
// same defect as a diagnostic that names nothing in the program -- the
// text is not describing the program. The language has no warnings, so
// its options are to accept a misleading spelling or reject it, and it
// rejects everywhere else it can't tell what was meant (ch06 §6's
// scalar conversions, ch05 §5.10's exact-match overload resolution).
//
// Requiring it also earns something structural: mir.cppm can lower the
// list in written order and be *identical* to codegen's declaration
// order, instead of carrying a second implementation of codegen's field
// walk that is free to drift from it.
//
// Repeating a member -- or a base -- is already rejected by the parser
// (parser.cppm:6337), which has the better message, so equal ranks are
// unreachable in practice; the comparison is still `<=` rather than `<`
// so the rule states a total order rather than depending on that.
[[nodiscard]] std::expected<void, DataflowError> check_member_initializer_order(
    const Function& ctor, const std::string& owner_name, const std::vector<std::optional<std::size_t>>& ranks) {
    std::size_t previous_index = 0;
    std::optional<std::size_t> previous;
    for (std::size_t i = 0; i < ranks.size(); i++) {
        if (!ranks[i].has_value()) continue;
        if (previous.has_value() && *ranks[i] <= *previous) {
            const MemberInitializer& earlier = ctor.member_initializers[previous_index];
            const MemberInitializer& later = ctor.member_initializers[i];
            std::string reason = *ranks[i] == *previous
                                     ? "'" + later.member_name + "' is initialized twice"
                                     : "'" + later.member_name + "' runs before '" + earlier.member_name +
                                           "' but is written after it";
            return std::unexpected(DataflowError(
                "member-initializer-list for class '" + owner_name + "' is not in initialization order: " + reason +
                    " -- entries must be written in the order they run (base classes first, then fields in "
                    "declaration order), because that is the order they are evaluated in (spec §6.1)",
                later.loc.is_known() ? later.loc : ctor.loc));
        }
        previous = ranks[i];
        previous_index = i;
    }
    return {};
}

// spec §6.1: the same initialization-order rule for a struct
// constructor. A struct has no bases, so the ranks are just the field
// declaration indices -- but codegen's struct branch of
// emit_constructor_member_initializers walks `struct_def->fields` in
// declaration order exactly as the class branch does, so writing them
// out of order reorders them just as silently.
//
// spec §6.1(3.1) and §6.1(4) both say "class **or struct**" in as many
// words -- this is not even the "a class type includes struct" reading
// the §6.4/§6.5 rules need. Only the order and `this`-usage halves were
// asked here, so `struct S { int a; int b; S(int x) : a{x} {} };`
// compiled and `s.b` read uninitialized storage, while the identical
// class was rejected. The two functions now ask the same questions in
// the same order; the array-typed-field exemption mirrors the class
// version's exactly.
[[nodiscard]] std::expected<void, DataflowError> validate_struct_constructor_member_initialization(const Function& ctor,
                                                                                                  const StructDef& def,
                                                                                                  const Program& program) {
    if (!is_constructor_function(ctor) || ctor.member_owner_class != def.name || def.is_forward_declaration ||
        is_defaulted_special_member_equivalent_to_implicit_omission(ctor)) {
        return {};
    }
    if (!ctor.body) return {};
    if (!ctor.generic_method_owner_id.empty() && ctor.generic_method_owner_id != def.template_owner_id) return {};
    std::unordered_set<std::string> direct_field_names;
    for (const StructField& field : def.fields) direct_field_names.insert(field.name);
    for (const MemberInitializer& init : ctor.member_initializers) {
        if (!direct_field_names.contains(init.member_name)) {
            return std::unexpected(DataflowError("constructor for struct '" + def.name + "' names unknown member '" +
                                    init.member_name + "' in its member-initializer-list",
                                init.loc.is_known() ? init.loc : ctor.loc));
        }
    }
    std::vector<std::string> missing;
    for (const StructField& field : def.fields) {
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
        return std::unexpected(DataflowError("constructor for struct '" + def.name + "' leaves member(s) " + names +
                                " uninitialized; each constructor must initialize every non-static data member "
                                "either via its own member-initializer-list or an in-class default member "
                                "initializer (spec §6.1(4))",
                            ctor.loc));
    }
    std::vector<std::optional<std::size_t>> ranks;
    ranks.reserve(ctor.member_initializers.size());
    for (const MemberInitializer& init : ctor.member_initializers) {
        std::optional<std::size_t> rank;
        for (std::size_t i = 0; i < def.fields.size(); i++) {
            if (def.fields[i].name == init.member_name) {
                rank = i;
                break;
            }
        }
        ranks.push_back(rank);
    }
    if (auto _r = check_member_initializer_order(ctor, def.name, ranks); !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    return check_member_initializer_this_usage(ctor, ConstructedOwner{&program, nullptr, &def, nullptr}, ranks);
}

[[nodiscard]] std::expected<void, DataflowError> validate_constructor_member_initialization(const Function& ctor, const ClassDef& def, const Program& program) {
    if (!is_constructor_function(ctor) || ctor.member_owner_class != def.name || def.is_forward_declaration ||
        is_defaulted_special_member_equivalent_to_implicit_omission(ctor)) {
        return {};
    }
    // A bodyless constructor is a stripped interface declaration (driver.cppm's
    // strip_concrete_function_bodies deliberately erases a concrete constructor's
    // member-initializer-list along with its body when reconstructing another
    // module's re-importable interface text -- see its own comment), re-parsed
    // here as a downstream dependency; its member coverage was already fully
    // validated when its own defining module was compiled from the real,
    // unstripped source, so there is nothing left here to (mis)diagnose as
    // "uninitialized" from a declaration that never had an init-list to begin
    // with.
    if (!ctor.body) return {};
    if (!ctor.generic_method_owner_id.empty() && ctor.generic_method_owner_id != def.template_owner_id) return {};
    std::unordered_set<std::string> direct_field_names;
    for (const ClassField& field : def.fields) direct_field_names.insert(field.name);
    std::vector<const ClassDef*> interface_bases = collect_virtual_interface_bases_in_construction_order(program, def);
    const MemberInitializer* explicit_base_init = find_explicit_base_initializer(ctor, def);
    auto base = def.direct_ordinary_base();
    if (explicit_base_init != nullptr && base.has_value() && direct_field_names.contains(base->get().base_type.name)) {
        return std::unexpected(DataflowError("constructor for class '" + def.name + "' cannot disambiguate '" + base->get().base_type.name +
                                "' in its member-initializer-list because that name matches both a direct field and "
                                "the direct base class",
                            explicit_base_init->loc.is_known() ? explicit_base_init->loc : ctor.loc));
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
            return std::unexpected(DataflowError("constructor for class '" + def.name + "' names unknown member '" + init.member_name +
                                    "' in its member-initializer-list",
                                init.loc.is_known() ? init.loc : ctor.loc));
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
        return std::unexpected(DataflowError("constructor for class '" + def.name + "' leaves member(s) " + names +
                                " uninitialized; each constructor must initialize every non-static data member "
                                "either via its own member-initializer-list or an in-class default member "
                                "initializer (spec §6.1(4))",
                            ctor.loc));
    }
    std::vector<std::optional<std::size_t>> ranks = member_initializer_execution_ranks(ctor, def, interface_bases);
    if (auto _r = check_member_initializer_order(ctor, def.name, ranks); !_r.has_value()) {
        return std::unexpected(std::move(_r).error());
    }
    return check_member_initializer_this_usage(
        ctor, ConstructedOwner{&program, &def, nullptr, &interface_bases}, ranks);
}

[[nodiscard]] const FunctionSignature* resolve_constructor_signature(const std::string& class_name,
                                                                     const std::vector<ExprPtr>& ctor_args,
                                                                     const Body& body, const Signatures& signatures);
[[nodiscard]] std::expected<void, DataflowError> ensure_implicit_default_construction_is_valid(const std::string& class_name, std::string_view current_class,
                                                   const Body& body, const Signatures& signatures,
                                                   const SourceLocation& loc, std::string_view context_message);
[[nodiscard]] std::expected<void, DataflowError> validate_constructor_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
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
    if (const Function* user_copy = find_user_declared_copy_ctor(class_name, program); user_copy != nullptr) {
        // [dcl.fct.def.delete]/1: a deleted definition is still a
        // declaration -- it suppresses nothing less than a defined one
        // would -- but the class then has no copy constructor that spec
        // §6.6(4)'s "has a copy constructor" test can be satisfied by.
        return !user_copy->is_deleted;
    }
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
    if (const Function* user_assign = find_user_declared_copy_assign(class_name, program); user_assign != nullptr) {
        return !user_assign->is_deleted;
    }
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
// The constructor-selection half of the shared [over.ics.rank]
// vocabulary. Parameter 0 is the constructor's own `this`, which is not
// an argument of the call, so it is skipped rather than ranked.
[[nodiscard]] std::vector<ArgumentConversion> constructor_argument_conversions(const FunctionSignature& candidate,
                                                                               const std::vector<ExprPtr>& ctor_args,
                                                                               const Body& body,
                                                                               const Signatures& signatures) {
    auto strip_to_value = [](Type type) {
        if (type.kind == TypeKind::Reference && type.pointee != nullptr) type = *type.pointee;
        type.is_const_qualified = false;
        return type;
    };
    std::vector<ArgumentConversion> result;
    for (std::size_t i = 0; i < ctor_args.size(); i++) {
        ArgumentConversion conversion{};
        conversion.rank = ConversionRank::Identity;
        if (i + 1 >= candidate.param_types.size()) {
            conversion.unknown = true;
            result.push_back(conversion);
            continue;
        }
        const Type& param_type = candidate.param_types[i + 1];
        Type target = param_type;
        if (param_type.kind == TypeKind::Reference) {
            conversion.binds_reference = true;
            conversion.reference_is_mutable = param_type.is_mutable_ref && !param_type.is_rvalue_ref;
            conversion.reference_is_rvalue = param_type.is_rvalue_ref;
            if (param_type.pointee != nullptr) target = *param_type.pointee;
        }
        Type target_value = target;
        target_value.is_const_qualified = false;
        conversion.argument_is_rvalue = produces_rvalue_of_type(*ctor_args[i], target_value, body, signatures);
        std::optional<Type> arg_type = infer_expr_type(*ctor_args[i], body, signatures);
        if (!arg_type.has_value()) {
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

[[nodiscard]] const FunctionSignature* resolve_constructor_signature(const std::string& class_name,
                                                                     const std::vector<ExprPtr>& ctor_args,
                                                                     const Body& body, const Signatures& signatures) {
    auto is_constructor_clone_name = [&](std::string_view name) {
        return name == class_name + "_new" ||
               (!name.empty() && name.starts_with(class_name + "_new."));
    };
    std::vector<const FunctionSignature*> matches;
    auto should_trace = [&] {
        return class_name == "std::thread" && ctor_args.size() == 1;
    };
    for (const auto& [name, overloads] : signatures) {
        if (!is_constructor_clone_name(name)) continue;
        for (const FunctionSignature& candidate : overloads) {
            if (candidate.member_owner_class != class_name) continue;
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
    }
    if (matches.empty()) return nullptr;
    if (should_trace()) {
        std::cerr << "[ctor-select] class=" << class_name << " arg0="
                  << (infer_expr_type(*ctor_args[0], body, signatures).has_value()
                          ? describe_type_brief(*infer_expr_type(*ctor_args[0], body, signatures))
                          : std::string("<unknown>"))
                  << " candidates=" << matches.size() << "\n";
        for (const FunctionSignature* candidate : matches) {
            std::cerr << "  candidate " << describe_constructor_candidate(*candidate) << "\n";
        }
    }
    if (matches.size() == 1) return matches[0];
    // [over.match.best]/2.4: a non-template is better than a template.
    {
        std::vector<const FunctionSignature*> non_generic;
        for (const FunctionSignature* candidate : matches) {
            if (!candidate->is_generic_template) non_generic.push_back(candidate);
        }
        if (!non_generic.empty()) matches = std::move(non_generic);
        if (matches.size() == 1) return matches[0];
    }
    // [over.ics.rank] through the shared algebra, the same one codegen's
    // two resolvers, movecheck's resolve_overload and the constant
    // evaluator use.
    //
    // This was a *sixth* independent answer to "which candidate is
    // better?", and the most arbitrary of them: it returned the first
    // candidate that matched exactly (not the best), then the first
    // non-generic one (not the best), then a mutable-reference score
    // that fell back to `matches[0]` on a tie. Three "first one wins"
    // rules in a row, in the pass whose job is to check the very
    // constructor codegen will emit.
    std::vector<std::vector<ArgumentConversion>> conversions;
    for (const FunctionSignature* candidate : matches) {
        conversions.push_back(constructor_argument_conversions(*candidate, ctor_args, body, signatures));
    }
    std::vector<std::size_t> best_indices = best_viable_candidates(conversions);
    if (best_indices.size() != 1) {
        if (should_trace()) std::cerr << "  no unique best candidate (" << best_indices.size() << " tied)\n";
        return nullptr;
    }
    const FunctionSignature* winner = matches[best_indices[0]];
    if (should_trace()) std::cerr << "  winner " << describe_constructor_candidate(*winner) << "\n";
    return winner;
}

[[nodiscard]] std::expected<void, DataflowError> ensure_implicit_default_construction_is_valid(const std::string& class_name, std::string_view current_class,
                                                   const Body& body, const Signatures& signatures,
                                                   const SourceLocation& loc, std::string_view context_message) {
    if (body.program == nullptr) return {};
    const ClassDef* class_def = find_class_def(*body.program, class_name);
    if (class_def == nullptr) return {};
    if (class_has_any_constructor(class_name, *body.program)) {
        static const std::vector<ExprPtr> no_ctor_args;
        const FunctionSignature* sig = resolve_constructor_signature(class_name, no_ctor_args, body, signatures);
        if (sig == nullptr) {
            return std::unexpected(DataflowError(std::string(context_message) + ": base class '" + class_name +
                                    "' has no default constructor; write an explicit base-class initializer",
                                loc));
        }
        if (sig->access == AccessSpecifier::Private && !sig->member_owner_class.empty() &&
            current_class != sig->member_owner_class) {
            return std::unexpected(DataflowError(std::string(context_message) + ": base class '" + class_name +
                                    "' default constructor is private; write an explicit base-class initializer "
                                    "calling an accessible constructor",
                                loc));
        }
        if (sig->is_unsafe) {
            return std::unexpected(DataflowError(std::string(context_message) + ": base class '" + class_name +
                                    "' default constructor is [[scpp::unsafe]]",
                                loc));
        }
        return {};
    }
    if (auto base = class_def->direct_ordinary_base()) {
        return ensure_implicit_default_construction_is_valid(base->get().base_type.name, current_class, body, signatures, loc,
                                                      context_message);
    }
    return {};
}

[[nodiscard]] std::expected<void, DataflowError> validate_constructor_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
                                              const Signatures& signatures) {
    auto base = def.direct_ordinary_base();
    if (!is_constructor_function(ctor) || ctor.member_owner_class != def.name || !base.has_value() ||
        is_defaulted_special_member_equivalent_to_implicit_omission(ctor)) {
        return {};
    }
    if (!ctor.generic_method_owner_id.empty() && ctor.generic_method_owner_id != def.template_owner_id) return {};
    const MemberInitializer* explicit_base_init = find_explicit_base_initializer(ctor, def);
    std::string context_message =
        "constructor for class '" + def.name + "' must initialize its direct base class '" + base->get().base_type.name + "'";
    if (explicit_base_init == nullptr) {
        return ensure_implicit_default_construction_is_valid(base->get().base_type.name, def.name, body, signatures, ctor.loc,
                                                      context_message);
    }
    const FunctionSignature* sig =
        resolve_constructor_signature(base->get().base_type.name, explicit_base_init->initializer.brace_args, body, signatures);
    if (sig == nullptr) {
        if (body.program != nullptr && !class_has_any_constructor(base->get().base_type.name, *body.program) &&
            explicit_base_init->initializer.brace_args.empty()) {
            return ensure_implicit_default_construction_is_valid(base->get().base_type.name, def.name, body, signatures,
                                                          explicit_base_init->loc, context_message);
        }
        return std::unexpected(DataflowError("base-class initializer for '" + base->get().base_type.name +
                                "' does not match any constructor of that class",
                            explicit_base_init->loc.is_known() ? explicit_base_init->loc : ctor.loc));
    }
    if (sig->access == AccessSpecifier::Private && !sig->member_owner_class.empty() && def.name != sig->member_owner_class) {
        return std::unexpected(DataflowError("cannot call private constructor of base class '" + base->get().base_type.name +
                                "' from derived class '" + def.name + "'",
                            explicit_base_init->loc.is_known() ? explicit_base_init->loc : ctor.loc));
    }
    if (sig->is_unsafe) {
        return std::unexpected(DataflowError("cannot call base class '" + base->get().base_type.name +
                                "' constructor outside '[[scpp::unsafe]] { }': its own declaration is marked "
                                "'[[scpp::unsafe]]'",
                            explicit_base_init->loc.is_known() ? explicit_base_init->loc : ctor.loc));
    }
    return {};
}

[[nodiscard]] std::expected<void, DataflowError> validate_constructor_virtual_interface_base_initialization(const Function& ctor, const ClassDef& def, const Body& body,
                                                                const Signatures& signatures) {
    if (!is_constructor_function(ctor) || ctor.member_owner_class != def.name || body.program == nullptr ||
        is_defaulted_special_member_equivalent_to_implicit_omission(ctor)) {
        return {};
    }
    if (!ctor.generic_method_owner_id.empty() && ctor.generic_method_owner_id != def.template_owner_id) return {};
    std::vector<const ClassDef*> interface_bases = collect_virtual_interface_bases_in_construction_order(*body.program, def);
    for (const ClassDef* interface_def : interface_bases) {
        if (interface_def == nullptr) continue;
        const MemberInitializer* explicit_init = find_explicit_interface_initializer(ctor, *interface_def);
        std::string context_message =
            "constructor for class '" + def.name + "' must initialize virtual interface base '" + interface_def->name + "'";
        if (explicit_init == nullptr) {
            if (!def.is_interface) {
                if (auto _r = ensure_implicit_default_construction_is_valid(interface_def->name, def.name, body, signatures, ctor.loc,
                                                              context_message);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            continue;
        }
        const FunctionSignature* sig = resolve_constructor_signature(interface_def->name, explicit_init->initializer.brace_args,
                                                                     body, signatures);
        if (sig == nullptr) {
            if (!class_has_any_constructor(interface_def->name, *body.program) &&
                explicit_init->initializer.brace_args.empty()) {
                if (auto _r = ensure_implicit_default_construction_is_valid(interface_def->name, def.name, body, signatures,
                                                              explicit_init->loc.is_known() ? explicit_init->loc : ctor.loc,
                                                              context_message);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                continue;
            }
            return std::unexpected(DataflowError("base-class initializer for '" + interface_def->name +
                                    "' does not match any constructor of that class",
                                explicit_init->loc.is_known() ? explicit_init->loc : ctor.loc));
        }
        if (sig->access == AccessSpecifier::Private && !sig->member_owner_class.empty() &&
            def.name != sig->member_owner_class) {
            return std::unexpected(DataflowError("cannot call private constructor of base class '" + interface_def->name +
                                    "' from derived class '" + def.name + "'",
                                explicit_init->loc.is_known() ? explicit_init->loc : ctor.loc));
        }
        if (sig->is_unsafe) {
            return std::unexpected(DataflowError("cannot call base class '" + interface_def->name +
                                    "' constructor outside '[[scpp::unsafe]] { }': its own declaration is marked "
                                    "'[[scpp::unsafe]]'",
                                explicit_init->loc.is_known() ? explicit_init->loc : ctor.loc));
        }
    }
    return {};
}
[[nodiscard]] std::expected<std::optional<std::size_t>, DataflowError> resolve_elided_param_index(const Function& fn) {
    if (fn.return_lifetime.present()) return std::optional<std::size_t>(std::nullopt);
    if (!is_reference(fn.return_type)) return std::optional<std::size_t>(std::nullopt);

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
            return std::unexpected(DataflowError("function '" + fn.name +
                                 "' returns a mutable reference ('T&') but its 'this' is a read-only ('const') "
                                 "receiver; a mutable reference cannot be manufactured from a shared one",
                fn.loc));
        }
        return std::optional<std::size_t>(0);
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
            return std::unexpected(DataflowError(
                "function '" + fn.name +
                "' returns a reference but has more than one reference parameter; scpp v0.1 can only infer a "
                "returned reference's lifetime when there is exactly one (spec ch05.3) -- refactor to take a "
                "single reference parameter, or return by value/std::unique_ptr instead",
                fn.loc));
        }
        found = i;
    }
    if (!found.has_value()) {
        return std::unexpected(DataflowError(
            "function '" + fn.name +
            "' returns a reference but has no reference parameter to infer its lifetime from (spec ch05.3) -- "
            "refactor to take a single reference parameter, or return by value/std::unique_ptr instead",
            fn.loc));
    }
    if (fn.return_type.is_mutable_ref && !fn.params[*found].type.is_mutable_ref) {
        return std::unexpected(DataflowError("function '" + fn.name +
                             "' returns a mutable reference ('T&') but its sole reference parameter '" +
                             fn.params[*found].name +
                             "' is a shared reference ('const T&'); a mutable reference cannot be manufactured "
                             "from a shared one",
            fn.loc));
    }
    return found;
}

[[nodiscard]] bool is_explicit_this_lifetime_annotation(const Function& fn) {
    return fn.return_lifetime.name == "this" && !fn.params.empty() && fn.params[0].name == "this" &&
           is_reference(fn.params[0].type);
}

[[nodiscard]] std::vector<std::size_t> infer_pointer_return_source_param_indices(const Function& fn) {
    if (fn.return_lifetime.present() || fn.return_type.kind != TypeKind::Pointer) return {};
    if (!fn.params.empty() && fn.params[0].name == "this" && is_reference(fn.params[0].type)) return {0};

    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < fn.params.size(); i++) {
        if (is_pointer_return_lifetime_source_type(fn.params[i].type) &&
            !(is_reference(fn.params[i].type) && fn.params[i].type.is_rvalue_ref)) {
            indices.push_back(i);
        }
    }
    return indices;
}

[[nodiscard]] bool param_can_outlive_call_for_lifetime_return(const Param& param) {
    if (!is_pointer_return_lifetime_source_type(param.type)) return false;
    return !(is_reference(param.type) && param.type.is_rvalue_ref);
}

[[nodiscard]] std::expected<void, DataflowError> validate_lifetime_annotation_placement(const Function& fn) {
    for (const Param& param : fn.params) {
        if (!param.lifetime.present()) continue;
        if (!is_pointer_return_lifetime_source_type(param.type)) {
            return std::unexpected(DataflowError("parameter '" + param.name +
                                    "' bears '[[scpp::lifetime(name)]]' but does not denote a reference, pointer, "
                                    "span, or std::reference_wrapper-carried reference",
                                fn.loc));
        }
    }
    if (fn.return_lifetime.present() && !is_pointer_return_lifetime_source_type(fn.return_type)) {
        return std::unexpected(DataflowError("function '" + fn.name +
                                "' bears '[[scpp::lifetime(name)]]' on its declarator, but its return type is not "
                                "a reference, pointer, span, or std::reference_wrapper-carried reference",
                            fn.loc));
    }
    return {};
}

[[nodiscard]] bool is_operator_arrow_function(const Function& fn) {
    return !fn.member_owner_class.empty() && fn.name.ends_with("_operator_arrow") && !fn.params.empty() &&
           fn.params[0].name == "this";
}

[[nodiscard]] std::expected<void, DataflowError> validate_equality_operator_signature(const Function& fn) {
    bool named_equality_operator =
        fn.name.ends_with("_operator_equal") || fn.name.ends_with("_operator_not_equal");
    if (!named_equality_operator) return {};
    if (fn.is_static) {
        return std::unexpected(DataflowError("equality operators of '" + fn.member_owner_class + "' shall not be static", fn.loc));
    }
    if (fn.params.size() != 2) {
        return std::unexpected(DataflowError("equality operators of '" + fn.member_owner_class + "' shall have exactly one parameter", fn.loc));
    }
    if (fn.return_type.kind != TypeKind::Named || fn.return_type.name != "bool") {
        return std::unexpected(DataflowError("equality operators of '" + fn.member_owner_class + "' shall return bool", fn.loc));
    }
    if (fn.is_defaulted) {
        if (!is_special_member_const_lvalue_self_param(fn.params[1].type, fn.member_owner_class) ||
            fn.receiver_ref_qualifier != ReceiverRefQualifier::None || fn.params[0].type.is_mutable_ref) {
            return std::unexpected(DataflowError("a defaulted equality operator of '" + fn.member_owner_class +
                                    "' shall have signature 'bool operator==(const " + fn.member_owner_class +
                                    "&) const' (and likewise for operator!=)",
                                fn.loc));
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, DataflowError> validate_operator_arrow_signature(const Function& fn) {
    if (!is_operator_arrow_function(fn)) return {};
    if (fn.params.size() != 1) {
        return std::unexpected(DataflowError("operator-> of class '" + fn.member_owner_class + "' shall have no parameters", fn.loc));
    }
    const Type& ret = fn.return_type;
    bool returns_pointer = ret.kind == TypeKind::Pointer;
    bool returns_class = ret.kind == TypeKind::Named;
    bool returns_class_ref = ret.kind == TypeKind::Reference && ret.pointee != nullptr && ret.pointee->kind == TypeKind::Named;
    if (!returns_pointer && !returns_class && !returns_class_ref) {
        return std::unexpected(DataflowError("operator-> of class '" + fn.member_owner_class +
                                "' shall return a pointer, a class, or a reference to class",
                            fn.loc));
    }
    return {};
}

[[nodiscard]] std::expected<std::vector<std::size_t>, DataflowError> resolve_returned_lifetime_param_indices(const Function& fn) {
    if (auto _r = validate_lifetime_annotation_placement(fn); !_r.has_value()) return std::unexpected(std::move(_r).error());
    if (fn.return_lifetime.present()) {
        if (is_explicit_this_lifetime_annotation(fn)) return std::vector<std::size_t>{0};
        if (fn.return_lifetime.is_any()) {
            return std::unexpected(DataflowError("function '" + fn.name +
                                    "' cannot name the reserved lifetime group 'any' in its return annotation",
                                fn.loc));
        }
        std::vector<std::size_t> indices;
        for (std::size_t i = 0; i < fn.params.size(); i++) {
            if (fn.params[i].lifetime.name != fn.return_lifetime.name) continue;
            if (!param_can_outlive_call_for_lifetime_return(fn.params[i])) {
                return std::unexpected(DataflowError("function '" + fn.name + "' ties its return to parameter '" + fn.params[i].name +
                                        "', but that parameter cannot outlive the call",
                                    fn.loc));
            }
            indices.push_back(i);
        }
        if (indices.empty() && !fn.member_owner_class.empty() && !fn.is_static && !fn.params.empty() &&
            fn.params[0].name == "this" && is_reference(fn.params[0].type) &&
            (fn.return_lifetime.name == "this" || fn.return_lifetime.name == "this" || is_operator_arrow_function(fn))) {
            indices.push_back(0);
        }
        if (indices.empty()) {
            return std::unexpected(DataflowError("function '" + fn.name + "' names lifetime group '" + fn.return_lifetime.name +
                                    "' in its return annotation, but no parameter belongs to that group",
                                fn.loc));
        }
        return indices;
    }
    if (!is_reference(fn.return_type) && fn.return_type.kind != TypeKind::Pointer) return std::vector<std::size_t>{};
    if (fn.return_type.kind == TypeKind::Pointer) {
        std::vector<std::size_t> indices = infer_pointer_return_source_param_indices(fn);
        return indices.size() == 1 ? indices : std::vector<std::size_t>{};
    }
    auto elided_result = resolve_elided_param_index(fn);
    if (!elided_result.has_value()) return std::unexpected(std::move(elided_result).error());
    std::optional<std::size_t> elided = elided_result.value();
    if (!elided.has_value()) return std::vector<std::size_t>{};
    const Param& param = fn.params[*elided];
    if (param.lifetime.is_any()) {
        return std::unexpected(DataflowError("function '" + fn.name +
                                "' returns a value derived from reserved lifetime group 'any', which may not "
                                "escape the call",
                            fn.loc));
    }
    return std::vector<std::size_t>{*elided};
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
[[nodiscard]] std::expected<Signatures, DataflowError> build_signatures(const Program& program) {
    Signatures signatures;
    for (const Function& fn : program.functions) {
        if (auto _r = validate_equality_operator_signature(fn); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = validate_operator_arrow_signature(fn); !_r.has_value()) return std::unexpected(std::move(_r).error());
        FunctionSignature sig;
        sig.param_types.reserve(fn.params.size());
        sig.param_is_forwarding_reference.reserve(fn.params.size());
        sig.param_names.reserve(fn.params.size());
        sig.param_default_exprs.reserve(fn.params.size());
        sig.param_require_thread_movable.reserve(fn.params.size());
        sig.param_require_thread_shareable.reserve(fn.params.size());
        for (const Param& param : fn.params) {
            sig.param_types.push_back(param.type);
            sig.param_is_forwarding_reference.push_back(is_forwarding_reference_param(fn, param));
            sig.param_names.push_back(param.name);
            sig.param_default_exprs.push_back(param.default_expr);
            sig.param_require_thread_movable.push_back(param.require_thread_movable);
            sig.param_require_thread_shareable.push_back(param.require_thread_shareable);
            sig.param_lifetimes.push_back(param.lifetime);
        }
        sig.return_type = fn.return_type;
        sig.return_lifetime = fn.return_lifetime;
        auto returned_lifetime_result = resolve_returned_lifetime_param_indices(fn);
        if (!returned_lifetime_result.has_value()) return std::unexpected(std::move(returned_lifetime_result).error());
        sig.returned_lifetime_param_indices = std::move(returned_lifetime_result).value();
        if (fn.return_lifetime.present()) {
            sig.elided_param_index = std::nullopt;
        } else {
            auto elided_result = resolve_elided_param_index(fn);
            if (!elided_result.has_value()) return std::unexpected(std::move(elided_result).error());
            sig.elided_param_index = std::move(elided_result).value();
        }
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
        sig.is_generic_template = fn.is_generic_template;
        sig.is_deleted = fn.is_deleted;
        sig.display_name = fn.name;
        std::vector<FunctionSignature>& overloads = signatures[fn.name];
        for (const FunctionSignature& existing : overloads) {
            if (existing.is_generic_template != sig.is_generic_template) continue;
            bool same_params = existing.param_types.size() == sig.param_types.size();
            for (std::size_t i = 0; same_params && i < sig.param_types.size(); i++) {
                same_params = types_equal(existing.param_types[i], sig.param_types[i]);
            }
            if (same_params && existing.receiver_ref_qualifier == sig.receiver_ref_qualifier) {
                return std::unexpected(DataflowError("redefinition of '" + fn.name +
                                     "': a previous declaration with an identical parameter list already "
                                     "exists ([basic.def.odr]/1 with [over.load]/2 -- functions can only be "
                                     "overloaded by parameter list, return type alone doesn't count as a "
                                     "difference)",
                    fn.loc));
            }
        }
        overloads.push_back(std::move(sig));
    }
    return signatures;
}

} // namespace scpp
