module;

module scpp.compiler.movecheck:monomorphize;

import std;
import scpp.ast;
import :errors;
import scpp.mir;
import :types;
import :signatures;
import :calls;
import :threadsafety;
import :generics_support;
import :lambdas;

namespace scpp {

void monomorphize_generics_impl(Program& program);

class Monomorphizer {
public:
    explicit Monomorphizer(Program& program) : program_(program) {
        for (const ConceptDef& c : program.concepts) concepts_by_name_[c.name] = &c;
        for (std::size_t i = 0; i < program.functions.size(); i++) {
            if (program.functions[i].is_generic_template) {
                generic_template_indices_[program.functions[i].name].push_back(i);
            }
        }
        // ch05 §5.12: names a blanket lambda capture must never
        // implicitly bind to -- a known type name (struct/class/
        // concept) or a known free-function name is never itself a
        // capturable *variable*, even though it may appear as a bare
        // Identifier-shaped token inside a requires-expression-like
        // context; excluded up front so collect_free_identifiers'
        // exclusion set (built per-lambda in resolve_lambda) doesn't
        // need to reconstruct these each time.
        for (const StructDef& s : program.structs) known_type_names_.insert(s.name);
        for (const EnumDef& e : program.enums) known_type_names_.insert(e.name);
        for (std::size_t i = 0; i < program.classes.size(); i++) {
            const ClassDef& c = program.classes[i];
            known_type_names_.insert(c.name);
            if (!c.template_owner_id.empty()) {
                class_template_indices_by_owner_id_[c.template_owner_id] = i;
                if (!c.is_variadic_primary_template && !c.is_variadic_specialization) {
                    ordinary_class_template_owner_ids_by_name_[c.name].push_back(c.template_owner_id);
                }
            }
        }
        for (const ConceptDef& c : program.concepts) known_type_names_.insert(c.name);
        for (const Function& fn : program.functions) known_function_names_.insert(fn.name);
        // ch05 §5.14: every generic class/struct *template*'s own name --
        // used to (a) skip its own unresolved-"T" methods from every
        // other pass in this file (movecheck's Body-based machinery has
        // no way to make sense of a type that isn't real anywhere in
        // the program) and (b) recognize a Type::template_args-bearing
        // Type as naming one of *these* specifically (parser.cppm's own
        // generic_type_names_ already guarantees nothing else could).
        for (const ClassDef& c : program.classes) {
            if (!c.template_params.empty()) generic_type_template_names_.insert(c.name);
        }
        for (const StructDef& s : program.structs) {
            if (!s.template_params.empty()) generic_type_template_names_.insert(s.name);
        }
        // ch05 §5.14: every variadic generic type's own *primary
        // template* name (e.g. "Tuple") -- distinguishes a variadic
        // instantiation (`Tuple<int,bool,char>`, resolved by
        // instantiate_variadic_generic_type, one concrete ClassDef per
        // recursive-inheritance level) from an ordinary, single-type-
        // parameter one (`Vec<int>`, resolved by instantiate_generic_
        // type). A variadic primary template's own name is always
        // already a member of generic_type_template_names_ too (its
        // own template_params is non-empty, a single pack parameter),
        // but the two mechanisms are otherwise unrelated -- this set is
        // what resolve_generic_type actually branches on.
        for (const ClassDef& c : program.classes) {
            if (c.is_variadic_primary_template) variadic_generic_type_names_.insert(c.name);
        }
    }

    void run() {
        signatures_ = build_signatures(program_);
        // ch05 §5.14: synthesizes a "forwarding stub" Function for every
        // inherited method/field access a derived class doesn't itself
        // override (see synthesize_inherited_method_forwards' own
        // comment) -- runs first, since resolve_generic_types/
        // check_generic_type_methods_once and the ordinary per-function
        // walk below all resolve an inherited method call by simply
        // finding "DerivedClass_methodName" already present in
        // program_.functions, exactly like an ordinary, non-inherited
        // method -- no inheritance-specific fallback logic needed
        // anywhere else in this file (or in codegen) as a result.
        synthesize_inherited_method_forwards();
        // ch05 §5.14: resolves every `GenericType<Concrete>` instantiation
        // anywhere in the program (struct/class fields, every function/
        // method's own signature, and every VarDecl inside a body) and
        // checks every generic class's own methods once, abstractly, at
        // their own definition -- both *before* the rest of this pass,
        // since neither depends on anything it does, and the ordinary
        // per-function walk just below would otherwise trip over an
        // unresolved generic-type Named type it can't make sense of.
        resolve_generic_types();
        check_generic_type_methods_once();
        // resolve_generic_types/check_generic_type_methods_once may
        // monomorphize generic types and rewrite existing function
        // signatures/return types (e.g. `MyBox<int>` -> its concrete
        // synthesized class name). Rebuild the signature table before any
        // later overload resolution or generic-function constraint check
        // consults it, so those queries see the fully concrete program
        // shape rather than the pre-resolution templates.
        signatures_ = build_signatures(program_);
        // A snapshot of the function count *before* any clone is
        // injected: new clones/synthesized closure classes are appended
        // to program_.functions/program_.classes as we go (see
        // get_or_create_clone/resolve_lambda) and must never themselves
        // be re-walked by *this* outer loop (they're already fully
        // concrete -- nothing left to monomorphize/resolve at the top
        // level; a synthesized closure's own body is instead walked
        // directly from within resolve_lambda itself, once, right after
        // being synthesized).
        //
        // A generic template's own body *is* walked here too (unlike an
        // earlier version of this pass) -- bare-call-redirect (ch05
        // §5.9/§5.11/§5.12, e.g. `f(x)` for a witness-typed parameter
        // `f`) and lambda-resolution both need to run there just as much
        // as anywhere else, and neither depends on knowing the eventual
        // concrete instantiation. Only the *generic-call-monomorphization*
        // half of walk_expr is suppressed while inside a generic
        // template's own body (see allow_generic_monomorphization
        // below) -- a nested generic-template-calling-another-generic-
        // template call site is left targeting the original,
        // codegen-excluded template, surfacing as a clear "unknown
        // function" error downstream rather than incorrectly attempting
        // to monomorphize against an abstract witness type as if it
        // were concrete.
        //
        // ch05 §5.14: a generic *type*'s own template methods (their
        // `this` parameter names an unresolved generic-type template
        // directly, never a witness) are skipped entirely here -- "T"
        // is never a real type anywhere in the program for them, so
        // there is nothing this walk (or check_moves/codegen after it)
        // could safely do with one; they exist purely as a body/
        // signature source for check_generic_type_methods_once and
        // resolve_generic_types' own per-instantiation clones, both
        // already done above.
        std::size_t original_count = program_.functions.size();
        for (std::size_t i = 0; i < original_count; i++) {
            if (program_.functions[i].body == nullptr) continue;
            if (belongs_to_unresolved_generic_type_template(program_.functions[i])) continue;
            // ch05 §5.11: a full-header-form generic function's own
            // template (Function::template_params non-empty, e.g.
            // `get`/`make`) is skipped here too, for the identical
            // reason -- its own body may reference a not-yet-bound
            // template parameter's own name as a type (or, for a
            // base-class-deduction pattern, something with no concrete
            // meaning at all outside a real call site); see
            // check_moves's own identical guard/comment. Each concrete
            // call site is instead resolved directly below (this same
            // walk, over an *already-monomorphized* caller's own body)
            // by monomorphize_generic_function_call.
            if (!program_.functions[i].template_params.empty()) continue;
            // build_mir's own Body holds raw (const Expr*) pointers into
            // this Function's *own* Stmt/Expr tree (see mir.cppm's
            // Terminator) -- safe to keep using after program_.functions
            // mutates below, since only the *vector's* backing storage
            // (and, incidentally, the Function objects it directly
            // holds) can move; a Function's own body is heap-allocated
            // independently (via StmtPtr/ExprPtr) and never relocates
            // just because the enclosing vector reallocates elsewhere.
            Body body = build_mir(program_.functions[i]);
            body.program = &program_;
            bool allow_generic_monomorphization = !program_.functions[i].is_generic_template;
            walk_stmt(*program_.functions[i].body, body, this_type_of(program_.functions[i]),
                      allow_generic_monomorphization);
        }
    }

private:
    Program& program_;
    std::unordered_map<std::string, const ConceptDef*> concepts_by_name_;
    std::unordered_map<std::string, std::vector<std::size_t>> generic_template_indices_;
    std::unordered_map<std::string, std::size_t> class_template_indices_by_owner_id_;
    std::unordered_map<std::string, std::vector<std::string>> ordinary_class_template_owner_ids_by_name_;
    std::unordered_map<std::string, std::string> clone_cache_;
    std::unordered_set<std::string> known_type_names_;
    std::unordered_set<std::string> known_function_names_;
    Signatures signatures_;
    // ch05 §5.14: every generic class/struct template's own name -- see
    // the constructor's own comment.
    std::unordered_set<std::string> generic_type_template_names_;
    // ch05 §5.14: every variadic generic type's own primary-template
    // name -- see the constructor's own comment.
    std::unordered_set<std::string> variadic_generic_type_names_;
    // ch05 §5.14: caches an already-synthesized concrete generic-type
    // instantiation by "TemplateName.MangledArgType" (mirrors
    // clone_cache_'s identical purpose for generic functions), so
    // `Vec<int>` used twice in the same program shares one concrete
    // class/method set rather than duplicating it.
    std::unordered_map<std::string, std::string> generic_type_instance_cache_;
    struct OrdinaryGenericInstanceInfo {
        std::string template_name;
        std::vector<Type> type_args;
    };
    std::unordered_map<std::string, OrdinaryGenericInstanceInfo> ordinary_generic_instance_info_;
    // ch05 §5.14: every concrete variadic-generic-type instantiation's
    // own recorded (non-type argument values, type arguments) --
    // populated by instantiate_variadic_generic_type, keyed by the
    // concrete ClassDef's own (mangled) name. The *only* way base-class
    // deduction (monomorphize_generic_function_call's own
    // deduce_via_base_class_chain) can recover "what was this level's
    // own Head/Tail/Idx" after the fact: a concrete ClassDef itself
    // records only its own *fields* (already-substituted types), not
    // which template arguments produced them.
    struct VariadicInstanceInfo {
        std::string template_name;
        std::vector<int> non_type_values;
        std::vector<Type> type_args;
    };
    std::unordered_map<std::string, VariadicInstanceInfo> variadic_instance_info_;
    // ch05 §5.11: caches an already-synthesized concrete monomorphized
    // clone of a full-header-form generic *function* template (e.g.
    // `get`/`make`) by its own template-parameter-binding cache key --
    // see monomorphize_generic_function_call's own comment. Kept
    // separate from clone_cache_ (the abbreviated-Concept-auto-form's
    // own identical-purpose cache) since the two forms' cache keys are
    // computed differently (this one keys off the *template parameter
    // bindings themselves*, not the concrete function-parameter types --
    // the two diverge for a base-class-deduction parameter, whose
    // concrete parameter type is the *deduced base*, not any input the
    // cache key would otherwise naturally be built from).
    std::unordered_map<std::string, std::string> generic_function_clone_cache_;
    // ch05 §5.14: the single, shared, globally-empty witness struct
    // representing a completely bare (unconstrained) generic-type
    // parameter -- see check_generic_type_methods_once's own comment.
    // Empty until first needed (lazily synthesized), since most
    // programs have no bare generic type at all.
    std::string bare_witness_struct_name_;
    // ch05 §5.14: a monotonically-increasing counter for synthesizing
    // each generic method's own unique "checking class" name
    // ("__genchk0", "__genchk1", ...) in check_generic_type_methods_once
    // -- mirrors lambda_counter_'s identical purpose/reasoning.
    int generic_check_counter_ = 0;
    // ch05 §5.12: a monotonically-increasing counter for synthesizing
    // each closure's own unique class name ("__lambda0", "__lambda1",
    // ...) -- a lambda literal has no user-spelled name to reuse (unlike
    // a concept's witness class, which shares the concept's own name),
    // and this codebase has no other source of process-wide uniqueness
    // to draw on.
    int lambda_counter_ = 0;

    // ch05 §5.14: true when `fn` is one of a generic class/struct
    // template's own, not-yet-resolved methods (its `this` parameter
    // names the template directly, e.g. "Vec", never a witness or a
    // concrete instantiation like "Vec_int") -- "T" is never a real
    // type anywhere in the program for these, so every other pass in
    // this file must skip them entirely (see run()'s own comment).
    [[nodiscard]] bool belongs_to_unresolved_generic_type_template(const Function& fn) const {
        return !fn.generic_method_owner_id.empty();
    }

    struct TemplateInstantiationBindings {
        std::vector<std::pair<std::string, Type>> type_replacements;
        std::unordered_map<std::string, std::vector<Type>> type_pack_replacements;
    };

    [[nodiscard]] const ClassDef* class_template_by_owner_id(const std::string& owner_id) const {
        auto it = class_template_indices_by_owner_id_.find(owner_id);
        if (it == class_template_indices_by_owner_id_.end()) return nullptr;
        return &program_.classes[it->second];
    }

    [[nodiscard]] std::string owning_module_of_named_type(std::string_view name) const {
        for (const ClassDef& def : program_.classes) {
            if (def.name == name) return def.owning_module;
        }
        for (const StructDef& def : program_.structs) {
            if (def.name == name) return def.owning_module;
        }
        for (const EnumDef& def : program_.enums) {
            if (def.name == name) return def.owning_module;
        }
        return {};
    }

    [[nodiscard]] bool type_mentions_foreign_module(const Type& type, std::string_view template_module) const {
        if (type.kind == TypeKind::Named) {
            std::string type_module = owning_module_of_named_type(type.name);
            if (!type_module.empty() && type_module != template_module) return true;
        }
        for (const Type& arg : type.template_args) {
            if (type_mentions_foreign_module(arg, template_module)) return true;
        }
        if (type.pointee && type_mentions_foreign_module(*type.pointee, template_module)) return true;
        if (type.element && type_mentions_foreign_module(*type.element, template_module)) return true;
        if (type.function_return && type_mentions_foreign_module(*type.function_return, template_module)) return true;
        for (const Type& param : type.function_params) {
            if (type_mentions_foreign_module(param, template_module)) return true;
        }
        return false;
    }

    [[nodiscard]] bool instantiate_imported_generic_locally(const std::vector<Type>& concrete_args,
                                                            std::string_view template_module) const {
        if (!program_.module_name.empty() || template_module.empty()) return false;
        for (const Type& arg : concrete_args) {
            if (type_mentions_foreign_module(arg, template_module)) return true;
        }
        return false;
    }

    [[nodiscard]] static std::string method_suffix_after_owner_prefix(const Function& fn, const std::string& class_name,
                                                                       const std::string& owner_id) {
        std::string owner_prefix = owner_id.empty() ? class_name : class_name + "__" + owner_id;
        if (fn.name.rfind(owner_prefix, 0) == 0) return fn.name.substr(owner_prefix.size());
        if (fn.name.rfind(class_name, 0) == 0) return fn.name.substr(class_name.size());
        return fn.name;
    }

    [[nodiscard]] static std::size_t find_matching_angle(const std::string& text, std::size_t open_pos) {
        int depth = 0;
        for (std::size_t i = open_pos; i < text.size(); i++) {
            if (text[i] == '<') depth++;
            else if (text[i] == '>') {
                depth--;
                if (depth == 0) return i;
            }
        }
        return std::string::npos;
    }

    [[nodiscard]] static std::size_t find_last_scope_outside_angles(const std::string& text) {
        int depth = 0;
        for (std::size_t i = text.size(); i-- > 1;) {
            char c = text[i];
            if (c == '>') depth++;
            else if (c == '<') depth--;
            else if (c == ':' && text[i - 1] == ':' && depth == 0) return i - 1;
        }
        return std::string::npos;
    }

    [[nodiscard]] static std::string trim_copy(std::string text) {
        std::size_t start = 0;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start++;
        std::size_t end = text.size();
        while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return text.substr(start, end - start);
    }

    [[nodiscard]] std::optional<Type> parse_type_spelling(std::string_view spelling) const {
        std::string text = trim_copy(std::string(spelling));
        if (text.empty()) return std::nullopt;
        Type type;
        type.kind = TypeKind::Named;
        std::size_t lt = text.find('<');
        if (lt == std::string::npos) {
            type.name = text;
            return type;
        }
        std::size_t gt = find_matching_angle(text, lt);
        if (gt == std::string::npos || gt + 1 != text.size()) return std::nullopt;
        type.name = text.substr(0, lt);
        std::string inner = text.substr(lt + 1, gt - lt - 1);
        int depth = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= inner.size(); i++) {
            bool at_end = i == inner.size();
            char c = at_end ? ',' : inner[i];
            if (!at_end) {
                if (c == '<') depth++;
                else if (c == '>') depth--;
            }
            if ((at_end || (c == ',' && depth == 0))) {
                std::optional<Type> arg = parse_type_spelling(inner.substr(start, i - start));
                if (!arg.has_value()) return std::nullopt;
                type.template_args.push_back(*arg);
                start = i + 1;
            }
        }
        return type;
    }

    struct StaticTemplateCallResolution {
        std::string concrete_class_name;
        std::string member_name;
    };

    [[nodiscard]] std::optional<StaticTemplateCallResolution>
    resolve_static_template_call_target(const std::string& name, SourceLocation loc) {
        std::size_t scope = find_last_scope_outside_angles(name);
        if (scope == std::string::npos) return std::nullopt;
        std::string owner = name.substr(0, scope);
        std::string member_name = name.substr(scope + 2);
        if (owner.find('<') == std::string::npos) return std::nullopt;
        std::optional<Type> owner_type = parse_type_spelling(owner);
        if (!owner_type.has_value() || owner_type->template_args.empty()) return std::nullopt;
        std::vector<Type> resolved_args;
        resolved_args.reserve(owner_type->template_args.size());
        for (const Type& arg : owner_type->template_args) {
            resolved_args.push_back(resolve_generic_type(arg, loc));
        }
        std::string concrete_class_name = instantiate_generic_type(owner_type->name, resolved_args, loc);
        return StaticTemplateCallResolution{concrete_class_name, member_name};
    }

    void walk_new_concrete_function(std::size_t fn_index) {
        if (fn_index >= program_.functions.size()) return;
        Function& fn = program_.functions[fn_index];
        if (fn.body == nullptr || !fn.template_params.empty()) return;
        signatures_ = build_signatures(program_);
        Body body = build_mir(fn);
        body.program = &program_;
        walk_stmt(*fn.body, body, this_type_of(fn), /*allow_generic_monomorphization=*/!fn.is_generic_template);
    }

    // ch04 §4.2/ch05 §5.9: the enclosing function's own `this` parameter
    // type (Named(ClassName)), or nullopt if `fn` isn't a method at all
    // -- used to type a `[this]` lambda capture. `this` is always
    // params[0] when present (parser's make_this_param).
    [[nodiscard]] static std::optional<Type> this_type_of(const Function& fn) {
        if (fn.params.empty() || fn.params[0].name != "this") return std::nullopt;
        return *fn.params[0].type.pointee;
    }

    // ch05 §5.14: replaces every occurrence of the generic type
    // parameter named `param_name` (a plain Named type, e.g. "T")
    // inside `type` with `replacement` -- used both to substitute a
    // real concrete argument (resolve_generic_types/instantiate_generic_
    // type) and a witness class (check_generic_type_methods_once), the
    // same way a generic function's own Concept-constrained parameter
    // is substituted at its own call site.
    [[nodiscard]] Type substitute_type_param(const Type& type, const std::string& param_name,
                                                     const Type& replacement) {
        if (type.kind == TypeKind::Named && type.name == param_name) return replacement;
        Type result = type;
        for (Type& arg : result.template_args) {
            arg = substitute_type_param(arg, param_name, replacement);
        }
        if (result.pointee) {
            result.pointee = std::make_shared<Type>(substitute_type_param(*result.pointee, param_name, replacement));
        }
        if (result.element) {
            result.element = std::make_shared<Type>(substitute_type_param(*result.element, param_name, replacement));
        }
        if (result.function_return) {
            result.function_return =
                std::make_shared<Type>(substitute_type_param(*result.function_return, param_name, replacement));
        }
        for (Type& param : result.function_params) {
            param = substitute_type_param(param, param_name, replacement);
        }
        // ch05 §9.4: an array bound may itself mention the type parameter
        // being substituted (e.g. `char storage[sizeof(T)];`) -- clone the
        // still-unresolved expression tree before substituting into it, so
        // the template primary's own `array_size_expr` is never mutated in
        // place (a *different* instantiation must still see the original,
        // unsubstituted expression). The clone is only ever partially
        // substituted here if `type` has multiple template parameters --
        // substitute_type_params below re-applies this once per
        // parameter, so the expression converges to fully concrete once
        // every parameter has been substituted.
        if (result.array_size_expr) {
            ExprPtr cloned = clone_expr(*result.array_size_expr);
            substitute_type_param_in_expr(*cloned, param_name, replacement);
            result.array_size_expr = std::shared_ptr<Expr>(std::move(cloned));
        }
        return result;
    }

    [[nodiscard]] Type substitute_type_pack(const Type& type, std::string_view pack_name,
                                                   const std::vector<Type>& pack_elems) {
        if (type.is_pack_expansion && type.kind == TypeKind::Named && type.name == pack_name) {
            if (pack_elems.size() == 1) return pack_elems.front();
            return type;
        }
        Type result = type;
        result.is_pack_expansion = false;
        std::vector<Type> expanded_template_args;
        for (const Type& arg : result.template_args) {
            if (arg.is_pack_expansion && arg.kind == TypeKind::Named && arg.name == pack_name) {
                for (const Type& concrete : pack_elems) expanded_template_args.push_back(concrete);
                continue;
            }
            expanded_template_args.push_back(substitute_type_pack(arg, pack_name, pack_elems));
        }
        result.template_args = std::move(expanded_template_args);
        if (result.pointee) {
            result.pointee = std::make_shared<Type>(substitute_type_pack(*result.pointee, pack_name, pack_elems));
        }
        if (result.element) {
            result.element = std::make_shared<Type>(substitute_type_pack(*result.element, pack_name, pack_elems));
        }
        if (result.function_return) {
            result.function_return =
                std::make_shared<Type>(substitute_type_pack(*result.function_return, pack_name, pack_elems));
        }
        std::vector<Type> expanded_function_params;
        for (const Type& param : result.function_params) {
            if (param.is_pack_expansion && param.kind == TypeKind::Named && param.name == pack_name) {
                for (const Type& concrete : pack_elems) expanded_function_params.push_back(concrete);
                continue;
            }
            expanded_function_params.push_back(substitute_type_pack(param, pack_name, pack_elems));
        }
        result.function_params = std::move(expanded_function_params);
        // ch05 §9.4: see the identical comment in substitute_type_param
        // above -- clone-then-substitute so the template primary's own
        // array_size_expr is never mutated in place.
        if (result.array_size_expr) {
            ExprPtr cloned = clone_expr(*result.array_size_expr);
            substitute_type_pack_in_expr(*cloned, pack_name, pack_elems);
            result.array_size_expr = std::shared_ptr<Expr>(std::move(cloned));
        }
        return result;
    }

    [[nodiscard]] Type substitute_type_params(
        const Type& type, const std::vector<std::pair<std::string, Type>>& replacements) {
        Type result = type;
        for (const auto& [param_name, replacement] : replacements) {
            result = substitute_type_param(result, param_name, replacement);
        }
        return result;
    }

    [[nodiscard]] Type substitute_type_packs(
        const Type& type, const std::unordered_map<std::string, std::vector<Type>>& replacements) {
        Type result = type;
        for (const auto& [pack_name, pack_elems] : replacements) {
            result = substitute_type_pack(result, pack_name, pack_elems);
        }
        return result;
    }

    [[nodiscard]] static std::optional<std::string>
    referenced_type_pack_param_name(const Type& type, const std::vector<GenericTypeParam>& template_params) {
        if (type.kind == TypeKind::Named) {
            for (const GenericTypeParam& tp : template_params) {
                if (tp.is_pack && !tp.is_non_type && tp.name == type.name) return tp.name;
            }
        }
        for (const Type& arg : type.template_args) {
            if (std::optional<std::string> found = referenced_type_pack_param_name(arg, template_params)) {
                return found;
            }
        }
        if (type.pointee) {
            if (std::optional<std::string> found =
                    referenced_type_pack_param_name(*type.pointee, template_params)) {
                return found;
            }
        }
        if (type.element) {
            if (std::optional<std::string> found =
                    referenced_type_pack_param_name(*type.element, template_params)) {
                return found;
            }
        }
        if (type.function_return) {
            if (std::optional<std::string> found =
                    referenced_type_pack_param_name(*type.function_return, template_params)) {
                return found;
            }
        }
        for (const Type& param : type.function_params) {
            if (std::optional<std::string> found = referenced_type_pack_param_name(param, template_params)) {
                return found;
            }
        }
        return std::nullopt;
    }

    // ch05 §5.14: applies substitute_type_param to every Type appearing
    // anywhere inside `expr`'s own sub-tree (currently only MakeUnique's
    // element type and a Lambda's own explicit return type carry a
    // meaningful `.type` -- substituting it on every other expr kind is
    // a harmless no-op, since their own `.type` is left default-
    // constructed and could never match `param_name` anyway) -- needed
    // because clone_stmt/clone_expr copy a generic method's own body
    // verbatim, "T" and all, so every Type embedded *inside* the body
    // (not just the method's own signature, handled separately by its
    // caller) must be substituted too.
    void substitute_type_param_in_expr(Expr& expr, const std::string& param_name, const Type& replacement) {
        expr.type = substitute_type_param(expr.type, param_name, replacement);
        for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
            if (arg.is_type) {
                arg.type = substitute_type_param(arg.type, param_name, replacement);
            } else if (arg.value) {
                substitute_type_param_in_expr(*arg.value, param_name, replacement);
            }
        }
        if (expr.lhs) substitute_type_param_in_expr(*expr.lhs, param_name, replacement);
        if (expr.rhs) substitute_type_param_in_expr(*expr.rhs, param_name, replacement);
        if (expr.third) substitute_type_param_in_expr(*expr.third, param_name, replacement);
        for (ExprPtr& arg : expr.args) substitute_type_param_in_expr(*arg, param_name, replacement);
        for (Param& p : expr.lambda_params) p.type = substitute_type_param(p.type, param_name, replacement);
        for (LambdaCapture& c : expr.lambda_captures) {
            if (c.init) substitute_type_param_in_expr(*c.init, param_name, replacement);
        }
        if (expr.lambda_body) substitute_type_param_in_stmt(*expr.lambda_body, param_name, replacement);
    }

    void substitute_type_params_in_expr(Expr& expr, const std::vector<std::pair<std::string, Type>>& replacements) {
        for (const auto& [param_name, replacement] : replacements) {
            substitute_type_param_in_expr(expr, param_name, replacement);
        }
    }

    void substitute_type_pack_in_expr(Expr& expr, std::string_view pack_name, const std::vector<Type>& pack_elems) {
        expr.type = substitute_type_pack(expr.type, pack_name, pack_elems);
        for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
            if (arg.is_type) {
                arg.type = substitute_type_pack(arg.type, pack_name, pack_elems);
            } else if (arg.value) {
                substitute_type_pack_in_expr(*arg.value, pack_name, pack_elems);
            }
        }
        if (expr.lhs) substitute_type_pack_in_expr(*expr.lhs, pack_name, pack_elems);
        if (expr.rhs) substitute_type_pack_in_expr(*expr.rhs, pack_name, pack_elems);
        if (expr.third) substitute_type_pack_in_expr(*expr.third, pack_name, pack_elems);
        for (ExprPtr& arg : expr.args) substitute_type_pack_in_expr(*arg, pack_name, pack_elems);
        for (Param& p : expr.lambda_params) p.type = substitute_type_pack(p.type, pack_name, pack_elems);
        for (LambdaCapture& c : expr.lambda_captures) {
            if (c.init) substitute_type_pack_in_expr(*c.init, pack_name, pack_elems);
        }
        if (expr.lambda_body) substitute_type_pack_in_stmt(*expr.lambda_body, pack_name, pack_elems);
    }

    void substitute_type_packs_in_expr(Expr& expr,
                                       const std::unordered_map<std::string, std::vector<Type>>& replacements) {
        for (const auto& [pack_name, pack_elems] : replacements) {
            substitute_type_pack_in_expr(expr, pack_name, pack_elems);
        }
    }

    void substitute_type_param_in_stmt(Stmt& stmt, const std::string& param_name, const Type& replacement) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                stmt.type = substitute_type_param(stmt.type, param_name, replacement);
                if (stmt.init) substitute_type_param_in_expr(*stmt.init, param_name, replacement);
                for (ExprPtr& arg : stmt.ctor_args) substitute_type_param_in_expr(*arg, param_name, replacement);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) substitute_type_param_in_expr(*stmt.expr, param_name, replacement);
                return;
            case StmtKind::If:
                substitute_type_param_in_expr(*stmt.condition, param_name, replacement);
                substitute_type_param_in_stmt(*stmt.then_branch, param_name, replacement);
                if (stmt.else_branch) substitute_type_param_in_stmt(*stmt.else_branch, param_name, replacement);
                return;
            case StmtKind::While:
                substitute_type_param_in_expr(*stmt.condition, param_name, replacement);
                substitute_type_param_in_stmt(*stmt.then_branch, param_name, replacement);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) substitute_type_param_in_stmt(*s, param_name, replacement);
                return;
        }
    }

    void substitute_type_params_in_stmt(Stmt& stmt, const std::vector<std::pair<std::string, Type>>& replacements) {
        for (const auto& [param_name, replacement] : replacements) {
            substitute_type_param_in_stmt(stmt, param_name, replacement);
        }
    }

    void substitute_type_bindings_in_stmt(Stmt& stmt, const std::unordered_map<std::string, Type>& replacements) {
        for (const auto& [param_name, replacement] : replacements) {
            substitute_type_param_in_stmt(stmt, param_name, replacement);
        }
    }

    void substitute_type_pack_in_stmt(Stmt& stmt, std::string_view pack_name, const std::vector<Type>& pack_elems) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                stmt.type = substitute_type_pack(stmt.type, pack_name, pack_elems);
                if (stmt.init) substitute_type_pack_in_expr(*stmt.init, pack_name, pack_elems);
                for (ExprPtr& arg : stmt.ctor_args) substitute_type_pack_in_expr(*arg, pack_name, pack_elems);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) substitute_type_pack_in_expr(*stmt.expr, pack_name, pack_elems);
                return;
            case StmtKind::If:
            case StmtKind::While:
                substitute_type_pack_in_expr(*stmt.condition, pack_name, pack_elems);
                substitute_type_pack_in_stmt(*stmt.then_branch, pack_name, pack_elems);
                if (stmt.else_branch) substitute_type_pack_in_stmt(*stmt.else_branch, pack_name, pack_elems);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) substitute_type_pack_in_stmt(*s, pack_name, pack_elems);
                return;
        }
    }

    void substitute_type_packs_in_stmt(Stmt& stmt,
                                       const std::unordered_map<std::string, std::vector<Type>>& replacements) {
        for (const auto& [pack_name, pack_elems] : replacements) {
            substitute_type_pack_in_stmt(stmt, pack_name, pack_elems);
        }
    }

    void substitute_non_type_param_in_expr(Expr& expr, const std::string& param_name, int replacement) {
        if (expr.kind == ExprKind::Identifier && expr.name == param_name) {
            expr.kind = ExprKind::IntegerLiteral;
            expr.int_value = replacement;
            expr.name.clear();
            expr.lhs.reset();
            expr.rhs.reset();
            expr.args.clear();
            expr.explicit_template_args.clear();
            return;
        }
        for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
            if (!arg.is_type && arg.value) substitute_non_type_param_in_expr(*arg.value, param_name, replacement);
        }
        if (expr.lhs) substitute_non_type_param_in_expr(*expr.lhs, param_name, replacement);
        if (expr.rhs) substitute_non_type_param_in_expr(*expr.rhs, param_name, replacement);
        for (ExprPtr& arg : expr.args) substitute_non_type_param_in_expr(*arg, param_name, replacement);
        for (LambdaCapture& c : expr.lambda_captures) {
            if (c.init) substitute_non_type_param_in_expr(*c.init, param_name, replacement);
        }
        if (expr.lambda_body) substitute_non_type_param_in_stmt(*expr.lambda_body, param_name, replacement);
    }

    void substitute_non_type_param_in_stmt(Stmt& stmt, const std::string& param_name, int replacement) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) substitute_non_type_param_in_expr(*stmt.init, param_name, replacement);
                for (ExprPtr& arg : stmt.ctor_args) substitute_non_type_param_in_expr(*arg, param_name, replacement);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) substitute_non_type_param_in_expr(*stmt.expr, param_name, replacement);
                return;
            case StmtKind::If:
                substitute_non_type_param_in_expr(*stmt.condition, param_name, replacement);
                substitute_non_type_param_in_stmt(*stmt.then_branch, param_name, replacement);
                if (stmt.else_branch) substitute_non_type_param_in_stmt(*stmt.else_branch, param_name, replacement);
                return;
            case StmtKind::While:
                substitute_non_type_param_in_expr(*stmt.condition, param_name, replacement);
                substitute_non_type_param_in_stmt(*stmt.then_branch, param_name, replacement);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) substitute_non_type_param_in_stmt(*s, param_name, replacement);
                return;
        }
    }

    [[nodiscard]] std::vector<Function> methods_of_type_name(const std::string& type_name) const {
        std::vector<Function> result;
        for (const Function& fn : program_.functions) {
            if (fn.member_owner_class == type_name) {
                result.push_back(clone_function(fn));
            }
        }
        return result;
    }

    // ch05 §5.14: every method (including a constructor/destructor) still
    // attached to exactly one unresolved generic class template definition
    // or ordinary partial specialization pattern, identified by that
    // template's own internal owner id rather than its exposed class name.
    // This keeps distinct `function<...>` template definitions from
    // colliding once more than one shares the same `this` pointee spelling.
    [[nodiscard]] std::vector<Function> method_templates_of_owner(const std::string& owner_id) const {
        std::vector<Function> result;
        for (const Function& fn : program_.functions) {
            if (fn.generic_method_owner_id == owner_id) result.push_back(clone_function(fn));
        }
        return result;
    }

    void clone_variadic_class_methods(const std::string& cache_key, const std::string& template_name,
                                      const std::string& owner_id, const std::vector<Function>& methods,
                                      const std::vector<GenericTypeParam>& template_params_copy,
                                      const std::vector<std::pair<std::string, Type>>& type_replacements,
                                      const std::unordered_map<std::string, std::vector<Type>>& pack_replacements,
                                      const std::vector<int>& non_type_args) {
        for (const Function& method_tmpl : methods) {
            Function clone;
            clone.name = cache_key + method_suffix_after_owner_prefix(method_tmpl, template_name, owner_id);
            clone.loc = method_tmpl.loc;
            clone.namespace_path = method_tmpl.namespace_path;
            clone.is_exported = false;
            clone.is_unsafe = method_tmpl.is_unsafe;
            clone.is_nodiscard = method_tmpl.is_nodiscard;
            clone.nodiscard_reason = method_tmpl.nodiscard_reason;
            clone.owning_module = program_.module_name.empty() ? std::string() : method_tmpl.owning_module;
            clone.visibility_module = method_tmpl.visibility_module.empty() ? method_tmpl.owning_module
                                                                            : method_tmpl.visibility_module;
            clone.eval_mode = method_tmpl.eval_mode;
            clone.is_generic_template = method_tmpl.is_generic_template;
            clone.template_params = method_tmpl.template_params;
            clone.method_requires_concept = method_tmpl.method_requires_concept;
            clone.member_owner_class = cache_key;
            clone.is_static = method_tmpl.is_static;
            clone.is_virtual = method_tmpl.is_virtual;
            clone.is_override = method_tmpl.is_override;
            clone.is_pure = method_tmpl.is_pure;
            clone.is_defaulted = method_tmpl.is_defaulted;
            clone.access = method_tmpl.access;
            clone.return_type = instantiate_type_pattern(method_tmpl.return_type, type_replacements, pack_replacements);
            clone.return_type = resolve_generic_type(clone.return_type, method_tmpl.loc);
            clone.return_lifetime = method_tmpl.return_lifetime;
            std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
            clone.params.reserve(method_tmpl.params.size());
            for (const Param& p : method_tmpl.params) {
                if (p.name == "this") {
                    Param np;
                    np.name = p.name;
                    Type this_type;
                    this_type.kind = TypeKind::Reference;
                    this_type.pointee = std::make_shared<Type>(named_type(cache_key));
                    this_type.is_mutable_ref = p.type.is_mutable_ref;
                    np.type = std::move(this_type);
                    clone.params.push_back(std::move(np));
                    continue;
                }
                if (p.is_parameter_pack) {
                    std::optional<std::string> pack_name =
                        referenced_type_pack_param_name(p.type, template_params_copy);
                    auto pack_it = pack_name ? pack_replacements.find(*pack_name) : pack_replacements.end();
                    if (pack_name && pack_it != pack_replacements.end()) {
                        pack_param_names[p.name] = {};
                        for (std::size_t j = 0; j < pack_it->second.size(); j++) {
                            Param np = p;
                            np.is_parameter_pack = false;
                            np.name = p.name + "$" + std::to_string(j);
                            std::vector<std::pair<std::string, Type>> param_replacements = type_replacements;
                            param_replacements.emplace_back(*pack_name, pack_it->second[j]);
                            np.type = instantiate_type_pattern(p.type, param_replacements, {});
                            np.type = resolve_generic_type(np.type, method_tmpl.loc);
                            clone.params.push_back(std::move(np));
                            pack_param_names[p.name].push_back(clone.params.back().name);
                        }
                        continue;
                    }
                }
                Param np = p;
                np.type = instantiate_type_pattern(p.type, type_replacements, pack_replacements);
                np.type = resolve_generic_type(np.type, method_tmpl.loc);
                clone.params.push_back(std::move(np));
            }
            clone.member_initializers = method_tmpl.member_initializers;
            for (MemberInitializer& init : clone.member_initializers) {
                if (init.initializer.expr) {
                    substitute_type_params_in_expr(*init.initializer.expr, type_replacements);
                    substitute_type_packs_in_expr(*init.initializer.expr, pack_replacements);
                    for (std::size_t i = 0; i < template_params_copy.size() && i < non_type_args.size(); i++) {
                        if (!template_params_copy[i].is_non_type) continue;
                        substitute_non_type_param_in_expr(*init.initializer.expr, template_params_copy[i].name,
                                                          non_type_args[i]);
                    }
                    resolve_generic_types_in_expr(*init.initializer.expr);
                }
                for (ExprPtr& arg : init.initializer.brace_args) {
                    substitute_type_params_in_expr(*arg, type_replacements);
                    substitute_type_packs_in_expr(*arg, pack_replacements);
                    for (std::size_t i = 0; i < template_params_copy.size() && i < non_type_args.size(); i++) {
                        if (!template_params_copy[i].is_non_type) continue;
                        substitute_non_type_param_in_expr(*arg, template_params_copy[i].name, non_type_args[i]);
                    }
                    resolve_generic_types_in_expr(*arg);
                }
            }
            clone.body = method_tmpl.body ? clone_stmt(*method_tmpl.body) : nullptr;
            if (clone.body) {
                substitute_type_params_in_stmt(*clone.body, type_replacements);
                substitute_type_packs_in_stmt(*clone.body, pack_replacements);
                for (std::size_t i = 0; i < template_params_copy.size() && i < non_type_args.size(); i++) {
                    if (!template_params_copy[i].is_non_type) continue;
                    substitute_non_type_param_in_stmt(*clone.body, template_params_copy[i].name, non_type_args[i]);
                }
                for (const auto& [class_pack_name, concrete_pack_types] : pack_replacements) {
                    std::vector<std::string> concrete_names;
                    concrete_names.reserve(concrete_pack_types.size());
                    for (const Type& concrete_type : concrete_pack_types) concrete_names.push_back(concrete_type.name);
                    expand_explicit_template_arg_packs_in_stmt(*clone.body, class_pack_name, concrete_names);
                }
                for (const auto& [pack_param_name, concrete_names] : pack_param_names) {
                    expand_pack_expansions_in_stmt(*clone.body, pack_param_name, concrete_names);
                    expand_pack_folds_in_stmt(*clone.body, pack_param_name, concrete_names);
                }
                resolve_generic_types_in_stmt(*clone.body);
            }
            known_function_names_.insert(clone.name);
            program_.functions.push_back(std::move(clone));
            walk_new_concrete_function(program_.functions.size() - 1);
            if (!program_.functions.back().template_params.empty()) {
                generic_template_indices_[program_.functions.back().name].push_back(program_.functions.size() - 1);
            }
        }
    }

    [[nodiscard]] static const Type* find_type_replacement(const std::vector<std::pair<std::string, Type>>& replacements,
                                                           const std::string& name) {
        for (const auto& [param_name, replacement] : replacements) {
            if (param_name == name) return &replacement;
        }
        return nullptr;
    }

    [[nodiscard]] Type instantiate_type_pattern(
        const Type& type, const std::vector<std::pair<std::string, Type>>& replacements,
        const std::unordered_map<std::string, std::vector<Type>>& pack_replacements) {
        if (!type.is_pack_expansion && type.kind == TypeKind::Named && type.template_args.empty() &&
            type.non_type_args.empty()) {
            if (const Type* replacement = find_type_replacement(replacements, type.name)) return *replacement;
        }
        Type result = type;
        result.is_pack_expansion = false;
        std::vector<Type> new_template_args;
        new_template_args.reserve(result.template_args.size());
        for (const Type& arg : result.template_args) {
            if (arg.is_pack_expansion && arg.kind == TypeKind::Named && pack_replacements.contains(arg.name)) {
                for (const Type& concrete : pack_replacements.at(arg.name)) new_template_args.push_back(concrete);
                continue;
            }
            new_template_args.push_back(instantiate_type_pattern(arg, replacements, pack_replacements));
        }
        result.template_args = std::move(new_template_args);
        if (result.pointee) {
            result.pointee =
                std::make_shared<Type>(instantiate_type_pattern(*result.pointee, replacements, pack_replacements));
        }
        if (result.element) {
            result.element =
                std::make_shared<Type>(instantiate_type_pattern(*result.element, replacements, pack_replacements));
        }
        if (result.function_return) {
            result.function_return = std::make_shared<Type>(
                instantiate_type_pattern(*result.function_return, replacements, pack_replacements));
        }
        std::vector<Type> new_function_params;
        new_function_params.reserve(result.function_params.size());
        for (const Type& param : result.function_params) {
            if (param.is_pack_expansion && param.kind == TypeKind::Named && pack_replacements.contains(param.name)) {
                for (const Type& concrete : pack_replacements.at(param.name)) new_function_params.push_back(concrete);
                continue;
            }
            new_function_params.push_back(instantiate_type_pattern(param, replacements, pack_replacements));
        }
        result.function_params = std::move(new_function_params);
        // ch05 §9.4: see the identical comment in substitute_type_param
        // above -- clone-then-substitute so the template primary's own
        // array_size_expr is never mutated in place. Both the ordinary
        // and pack replacements are applied, mirroring how nested Types
        // above are substituted via both replacement maps.
        if (result.array_size_expr) {
            ExprPtr cloned = clone_expr(*result.array_size_expr);
            substitute_type_params_in_expr(*cloned, replacements);
            substitute_type_packs_in_expr(*cloned, pack_replacements);
            result.array_size_expr = std::shared_ptr<Expr>(std::move(cloned));
        }
        return result;
    }

    [[nodiscard]] static bool bind_type_pattern(
        const std::string& name, const Type& concrete, TemplateInstantiationBindings& bindings) {
        if (const Type* existing = find_type_replacement(bindings.type_replacements, name)) {
            return types_equal(*existing, concrete);
        }
        bindings.type_replacements.emplace_back(name, concrete);
        return true;
    }

    [[nodiscard]] static bool bind_type_pack_pattern(
        const std::string& name, const std::vector<Type>& concretes, TemplateInstantiationBindings& bindings) {
        auto it = bindings.type_pack_replacements.find(name);
        if (it != bindings.type_pack_replacements.end()) {
            if (it->second.size() != concretes.size()) return false;
            for (std::size_t i = 0; i < concretes.size(); i++) {
                if (!types_equal(it->second[i], concretes[i])) return false;
            }
            return true;
        }
        bindings.type_pack_replacements[name] = concretes;
        return true;
    }

    [[nodiscard]] static bool match_type_pattern_list(
        const std::vector<Type>& patterns, const std::vector<Type>& concretes, const std::vector<GenericTypeParam>& params,
        TemplateInstantiationBindings& bindings) {
        std::function<bool(const Type&, const Type&)> match_one;
        std::function<bool(const std::vector<Type>&, const std::vector<Type>&)> match_list;
        match_list = [&](const std::vector<Type>& inner_patterns, const std::vector<Type>& inner_concretes) -> bool {
            if (!inner_patterns.empty()) {
                const Type& last = inner_patterns.back();
                if (last.is_pack_expansion && last.kind == TypeKind::Named) {
                    for (const GenericTypeParam& param : params) {
                        if (!param.is_pack || param.is_non_type || param.name != last.name) continue;
                        if (inner_concretes.size() + 1 < inner_patterns.size()) return false;
                        for (std::size_t i = 0; i + 1 < inner_patterns.size(); i++) {
                            if (!match_one(inner_patterns[i], inner_concretes[i])) return false;
                        }
                        std::vector<Type> pack_slice(
                            inner_concretes.begin() + static_cast<std::ptrdiff_t>(inner_patterns.size() - 1),
                            inner_concretes.end());
                        return bind_type_pack_pattern(param.name, pack_slice, bindings);
                    }
                }
            }
            if (inner_patterns.size() != inner_concretes.size()) return false;
            for (std::size_t i = 0; i < inner_patterns.size(); i++) {
                if (!match_one(inner_patterns[i], inner_concretes[i])) return false;
            }
            return true;
        };
        match_one = [&](const Type& pattern, const Type& concrete) -> bool {
            if (!pattern.is_pack_expansion && pattern.kind == TypeKind::Named && pattern.template_args.empty() &&
                pattern.non_type_args.empty()) {
                for (const GenericTypeParam& param : params) {
                    if (param.is_non_type || param.is_pack || param.name != pattern.name) continue;
                    return bind_type_pattern(param.name, concrete, bindings);
                }
            }
            if (pattern.kind != concrete.kind || pattern.is_const_qualified != concrete.is_const_qualified) return false;
            switch (pattern.kind) {
                case TypeKind::Named:
                    if (pattern.name != concrete.name || pattern.non_type_args.size() != concrete.non_type_args.size()) {
                        return false;
                    }
                    return match_list(pattern.template_args, concrete.template_args);
                case TypeKind::Pointer:
                    return pattern.is_mutable_pointee == concrete.is_mutable_pointee &&
                           match_one(*pattern.pointee, *concrete.pointee);
                case TypeKind::Reference:
                    return pattern.is_mutable_ref == concrete.is_mutable_ref &&
                           pattern.is_rvalue_ref == concrete.is_rvalue_ref &&
                           match_one(*pattern.pointee, *concrete.pointee);
                case TypeKind::Span:
                    return pattern.is_mutable_ref == concrete.is_mutable_ref &&
                           match_one(*pattern.pointee, *concrete.pointee);
                case TypeKind::Array:
                    return pattern.array_size == concrete.array_size &&
                           match_one(*pattern.element, *concrete.element);
                case TypeKind::FunctionPointer:
                    if (pattern.is_unsafe_function_pointer != concrete.is_unsafe_function_pointer) return false;
                    [[fallthrough]];
                case TypeKind::Function:
                    return pattern.is_const_function == concrete.is_const_function &&
                           pattern.function_ref_qualifier == concrete.function_ref_qualifier &&
                           match_one(*pattern.function_return, *concrete.function_return) &&
                           match_list(pattern.function_params, concrete.function_params);
            }
            return false;
        };
        return match_list(patterns, concretes);
    }

    struct OrdinaryClassTemplateSelection {
        const ClassDef* def = nullptr;
        TemplateInstantiationBindings bindings;
    };

    [[nodiscard]] OrdinaryClassTemplateSelection select_ordinary_class_template(
        const std::string& template_name, const std::vector<Type>& concrete_args, SourceLocation loc) const {
        OrdinaryClassTemplateSelection primary_selection;
        bool have_primary_definition = false;
        bool have_primary_forward_decl = false;
        std::vector<OrdinaryClassTemplateSelection> matching_specializations;

        auto owner_it = ordinary_class_template_owner_ids_by_name_.find(template_name);
        if (owner_it == ordinary_class_template_owner_ids_by_name_.end()) {
            return primary_selection;
        }

        for (const std::string& owner_id : owner_it->second) {
            const ClassDef* candidate = class_template_by_owner_id(owner_id);
            if (candidate == nullptr || candidate->is_variadic_primary_template || candidate->is_variadic_specialization) {
                continue;
            }
            if (candidate->is_partial_specialization) {
                if (candidate->specialization_template_args.size() != concrete_args.size()) continue;
                TemplateInstantiationBindings bindings;
                if (!match_type_pattern_list(candidate->specialization_template_args, concrete_args, candidate->template_params,
                                             bindings)) {
                    continue;
                }
                matching_specializations.push_back(OrdinaryClassTemplateSelection{candidate, std::move(bindings)});
                continue;
            }
            if (candidate->template_params.size() != concrete_args.size()) continue;
            TemplateInstantiationBindings bindings;
            bool valid = true;
            for (std::size_t param_index = 0; param_index < candidate->template_params.size(); ++param_index) {
                const GenericTypeParam& param = candidate->template_params[param_index];
                if (param.is_non_type || param.is_pack) {
                    valid = false;
                    break;
                }
                bindings.type_replacements.emplace_back(param.name, concrete_args[param_index]);
            }
            if (!valid) continue;
            OrdinaryClassTemplateSelection selection{candidate, std::move(bindings)};
            if (candidate->is_forward_declaration) {
                if (!have_primary_definition) {
                    primary_selection = std::move(selection);
                    have_primary_forward_decl = true;
                }
            } else {
                primary_selection = std::move(selection);
                have_primary_definition = true;
            }
        }

        if (matching_specializations.size() > 1) {
            throw DataflowError("multiple partial specializations of '" + template_name +
                                    "' match this concrete argument list; this version requires an unambiguous "
                                    "single best match",
                                loc);
        }
        if (!matching_specializations.empty()) return matching_specializations.front();
        if (have_primary_definition) return primary_selection;
        if (have_primary_forward_decl) {
            throw DataflowError("'" + template_name +
                                    "' has no matching class-template definition for these concrete arguments "
                                    "(the primary template is only forward-declared)",
                                loc);
        }
        return primary_selection;
    }

    // ch05 §5.14 / ch11: for every class with any direct base (ordinary
    // or interface), synthesizes a "forwarding stub"
    // Function (Function::forwards_to) for every inherited method the
    // base defines (recursively -- including any forward the base class
    // or interface itself already synthesized from *its* own bases,
    // since declarations are processed in source order and every base is
    // already complete before a derived class naming it). This means
    // every other pass in this file (and every codegen call-resolution
    // path) resolves an inherited method call by simply finding
    // "DerivedClass_methodName" already present in program_.functions,
    // exactly like an ordinary, non-inherited method -- no separate
    // inheritance-aware fallback logic needed anywhere else. A
    // constructor/destructor ("_new"/"_delete") is never forwarded
    // here: constructor/base-destructor chaining is handled directly by
    // dedicated validation/codegen logic instead, and synthesizing a
    // same-named forwarding constructor would get in the way of a
    // derived class later defining its own constructor with a different
    // parameter list.
    void synthesize_inherited_method_forwards() {
        std::size_t original_class_count = program_.classes.size();
        for (std::size_t i = 0; i < original_class_count; i++) {
            // ch05 §5.14: a variadic specialization's own base-specifier
            // (e.g. "Tuple", set by parse_variadic_specialization's base-
            // clause handling) names the *template*, not a real,
            // concrete base class -- there is nothing to forward yet
            // (neither of the doc's own variadic examples defines a
            // method on a specialization at all; see this class's own
            // instantiate_variadic_generic_type comment). The real,
            // concrete per-level base chain is instead built directly
            // by instantiate_variadic_generic_type once resolve_generic_
            // types actually instantiates a concrete `Tuple<...>` --
            // skipped here explicitly rather than relying on
            // method_templates_of happening to return empty.
            if (program_.classes[i].is_variadic_primary_template || program_.classes[i].is_variadic_specialization) {
                continue;
            }
            std::string derived_name = program_.classes[i].name;
            std::vector<std::string> namespace_path_copy = program_.classes[i].namespace_path;
            bool is_exported_copy = program_.classes[i].is_exported;
            for (const BaseSpecifier& base : program_.classes[i].base_specifiers) {
                std::string base_name = base.base_type.name;
                std::vector<Function> base_methods = methods_of_type_name(base_name);
                for (const Function& base_method : base_methods) {
                    if (base_method.is_static) continue;
                    std::string suffix = base_method.name.substr(base_name.size());
                    if (suffix == "_new" || suffix == "_delete") continue;
                    std::string derived_method_name = derived_name + suffix;
                    bool already_defined = false;
                    for (const Function& fn : program_.functions) {
                        if (fn.name != derived_method_name || fn.params.size() != base_method.params.size()) continue;
                        bool same_signature = true;
                        for (std::size_t p = 1; p < fn.params.size(); ++p) {
                            if (!types_equal(fn.params[p].type, base_method.params[p].type)) {
                                same_signature = false;
                                break;
                            }
                        }
                        if (same_signature) {
                            already_defined = true;
                            break;
                        }
                    }
                    if (already_defined) continue;

                    Function forward;
                    forward.name = derived_method_name;
                    forward.loc = base_method.loc;
                    forward.return_type = base_method.return_type;
                    forward.namespace_path = namespace_path_copy;
                    forward.is_exported = is_exported_copy;
                    forward.member_owner_class = derived_name;
                    forward.receiver_ref_qualifier = base_method.receiver_ref_qualifier;
                    forward.access = base_method.access;
                    forward.forwards_to = base_method.name;
                    forward.body = nullptr;
                    Param this_param;
                    this_param.name = "this";
                    Type this_type;
                    this_type.kind = TypeKind::Reference;
                    this_type.pointee = std::make_shared<Type>(named_type(derived_name));
                    this_type.is_mutable_ref = base_method.params[0].type.is_mutable_ref;
                    this_param.type = std::move(this_type);
                    forward.params.push_back(std::move(this_param));
                    for (std::size_t p = 1; p < base_method.params.size(); p++) {
                        forward.params.push_back(base_method.params[p]);
                    }
                    program_.functions.push_back(std::move(forward));
                }
            }
        }
    }

    // ch05 §5.14: checks every generic class's own method bodies once,
    // abstractly, at their own definition (ch05 §5.11/§5.14's "checked
    // once at that method's own definition" principle, decomposed per
    // member) -- for each method, substitutes its own constraint's
    // witness (that method's own concept's existing witness class if it
    // has a `requires Concept<T>` clause, or a single, shared,
    // globally-empty "bare witness" struct otherwise -- representing "no
    // operations guaranteed beyond the universal move/store/pass-
    // through/return baseline", ch05 §5.11/§5.14's own words) for the
    // class's own type parameter, both in the method's own signature and
    // throughout its (deep-cloned) body, and in a temporary "checking
    // class" holding the class's own fields substituted the same way.
    // The checking class/method pair is marked so codegen never emits it
    // (ClassDef::is_synthetic_check_only) and so it's checked normally
    // by movecheck but excluded from codegen exactly like an ordinary
    // generic function template (Function::is_generic_template) -- it
    // is never itself reachable from any real call site, existing
    // purely to be type-checked.
    //
    // Deliberately generates one checking class/method pair *per
    // method* rather than trying to detect and reuse ones that happen
    // to share the same witness substitution -- simpler, and the
    // redundant work is harmless (v0.1 doesn't need to be optimal, only
    // correct) -- unlike generic *function* clones (get_or_create_clone),
    // which are cached because they're reachable from arbitrarily many
    // real call sites and would otherwise be duplicated per call.
    //
    // Generic *structs* have no methods to check this way at all (ch04
    // §4.1: no methods, ever) -- their own type parameter is always
    // concept-constrained (parser-enforced), and the only thing that
    // constraint could possibly gate (field triviality) is already
    // re-verified at every concrete instantiation by the ordinary
    // declare_struct check codegen already performs -- nothing here
    // would add anything, so structs are skipped entirely.
    void check_generic_type_methods_once() {
        // Index-based, snapshotting the original class count up front --
        // same reasoning as resolve_generic_types: this loop's own body
        // pushes new entries into program_.classes/program_.functions
        // (once per method), which can reallocate their backing storage.
        // Everything needed from `program_.classes[i]` is copied out
        // into local variables *before* the first push_back in each
        // outer iteration -- a `ClassDef&`/`GenericTypeParam&` reference
        // held across it (as an earlier version of this function did)
        // would silently dangle on the very next inner-loop iteration.
        std::size_t original_class_count = program_.classes.size();
        for (std::size_t i = 0; i < original_class_count; i++) {
            if (program_.classes[i].template_params.empty()) continue;
            if (program_.classes[i].is_partial_specialization) continue;
            // ch05 §5.14: a variadic primary template's own bodyless
            // forward declaration, or one of its two fixed
            // specializations, is never itself witness-checked this
            // way -- its "template_params" don't name a single ordinary
            // type parameter the way an the-generic-class-phase-1
            // shape's own does (a pack like "Ts"/"Tail" is never a real
            // type substitutable by a witness at all, and neither
            // variadic shape has ever needed per-method abstract
            // checking so far -- see this class's own constructor
            // comment on variadic_generic_type_names_).
            if (program_.classes[i].is_variadic_primary_template || program_.classes[i].is_variadic_specialization) {
                continue;
            }
            std::vector<GenericTypeParam> template_params = program_.classes[i].template_params;
            std::vector<ClassField> fields_copy = program_.classes[i].fields;
            std::string class_name_copy = program_.classes[i].name;
            std::string owner_id_copy = program_.classes[i].template_owner_id;
            std::vector<Function> methods = method_templates_of_owner(owner_id_copy);
            for (const Function& method_tmpl : methods) {
                if (!method_tmpl.template_params.empty()) continue;
                std::vector<std::pair<std::string, Type>> type_replacements;
                type_replacements.reserve(template_params.size());
                for (std::size_t param_index = 0; param_index < template_params.size(); ++param_index) {
                    const GenericTypeParam& param = template_params[param_index];
                    if (param.is_non_type) continue;
                    std::string witness_name;
                    if (param_index == 0 && !method_tmpl.method_requires_concept.empty()) {
                        witness_name = method_tmpl.method_requires_concept;
                    } else if (!param.concept_name.empty()) {
                        witness_name = param.concept_name;
                    } else {
                        witness_name = bare_witness_struct_name();
                    }
                    type_replacements.emplace_back(param.name, named_type(witness_name));
                }

                std::string check_class_name = "__genchk" + std::to_string(generic_check_counter_++);
                ClassDef check_class;
                check_class.name = check_class_name;
                check_class.is_synthetic_check_only = true;
                check_class.thread_movable_override = program_.classes[i].thread_movable_override;
                check_class.thread_shareable_override = program_.classes[i].thread_shareable_override;
                if (program_.classes[i].thread_movable_if_movable_expr) {
                    check_class.thread_movable_if_movable_expr = clone_expr(*program_.classes[i].thread_movable_if_movable_expr);
                    substitute_type_params_in_expr(*check_class.thread_movable_if_movable_expr, type_replacements);
                    resolve_generic_types_in_expr(*check_class.thread_movable_if_movable_expr);
                }
                if (program_.classes[i].thread_movable_if_shareable_expr) {
                    check_class.thread_movable_if_shareable_expr = clone_expr(*program_.classes[i].thread_movable_if_shareable_expr);
                    substitute_type_params_in_expr(*check_class.thread_movable_if_shareable_expr, type_replacements);
                    resolve_generic_types_in_expr(*check_class.thread_movable_if_shareable_expr);
                }
                std::unordered_map<std::string, Type> field_types;
                for (const ClassField& f : fields_copy) {
                    ClassField nf;
                    nf.name = f.name;
                    nf.access = f.access;
                    nf.type = substitute_type_params(f.type, type_replacements);
                    if (f.default_initializer) {
                        nf.default_initializer = f.default_initializer;
                        if (nf.default_initializer->expr) {
                            substitute_type_params_in_expr(*nf.default_initializer->expr, type_replacements);
                            resolve_generic_types_in_expr(*nf.default_initializer->expr);
                        }
                        for (const ExprPtr& arg : nf.default_initializer->brace_args) {
                            substitute_type_params_in_expr(*arg, type_replacements);
                            resolve_generic_types_in_expr(*arg);
                        }
                    }
                    field_types[nf.name] = nf.type;
                    check_class.fields.push_back(std::move(nf));
                }
                program_.classes.push_back(std::move(check_class));

                Function check_fn;
                // Keeps the "_methodName" suffix (e.g. "_push"), just
                // against the checking class's own synthesized name
                // instead of the template's -- mirrors ClassName_
                // memberName's own established scheme.
                check_fn.name = check_class_name + method_suffix_after_owner_prefix(method_tmpl, class_name_copy, owner_id_copy);
                check_fn.loc = method_tmpl.loc;
                check_fn.is_generic_template = true;
                check_fn.return_type = substitute_type_params(method_tmpl.return_type, type_replacements);
                check_fn.params.reserve(method_tmpl.params.size());
                for (const Param& p : method_tmpl.params) {
                    Param np;
                    np.name = p.name;
                    if (p.name == "this") {
                        Type this_type;
                        this_type.kind = TypeKind::Reference;
                        this_type.pointee = std::make_shared<Type>(named_type(check_class_name));
                        this_type.is_mutable_ref = p.type.is_mutable_ref;
                        np.type = std::move(this_type);
                    } else {
                        np.type = substitute_type_params(p.type, type_replacements);
                    }
                    check_fn.params.push_back(std::move(np));
                }
                check_fn.body = method_tmpl.body ? clone_stmt(*method_tmpl.body) : nullptr;
                if (check_fn.body) substitute_type_params_in_stmt(*check_fn.body, type_replacements);
                // ch05 §5.11/§5.14: "calling any method on it or applying
                // any operator to it is a compile error" -- for the
                // *bare* (unconstrained) case specifically (never the
                // concept-constrained one: that witness genuinely has
                // whatever methods its own requires-expression declares,
                // already validated normally by check_moves's ordinary
                // per-function walk over this very check_fn once it's
                // pushed below). The bare witness struct
                // (bare_witness_struct_name) has zero fields/methods by
                // construction, so this can never itself be a false
                // positive -- see reject_calls_on_bare_witness_type's
                // own comment for why check_moves's ordinary call-
                // argument-checking can't already catch this on its own.
                bool uses_bare_witness = false;
                for (const auto& [_, replacement] : type_replacements) {
                    if (replacement.kind == TypeKind::Named && replacement.name == bare_witness_struct_name()) {
                        uses_bare_witness = true;
                        break;
                    }
                }
                if (check_fn.body && uses_bare_witness) {
                    reject_calls_on_bare_witness_type(*check_fn.body, check_class_name, bare_witness_struct_name(),
                                                      field_types);
                }
                program_.functions.push_back(std::move(check_fn));
            }
        }
    }

    // ch05 §5.11/§5.14: recursively walks `stmt` (a synthesized check
    // function's body, check_generic_type_methods_once's own comment)
    // for any Call expression whose receiver -- resolved through a
    // chain of plain Identifier/Member projections, using `field_types`
    // for the one-level "this.field" case and `this_class_name` for
    // `this` itself -- is exactly the bare witness struct
    // (`bare_witness_name`), and throws immediately if one is found: a
    // bare (unconstrained) type parameter guarantees nothing at all, so
    // *any* method call on it is invalid, unconditionally, with no
    // possible false positive (the witness has zero fields/methods by
    // construction). This exists because check_moves's own ordinary
    // call-argument-checking (check_call_arguments) can't reliably catch
    // this on its own: a Member-based receiver's callee resolves to an
    // unmangled, unqualified name it has no way to confirm doesn't exist
    // anywhere else in the whole program (see resolve_callee_signature's
    // own documented scope limitation), and even a precise resolution
    // would find nothing to compare against here anyway, since this
    // synthesized check function is deliberately excluded from codegen
    // (ClassDef::is_synthetic_check_only) -- codegen's own "call to
    // unknown function" check, which every real, compiled method call
    // still gets, never runs for it at all.
    void reject_calls_on_bare_witness_type(const Stmt& stmt, const std::string& this_class_name,
                                            const std::string& bare_witness_name,
                                            const std::unordered_map<std::string, Type>& field_types) {
        // Resolves `expr`'s own type, restricted to exactly the two
        // shapes needed here: a bare `this` (always `this_class_name`)
        // and a single `this.field`/`self.field` projection off it
        // (via `field_types`) -- anything else (a plain local, a
        // deeper chain, a call result, ...) returns nullopt, since a
        // bare type parameter's *only* legal use here is as a field of
        // the generic type's own instance.
        std::function<std::optional<std::string>(const Expr&)> resolve_type_name =
            [&](const Expr& e) -> std::optional<std::string> {
            if (e.kind == ExprKind::Identifier && e.name == "this") return this_class_name;
            if (e.kind == ExprKind::Member) {
                std::optional<std::string> base = resolve_type_name(*e.lhs);
                if (base.has_value() && *base == this_class_name) {
                    auto it = field_types.find(e.name);
                    if (it != field_types.end() && it->second.kind == TypeKind::Named) return it->second.name;
                }
            }
            return std::nullopt;
        };
        std::function<void(const Expr&)> walk_expr = [&](const Expr& e) {
            if (e.kind == ExprKind::Call) {
                if (e.lhs) {
                    std::optional<std::string> receiver_type = resolve_type_name(*e.lhs);
                    if (receiver_type.has_value() && *receiver_type == bare_witness_name) {
                        throw DataflowError(
                            "cannot call method '" + e.name +
                                "' on a value of a bare (unconstrained) generic type parameter -- it guarantees no "
                                "methods at all (spec ch05 §5.11/§5.14); constrain it with a concept to allow this",
                            e.loc);
                    }
                    walk_expr(*e.lhs);
                }
                for (const auto& arg : e.args) walk_expr(*arg);
                return;
            }
            if (e.lhs) walk_expr(*e.lhs);
            if (e.rhs) walk_expr(*e.rhs);
            for (const auto& arg : e.args) walk_expr(*arg);
        };
        std::function<void(const Stmt&)> walk_stmt = [&](const Stmt& s) {
            if (s.init) walk_expr(*s.init);
            for (const auto& arg : s.ctor_args) walk_expr(*arg);
            if (s.expr) walk_expr(*s.expr);
            if (s.condition) walk_expr(*s.condition);
            if (s.then_branch) walk_stmt(*s.then_branch);
            if (s.else_branch) walk_stmt(*s.else_branch);
            for (const auto& inner : s.statements) walk_stmt(*inner);
        };
        walk_stmt(stmt);
    }


    // witness struct standing in for a bare (unconstrained) generic
    // type parameter -- see check_generic_type_methods_once's own
    // comment for why this is a struct (never registered as a "known
    // class" for movecheck's by-value-parameter/no-reassignment
    // restrictions, DataflowState::class_names -- deliberately: a bare
    // type parameter is checked optimistically, as if freely copyable).
    [[nodiscard]] std::string bare_witness_struct_name() {
        if (bare_witness_struct_name_.empty()) {
            bare_witness_struct_name_ = "__generic_bare_witness";
            StructDef witness;
            witness.name = bare_witness_struct_name_;
            program_.structs.push_back(std::move(witness));
        }
        return bare_witness_struct_name_;
    }

    // ch05 §5.14: resolves every not-yet-resolved generic-type
    // instantiation (Type::template_args non-empty) anywhere in the
    // program -- struct/class field types, every function/method's own
    // parameter and return types, and every VarDecl inside a body --
    // synthesizing a concrete instantiation (or reusing an
    // already-cached one, see instantiate_generic_type) and rewriting
    // the Type node's own `name`/clearing `template_args` in place.
    // Struct/class fields and function signatures are resolved first
    // (order doesn't actually matter -- a generic type's own concrete
    // instantiation never depends on anything else being walked -- but
    // this mirrors the natural "declarations before bodies" order), then
    // every function body's own VarDecls.
    void resolve_generic_types() {
        // ch05 §5.14: index-based throughout, snapshotting each original
        // count up front -- resolving one field/parameter/return-type
        // may itself synthesize new struct/class/function entries
        // (instantiate_generic_type), which can reallocate program_.
        // structs/classes/functions' own backing storage. A `Def&`/
        // `Function&` reference held *across* such a call (or even
        // across two separate field accesses straddling one) would
        // silently dangle -- every Type is therefore read into a local
        // copy, resolved by value (resolve_generic_type never mutates
        // through a reference into any of these vectors), and written
        // back via a *fresh* index-based access afterward, never a
        // cached reference spanning the call.
        std::size_t original_struct_count = program_.structs.size();
        for (std::size_t i = 0; i < original_struct_count; i++) {
            if (!program_.structs[i].template_params.empty()) continue; // the template itself, never instantiated in place
            std::size_t field_count = program_.structs[i].fields.size();
            for (std::size_t j = 0; j < field_count; j++) {
                Type old_type = program_.structs[i].fields[j].type;
                Type new_type = resolve_generic_type(old_type, SourceLocation{});
                program_.structs[i].fields[j].type = new_type;
            }
        }
        std::size_t original_class_count = program_.classes.size();
        for (std::size_t i = 0; i < original_class_count; i++) {
            if (!program_.classes[i].template_params.empty()) continue;
            // ch05 §5.14: the empty-pack base-case specialization
            // (`Tuple<>`) is the only variadic ClassDef shape whose own
            // template_params is empty -- it's still a template itself
            // (of the enclosing variadic primary template), never a
            // real, directly-instantiated class, so it's excluded here
            // exactly like every other template's own definition is
            // (see instantiate_variadic_generic_type, which is what
            // actually synthesizes a *concrete* base case).
            if (program_.classes[i].is_variadic_specialization) continue;
            std::size_t field_count = program_.classes[i].fields.size();
            for (std::size_t j = 0; j < field_count; j++) {
                Type old_type = program_.classes[i].fields[j].type;
                Type new_type = resolve_generic_type(old_type, SourceLocation{});
                program_.classes[i].fields[j].type = new_type;
            }
            if (program_.classes[i].thread_movable_if_movable_expr) {
                resolve_generic_types_in_expr(*program_.classes[i].thread_movable_if_movable_expr);
            }
            if (program_.classes[i].thread_movable_if_shareable_expr) {
                resolve_generic_types_in_expr(*program_.classes[i].thread_movable_if_shareable_expr);
            }
        }
        std::size_t original_count = program_.functions.size();
        for (std::size_t i = 0; i < original_count; i++) {
            if (belongs_to_unresolved_generic_type_template(program_.functions[i])) continue;
            // ch05 §5.11/§5.14: a full-header-form generic *function*'s
            // own template (e.g. `get`/`make`, Function::template_params
            // non-empty) is never resolved here at all -- its own
            // signature may contain a base-class-deduction pattern
            // (`TupleImpl<I, Head, Tail...>& t`) whose "arguments" are
            // only meaningful *symbolically*, referencing this
            // function's own not-yet-bound template parameters, not
            // real concrete types/values resolve_generic_type could
            // make sense of at all. Each concrete call site is instead
            // resolved directly by monomorphize_generic_function_call
            // (mirroring exactly how an abbreviated-Concept-auto-form
            // generic function's own body is similarly left untouched
            // here and only monomorphized per call site).
            if (!program_.functions[i].template_params.empty()) continue;
            SourceLocation loc = program_.functions[i].loc;
            std::size_t param_count = program_.functions[i].params.size();
            for (std::size_t j = 0; j < param_count; j++) {
                Type old_type = program_.functions[i].params[j].type;
                Type new_type = resolve_generic_type(old_type, loc);
                program_.functions[i].params[j].type = new_type;
            }
            Type old_return = program_.functions[i].return_type;
            Type new_return = resolve_generic_type(old_return, loc);
            program_.functions[i].return_type = new_return;
            // A function's own body is a stable, independently heap-
            // allocated tree (via StmtPtr) -- never relocated by
            // program_.functions/classes/structs reallocating elsewhere
            // -- so resolving Types *inside* it can safely mutate in
            // place (see resolve_generic_types_in_stmt/_in_expr).
            if (program_.functions[i].body) resolve_generic_types_in_stmt(*program_.functions[i].body);
        }
    }

    void resolve_generic_types_in_stmt(Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                stmt.type = resolve_generic_type(stmt.type, stmt.loc);
                if (stmt.init) resolve_generic_types_in_expr(*stmt.init);
                for (ExprPtr& arg : stmt.ctor_args) resolve_generic_types_in_expr(*arg);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) resolve_generic_types_in_expr(*stmt.expr);
                return;
            case StmtKind::If:
                resolve_generic_types_in_expr(*stmt.condition);
                resolve_generic_types_in_stmt(*stmt.then_branch);
                if (stmt.else_branch) resolve_generic_types_in_stmt(*stmt.else_branch);
                return;
            case StmtKind::While:
                resolve_generic_types_in_expr(*stmt.condition);
                resolve_generic_types_in_stmt(*stmt.then_branch);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) resolve_generic_types_in_stmt(*s);
                return;
        }
    }

    void resolve_generic_types_in_expr(Expr& expr) {
        // MakeUnique's element type / Lambda's explicit return type --
        // safe to mutate directly (see resolve_generic_types' own
        // comment: Expr nodes are stable, independent heap allocations).
        expr.type = resolve_generic_type(expr.type, expr.loc);
        if (expr.lhs) resolve_generic_types_in_expr(*expr.lhs);
        if (expr.rhs) resolve_generic_types_in_expr(*expr.rhs);
        for (ExprPtr& arg : expr.args) resolve_generic_types_in_expr(*arg);
        for (Param& p : expr.lambda_params) p.type = resolve_generic_type(p.type, expr.loc);
        for (LambdaCapture& c : expr.lambda_captures) {
            if (c.init) resolve_generic_types_in_expr(*c.init);
        }
        if (expr.lambda_body) resolve_generic_types_in_stmt(*expr.lambda_body);
        if (expr.kind == ExprKind::Call && expr.lhs == nullptr) {
            if (std::optional<StaticTemplateCallResolution> resolved =
                    resolve_static_template_call_target(expr.name, expr.loc)) {
                expr.name = resolved->concrete_class_name + "_" + resolved->member_name;
                expr.explicit_global_qualification = false;
            }
        }
        if (expr.kind == ExprKind::TypeTrait) {
            bool value = expr.name == "is_thread_movable" ? is_thread_movable(expr.type) : is_thread_shareable(expr.type);
            expr.kind = ExprKind::BoolLiteral;
            expr.bool_value = value;
            expr.name.clear();
            expr.lhs.reset();
            expr.rhs.reset();
            expr.args.clear();
        }
    }

    // ch05 §5.14: resolves a (possibly not-yet-resolved) generic-type
    // Type value, returning the fully-resolved result *by value* --
    // deliberately never mutating through a reference into
    // program_.functions/classes/structs directly (see this class's
    // other generic-type methods' identical concern): resolving one
    // instantiation may itself append new entries to any of those
    // (instantiate_generic_type), which can reallocate their own
    // backing storage. Operating purely on an owned copy (and, when
    // rebinding pointee/element, a *freshly allocated* shared_ptr rather
    // than mutating through the existing one, which some other, unrelated
    // Type value might still share) sidesteps every such hazard --
    // every caller reads into a local copy and writes the result back
    // via a fresh index-based access afterward, never holding a
    // reference across the call.
    [[nodiscard]] Type resolve_generic_type(Type type, SourceLocation loc) {
        // ch05 §5.14: a variadic generic type (`Tuple<int,bool,char>`,
        // or even the zero-argument `Tuple<>` base case) is checked
        // *before* the ordinary "template_args empty means not a
        // generic instantiation at all" fast path below -- a variadic
        // instantiation's own template_args being empty is itself
        // meaningful (the empty-pack case), unlike an ordinary,
        // non-generic Type (e.g. "int"), which never populates
        // template_args at all. The parser guarantees an *ordinary*
        // (non-variadic) generic type's own template_args is always
        // exactly 1 (see parse_unqualified_type), so this branch can
        // never misfire against one.
        if (variadic_generic_type_names_.contains(type.name)) {
            std::vector<Type> resolved_args;
            resolved_args.reserve(type.template_args.size());
            for (const Type& arg : type.template_args) resolved_args.push_back(resolve_generic_type(arg, loc));
            // ch05 §5.14: this Type's own non_type_args (e.g. the "0" in
            // `TupleImpl<0, int, bool, char>`) are ordinary, self-
            // contained expressions at a top-level use site like this
            // one (never referencing any enclosing template's own
            // parameter -- that symbolic-reference shape only ever
            // appears inside a *generic function's own* deduction-
            // pattern parameter type, which this pass never reaches at
            // all, see run()'s own guard) -- evaluated with an empty
            // parameter-value scope.
            std::vector<int> resolved_non_type_args;
            resolved_non_type_args.reserve(type.non_type_args.size());
            for (const std::shared_ptr<Expr>& arg : type.non_type_args) {
                resolved_non_type_args.push_back(evaluate_non_type_arg(*arg, {}));
            }
            std::string concrete_name =
                instantiate_variadic_generic_type(type.name, resolved_non_type_args, resolved_args, loc);
            type.name = concrete_name;
            type.template_args.clear();
            type.non_type_args.clear();
            return type;
        }
        if (type.template_args.empty()) {
            if (!type.non_type_args.empty()) {
                std::vector<int> resolved_non_type_args;
                resolved_non_type_args.reserve(type.non_type_args.size());
                for (const std::shared_ptr<Expr>& arg : type.non_type_args) {
                    resolved_non_type_args.push_back(evaluate_non_type_arg(*arg, {}));
                }
                std::string concrete_name = instantiate_non_type_generic_type(type.name, resolved_non_type_args, loc);
                type.name = concrete_name;
                type.non_type_args.clear();
                return type;
            }
            if (type.pointee) type.pointee = std::make_shared<Type>(resolve_generic_type(*type.pointee, loc));
            if (type.element) type.element = std::make_shared<Type>(resolve_generic_type(*type.element, loc));
            return type;
        }
        std::vector<Type> resolved_args;
        resolved_args.reserve(type.template_args.size());
        for (const Type& arg : type.template_args) resolved_args.push_back(resolve_generic_type(arg, loc));
        std::string concrete_name = instantiate_generic_type(type.name, resolved_args, loc);
        type.name = concrete_name;
        type.template_args.clear();
        return type;
    }


    // ch05 §5.14: synthesizes (or reuses an already-cached) concrete
    // instantiation of the generic class/struct template named
    // `template_name` for the concrete arguments `concrete_args`, and
    // returns its own mangled name. Validates the template's own
    // class/struct-level concept constraint(s) (if any) against each
    // concrete argument first -- a precise, immediate rejection here,
    // exactly like a generic function's own call-site concept check.
    // For a class template, clones every method whose own
    // `requires Concept<T>` clause (if any) the first concrete argument also
    // satisfies; a method whose own constraint *isn't* satisfied is
    // simply not cloned for this instantiation at all -- calling it
    // surfaces as an ordinary "unknown function" downstream, mirroring
    // the already-accepted precedent for an ungranted operation inside
    // an ordinary generic function's own body (ch05 §5.11) rather than
    // a bespoke "precise diagnostic" message this version doesn't
    // implement.
    [[nodiscard]] std::string instantiate_generic_type(const std::string& template_name,
                                                        const std::vector<Type>& concrete_args,
                                                        SourceLocation loc) {
        std::string cache_key = template_name;
        for (const Type& concrete_arg : concrete_args) {
            cache_key += "." + mangle_type_for_clone_name(concrete_arg);
        }
        auto cached = generic_type_instance_cache_.find(cache_key);
        if (cached != generic_type_instance_cache_.end()) return cached->second;
        generic_type_instance_cache_[cache_key] = cache_key;

        std::vector<Type> named_concretes;
        named_concretes.reserve(concrete_args.size());
        for (const Type& concrete_arg : concrete_args) {
            named_concretes.push_back(concrete_arg.kind == TypeKind::Reference ? *concrete_arg.pointee : concrete_arg);
        }

        // Structs and classes share the same instantiation shape except
        // for AccessSpecifier/method-cloning, so the two are handled
        // by two small, parallel branches rather than one deeply
        // conditional block.
        for (const StructDef& tmpl : program_.structs) {
            if (tmpl.name != template_name || tmpl.template_params.empty()) continue;
            if (tmpl.template_params.size() != named_concretes.size()) {
                throw DataflowError("'" + template_name + "' takes exactly " +
                                        std::to_string(tmpl.template_params.size()) + " template argument(s)",
                                    loc);
            }
            std::vector<std::pair<std::string, Type>> type_replacements;
            type_replacements.reserve(tmpl.template_params.size());
            for (std::size_t param_index = 0; param_index < tmpl.template_params.size(); ++param_index) {
                const GenericTypeParam& type_param = tmpl.template_params[param_index];
                if (type_param.is_non_type) {
                    throw DataflowError("'" + template_name + "' is not a type-parameter generic class/struct",
                                        loc);
                }
                check_type_param_constraint(type_param, named_concretes[param_index], template_name, loc);
                type_replacements.emplace_back(type_param.name, named_concretes[param_index]);
            }
            StructDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = tmpl.namespace_path;
            concrete.is_nodiscard = tmpl.is_nodiscard;
            concrete.nodiscard_reason = tmpl.nodiscard_reason;
            for (const StructField& f : tmpl.fields) {
                StructField nf;
                nf.name = f.name;
                nf.type = substitute_type_params(f.type, type_replacements);
                nf.type = resolve_generic_type(nf.type, loc);
                if (f.default_initializer) {
                    nf.default_initializer = f.default_initializer;
                    if (nf.default_initializer->expr) {
                        substitute_type_params_in_expr(*nf.default_initializer->expr, type_replacements);
                        resolve_generic_types_in_expr(*nf.default_initializer->expr);
                    }
                    for (const ExprPtr& arg : nf.default_initializer->brace_args) {
                        substitute_type_params_in_expr(*arg, type_replacements);
                        resolve_generic_types_in_expr(*arg);
                    }
                }
                concrete.fields.push_back(std::move(nf));
            }
            program_.structs.push_back(std::move(concrete));
            ordinary_generic_instance_info_[cache_key] = OrdinaryGenericInstanceInfo{template_name, named_concretes};
            return cache_key;
        }

        OrdinaryClassTemplateSelection class_selection = select_ordinary_class_template(template_name, named_concretes, loc);
        if (class_selection.def != nullptr) {
            const ClassDef& tmpl = *class_selection.def;
            std::string tmpl_owner_id = tmpl.template_owner_id;
            std::vector<std::string> tmpl_namespace_path = tmpl.namespace_path;
            std::vector<BaseSpecifier> tmpl_base_specifiers = tmpl.base_specifiers;
            bool tmpl_thread_movable_override = tmpl.thread_movable_override;
            bool tmpl_thread_shareable_override = tmpl.thread_shareable_override;
            ExprPtr tmpl_thread_movable_if_movable_expr =
                tmpl.thread_movable_if_movable_expr ? clone_expr(*tmpl.thread_movable_if_movable_expr) : nullptr;
            ExprPtr tmpl_thread_movable_if_shareable_expr =
                tmpl.thread_movable_if_shareable_expr ? clone_expr(*tmpl.thread_movable_if_shareable_expr) : nullptr;
            std::vector<GenericTypeParam> template_params_copy = tmpl.template_params;
            std::vector<Function> methods = method_templates_of_owner(tmpl_owner_id);
            for (const GenericTypeParam& type_param : template_params_copy) {
                if (type_param.is_non_type) {
                    throw DataflowError("'" + template_name + "' is not a type-parameter generic class/struct", loc);
                }
                if (type_param.is_pack) {
                    auto pack_it = class_selection.bindings.type_pack_replacements.find(type_param.name);
                    if (pack_it == class_selection.bindings.type_pack_replacements.end()) {
                        throw DataflowError("partial specialization of '" + template_name +
                                                "' did not bind required type pack parameter '" + type_param.name + "'",
                                            loc);
                    }
                    for (const Type& concrete : pack_it->second) {
                        check_type_param_constraint(type_param, concrete, template_name, loc);
                    }
                    continue;
                }
                const Type* bound = find_type_replacement(class_selection.bindings.type_replacements, type_param.name);
                if (bound == nullptr) {
                    throw DataflowError("partial specialization of '" + template_name +
                                            "' did not bind required type parameter '" + type_param.name + "'",
                                        loc);
                }
                check_type_param_constraint(type_param, *bound, template_name, loc);
            }

            std::vector<ClassField> fields_copy = tmpl.fields;
            ClassDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = tmpl_namespace_path;
            concrete.is_interface = tmpl.is_interface;
            concrete.base_specifiers = std::move(tmpl_base_specifiers);
            concrete.using_declarations = tmpl.using_declarations;
            concrete.thread_movable_override = tmpl_thread_movable_override;
            concrete.thread_shareable_override = tmpl_thread_shareable_override;
            concrete.is_nodiscard = tmpl.is_nodiscard;
            concrete.nodiscard_reason = tmpl.nodiscard_reason;
            if (tmpl_thread_movable_if_movable_expr) {
                concrete.thread_movable_if_movable_expr = std::move(tmpl_thread_movable_if_movable_expr);
                substitute_type_params_in_expr(*concrete.thread_movable_if_movable_expr,
                                               class_selection.bindings.type_replacements);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_movable_expr);
            }
            if (tmpl_thread_movable_if_shareable_expr) {
                concrete.thread_movable_if_shareable_expr = std::move(tmpl_thread_movable_if_shareable_expr);
                substitute_type_params_in_expr(*concrete.thread_movable_if_shareable_expr,
                                               class_selection.bindings.type_replacements);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_shareable_expr);
            }
            for (const ClassField& f : fields_copy) {
                ClassField nf;
                nf.name = f.name;
                nf.access = f.access;
                nf.type = instantiate_type_pattern(f.type, class_selection.bindings.type_replacements,
                                                   class_selection.bindings.type_pack_replacements);
                nf.type = resolve_generic_type(nf.type, loc);
                if (f.default_initializer) {
                    nf.default_initializer = f.default_initializer;
                    if (nf.default_initializer->expr) {
                        substitute_type_params_in_expr(*nf.default_initializer->expr,
                                                       class_selection.bindings.type_replacements);
                        resolve_generic_types_in_expr(*nf.default_initializer->expr);
                    }
                    for (const ExprPtr& arg : nf.default_initializer->brace_args) {
                        substitute_type_params_in_expr(*arg, class_selection.bindings.type_replacements);
                        resolve_generic_types_in_expr(*arg);
                    }
                }
                concrete.fields.push_back(std::move(nf));
            }
            program_.classes.push_back(std::move(concrete));
            ordinary_generic_instance_info_[cache_key] = OrdinaryGenericInstanceInfo{template_name, named_concretes};
            for (const Function& method_tmpl : methods) {
                if (!method_tmpl.method_requires_concept.empty()) {
                    auto concept_it = concepts_by_name_.find(method_tmpl.method_requires_concept);
                    const Type* constrained_type = class_selection.bindings.type_replacements.empty()
                                                       ? nullptr
                                                       : &class_selection.bindings.type_replacements.front().second;
                    bool satisfied = constrained_type != nullptr && concept_it != concepts_by_name_.end() &&
                                      type_satisfies_concept(*constrained_type, *concept_it->second, program_);
                    if (!satisfied) continue;
                }
                Function clone;
                clone.name = cache_key + method_suffix_after_owner_prefix(method_tmpl, template_name, tmpl_owner_id);
                clone.loc = method_tmpl.loc;
                clone.namespace_path = method_tmpl.namespace_path;
                clone.is_exported = false;
                clone.is_unsafe = method_tmpl.is_unsafe;
                clone.is_nodiscard = method_tmpl.is_nodiscard;
                clone.nodiscard_reason = method_tmpl.nodiscard_reason;
                clone.owning_module =
                    instantiate_imported_generic_locally(named_concretes, method_tmpl.owning_module) ? std::string()
                                                                                                      : method_tmpl.owning_module;
                clone.visibility_module = method_tmpl.visibility_module.empty() ? method_tmpl.owning_module
                                                                                : method_tmpl.visibility_module;
                clone.eval_mode = method_tmpl.eval_mode;
                clone.is_generic_template = method_tmpl.is_generic_template;
                clone.template_params = method_tmpl.template_params;
                clone.method_requires_concept = method_tmpl.method_requires_concept;
                clone.member_owner_class = cache_key;
                clone.is_static = method_tmpl.is_static;
                clone.is_virtual = method_tmpl.is_virtual;
                clone.is_override = method_tmpl.is_override;
                clone.is_pure = method_tmpl.is_pure;
                clone.is_defaulted = method_tmpl.is_defaulted;
                clone.access = method_tmpl.access;
                clone.return_type = instantiate_type_pattern(method_tmpl.return_type, class_selection.bindings.type_replacements,
                                                             class_selection.bindings.type_pack_replacements);
                clone.return_type = resolve_generic_type(clone.return_type, method_tmpl.loc);
                clone.return_lifetime = method_tmpl.return_lifetime;
                std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
                clone.params.reserve(method_tmpl.params.size());
                for (const Param& p : method_tmpl.params) {
                    if (p.name == "this") {
                        Param np;
                        np.name = p.name;
                        Type this_type;
                        this_type.kind = TypeKind::Reference;
                        this_type.pointee = std::make_shared<Type>(named_type(cache_key));
                        this_type.is_mutable_ref = p.type.is_mutable_ref;
                        np.type = std::move(this_type);
                        clone.params.push_back(std::move(np));
                        continue;
                    }
                    if (p.is_parameter_pack) {
                        std::optional<std::string> pack_name =
                            referenced_type_pack_param_name(p.type, template_params_copy);
                        auto pack_it = pack_name ? class_selection.bindings.type_pack_replacements.find(*pack_name)
                                                 : class_selection.bindings.type_pack_replacements.end();
                        if (pack_name && pack_it != class_selection.bindings.type_pack_replacements.end()) {
                            pack_param_names[p.name] = {};
                            for (std::size_t j = 0; j < pack_it->second.size(); j++) {
                                Param np = p;
                                np.is_parameter_pack = false;
                                np.name = p.name + "$" + std::to_string(j);
                                std::vector<std::pair<std::string, Type>> param_replacements =
                                    class_selection.bindings.type_replacements;
                                param_replacements.emplace_back(*pack_name, pack_it->second[j]);
                                np.type = instantiate_type_pattern(p.type, param_replacements, {});
                                np.type = resolve_generic_type(np.type, method_tmpl.loc);
                                clone.params.push_back(std::move(np));
                                pack_param_names[p.name].push_back(clone.params.back().name);
                            }
                            continue;
                        }
                    }
                    Param np = p;
                    np.type = instantiate_type_pattern(p.type, class_selection.bindings.type_replacements,
                                                       class_selection.bindings.type_pack_replacements);
                    np.type = resolve_generic_type(np.type, method_tmpl.loc);
                    clone.params.push_back(std::move(np));
                }
                clone.member_initializers = method_tmpl.member_initializers;
                for (MemberInitializer& init : clone.member_initializers) {
                    if (init.initializer.expr) {
                        substitute_type_params_in_expr(*init.initializer.expr,
                                                       class_selection.bindings.type_replacements);
                        substitute_type_packs_in_expr(*init.initializer.expr,
                                                      class_selection.bindings.type_pack_replacements);
                        resolve_generic_types_in_expr(*init.initializer.expr);
                    }
                    for (ExprPtr& arg : init.initializer.brace_args) {
                        substitute_type_params_in_expr(*arg, class_selection.bindings.type_replacements);
                        substitute_type_packs_in_expr(*arg, class_selection.bindings.type_pack_replacements);
                        resolve_generic_types_in_expr(*arg);
                    }
                }
                clone.body = method_tmpl.body ? clone_stmt(*method_tmpl.body) : nullptr;
                if (clone.body) {
                    substitute_type_params_in_stmt(*clone.body, class_selection.bindings.type_replacements);
                    for (const auto& [class_pack_name, concrete_pack_types] : class_selection.bindings.type_pack_replacements) {
                        std::vector<std::string> concrete_names;
                        concrete_names.reserve(concrete_pack_types.size());
                        for (const Type& concrete_type : concrete_pack_types) concrete_names.push_back(concrete_type.name);
                        expand_explicit_template_arg_packs_in_stmt(*clone.body, class_pack_name, concrete_names);
                    }
                    for (const auto& [pack_param_name, concrete_names] : pack_param_names) {
                        expand_pack_expansions_in_stmt(*clone.body, pack_param_name, concrete_names);
                        expand_pack_folds_in_stmt(*clone.body, pack_param_name, concrete_names);
                    }
                    resolve_generic_types_in_stmt(*clone.body);
                }
                known_function_names_.insert(clone.name);
                program_.functions.push_back(std::move(clone));
                walk_new_concrete_function(program_.functions.size() - 1);
                if (!program_.functions.back().template_params.empty()) {
                    generic_template_indices_[program_.functions.back().name].push_back(program_.functions.size() - 1);
                }
            }
            return cache_key;
        }

        throw DataflowError("'" + template_name + "' is not a declared generic type (ch05 §5.14)", loc);
    }

    [[nodiscard]] std::string instantiate_non_type_generic_type(const std::string& template_name,
                                                                const std::vector<int>& non_type_args,
                                                                SourceLocation loc) {
        std::string cache_key = template_name;
        for (int value : non_type_args) cache_key += "." + std::to_string(value);
        auto cached = generic_type_instance_cache_.find(cache_key);
        if (cached != generic_type_instance_cache_.end()) return cached->second;
        generic_type_instance_cache_[cache_key] = cache_key;

        for (const StructDef& tmpl : program_.structs) {
            if (tmpl.name != template_name || tmpl.template_params.size() != non_type_args.size() ||
                tmpl.template_params.empty() || !tmpl.template_params[0].is_non_type) {
                continue;
            }
            StructDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = tmpl.namespace_path;
            concrete.fields = tmpl.fields;
            concrete.is_nodiscard = tmpl.is_nodiscard;
            concrete.nodiscard_reason = tmpl.nodiscard_reason;
            program_.structs.push_back(std::move(concrete));
            return cache_key;
        }

        for (const ClassDef& tmpl : program_.classes) {
            if (tmpl.name != template_name || tmpl.template_params.size() != non_type_args.size() ||
                tmpl.template_params.empty() || !tmpl.template_params[0].is_non_type) {
                continue;
            }
            std::vector<GenericTypeParam> params_copy = tmpl.template_params;
            std::string owner_id_copy = tmpl.template_owner_id;
            std::vector<ClassField> fields_copy = tmpl.fields;
            ClassDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = tmpl.namespace_path;
            concrete.thread_movable_override = tmpl.thread_movable_override;
            concrete.thread_shareable_override = tmpl.thread_shareable_override;
            concrete.is_nodiscard = tmpl.is_nodiscard;
            concrete.nodiscard_reason = tmpl.nodiscard_reason;
            if (tmpl.thread_movable_if_movable_expr) {
                concrete.thread_movable_if_movable_expr = clone_expr(*tmpl.thread_movable_if_movable_expr);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_movable_expr);
            }
            if (tmpl.thread_movable_if_shareable_expr) {
                concrete.thread_movable_if_shareable_expr = clone_expr(*tmpl.thread_movable_if_shareable_expr);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_shareable_expr);
            }
            for (const ClassField& field : fields_copy) concrete.fields.push_back(field);
            program_.classes.push_back(std::move(concrete));

            std::vector<Function> methods = method_templates_of_owner(owner_id_copy);
            for (const Function& method_tmpl : methods) {
                Function clone;
                clone.name = cache_key + method_suffix_after_owner_prefix(method_tmpl, template_name, owner_id_copy);
                clone.loc = method_tmpl.loc;
                clone.namespace_path = method_tmpl.namespace_path;
                clone.is_exported = false;
                clone.is_unsafe = method_tmpl.is_unsafe;
                clone.is_nodiscard = method_tmpl.is_nodiscard;
                clone.nodiscard_reason = method_tmpl.nodiscard_reason;
                clone.owning_module = method_tmpl.owning_module;
                clone.visibility_module = method_tmpl.visibility_module.empty() ? method_tmpl.owning_module
                                                                                : method_tmpl.visibility_module;
                clone.eval_mode = method_tmpl.eval_mode;
                clone.is_generic_template = method_tmpl.is_generic_template;
                clone.template_params = method_tmpl.template_params;
                clone.method_requires_concept = method_tmpl.method_requires_concept;
                clone.member_owner_class = cache_key;
                clone.is_static = method_tmpl.is_static;
                clone.is_virtual = method_tmpl.is_virtual;
                clone.is_override = method_tmpl.is_override;
                clone.is_pure = method_tmpl.is_pure;
                clone.is_defaulted = method_tmpl.is_defaulted;
                clone.access = method_tmpl.access;
                clone.return_type = method_tmpl.return_type;
                clone.return_lifetime = method_tmpl.return_lifetime;
                clone.params.reserve(method_tmpl.params.size());
                for (const Param& param : method_tmpl.params) {
                    Param new_param = param;
                    new_param.name = param.name;
                    if (param.name == "this") {
                        Type this_type;
                        this_type.kind = TypeKind::Reference;
                        this_type.pointee = std::make_shared<Type>(named_type(cache_key));
                        this_type.is_mutable_ref = param.type.is_mutable_ref;
                        new_param.type = std::move(this_type);
                    } else {
                        new_param.type = param.type;
                    }
                    clone.params.push_back(std::move(new_param));
                }
                clone.member_initializers = method_tmpl.member_initializers;
                for (MemberInitializer& init : clone.member_initializers) {
                    if (init.initializer.expr) {
                        for (std::size_t i = 0; i < params_copy.size(); i++) {
                            substitute_non_type_param_in_expr(*init.initializer.expr, params_copy[i].name, non_type_args[i]);
                        }
                    }
                    for (ExprPtr& arg : init.initializer.brace_args) {
                        for (std::size_t i = 0; i < params_copy.size(); i++) {
                            substitute_non_type_param_in_expr(*arg, params_copy[i].name, non_type_args[i]);
                        }
                    }
                }
                clone.body = method_tmpl.body ? clone_stmt(*method_tmpl.body) : nullptr;
                if (clone.body) {
                    for (std::size_t i = 0; i < params_copy.size(); i++) {
                        substitute_non_type_param_in_stmt(*clone.body, params_copy[i].name, non_type_args[i]);
                    }
                }
                known_function_names_.insert(clone.name);
                program_.functions.push_back(std::move(clone));
                walk_new_concrete_function(program_.functions.size() - 1);
                if (!program_.functions.back().template_params.empty()) {
                    generic_template_indices_[program_.functions.back().name].push_back(program_.functions.size() - 1);
                }
            }
            return cache_key;
        }

        throw DataflowError("'" + template_name + "' is not a declared generic type (ch05 §5.14)", loc);
    }

    // ch05 §5.14: synthesizes (or reuses an already-cached) concrete
    // instantiation of a variadic generic type's own recursive-
    // inheritance chain -- one concrete ClassDef per level, from
    // `type_args[0]` down to the terminal empty-pack base case, each
    // level's own ordinary direct base pointing at the next level's own
    // synthesized name (mirroring exactly how the doc's own
    // `Tuple<Head, Tail...> : private Tuple<Tail...>` recursive
    // specialization is meant to expand). `non_type_args` holds every
    // *leading* non-type argument's own already-evaluated concrete
    // value (e.g. TupleImpl's own "Idx" -- empty for a primary template
    // with no non-type parameter at all, like plain Tuple). Returns the
    // *outermost* (fullest) level's own mangled name -- what
    // `TupleImpl<0,int,bool,char>` itself resolves to. Neither of the
    // doc's own two variadic examples (Tuple, TupleImpl) ever defines a
    // method on a variadic specialization, so unlike
    // instantiate_generic_type's class branch, no method-cloning
    // happens here at all -- see method_templates_of's own comment:
    // every specialization sharing the same `name` would be
    // indistinguishable by a `this`-type-pointee-name scan alone, so
    // naively reusing it here would be unsound (a known, deliberately
    // out-of-scope gap for now).
    [[nodiscard]] std::string instantiate_variadic_generic_type(const std::string& template_name,
                                                                 const std::vector<int>& non_type_args,
                                                                 const std::vector<Type>& type_args,
                                                                 SourceLocation loc) {
        std::string cache_key = template_name;
        for (int v : non_type_args) cache_key += "." + std::to_string(v);
        cache_key += type_args.empty() ? ".empty" : "";
        for (const Type& arg : type_args) cache_key += "." + mangle_type_for_clone_name(arg);
        auto cached = generic_type_instance_cache_.find(cache_key);
        if (cached != generic_type_instance_cache_.end()) return cached->second;
        generic_type_instance_cache_[cache_key] = cache_key;

        if (type_args.empty()) {
            // The empty-pack base case: `template<> class Tuple<>
            // { ... };`, or (with a leading non-type parameter, e.g.
            // TupleImpl) `template<int Idx> class TupleImpl<Idx>
            // { ... };` -- must already be declared (parser-enforced
            // for every variadic primary template that's ever
            // specialized at all, but not necessarily reached by any
            // *use* of the recursive case -- a `TupleImpl<0,int>`
            // instantiation still needs to bottom out at TupleImpl<1>'s
            // own concrete instance one level down).
            const ClassDef* base_case_tmpl = nullptr;
            for (const ClassDef& c : program_.classes) {
                if (c.name == template_name && c.is_variadic_specialization &&
                    c.template_params.size() == non_type_args.size()) {
                    base_case_tmpl = &c;
                    break;
                }
            }
            if (!base_case_tmpl) {
                throw DataflowError("'" + template_name + "' has no declared empty-pack base-case specialization "
                                                            "matching " +
                                         std::to_string(non_type_args.size()) + " non-type argument(s) (ch05 §5.14)",
                    loc);
            }
            std::vector<GenericTypeParam> params_copy = base_case_tmpl->template_params;
            std::string owner_id_copy = base_case_tmpl->template_owner_id;
            std::vector<ClassField> fields_copy = base_case_tmpl->fields;
            std::vector<Function> methods = method_templates_of_owner(owner_id_copy);
            ClassDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = base_case_tmpl->namespace_path;
            concrete.thread_movable_override = base_case_tmpl->thread_movable_override;
            concrete.thread_shareable_override = base_case_tmpl->thread_shareable_override;
            concrete.is_nodiscard = base_case_tmpl->is_nodiscard;
            concrete.nodiscard_reason = base_case_tmpl->nodiscard_reason;
            if (base_case_tmpl->thread_movable_if_movable_expr) {
                concrete.thread_movable_if_movable_expr = clone_expr(*base_case_tmpl->thread_movable_if_movable_expr);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_movable_expr);
            }
            if (base_case_tmpl->thread_movable_if_shareable_expr) {
                concrete.thread_movable_if_shareable_expr = clone_expr(*base_case_tmpl->thread_movable_if_shareable_expr);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_shareable_expr);
            }
            for (const ClassField& field : fields_copy) concrete.fields.push_back(field);
            program_.classes.push_back(std::move(concrete));
            clone_variadic_class_methods(cache_key, template_name, owner_id_copy, methods, params_copy,
                                         /*type_replacements=*/{}, /*pack_replacements=*/{}, non_type_args);
            variadic_instance_info_[cache_key] = VariadicInstanceInfo{template_name, non_type_args, type_args};
            return cache_key;
        }

        const ClassDef* recursive_tmpl = nullptr;
        for (const ClassDef& c : program_.classes) {
            if (c.name == template_name && c.is_variadic_specialization &&
                c.template_params.size() == non_type_args.size() + 2 &&
                c.template_params[non_type_args.size()].is_pack == false &&
                c.template_params[non_type_args.size() + 1].is_pack) {
                recursive_tmpl = &c;
                break;
            }
        }
        if (!recursive_tmpl) {
            throw DataflowError("'" + template_name + "' has no declared recursive-case specialization to match " +
                                     std::to_string(non_type_args.size()) + " non-type and " +
                                     std::to_string(type_args.size()) + " type argument(s) (ch05 §5.14)",
                loc);
        }

        // Copy everything needed out of `recursive_tmpl` *before* the
        // recursive instantiate_variadic_generic_type call below,
        // which pushes a new ClassDef into program_.classes and can
        // reallocate its backing storage -- `recursive_tmpl` itself
        // (a bare pointer into that vector) would otherwise dangle
        // (see this class's other generic-type methods' identical
        // concern).
        std::size_t leading_non_type_count = non_type_args.size();
        std::vector<GenericTypeParam> leading_non_type_params(
            recursive_tmpl->template_params.begin(), recursive_tmpl->template_params.begin() + leading_non_type_count);
        GenericTypeParam head_param = recursive_tmpl->template_params[leading_non_type_count];
        const BaseSpecifier* recursive_base = recursive_tmpl->direct_ordinary_base();
        std::string base_template_name = recursive_base != nullptr ? recursive_base->base_type.name : std::string();
        AccessSpecifier base_access = recursive_base != nullptr ? recursive_base->access : AccessSpecifier::Private;
        bool thread_movable_override = recursive_tmpl->thread_movable_override;
        bool thread_shareable_override = recursive_tmpl->thread_shareable_override;
        bool is_nodiscard = recursive_tmpl->is_nodiscard;
        std::string nodiscard_reason = recursive_tmpl->nodiscard_reason;
        bool is_interface = recursive_tmpl->is_interface;
        std::vector<ClassUsingDeclaration> using_declarations = recursive_tmpl->using_declarations;
        std::vector<ClassField> fields_copy = recursive_tmpl->fields;
        std::vector<std::string> namespace_path_copy = recursive_tmpl->namespace_path;
        std::shared_ptr<Expr> base_non_type_arg_expr =
            recursive_base != nullptr && !recursive_base->base_type.non_type_args.empty() ? recursive_base->base_type.non_type_args.front()
                                                                                           : nullptr;
        ExprPtr thread_movable_if_movable_expr_copy = recursive_tmpl->thread_movable_if_movable_expr
                                                          ? clone_expr(*recursive_tmpl->thread_movable_if_movable_expr)
                                                          : nullptr;
        ExprPtr thread_movable_if_shareable_expr_copy = recursive_tmpl->thread_movable_if_shareable_expr
                                                            ? clone_expr(*recursive_tmpl->thread_movable_if_shareable_expr)
                                                            : nullptr;
        std::vector<GenericTypeParam> template_params_copy = recursive_tmpl->template_params;
        std::string owner_id_copy = recursive_tmpl->template_owner_id;
        std::vector<Function> methods = method_templates_of_owner(owner_id_copy);

        Type head_concrete = type_args[0];
        std::vector<Type> tail_concrete(type_args.begin() + 1, type_args.end());
        check_type_param_constraint(head_param, head_concrete, template_name, loc);
        std::vector<std::pair<std::string, Type>> type_replacements = {{head_param.name, head_concrete}};
        std::unordered_map<std::string, std::vector<Type>> pack_replacements;
        pack_replacements[template_params_copy[leading_non_type_count + 1].name] = tail_concrete;

        // ch05 §5.14: the base's own non-type argument (e.g. "Idx + 1"
        // in TupleImpl's own `: public TupleImpl<Idx + 1, Tail...>`) is
        // evaluated using *this* level's own non-type parameter values
        // (e.g. this level's own concrete "Idx") -- empty when the base
        // template has no non-type parameter at all (plain Tuple's own
        // `: private Tuple<Tail...>`).
        std::string base_concrete_name;
        if (!base_template_name.empty()) {
            std::vector<int> base_non_type_args;
            if (base_non_type_arg_expr) {
                std::unordered_map<std::string, int> param_values;
                for (std::size_t i = 0; i < leading_non_type_params.size(); i++) {
                    param_values[leading_non_type_params[i].name] = non_type_args[i];
                }
                base_non_type_args.push_back(evaluate_non_type_arg(*base_non_type_arg_expr, param_values));
            }

            base_concrete_name =
                instantiate_variadic_generic_type(base_template_name, base_non_type_args, tail_concrete, loc);
        }

        ClassDef concrete;
        concrete.name = cache_key;
        concrete.namespace_path = namespace_path_copy;
        if (!base_concrete_name.empty()) {
            BaseSpecifier base;
            base.base_type = named_type(base_concrete_name);
            base.access = base_access;
            base.kind = BaseClassKind::OrdinaryClass;
            concrete.base_specifiers.push_back(std::move(base));
        }
        concrete.is_interface = is_interface;
        concrete.using_declarations = std::move(using_declarations);
        concrete.thread_movable_override = thread_movable_override;
        concrete.thread_shareable_override = thread_shareable_override;
        concrete.is_nodiscard = is_nodiscard;
        concrete.nodiscard_reason = std::move(nodiscard_reason);
        if (thread_movable_if_movable_expr_copy) {
            concrete.thread_movable_if_movable_expr = std::move(thread_movable_if_movable_expr_copy);
            substitute_type_params_in_expr(*concrete.thread_movable_if_movable_expr, type_replacements);
            resolve_generic_types_in_expr(*concrete.thread_movable_if_movable_expr);
        }
        if (thread_movable_if_shareable_expr_copy) {
            concrete.thread_movable_if_shareable_expr = std::move(thread_movable_if_shareable_expr_copy);
            substitute_type_params_in_expr(*concrete.thread_movable_if_shareable_expr, type_replacements);
            resolve_generic_types_in_expr(*concrete.thread_movable_if_shareable_expr);
        }
        for (const ClassField& f : fields_copy) {
            ClassField nf;
            nf.name = f.name;
            nf.access = f.access;
            nf.type = instantiate_type_pattern(f.type, type_replacements, pack_replacements);
            nf.type = resolve_generic_type(nf.type, loc);
            if (f.default_initializer) {
                nf.default_initializer = f.default_initializer;
                if (nf.default_initializer->expr) {
                    substitute_type_params_in_expr(*nf.default_initializer->expr, type_replacements);
                    resolve_generic_types_in_expr(*nf.default_initializer->expr);
                }
                for (const ExprPtr& arg : nf.default_initializer->brace_args) {
                    substitute_type_params_in_expr(*arg, type_replacements);
                    resolve_generic_types_in_expr(*arg);
                }
            }
            concrete.fields.push_back(std::move(nf));
        }
        program_.classes.push_back(std::move(concrete));
        clone_variadic_class_methods(cache_key, template_name, owner_id_copy, methods, template_params_copy,
                                     type_replacements, pack_replacements, non_type_args);
        variadic_instance_info_[cache_key] = VariadicInstanceInfo{template_name, non_type_args, type_args};
        return cache_key;
    }

    // ch05 §5.14: evaluates a variadic generic type's own non-type
    // argument expression down to a concrete int -- restricted to a
    // small, purpose-scoped shape (an integer literal; a bare
    // identifier, looked up in `param_values`, e.g. an enclosing
    // specialization's own non-type parameter name; or a `+` of the
    // two, e.g. "Idx + 1"), not a general compile-time constant-
    // expression evaluator (ch05 §5.14's own scoping: non-type
    // parameters are a narrow, purpose-built feature for a variadic
    // type's own recursive indexing, not a general `consteval`
    // mechanism). Throws a precise DataflowError for any other shape,
    // or an identifier not found in `param_values`.
    [[nodiscard]] int evaluate_non_type_arg(const Expr& expr, const std::unordered_map<std::string, int>& param_values) {
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return static_cast<int>(expr.int_value);
            case ExprKind::Identifier: {
                auto it = param_values.find(expr.name);
                if (it == param_values.end()) {
                    throw DataflowError("'" + expr.name +
                                         "' does not name a known non-type template parameter here (ch05 §5.14)",
                        expr.loc);
                }
                return it->second;
            }
            case ExprKind::Binary:
                if (expr.binary_op == BinaryOp::Add) {
                    return evaluate_non_type_arg(*expr.lhs, param_values) +
                           evaluate_non_type_arg(*expr.rhs, param_values);
                }
                [[fallthrough]];
            default:
                throw DataflowError("unsupported non-type template argument expression (ch05 §5.14 only supports "
                                     "an integer literal, a bare parameter name, or a '+' of the two)",
                    expr.loc);
        }
    }

    // ch05 §5.14: given `pattern` (a generic function's own base-class-
    // deduction parameter type, e.g. `TupleImpl<I, Head, Tail...>`,
    // still bearing its own *symbolic* template_args/non_type_args
    // referencing the enclosing function template's own parameter
    // names) and the actual call argument at `arg_index`, walks that
    // argument's own concrete type's base-class chain (via
    // variadic_instance_info_, populated by instantiate_variadic_
    // generic_type) looking for the unique level whose own recorded
    // non-type value(s) match `pattern`'s own (by-now-substituted-with-
    // already-bound-values) non-type arguments -- real, standard C++
    // template-argument deduction from a base class ([temp.deduct.call]),
    // not a scpp-specific mechanism (see ch05 §5.14's own doc comment).
    // Binds every one of `pattern`'s own type-parameter-position
    // symbolic references (e.g. "Head") to that matched level's own
    // concrete type argument, and records an upcast for `arg_index`
    // (its actual argument needs to be treated as the matched level's
    // own, less-derived concrete type when calling the monomorphized
    // clone -- codegen needs no actual conversion instruction for this,
    // since every level's own flattened layout is already byte-
    // compatible with its base, see ClassDef::base_specifiers' own
    // comment; this is purely a scpp-level type-compatibility fact).
    void deduce_via_base_class_chain(const Expr& expr, std::size_t arg_index, const Type& pattern, Body& body,
                                      std::unordered_map<std::string, Type>& type_bindings,
                                      std::unordered_map<std::string, int>& value_bindings,
                                      std::unordered_map<std::string, std::vector<Type>>& pack_bindings,
                                      std::vector<std::pair<std::size_t, Type>>& upcasts) {
        std::vector<int> search_non_type_values;
        search_non_type_values.reserve(pattern.non_type_args.size());
        for (const std::shared_ptr<Expr>& e : pattern.non_type_args) {
            search_non_type_values.push_back(evaluate_non_type_arg(*e, value_bindings));
        }

        std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_index], body, signatures_);
        if (!arg_type.has_value()) {
            throw DataflowError("cannot resolve the type of this argument for base-class deduction (ch05 §5.14)",
                expr.loc);
        }
        Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;

        std::string current_name = named.name;
        const VariadicInstanceInfo* matched = nullptr;
        std::string matched_name;
        while (true) {
            auto it = variadic_instance_info_.find(current_name);
            if (it == variadic_instance_info_.end()) break;
            if (it->second.template_name == pattern.name && it->second.non_type_values == search_non_type_values) {
                matched = &it->second;
                matched_name = current_name;
                break;
            }
            const ClassDef* cd = nullptr;
            for (const ClassDef& c : program_.classes) {
                if (c.name == current_name) {
                    cd = &c;
                    break;
                }
            }
            const BaseSpecifier* base = cd != nullptr ? cd->direct_ordinary_base() : nullptr;
            if (base == nullptr) break;
            current_name = base->base_type.name;
        }
        if (!matched) {
            throw DataflowError("no base class (direct or indirect) of the argument's own type matches the "
                                 "pattern '" +
                                     pattern.name + "<...>' (ch05 §5.14 base-class deduction)",
                expr.loc);
        }

        std::size_t ti = 0;
        for (const Type& sym : pattern.template_args) {
            if (sym.is_pack_expansion) {
                std::vector<Type> remaining_types;
                for (; ti < matched->type_args.size(); ti++) remaining_types.push_back(matched->type_args[ti]);
                if (!bind_type_pack_binding(pack_bindings, sym.name, remaining_types)) {
                    throw DataflowError("deduced types for template parameter pack '" + sym.name +
                                            "' disagree across base-class-deduction and later arguments",
                        expr.loc);
                }
                break;
            }
            if (ti < matched->type_args.size()) {
                if (!bind_type_binding(type_bindings, sym.name, matched->type_args[ti])) {
                    throw DataflowError("deduced type for template parameter '" + sym.name +
                                            "' disagrees across base-class-deduction and later arguments",
                        expr.loc);
                }
                ti++;
            }
        }

        Type target;
        target.kind = TypeKind::Named;
        target.name = matched_name;
        upcasts.emplace_back(arg_index, std::move(target));
    }

    // ch05 §5.15: is `name` one of the scalar type names this version
    // actually implements? (Only `int`/`bool`/`char` exist as real scpp
    // types so far -- see this file's own earlier notes on `size_t`/
    // fixed-width integers/`float32_t`/`float64_t` not existing yet;
    // every scalar is both thread-movable and thread-shareable.)
    [[nodiscard]] static bool is_scalar_type_name(const std::string& name) {
        return name == "int" || name == "bool" || name == "char";
    }

    // ch05 §5.15: recursively computes whether `type` is thread-movable
    // (mirrors Rust's `Send`) -- see this document's own §5.15 for the
    // full structural-derivation rules. `visiting` guards against
    // infinite recursion through a self-referential type (e.g. `class
    // Node { std::unique_ptr<Node> next; };`, a realistic linked-list
    // shape) -- coinductively assumed thread-movable the moment a cycle
    // is detected (the recursive occurrence contributes no *new*
    // violation beyond whatever the rest of the type's own fields
    // already determine), mirroring how a real compiler's own auto-trait
    // computation (e.g. Rust's `Send`/`Sync` auto-derivation) handles a
    // recursive type without looping forever.

[[nodiscard]] bool evaluate_thread_bool_constant_expr(const Expr& expr, std::unordered_set<std::string> visiting = {}) {
    return evaluate_thread_bool_constant_expr_for_program(expr, program_, std::move(visiting));
}

[[nodiscard]] bool is_thread_movable(const Type& type, std::unordered_set<std::string> visiting = {}) {
    return thread_movable_of(type, program_, std::move(visiting));
}

[[nodiscard]] bool is_thread_shareable(const Type& type, std::unordered_set<std::string> visiting = {}) {
    return thread_shareable_of(type, program_, std::move(visiting));
}

    // ch05 §5.15: checks a call to a generic function whose own
    // parameter is tagged `[[scpp::thread_movable]]`/
    // `[[scpp::thread_shareable]]` (Param::require_thread_movable/
    // require_thread_shareable) -- once that parameter's own
    // (possibly template-deduced) concrete type is known, rejects the
    // call with a precise diagnostic if the concrete type doesn't
    // actually satisfy the required property.
    [[nodiscard]] static bool bind_type_binding(std::unordered_map<std::string, Type>& bindings, const std::string& name,
                                               const Type& concrete) {
        auto it = bindings.find(name);
        if (it != bindings.end()) return types_equal(it->second, concrete);
        bindings[name] = concrete;
        return true;
    }

    [[nodiscard]] static bool bind_type_pack_binding(std::unordered_map<std::string, std::vector<Type>>& bindings,
                                                    const std::string& name, const std::vector<Type>& concretes) {
        auto it = bindings.find(name);
        if (it != bindings.end()) {
            if (it->second.size() != concretes.size()) return false;
            for (std::size_t i = 0; i < concretes.size(); i++) {
                if (!types_equal(it->second[i], concretes[i])) return false;
            }
            return true;
        }
        bindings[name] = concretes;
        return true;
    }

    bool deduce_template_bindings_from_type_pattern(
        const Type& pattern, const Type& concrete, const std::vector<GenericTypeParam>& template_params,
        std::unordered_map<std::string, Type>& type_bindings, std::unordered_map<std::string, int>& value_bindings,
        std::unordered_map<std::string, std::vector<Type>>& pack_bindings) {
        (void)value_bindings;
        std::function<bool(const std::vector<Type>&, const std::vector<Type>&)> deduce_type_list =
            [&](const std::vector<Type>& patterns, const std::vector<Type>& concretes) -> bool {
            if (!patterns.empty()) {
                const Type& last = patterns.back();
                if (last.is_pack_expansion && last.kind == TypeKind::Named && last.template_args.empty() &&
                    last.non_type_args.empty()) {
                    for (const GenericTypeParam& tp : template_params) {
                        if (!tp.is_pack || tp.is_non_type || tp.name != last.name) continue;
                        if (concretes.size() + 1 < patterns.size()) return false;
                        for (std::size_t i = 0; i + 1 < patterns.size(); i++) {
                            if (!deduce_template_bindings_from_type_pattern(
                                    patterns[i], concretes[i], template_params, type_bindings, value_bindings,
                                    pack_bindings)) {
                                return false;
                            }
                        }
                        std::vector<Type> pack_slice(
                            concretes.begin() + static_cast<std::ptrdiff_t>(patterns.size() - 1), concretes.end());
                        return bind_type_pack_binding(pack_bindings, tp.name, pack_slice);
                    }
                }
            }
            if (patterns.size() != concretes.size()) return false;
            for (std::size_t i = 0; i < patterns.size(); i++) {
                if (!deduce_template_bindings_from_type_pattern(
                        patterns[i], concretes[i], template_params, type_bindings, value_bindings, pack_bindings)) {
                    return false;
                }
            }
            return true;
        };

        if (!pattern.is_pack_expansion && pattern.kind == TypeKind::Named && pattern.template_args.empty() &&
            pattern.non_type_args.empty()) {
            for (const GenericTypeParam& tp : template_params) {
                if (tp.is_non_type || tp.name != pattern.name) continue;
                if (tp.is_pack) return bind_type_pack_binding(pack_bindings, tp.name, {concrete});
                return bind_type_binding(type_bindings, tp.name, concrete);
            }
        }

        if (pattern.kind != concrete.kind || pattern.is_const_qualified != concrete.is_const_qualified) return false;
        switch (pattern.kind) {
            case TypeKind::Named: {
                const std::vector<Type>* concrete_template_args = &concrete.template_args;
                if (pattern.name != concrete.name) {
                    auto it = ordinary_generic_instance_info_.find(concrete.name);
                    if (it == ordinary_generic_instance_info_.end() || it->second.template_name != pattern.name) {
                        return false;
                    }
                    concrete_template_args = &it->second.type_args;
                }
                if (!pattern.non_type_args.empty()) return false;
                return deduce_type_list(pattern.template_args, *concrete_template_args);
            }
            case TypeKind::Pointer:
                return pattern.is_mutable_pointee == concrete.is_mutable_pointee && pattern.pointee && concrete.pointee &&
                       deduce_template_bindings_from_type_pattern(*pattern.pointee, *concrete.pointee, template_params,
                                                                  type_bindings, value_bindings, pack_bindings);
            case TypeKind::Reference:
                return pattern.is_mutable_ref == concrete.is_mutable_ref &&
                       pattern.is_rvalue_ref == concrete.is_rvalue_ref && pattern.pointee && concrete.pointee &&
                       deduce_template_bindings_from_type_pattern(*pattern.pointee, *concrete.pointee, template_params,
                                                                  type_bindings, value_bindings, pack_bindings);
            case TypeKind::Span:
                return pattern.is_mutable_ref == concrete.is_mutable_ref && pattern.pointee && concrete.pointee &&
                       deduce_template_bindings_from_type_pattern(*pattern.pointee, *concrete.pointee, template_params,
                                                                  type_bindings, value_bindings, pack_bindings);
            case TypeKind::Array:
                return pattern.array_size == concrete.array_size && pattern.element && concrete.element &&
                       deduce_template_bindings_from_type_pattern(*pattern.element, *concrete.element, template_params,
                                                                  type_bindings, value_bindings, pack_bindings);
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if ((pattern.kind == TypeKind::FunctionPointer &&
                     pattern.is_unsafe_function_pointer != concrete.is_unsafe_function_pointer) ||
                    (pattern.kind == TypeKind::Function &&
                     (pattern.is_const_function != concrete.is_const_function ||
                      pattern.function_ref_qualifier != concrete.function_ref_qualifier)) ||
                    !pattern.function_return || !concrete.function_return ||
                    !deduce_template_bindings_from_type_pattern(*pattern.function_return, *concrete.function_return,
                                                               template_params, type_bindings, value_bindings,
                                                               pack_bindings)) {
                    return false;
                }
                return deduce_type_list(pattern.function_params, concrete.function_params);
        }
        return false;
    }

    [[nodiscard]] static bool type_depends_on_template_params(const Type& type,
                                                             const std::vector<GenericTypeParam>& template_params) {
        if (type.kind == TypeKind::Named) {
            for (const GenericTypeParam& tp : template_params) {
                if (tp.name == type.name) return true;
            }
        }
        for (const Type& arg : type.template_args) {
            if (type_depends_on_template_params(arg, template_params)) return true;
        }
        if (type.pointee && type_depends_on_template_params(*type.pointee, template_params)) return true;
        if (type.element && type_depends_on_template_params(*type.element, template_params)) return true;
        if (type.function_return && type_depends_on_template_params(*type.function_return, template_params)) return true;
        for (const Type& param : type.function_params) {
            if (type_depends_on_template_params(param, template_params)) return true;
        }
        return false;
    }

    [[nodiscard]] bool argument_type_can_participate_in_variadic_base_deduction(const Expr& expr, std::size_t arg_index,
                                                                                 const std::string& template_name,
                                                                                 Body& body) {
        if (arg_index >= expr.args.size()) return false;
        std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_index], body, signatures_);
        if (!arg_type.has_value()) return false;
        Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;

        std::string current_name = named.name;
        while (true) {
            auto it = variadic_instance_info_.find(current_name);
            if (it != variadic_instance_info_.end() && it->second.template_name == template_name) return true;
            const ClassDef* cd = nullptr;
            for (const ClassDef& c : program_.classes) {
                if (c.name == current_name) {
                    cd = &c;
                    break;
                }
            }
            const BaseSpecifier* base = cd != nullptr ? cd->direct_ordinary_base() : nullptr;
            if (base == nullptr) break;
            current_name = base->base_type.name;
        }
        return false;
    }

    [[nodiscard]] bool type_still_depends_on_unbound_template_params(
        Type type, const std::vector<GenericTypeParam>& template_params,
        const std::unordered_map<std::string, Type>& type_bindings,
        const std::unordered_map<std::string, std::vector<Type>>& pack_bindings) {
        for (const auto& [name, replacement] : type_bindings) type = substitute_type_param(type, name, replacement);
        type = substitute_type_packs(type, pack_bindings);
        return type_depends_on_template_params(type, template_params);
    }

    [[nodiscard]] Type apply_template_bindings_to_type(
        Type type, const std::unordered_map<std::string, Type>& type_bindings,
        const std::unordered_map<std::string, std::vector<Type>>& pack_bindings, SourceLocation loc) {
        for (const auto& [name, replacement] : type_bindings) type = substitute_type_param(type, name, replacement);
        type = substitute_type_packs(type, pack_bindings);
        return resolve_generic_type(std::move(type), loc);
    }

    struct DeferredTemplateObligation {
        std::size_t param_index = 0;
        std::size_t arg_index = 0;
        Type parameter_type_pattern;
    };

    void check_thread_safety_constraints(const Expr& expr, const Function& tmpl,
                                         const std::unordered_map<std::string, Type>& type_bindings,
                                         const std::unordered_map<std::string, std::vector<Type>>& pack_bindings) {
        for (std::size_t i = 0; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (!param.require_thread_movable && !param.require_thread_shareable) continue;
            Type concrete = apply_template_bindings_to_type(param.type, type_bindings, pack_bindings, expr.loc);
            if (param.require_thread_movable && !is_thread_movable(concrete)) {
                throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                         tmpl.name +
                                         "' does not satisfy '[[scpp::thread_movable]]' (ch05 §5.15)",
                    expr.loc);
            }
            if (param.require_thread_shareable && !is_thread_shareable(concrete)) {
                throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                         tmpl.name +
                                         "' does not satisfy '[[scpp::thread_shareable]]' (ch05 §5.15)",
                    expr.loc);
            }
        }
    }

    [[maybe_unused]] void maybe_instantiate_generic_constructor_overloads(const std::string& class_name,
                                                                           const std::vector<ExprPtr>& args,
                                                                           Body& body, SourceLocation loc) {
        std::string ctor_name = class_name + "_new";
        for (const Function& tmpl : program_.functions) {
            if (tmpl.name != ctor_name || tmpl.template_params.empty()) continue;
            try {
                std::unordered_map<std::string, Type> type_bindings;
                std::unordered_map<std::string, int> value_bindings;
                std::unordered_map<std::string, std::vector<Type>> pack_bindings;
                std::vector<std::pair<std::size_t, Type>> upcasts;
                std::vector<std::vector<Type>> concrete_pack_param_types(tmpl.params.size());

                std::size_t arg_cursor = 0;
                for (std::size_t i = 1; i < tmpl.params.size() && arg_cursor < args.size(); i++) {
                    if (tmpl.params[i].is_parameter_pack) {
                        std::optional<std::string> pack_type_name =
                            referenced_type_pack_param_name(tmpl.params[i].type, tmpl.template_params);
                        for (; arg_cursor < args.size(); arg_cursor++) {
                            std::optional<Type> arg_type = infer_expr_type(*args[arg_cursor], body, signatures_);
                            if (!arg_type.has_value()) continue;
                            Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                            Type replacement = named;
                            const Type& pack_param_type = tmpl.params[i].type;
                            const Type& underlying = pack_param_type.kind == TypeKind::Reference ? *pack_param_type.pointee
                                                                                                  : pack_param_type;
                            bool direct_pack = pack_type_name.has_value() && underlying.is_pack_expansion &&
                                               underlying.kind == TypeKind::Named &&
                                               underlying.template_args.empty() && underlying.non_type_args.empty() &&
                                               underlying.name == *pack_type_name;
                            if (pack_type_name.has_value() && !direct_pack) {
                                std::unordered_map<std::string, Type> arg_type_bindings;
                                std::unordered_map<std::string, int> arg_value_bindings;
                                std::unordered_map<std::string, std::vector<Type>> arg_pack_bindings;
                                if (!deduce_template_bindings_from_type_pattern(underlying, named, tmpl.template_params,
                                                                                arg_type_bindings, arg_value_bindings,
                                                                                arg_pack_bindings)) {
                                    continue;
                                }
                                auto pack_it = arg_pack_bindings.find(*pack_type_name);
                                if (pack_it == arg_pack_bindings.end() || pack_it->second.size() != 1) continue;
                                replacement = pack_it->second.front();
                            }
                            Type substituted = pack_type_name.has_value()
                                                   ? substitute_type_param(tmpl.params[i].type, *pack_type_name, replacement)
                                                   : tmpl.params[i].type;
                            substituted = resolve_generic_type(std::move(substituted), loc);
                            concrete_pack_param_types[i].push_back(std::move(substituted));
                        }
                        continue;
                    }
                    const Type& param_type = tmpl.params[i].type;
                    const Type& underlying = param_type.kind == TypeKind::Reference ? *param_type.pointee : param_type;
                    std::optional<Type> arg_type = infer_expr_type(*args[arg_cursor], body, signatures_);
                    if (arg_type.has_value()) {
                        Type concrete = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                        if (underlying.kind == TypeKind::Named && variadic_generic_type_names_.contains(underlying.name)) {
                            Expr fake_call;
                            fake_call.loc = loc;
                            for (const ExprPtr& arg : args) fake_call.args.push_back(clone_expr(*arg));
                            deduce_via_base_class_chain(fake_call, arg_cursor, underlying, body, type_bindings,
                                                        value_bindings, pack_bindings,
                                                        upcasts);
                        } else {
                            deduce_template_bindings_from_type_pattern(underlying, concrete, tmpl.template_params,
                                                                       type_bindings, value_bindings, pack_bindings);
                        }
                    }
                    arg_cursor++;
                }

                for (const GenericTypeParam& tp : tmpl.template_params) {
                    if (tp.is_pack) continue;
                    bool bound = tp.is_non_type ? value_bindings.contains(tp.name) : type_bindings.contains(tp.name);
                    if (!bound) throw DataflowError("constructor template parameter not deduced", loc);
                }

                Expr fake_call;
                fake_call.loc = loc;
                check_thread_safety_constraints(fake_call, tmpl, type_bindings, {});

                std::string cache_key = tmpl.name;
                for (const GenericTypeParam& tp : tmpl.template_params) {
                    if (tp.is_pack) continue;
                    cache_key += tp.is_non_type ? ("." + std::to_string(value_bindings[tp.name]))
                                                : ("." + mangle_type_for_clone_name(type_bindings[tp.name]));
                }
                for (std::size_t i = 0; i < tmpl.params.size(); i++) {
                    if (!tmpl.params[i].is_parameter_pack) continue;
                    for (const Type& t : concrete_pack_param_types[i]) cache_key += "." + mangle_type_for_clone_name(t);
                }
                if (generic_function_clone_cache_.contains(cache_key)) continue;
                generic_function_clone_cache_[cache_key] = tmpl.name;

                Function clone;
                std::string concrete_ctor_owner_name = class_name;
                if (std::optional<Type> this_type = this_type_of(tmpl)) concrete_ctor_owner_name = this_type->name;
                clone.name = class_name +
                             method_suffix_after_owner_prefix(tmpl, concrete_ctor_owner_name, tmpl.generic_method_owner_id);
                clone.loc = tmpl.loc;
                clone.namespace_path = tmpl.namespace_path;
                clone.is_exported = false;
                clone.is_unsafe = tmpl.is_unsafe;
                clone.is_nodiscard = tmpl.is_nodiscard;
                clone.nodiscard_reason = tmpl.nodiscard_reason;
                clone.visibility_module = tmpl.visibility_module.empty() ? tmpl.owning_module : tmpl.visibility_module;
                clone.member_owner_class = class_name;
                clone.is_static = tmpl.is_static;
                clone.access = tmpl.access;
                clone.return_type = tmpl.return_type;
                clone.return_lifetime = tmpl.return_lifetime;
                for (const auto& [name, replacement] : type_bindings) {
                    clone.return_type = substitute_type_param(clone.return_type, name, replacement);
                }
                clone.return_type = resolve_generic_type(clone.return_type, tmpl.loc);
                clone.params.reserve(tmpl.params.size());
                std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
                for (std::size_t i = 0; i < tmpl.params.size(); i++) {
                    if (tmpl.params[i].is_parameter_pack) {
                        pack_param_names[tmpl.params[i].name] = {};
                        for (std::size_t j = 0; j < concrete_pack_param_types[i].size(); j++) {
                            Param p = tmpl.params[i];
                            p.name = tmpl.params[i].name + "$" + std::to_string(j);
                            p.type = concrete_pack_param_types[i][j];
                            p.require_thread_movable = tmpl.params[i].require_thread_movable;
                            p.require_thread_shareable = tmpl.params[i].require_thread_shareable;
                            clone.params.push_back(std::move(p));
                            pack_param_names[tmpl.params[i].name].push_back(tmpl.params[i].name + "$" +
                                                                            std::to_string(j));
                        }
                        continue;
                    }
                    Param p = tmpl.params[i];
                    p.name = tmpl.params[i].name;
                    p.type = tmpl.params[i].type;
                    p.require_thread_movable = tmpl.params[i].require_thread_movable;
                    p.require_thread_shareable = tmpl.params[i].require_thread_shareable;
                    bool upcasted = false;
                    for (const auto& [idx, target] : upcasts) {
                        if (idx != i) continue;
                        if (p.type.kind == TypeKind::Reference) {
                            p.type.pointee = std::make_shared<Type>(target);
                        } else {
                            p.type = target;
                        }
                        upcasted = true;
                        break;
                    }
                    if (!upcasted) {
                        for (const auto& [name, replacement] : type_bindings) {
                            p.type = substitute_type_param(p.type, name, replacement);
                        }
                    }
                    p.type = resolve_generic_type(p.type, tmpl.loc);
                    clone.params.push_back(std::move(p));
                }
                clone.body = tmpl.body ? clone_stmt(*tmpl.body) : nullptr;
                if (clone.body) {
                    for (const auto& [name, replacement] : type_bindings) {
                        substitute_type_param_in_stmt(*clone.body, name, replacement);
                    }
                    for (const auto& [pack_name, concrete_names] : pack_param_names) {
                        expand_pack_expansions_in_stmt(*clone.body, pack_name, concrete_names);
                        expand_pack_folds_in_stmt(*clone.body, pack_name, concrete_names);
                    }
                    resolve_generic_types_in_stmt(*clone.body);
                }
                known_function_names_.insert(clone.name);
                program_.functions.push_back(std::move(clone));
                walk_new_concrete_function(program_.functions.size() - 1);
            } catch (const DataflowError&) {
                continue;
            }
        }
    }

    std::string instantiate_full_header_generic_clone(const Function& tmpl,
                                                      const std::unordered_map<std::string, Type>& type_bindings,
                                                      const std::unordered_map<std::string, int>& value_bindings,
                                                      const std::unordered_map<std::string, std::vector<Type>>& pack_bindings,
                                                      const std::vector<std::vector<Type>>& concrete_pack_param_types,
                                                      const std::vector<std::pair<std::size_t, Type>>& upcasts = {}) {
        std::string cache_key = tmpl.name;
        for (const GenericTypeParam& tp : tmpl.template_params) {
            if (tp.is_pack) {
                auto pack_it = pack_bindings.find(tp.name);
                if (pack_it != pack_bindings.end()) {
                    for (const Type& t : pack_it->second) cache_key += "." + mangle_type_for_clone_name(t);
                }
                continue;
            }
            cache_key += tp.is_non_type ? ("." + std::to_string(value_bindings.at(tp.name)))
                                        : ("." + mangle_type_for_clone_name(type_bindings.at(tp.name)));
        }
        for (std::size_t i = 0; i < tmpl.params.size(); i++) {
            if (!tmpl.params[i].is_parameter_pack) continue;
            for (const Type& t : concrete_pack_param_types[i]) cache_key += "." + mangle_type_for_clone_name(t);
        }
        auto cached = generic_function_clone_cache_.find(cache_key);
        if (cached != generic_function_clone_cache_.end()) return cached->second;
        generic_function_clone_cache_[cache_key] = cache_key;

        Function clone;
        clone.name = cache_key;
        clone.loc = tmpl.loc;
        clone.namespace_path = tmpl.namespace_path;
        clone.is_exported = false;
        clone.is_unsafe = tmpl.is_unsafe;
        clone.is_nodiscard = tmpl.is_nodiscard;
        clone.nodiscard_reason = tmpl.nodiscard_reason;
        clone.owning_module = tmpl.owning_module;
        clone.visibility_module = tmpl.visibility_module.empty() ? tmpl.owning_module : tmpl.visibility_module;
        clone.eval_mode = tmpl.eval_mode;
        clone.member_owner_class = tmpl.member_owner_class;
        clone.receiver_ref_qualifier = tmpl.receiver_ref_qualifier;
        clone.is_static = tmpl.is_static;
        clone.access = tmpl.access;
        clone.return_type = apply_template_bindings_to_type(tmpl.return_type, type_bindings, pack_bindings, tmpl.loc);
        clone.return_lifetime = tmpl.return_lifetime;
        clone.params.reserve(tmpl.params.size());
        std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
        for (std::size_t i = 0; i < tmpl.params.size(); i++) {
            if (tmpl.params[i].is_parameter_pack) {
                pack_param_names[tmpl.params[i].name] = {};
                for (std::size_t j = 0; j < concrete_pack_param_types[i].size(); j++) {
                    Param p = tmpl.params[i];
                    p.name = tmpl.params[i].name + "$" + std::to_string(j);
                    p.type = concrete_pack_param_types[i][j];
                    p.require_thread_movable = tmpl.params[i].require_thread_movable;
                    p.require_thread_shareable = tmpl.params[i].require_thread_shareable;
                    clone.params.push_back(std::move(p));
                    pack_param_names[tmpl.params[i].name].push_back(tmpl.params[i].name + "$" + std::to_string(j));
                }
                continue;
            }
            Param p = tmpl.params[i];
            p.name = tmpl.params[i].name;
            p.type = tmpl.params[i].type;
            p.require_thread_movable = tmpl.params[i].require_thread_movable;
            p.require_thread_shareable = tmpl.params[i].require_thread_shareable;
            bool upcasted = false;
            for (const auto& [idx, target] : upcasts) {
                if (idx != i) continue;
                if (p.type.kind == TypeKind::Reference) {
                    p.type.pointee = std::make_shared<Type>(target);
                } else {
                    p.type = target;
                }
                upcasted = true;
                break;
            }
            if (!upcasted) {
                p.type = apply_template_bindings_to_type(p.type, type_bindings, pack_bindings, tmpl.loc);
            }
            clone.params.push_back(std::move(p));
        }
        clone.body = tmpl.body ? clone_stmt(*tmpl.body) : nullptr;
        if (clone.body) {
            substitute_type_bindings_in_stmt(*clone.body, type_bindings);
            substitute_type_packs_in_stmt(*clone.body, pack_bindings);
            for (const auto& [pack_name, concrete_names] : pack_param_names) {
                expand_pack_expansions_in_stmt(*clone.body, pack_name, concrete_names);
                expand_pack_folds_in_stmt(*clone.body, pack_name, concrete_names);
            }
            resolve_generic_types_in_stmt(*clone.body);
        }
        known_function_names_.insert(clone.name);
        program_.functions.push_back(std::move(clone));
        walk_new_concrete_function(program_.functions.size() - 1);
        return cache_key;
    }

    void seed_explicit_template_arguments(const Expr& expr, const Function& tmpl,
                                          std::unordered_map<std::string, Type>& type_bindings,
                                          std::unordered_map<std::string, int>& value_bindings,
                                          std::unordered_map<std::string, std::vector<Type>>& pack_bindings) {
        std::size_t explicit_index = 0;
        for (std::size_t p = 0; p < tmpl.template_params.size(); p++) {
            const GenericTypeParam& tp = tmpl.template_params[p];
            if (tp.is_pack) {
                std::vector<Type> pack;
                while (explicit_index < expr.explicit_template_args.size()) {
                    const ExplicitTemplateArg& arg = expr.explicit_template_args[explicit_index++];
                    if (!arg.is_type) {
                        throw DataflowError("template parameter pack '" + tp.name + "' of generic function '" +
                                                tmpl.name + "' only accepts type arguments in this version",
                            expr.loc);
                    }
                    pack.push_back(arg.type);
                }
                if (!pack.empty()) pack_bindings[tp.name] = std::move(pack);
                continue;
            }
            if (explicit_index >= expr.explicit_template_args.size()) break;
            const ExplicitTemplateArg& arg = expr.explicit_template_args[explicit_index++];
            if (tp.is_non_type) {
                if (arg.is_type || !arg.value) {
                    throw DataflowError("template parameter '" + tp.name + "' of generic function '" + tmpl.name +
                                            "' is a non-type parameter, but a type argument was given (ch05 §5.11)",
                        expr.loc);
                }
                value_bindings[tp.name] = evaluate_non_type_arg(*arg.value, value_bindings);
            } else {
                if (!arg.is_type) {
                    throw DataflowError("template parameter '" + tp.name + "' of generic function '" + tmpl.name +
                                            "' is a type parameter, but a non-type argument was given (ch05 §5.11)",
                        expr.loc);
                }
                type_bindings[tp.name] = arg.type;
            }
        }
        if (explicit_index != expr.explicit_template_args.size()) {
            throw DataflowError("too many explicit template arguments for generic function '" + tmpl.name + "'",
                expr.loc);
        }
    }

    void populate_concrete_pack_param_types(
        const Function& tmpl, const std::unordered_map<std::string, std::vector<Type>>& pack_bindings,
        std::vector<std::vector<Type>>& concrete_pack_param_types) {
        for (std::size_t i = 0; i < tmpl.params.size(); i++) {
            if (!tmpl.params[i].is_parameter_pack) continue;
            std::optional<std::string> pack_type_name =
                referenced_type_pack_param_name(tmpl.params[i].type, tmpl.template_params);
            if (!pack_type_name.has_value()) continue;
            auto pack_it = pack_bindings.find(*pack_type_name);
            if (pack_it == pack_bindings.end()) continue;
            for (const Type& concrete : pack_it->second) {
                concrete_pack_param_types[i].push_back(
                    resolve_generic_type(substitute_type_param(tmpl.params[i].type, *pack_type_name, concrete), tmpl.loc));
            }
        }
    }

    [[nodiscard]] bool has_non_generic_overload(const std::string& name) const {
        for (const Function& fn : program_.functions) {
            if (fn.name == name && !fn.is_generic_template) return true;
        }
        return false;
    }

    [[nodiscard]] bool compile_time_dependency_visible(const Function& fn, const Body& body) const {
        if (!fn.is_compile_time_dependency) return true;
        if (!fn.owning_module.empty() && fn.owning_module == body.function_visibility_module) return true;
        return !body.function_source_path.empty() && body.function_source_path == fn.loc.source_path_text();
    }

    struct FullHeaderGenericCallResolution {
        std::unordered_map<std::string, Type> type_bindings;
        std::unordered_map<std::string, int> value_bindings;
        std::unordered_map<std::string, std::vector<Type>> pack_bindings;
        std::vector<std::pair<std::size_t, Type>> upcasts;
        std::vector<DeferredTemplateObligation> deferred_obligations;
        std::vector<std::vector<Type>> concrete_pack_param_types;
    };

    [[nodiscard]] bool try_resolve_full_header_generic_function_call(const Expr& expr, const Function& tmpl, Body& body,
                                                                     std::size_t param_offset,
                                                                     FullHeaderGenericCallResolution& resolution) {
        try {
            Function tmpl_snapshot = clone_function(tmpl);
            const Function& stable_tmpl = tmpl_snapshot;
            resolution.type_bindings.clear();
            resolution.value_bindings.clear();
            resolution.pack_bindings.clear();
            resolution.upcasts.clear();
            resolution.deferred_obligations.clear();
            resolution.concrete_pack_param_types.assign(stable_tmpl.params.size(), {});

            ExprPtr expr_copy = clone_expr(expr);
            seed_explicit_template_arguments(*expr_copy, stable_tmpl, resolution.type_bindings, resolution.value_bindings,
                                             resolution.pack_bindings);

            std::size_t arg_cursor = 0;
            std::size_t param_cursor = param_offset;
            for (; param_cursor < stable_tmpl.params.size() && arg_cursor < expr.args.size(); param_cursor++) {
                if (stable_tmpl.params[param_cursor].is_parameter_pack) {
                    std::optional<std::string> pack_type_name =
                        referenced_type_pack_param_name(stable_tmpl.params[param_cursor].type, stable_tmpl.template_params);
                    const Type& pack_param_type = stable_tmpl.params[param_cursor].type;
                    const Type& underlying = pack_param_type.kind == TypeKind::Reference ? *pack_param_type.pointee
                                                                                         : pack_param_type;
                    bool direct_pack = pack_type_name.has_value() && underlying.is_pack_expansion &&
                                       underlying.kind == TypeKind::Named && underlying.template_args.empty() &&
                                       underlying.non_type_args.empty() && underlying.name == *pack_type_name;
                    std::vector<Type> deduced_pack_types;
                    for (; arg_cursor < expr.args.size(); arg_cursor++) {
                        std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
                        if (!arg_type.has_value()) return false;
                        Type concrete = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                        if (pack_type_name.has_value() && !direct_pack) {
                            std::unordered_map<std::string, Type> arg_type_bindings;
                            std::unordered_map<std::string, int> arg_value_bindings;
                            std::unordered_map<std::string, std::vector<Type>> arg_pack_bindings;
                            if (!deduce_template_bindings_from_type_pattern(
                                    underlying, concrete, stable_tmpl.template_params, arg_type_bindings,
                                    arg_value_bindings, arg_pack_bindings)) {
                                return false;
                            }
                            for (const auto& [name, type] : arg_type_bindings) {
                                if (!bind_type_binding(resolution.type_bindings, name, type)) return false;
                            }
                            auto pack_it = arg_pack_bindings.find(*pack_type_name);
                            if (pack_it == arg_pack_bindings.end()) return false;
                            deduced_pack_types.insert(
                                deduced_pack_types.end(), pack_it->second.begin(), pack_it->second.end());
                        } else {
                            deduced_pack_types.push_back(concrete);
                        }
                    }
                    if (pack_type_name.has_value() &&
                        !bind_type_pack_binding(resolution.pack_bindings, *pack_type_name, deduced_pack_types)) {
                        return false;
                    }
                    continue;
                }
                const Type& param_type = stable_tmpl.params[param_cursor].type;
                const Type& underlying = param_type.kind == TypeKind::Reference ? *param_type.pointee : param_type;
                std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
                if (!arg_type.has_value()) return false;
                Type concrete = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                if (underlying.kind == TypeKind::Named && variadic_generic_type_names_.contains(underlying.name)) {
                    if (argument_type_can_participate_in_variadic_base_deduction(*expr_copy, arg_cursor, underlying.name,
                                                                                 body)) {
                        ExprPtr expr_copy_for_base_deduction = clone_expr(expr);
                        deduce_via_base_class_chain(*expr_copy_for_base_deduction, arg_cursor, underlying, body,
                                                    resolution.type_bindings, resolution.value_bindings,
                                                    resolution.pack_bindings, resolution.upcasts);
                    }
                } else {
                    deduce_template_bindings_from_type_pattern(underlying, concrete, stable_tmpl.template_params,
                                                               resolution.type_bindings, resolution.value_bindings,
                                                               resolution.pack_bindings);
                }
                if (type_depends_on_template_params(param_type, stable_tmpl.template_params)) {
                    if (type_still_depends_on_unbound_template_params(param_type, stable_tmpl.template_params,
                                                                      resolution.type_bindings,
                                                                      resolution.pack_bindings)) {
                        resolution.deferred_obligations.push_back(
                            DeferredTemplateObligation{param_cursor, arg_cursor, param_type});
                    }
                }
                arg_cursor++;
            }
            if (arg_cursor != expr.args.size()) return false;
            for (; param_cursor < stable_tmpl.params.size(); param_cursor++) {
                if (stable_tmpl.params[param_cursor].is_parameter_pack) continue;
                if (!stable_tmpl.params[param_cursor].has_empty_brace_default) return false;
            }

            populate_concrete_pack_param_types(stable_tmpl, resolution.pack_bindings, resolution.concrete_pack_param_types);

            for (const GenericTypeParam& tp : stable_tmpl.template_params) {
                if (tp.is_pack) continue;
                bool bound =
                    tp.is_non_type ? resolution.value_bindings.contains(tp.name) : resolution.type_bindings.contains(tp.name);
                if (!bound) return false;
            }

            for (const DeferredTemplateObligation& obligation : resolution.deferred_obligations) {
                Type concrete_pattern = apply_template_bindings_to_type(
                    obligation.parameter_type_pattern, resolution.type_bindings, resolution.pack_bindings, expr.loc);
                if (type_depends_on_template_params(concrete_pattern, stable_tmpl.template_params)) return false;
                if (!argument_matches_parameter(*expr.args[obligation.arg_index], concrete_pattern, body, signatures_)) {
                    return false;
                }
            }

            check_thread_safety_constraints(*expr_copy, stable_tmpl, resolution.type_bindings, resolution.pack_bindings);
            return true;
        } catch (const DataflowError&) {
            return false;
        }
    }

    struct AbbreviatedGenericCallResolution {
        std::vector<Type> concrete_param_types;
        std::vector<std::vector<Type>> concrete_pack_param_types;
    };

    [[nodiscard]] bool try_resolve_abbreviated_generic_function_call(
        const Expr& expr, const Function& tmpl, Body& body, std::size_t param_offset,
        AbbreviatedGenericCallResolution& resolution) {
        resolution.concrete_param_types.clear();
        resolution.concrete_pack_param_types.assign(tmpl.params.size(), {});
        std::size_t arg_cursor = 0;
        for (std::size_t i = 0; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (i < param_offset) {
                resolution.concrete_param_types.push_back(param.type);
                continue;
            }
            if (param.is_parameter_pack) {
                for (; arg_cursor < expr.args.size(); arg_cursor++) {
                    std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
                    if (!arg_type.has_value()) return false;
                    Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                    if (param.generic_concept != "$auto") {
                        auto concept_it = concepts_by_name_.find(param.generic_concept);
                        if (concept_it == concepts_by_name_.end()) return false;
                        if (!type_satisfies_concept(named, *concept_it->second, program_)) return false;
                    }
                    Type substituted = param.type;
                    if (substituted.kind == TypeKind::Reference) {
                        substituted.pointee = std::make_shared<Type>(named);
                    } else {
                        substituted = named;
                    }
                    resolution.concrete_pack_param_types[i].push_back(std::move(substituted));
                }
                resolution.concrete_param_types.push_back(resolution.concrete_pack_param_types[i].empty()
                                                              ? param.type
                                                              : resolution.concrete_pack_param_types[i][0]);
                continue;
            }
            if (param.generic_concept.empty()) {
                resolution.concrete_param_types.push_back(param.type);
                arg_cursor++;
                continue;
            }
            if (arg_cursor >= expr.args.size()) return false;
            std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
            if (!arg_type.has_value()) return false;
            Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
            if (param.generic_concept != "$auto") {
                auto concept_it = concepts_by_name_.find(param.generic_concept);
                if (concept_it == concepts_by_name_.end()) return false;
                if (!type_satisfies_concept(named, *concept_it->second, program_)) return false;
            }
            Type substituted = param.type;
            if (substituted.kind == TypeKind::Reference) {
                substituted.pointee = std::make_shared<Type>(named);
            } else {
                substituted = named;
            }
            resolution.concrete_param_types.push_back(std::move(substituted));
            arg_cursor++;
        }
        if (arg_cursor != expr.args.size()) return false;

        for (std::size_t i = param_offset; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (!param.require_thread_movable && !param.require_thread_shareable) continue;
            const std::vector<Type>* types_to_check = param.is_parameter_pack ? &resolution.concrete_pack_param_types[i]
                                                                              : nullptr;
            if (types_to_check == nullptr) {
                if (param.require_thread_movable && !is_thread_movable(resolution.concrete_param_types[i])) return false;
                if (param.require_thread_shareable && !is_thread_shareable(resolution.concrete_param_types[i])) {
                    return false;
                }
                continue;
            }
            for (const Type& concrete_type : *types_to_check) {
                if (param.require_thread_movable && !is_thread_movable(concrete_type)) return false;
                if (param.require_thread_shareable && !is_thread_shareable(concrete_type)) return false;
            }
        }
        return true;
    }

    void monomorphize_abbreviated_generic_function_call(Expr& expr, const Function& tmpl, Body& body, std::size_t param_offset = 0,
                                                        const std::string& cloned_method_suffix_prefix = "") {
        std::vector<Type> concrete_param_types;
        concrete_param_types.reserve(tmpl.params.size());
        std::vector<std::vector<Type>> concrete_pack_param_types(tmpl.params.size());
        std::size_t arg_cursor = 0;
        for (std::size_t i = 0; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (i < param_offset) {
                concrete_param_types.push_back(param.type);
                continue;
            }
            if (param.is_parameter_pack) {
                for (; arg_cursor < expr.args.size(); arg_cursor++) {
                    std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
                    if (!arg_type.has_value()) return;
                    Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                    if (param.generic_concept != "$auto") {
                        auto concept_it = concepts_by_name_.find(param.generic_concept);
                        if (concept_it == concepts_by_name_.end()) return;
                        if (!type_satisfies_concept(named, *concept_it->second, program_)) {
                            throw DataflowError("argument type '" + named.name + "' does not satisfy concept '" +
                                                    param.generic_concept + "' required by generic function '" +
                                                    tmpl.name +
                                                    "' (ch05 §5.11 -- every requirement's method must exist with a "
                                                    "matching signature)",
                                expr.loc);
                        }
                    }
                    Type substituted = param.type;
                    if (substituted.kind == TypeKind::Reference) {
                        substituted.pointee = std::make_shared<Type>(named);
                    } else {
                        substituted = named;
                    }
                    concrete_pack_param_types[i].push_back(std::move(substituted));
                }
                concrete_param_types.push_back(concrete_pack_param_types[i].empty() ? param.type
                                                                                    : concrete_pack_param_types[i][0]);
                continue;
            }
            if (param.generic_concept.empty()) {
                concrete_param_types.push_back(param.type);
                arg_cursor++;
                continue;
            }
            if (arg_cursor >= expr.args.size()) return;
            std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
            if (!arg_type.has_value()) return;
            Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
            if (param.generic_concept != "$auto") {
                auto concept_it = concepts_by_name_.find(param.generic_concept);
                if (concept_it == concepts_by_name_.end()) return;
                if (!type_satisfies_concept(named, *concept_it->second, program_)) {
                    throw DataflowError("argument type '" + named.name + "' does not satisfy concept '" +
                                           param.generic_concept + "' required by generic function '" + tmpl.name +
                                           "' (ch05 §5.11 -- every requirement's method must exist with a matching "
                                           "signature)",
                        expr.loc);
                }
            }
            Type substituted = param.type;
            if (substituted.kind == TypeKind::Reference) {
                substituted.pointee = std::make_shared<Type>(named);
            } else {
                substituted = named;
            }
            concrete_param_types.push_back(std::move(substituted));
            arg_cursor++;
        }

        for (std::size_t i = param_offset; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (!param.require_thread_movable && !param.require_thread_shareable) continue;
            const std::vector<Type>* types_to_check = param.is_parameter_pack ? &concrete_pack_param_types[i] : nullptr;
            if (types_to_check == nullptr) {
                if (param.require_thread_movable && !is_thread_movable(concrete_param_types[i])) {
                    throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                            tmpl.name +
                                            "' does not satisfy '[[scpp::thread_movable]]' (ch05 §5.15)",
                        expr.loc);
                }
                if (param.require_thread_shareable && !is_thread_shareable(concrete_param_types[i])) {
                    throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                            tmpl.name +
                                            "' does not satisfy '[[scpp::thread_shareable]]' (ch05 §5.15)",
                        expr.loc);
                }
                continue;
            }
            for (const Type& concrete_type : *types_to_check) {
                if (param.require_thread_movable && !is_thread_movable(concrete_type)) {
                    throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                            tmpl.name +
                                            "' does not satisfy '[[scpp::thread_movable]]' (ch05 §5.15)",
                        expr.loc);
                }
                if (param.require_thread_shareable && !is_thread_shareable(concrete_type)) {
                    throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                            tmpl.name +
                                            "' does not satisfy '[[scpp::thread_shareable]]' (ch05 §5.15)",
                        expr.loc);
                }
            }
        }

        std::string clone_name = get_or_create_clone(tmpl, concrete_param_types, concrete_pack_param_types);
        if (expr.lhs == nullptr) {
            expr.name = std::move(clone_name);
        } else {
            expr.name = clone_name.substr(cloned_method_suffix_prefix.size());
        }
    }

    void monomorphize_generic_function_designator(Expr& expr, const Function& tmpl) {
        std::unordered_map<std::string, Type> type_bindings;
        std::unordered_map<std::string, int> value_bindings;
        std::unordered_map<std::string, std::vector<Type>> explicit_pack_bindings;
        std::vector<std::vector<Type>> concrete_pack_param_types(tmpl.params.size());

        seed_explicit_template_arguments(expr, tmpl, type_bindings, value_bindings, explicit_pack_bindings);
        populate_concrete_pack_param_types(tmpl, explicit_pack_bindings, concrete_pack_param_types);

        for (const GenericTypeParam& tp : tmpl.template_params) {
            if (tp.is_pack) continue;
            bool bound = tp.is_non_type ? value_bindings.contains(tp.name) : type_bindings.contains(tp.name);
            if (!bound) {
                throw DataflowError("cannot form a function designator for generic function '" + tmpl.name +
                                        "' without an explicit argument for template parameter '" + tp.name + "'",
                    expr.loc);
            }
        }

        expr.name = instantiate_full_header_generic_clone(tmpl, type_bindings, value_bindings, explicit_pack_bindings,
                                                          concrete_pack_param_types);
        expr.explicit_global_qualification = false;
        expr.explicit_template_args.clear();
    }

    // ch05 §5.11: monomorphizes a call to a full-header-form generic
    // function template (Function::template_params non-empty, e.g.
    // `get`/`make`) -- binds each of the template's own parameters to a
    // concrete Type (type parameter) or int value (non-type parameter),
    // either from an explicit call-site argument (Expr::
    // explicit_template_args) or by deduction from the corresponding
    // function-parameter's own argument (an ordinary bare-`T`-shaped
    // parameter, or ch05 §5.14's own base-class-deduction accessor
    // pattern, see deduce_via_base_class_chain), then synthesizes (or
    // reuses an already-cached) concrete clone and rewrites `expr.name`
    // to it. The template definition itself lives in `program_.functions`,
    // so recursive generic instantiation can reallocate that vector; take
    // a deep snapshot first and read everything from it.
    void monomorphize_generic_function_call(Expr& expr, const Function& tmpl, Body& body, std::size_t param_offset = 0,
                                            const std::string& member_name_prefix = "") {
        Function tmpl_snapshot = clone_function(tmpl);
        const Function& stable_tmpl = tmpl_snapshot;
        std::unordered_map<std::string, Type> type_bindings;
        std::unordered_map<std::string, int> value_bindings;
        std::unordered_map<std::string, std::vector<Type>> pack_bindings;
        std::vector<std::pair<std::size_t, Type>> upcasts;
        std::vector<DeferredTemplateObligation> deferred_obligations;
        std::vector<std::vector<Type>> concrete_pack_param_types(stable_tmpl.params.size());

        seed_explicit_template_arguments(expr, stable_tmpl, type_bindings, value_bindings, pack_bindings);

        std::size_t arg_cursor = 0;
        std::size_t param_cursor = param_offset;
        for (; param_cursor < stable_tmpl.params.size() && arg_cursor < expr.args.size(); param_cursor++) {
            if (stable_tmpl.params[param_cursor].is_parameter_pack) {
                std::optional<std::string> pack_type_name =
                    referenced_type_pack_param_name(stable_tmpl.params[param_cursor].type, stable_tmpl.template_params);
                const Type& pack_param_type = stable_tmpl.params[param_cursor].type;
                const Type& underlying = pack_param_type.kind == TypeKind::Reference ? *pack_param_type.pointee
                                                                                     : pack_param_type;
                bool direct_pack = pack_type_name.has_value() && underlying.is_pack_expansion &&
                                   underlying.kind == TypeKind::Named && underlying.template_args.empty() &&
                                   underlying.non_type_args.empty() && underlying.name == *pack_type_name;
                std::vector<Type> deduced_pack_types;
                for (; arg_cursor < expr.args.size(); arg_cursor++) {
                    std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
                    if (!arg_type.has_value()) continue;
                    Type concrete = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                    if (pack_type_name.has_value() && !direct_pack) {
                        std::unordered_map<std::string, Type> arg_type_bindings;
                        std::unordered_map<std::string, int> arg_value_bindings;
                        std::unordered_map<std::string, std::vector<Type>> arg_pack_bindings;
                        if (!deduce_template_bindings_from_type_pattern(
                                underlying, concrete, stable_tmpl.template_params, arg_type_bindings,
                                arg_value_bindings, arg_pack_bindings)) {
                            throw DataflowError("cannot deduce template parameter pack types for generic function '" +
                                                    stable_tmpl.name + "' from this argument list",
                                expr.loc);
                        }
                        for (const auto& [name, type] : arg_type_bindings) {
                            if (!bind_type_binding(type_bindings, name, type)) {
                                throw DataflowError("deduced type for template parameter of generic function '" +
                                                        stable_tmpl.name + "' disagrees across arguments",
                                    expr.loc);
                            }
                        }
                        auto pack_it = arg_pack_bindings.find(*pack_type_name);
                        if (pack_it == arg_pack_bindings.end()) {
                            throw DataflowError("cannot deduce template parameter pack types for generic function '" +
                                                    stable_tmpl.name + "' from this argument list",
                                expr.loc);
                        }
                        deduced_pack_types.insert(deduced_pack_types.end(), pack_it->second.begin(), pack_it->second.end());
                    } else {
                        deduced_pack_types.push_back(concrete);
                    }
                }
                if (pack_type_name.has_value() && !bind_type_pack_binding(pack_bindings, *pack_type_name, deduced_pack_types)) {
                        throw DataflowError("deduced template parameter pack types for generic function '" +
                                                stable_tmpl.name + "' disagree across arguments",
                            expr.loc);
                }
                continue;
            }
            const Type& param_type = stable_tmpl.params[param_cursor].type;
            const Type& underlying = param_type.kind == TypeKind::Reference ? *param_type.pointee : param_type;
            std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
            if (arg_type.has_value()) {
                Type concrete = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                if (underlying.kind == TypeKind::Named && variadic_generic_type_names_.contains(underlying.name)) {
                    // Case A: a base-class-deduction pattern (e.g.
                    // "TupleImpl<I, Head, Tail...>& t").
                    if (argument_type_can_participate_in_variadic_base_deduction(expr, arg_cursor, underlying.name, body)) {
                        deduce_via_base_class_chain(expr, arg_cursor, underlying, body, type_bindings, value_bindings,
                                                    pack_bindings, upcasts);
                    }
                } else {
                    deduce_template_bindings_from_type_pattern(underlying, concrete, stable_tmpl.template_params,
                                                               type_bindings, value_bindings, pack_bindings);
                }
            }
            if (type_depends_on_template_params(param_type, stable_tmpl.template_params)) {
                if (type_still_depends_on_unbound_template_params(param_type, stable_tmpl.template_params, type_bindings,
                                                                  pack_bindings)) {
                    deferred_obligations.push_back(DeferredTemplateObligation{param_cursor, arg_cursor, param_type});
                }
            }
            arg_cursor++;
        }
        for (; param_cursor < stable_tmpl.params.size(); param_cursor++) {
            if (stable_tmpl.params[param_cursor].is_parameter_pack) continue;
            if (!stable_tmpl.params[param_cursor].has_empty_brace_default) {
                throw DataflowError("no overload of generic function '" + stable_tmpl.name +
                                        "' matches this argument list",
                                    expr.loc);
            }
        }

        populate_concrete_pack_param_types(stable_tmpl, pack_bindings, concrete_pack_param_types);

        for (const GenericTypeParam& tp : stable_tmpl.template_params) {
            // ch05 §5.14: a *pack* template parameter (e.g. "Tail") can
            // only ever appear spread inside a base-class-deduction
            // pattern in this language's current scope -- never bound
            // individually (the whole pattern it's part of is replaced
            // wholesale by the deduced upcast target type instead, see
            // deduce_via_base_class_chain), so it needs no binding of
            // its own at all.
            if (tp.is_pack) continue;
            bool bound = tp.is_non_type ? value_bindings.contains(tp.name) : type_bindings.contains(tp.name);
            if (!bound) {
                throw DataflowError("cannot deduce template parameter '" + tp.name + "' of generic function '" +
                                         stable_tmpl.name + "', and no explicit argument was given for it (ch05 §5.11)",
                    expr.loc);
            }
        }

        for (const DeferredTemplateObligation& obligation : deferred_obligations) {
            Type concrete_pattern =
                apply_template_bindings_to_type(obligation.parameter_type_pattern, type_bindings, pack_bindings, expr.loc);
            if (type_depends_on_template_params(concrete_pattern, stable_tmpl.template_params)) {
                throw DataflowError("cannot deduce every template argument needed by parameter '" +
                                        stable_tmpl.params[obligation.param_index].name + "' of generic function '" +
                                        stable_tmpl.name +
                                        "' after scanning the whole call",
                    expr.loc);
            }
            if (!argument_matches_parameter(*expr.args[obligation.arg_index], concrete_pattern, body, signatures_)) {
                throw DataflowError("argument for parameter '" + stable_tmpl.params[obligation.param_index].name +
                                        "' of generic function '" + stable_tmpl.name +
                                        "' is incompatible after substituting deduced template arguments",
                    expr.args[obligation.arg_index]->loc);
            }
        }

        // ch05 §5.15: once every template parameter is bound to a
        // concrete type, check any `[[scpp::thread_movable]]`/
        // `[[scpp::thread_shareable]]`-tagged parameter's own concrete
        // (post-substitution) type actually satisfies what it requires
        // -- before synthesizing/caching a clone, so a violation is
        // reported at the call site that triggered it.
        check_thread_safety_constraints(expr, stable_tmpl, type_bindings, pack_bindings);

        std::string clone_name = instantiate_full_header_generic_clone(
            stable_tmpl, type_bindings, value_bindings, pack_bindings, concrete_pack_param_types, upcasts);
        expr.name = member_name_prefix.empty() ? clone_name : clone_name.substr(member_name_prefix.size());
        expr.explicit_template_args.clear();
    }


    // Shared by instantiate_generic_type's struct/class branches: throws
    // a precise error if `type_param` is concept-constrained and
    // `concrete_arg` doesn't structurally satisfy it -- a no-op when
    // `type_param` is bare (nothing to check).
    void check_type_param_constraint(const GenericTypeParam& type_param, const Type& concrete_arg,
                                      const std::string& template_name, SourceLocation loc) {
        if (type_param.concept_name.empty()) return;
        auto concept_it = concepts_by_name_.find(type_param.concept_name);
        if (concept_it != concepts_by_name_.end() &&
            type_satisfies_concept(concrete_arg, *concept_it->second, program_)) {
            return;
        }
        throw DataflowError("type argument '" + concrete_arg.name + "' does not satisfy concept '" +
                             type_param.concept_name + "' required by generic type '" + template_name +
                             "' (ch05 §5.14)",
            loc);
    }

    void walk_stmt(Stmt& stmt, Body& body, const std::optional<Type>& enclosing_this_type,
                   bool allow_generic_monomorphization) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) walk_expr(*stmt.init, body, enclosing_this_type, allow_generic_monomorphization);
                for (ExprPtr& arg : stmt.ctor_args) {
                    walk_expr(*arg, body, enclosing_this_type, allow_generic_monomorphization);
                }
                if (!stmt.ctor_args.empty() && stmt.type.kind == TypeKind::Named) {
                    maybe_instantiate_generic_constructor_overloads(stmt.type.name, stmt.ctor_args, body, stmt.loc);
                }
                // ch05 §5.12: `auto name = expr;` -- infer the concrete
                // type from the (by-now-fully-resolved, e.g. a Lambda's
                // own synthesized class) initializer. Must overwrite
                // *both* the AST's own `stmt.type` (so check_moves/
                // codegen's later, fresh `build_mir` call sees a
                // concrete type) and this pass's own `body.local_types`
                // entry in place (so a *later* statement in this same
                // function -- e.g. `f(x)`'s bare-call-redirect just
                // below, or another lambda capturing `f` by reference --
                // resolves this variable's real type too, not the stale
                // "auto" placeholder `build_mir` originally saw before
                // any resolution ran).
                if (stmt.type.kind == TypeKind::Named && stmt.type.name == "auto") {
                    if (!stmt.init) {
                        throw DataflowError("'auto' requires an initializer", stmt.loc);
                    }
                    std::optional<Type> inferred = infer_expr_type(*stmt.init, body, signatures_);
                    if (!inferred.has_value()) {
                        throw DataflowError(
                            "cannot infer 'auto' variable '" + stmt.var_name + "'s type from its initializer",
                            stmt.loc);
                    }
                    if (is_synthesized_for_range_storage(stmt.var_name) &&
                        (inferred->kind == TypeKind::Array || inferred->kind == TypeKind::Span)) {
                        Type inferred_ref;
                        inferred_ref.kind = TypeKind::Reference;
                        inferred_ref.pointee = std::make_shared<Type>(*inferred);
                        inferred_ref.is_mutable_ref = inferred->kind == TypeKind::Span ? inferred->is_mutable_ref : true;
                        stmt.type = inferred_ref;
                        body.local_types[stmt.var_name] = inferred_ref;
                    } else {
                        stmt.type = *inferred;
                        body.local_types[stmt.var_name] = *inferred;
                    }
                } else if (stmt.type.kind == TypeKind::Reference && stmt.type.pointee != nullptr &&
                           stmt.type.pointee->kind == TypeKind::Named && stmt.type.pointee->name == "auto") {
                    if (!stmt.init) {
                        throw DataflowError("'auto' requires an initializer", stmt.loc);
                    }
                    std::optional<Type> inferred = infer_expr_type(*stmt.init, body, signatures_);
                    if (!inferred.has_value()) {
                        throw DataflowError(
                            "cannot infer 'auto' variable '" + stmt.var_name + "'s type from its initializer", stmt.loc);
                    }
                    stmt.type.pointee = std::make_shared<Type>(*inferred);
                    body.local_types[stmt.var_name] = stmt.type;
                }
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) walk_expr(*stmt.expr, body, enclosing_this_type, allow_generic_monomorphization);
                return;
            case StmtKind::If:
                walk_expr(*stmt.condition, body, enclosing_this_type, allow_generic_monomorphization);
                walk_stmt(*stmt.then_branch, body, enclosing_this_type, allow_generic_monomorphization);
                if (stmt.else_branch) {
                    walk_stmt(*stmt.else_branch, body, enclosing_this_type, allow_generic_monomorphization);
                }
                return;
            case StmtKind::While:
                walk_expr(*stmt.condition, body, enclosing_this_type, allow_generic_monomorphization);
                walk_stmt(*stmt.then_branch, body, enclosing_this_type, allow_generic_monomorphization);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) {
                    walk_stmt(*s, body, enclosing_this_type, allow_generic_monomorphization);
                }
                return;
        }
    }

    [[nodiscard]] ExprPtr make_deref_expr(ExprPtr operand, SourceLocation loc, bool implicit_arrow_deref = false,
                                          bool implicit_arrow_chain_safe = false) {
        auto deref = std::make_unique<Expr>();
        deref->kind = ExprKind::Unary;
        deref->loc = loc;
        deref->unary_op = UnaryOp::Deref;
        deref->lhs = std::move(operand);
        deref->implicit_arrow_deref = implicit_arrow_deref;
        deref->implicit_arrow_chain_safe = implicit_arrow_chain_safe;
        return deref;
    }

    [[nodiscard]] ExprPtr rewrite_arrow_receiver(ExprPtr receiver, Body& body, const SourceLocation& loc) {
        std::optional<Type> receiver_type = infer_expr_type(*receiver, body, signatures_);
        bool selected_any_operator_arrow = false;
        bool all_steps_receiver_tied = true;
        for (int depth = 0; depth < 64; depth++) {
            if (!receiver_type.has_value()) {
                throw DataflowError("operator-> chain did not yield a pointer", loc);
            }
            const Type& effective =
                receiver_type->kind == TypeKind::Reference && receiver_type->pointee ? *receiver_type->pointee : *receiver_type;
            if (effective.kind == TypeKind::Pointer) {
                return make_deref_expr(std::move(receiver), loc, selected_any_operator_arrow, all_steps_receiver_tied);
            }
            if (effective.kind != TypeKind::Named) {
                throw DataflowError("operator-> chain did not yield a pointer", loc);
            }
            auto call = std::make_unique<Expr>();
            call->kind = ExprKind::Call;
            call->loc = loc;
            call->name = "operator_arrow";
            call->lhs = std::move(receiver);
            CalleeSignature callee{effective.name + "_operator_arrow", 1, std::nullopt};
            const FunctionSignature* sig = resolve_overload(*call, callee, body, signatures_);
            if (sig == nullptr) {
                if (!selected_any_operator_arrow) {
                    throw DataflowError("cannot use '->' on class type '" + effective.name +
                                            "': it has no matching operator->()",
                                        loc);
                }
                throw DataflowError("operator-> chain did not yield a pointer", loc);
            }
            selected_any_operator_arrow = true;
            all_steps_receiver_tied =
                all_steps_receiver_tied && sig->return_lifetime.present() && sig->param_types.size() == 1;
            receiver = std::move(call);
            receiver_type = infer_expr_type(*receiver, body, signatures_);
        }
        throw DataflowError("operator-> chain did not yield a pointer", loc);
    }

    void walk_expr(Expr& expr, Body& body, const std::optional<Type>& enclosing_this_type,
                   bool allow_generic_monomorphization) {
        // ch05 §5.12: a Lambda's own sub-tree (captures' init-exprs,
        // params, body) is handled entirely inside resolve_lambda --
        // never via the generic lhs/rhs/args recursion below (which
        // would find them all empty/unused for a Lambda node anyway,
        // since captures/params/body are Lambda's own dedicated fields,
        // not lhs/rhs/args -- see ast.cppm's Expr).
        if (expr.kind == ExprKind::Lambda) {
            resolve_lambda(expr, body, enclosing_this_type);
            return;
        }

        // ch05 §5.9/§5.11/§5.12: a bare (no-receiver) Call whose own
        // name resolves to a *local variable* (not a function) of class
        // type is sugar for calling that class's own "call" method --
        // `f(args)` desugars in place to `f.call(args)`, reusing 100% of
        // the existing method-call machinery with zero further new
        // logic. Shared by an ordinary user-defined callable class (any
        // class with a method literally named "call"), a concept's own
        // witness class (ch05 §5.11's IntConsumer-style direct-
        // invocation requirement, e.g. `f(x)` inside a generic
        // function's own body), and a real closure (ch05 §5.12's
        // `c(args)`). A local variable always shadows an outer function
        // of the same name here, matching ordinary C++ scoping -- and
        // there is no genuine ambiguity in practice, since a generic
        // template's own name (checked further below) is never itself
        // registered as a local.
        if (expr.kind == ExprKind::Call && expr.lhs == nullptr && !expr.explicit_global_qualification) {
            auto local_it = body.local_types.find(expr.name);
            if (local_it != body.local_types.end()) {
                const Type& local_type = local_it->second;
                const Type& underlying = local_type.kind == TypeKind::Reference ? *local_type.pointee : local_type;
                if (underlying.kind == TypeKind::Named) {
                    auto receiver = std::make_unique<Expr>();
                    receiver->kind = ExprKind::Identifier;
                    receiver->loc = expr.loc;
                    receiver->name = expr.name;
                    expr.lhs = std::move(receiver);
                    expr.name = "call";
                }
            }
        }

        if (expr.lhs) walk_expr(*expr.lhs, body, enclosing_this_type, allow_generic_monomorphization);
        if (expr.rhs) walk_expr(*expr.rhs, body, enclosing_this_type, allow_generic_monomorphization);
        for (ExprPtr& arg : expr.args) walk_expr(*arg, body, enclosing_this_type, allow_generic_monomorphization);
        if (expr.kind == ExprKind::New && expr.type.kind == TypeKind::Named) {
            maybe_instantiate_generic_constructor_overloads(expr.type.name, expr.args, body, expr.loc);
        }
        if (expr.kind == ExprKind::Call && expr.lhs == nullptr) {
            std::optional<Type> direct_call_type = infer_expr_type(expr, body, signatures_);
            bool names_known_class = false;
            if (direct_call_type.has_value() && direct_call_type->kind == TypeKind::Named) {
                for (const ClassDef& def : program_.classes) {
                    if (def.name == direct_call_type->name) {
                        names_known_class = true;
                        break;
                    }
                }
            }
            if (names_known_class) {
                maybe_instantiate_generic_constructor_overloads(direct_call_type->name, expr.args, body, expr.loc);
            }
        }

        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr) {
            std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures_);
            if (operand_type.has_value()) {
                const Type& underlying =
                    operand_type->kind == TypeKind::Reference && operand_type->pointee ? *operand_type->pointee
                                                                                        : *operand_type;
                if (underlying.kind == TypeKind::Named &&
                    signatures_.contains(underlying.name + "_operator_deref")) {
                    ExprPtr receiver = std::move(expr.lhs);
                    expr.kind = ExprKind::Call;
                    expr.name = "operator_deref";
                    expr.lhs = std::move(receiver);
                    expr.unary_op = UnaryOp::Not;
                }
            }
        }

        if ((expr.kind == ExprKind::Member || expr.kind == ExprKind::Call) && expr.through_arrow && expr.lhs != nullptr) {
            expr.lhs = rewrite_arrow_receiver(std::move(expr.lhs), body, expr.loc);
            expr.through_arrow = false;
        }

        if (expr.kind == ExprKind::Call && expr.lhs != nullptr && expr.name.empty()) {
            std::optional<Type> callee_type = infer_expr_type(*expr.lhs, body, signatures_);
            if (callee_type.has_value()) {
                const Type& underlying =
                    callee_type->kind == TypeKind::Reference && callee_type->pointee ? *callee_type->pointee
                                                                                      : *callee_type;
                if (underlying.kind == TypeKind::Named) expr.name = "call";
            }
        }

        // Generic-call monomorphization is suppressed entirely while
        // walking a generic template's own body (see run()'s own
        // comment): a nested generic-to-generic call is left targeting
        // the original, codegen-excluded template instead.
        if (!allow_generic_monomorphization) return;
        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::AddressOf && expr.lhs &&
            expr.lhs->kind == ExprKind::Identifier && !expr.lhs->explicit_template_args.empty()) {
            walk_expr(*expr.lhs, body, enclosing_this_type, allow_generic_monomorphization);
            return;
        }
        if (expr.kind == ExprKind::Identifier && !expr.explicit_template_args.empty()) {
            auto template_it = generic_template_indices_.find(expr.name);
            if (template_it == generic_template_indices_.end()) return;
            std::vector<std::size_t> matching_candidates;
            for (std::size_t candidate_index : template_it->second) {
                const Function& tmpl = program_.functions[candidate_index];
                if (!compile_time_dependency_visible(tmpl, body)) continue;
                if (tmpl.template_params.empty()) continue;
                try {
                    ExprPtr expr_copy = clone_expr(expr);
                    std::unordered_map<std::string, Type> type_bindings;
                    std::unordered_map<std::string, int> value_bindings;
                    std::unordered_map<std::string, std::vector<Type>> explicit_pack_bindings;
                    std::vector<std::vector<Type>> concrete_pack_param_types(tmpl.params.size());
                    seed_explicit_template_arguments(*expr_copy, tmpl, type_bindings, value_bindings, explicit_pack_bindings);
                    populate_concrete_pack_param_types(tmpl, explicit_pack_bindings, concrete_pack_param_types);
                    bool every_non_pack_bound = true;
                    for (const GenericTypeParam& tp : tmpl.template_params) {
                        if (tp.is_pack) continue;
                        bool bound = tp.is_non_type ? value_bindings.contains(tp.name) : type_bindings.contains(tp.name);
                        every_non_pack_bound = every_non_pack_bound && bound;
                    }
                    if (every_non_pack_bound) matching_candidates.push_back(candidate_index);
                } catch (const DataflowError&) {
                }
            }
            if (matching_candidates.empty()) return;
            if (matching_candidates.size() > 1) {
                throw DataflowError("ambiguous generic function designator '" + expr.name + "'", expr.loc);
            }
            monomorphize_generic_function_designator(expr, program_.functions[matching_candidates[0]]);
            return;
        }
        if (expr.kind != ExprKind::Call) return;
        std::string generic_template_name = expr.name;
        std::size_t param_offset = 0;
        std::string cloned_method_suffix_prefix;
        if (expr.lhs != nullptr) {
            std::optional<Type> receiver = infer_expr_type(*expr.lhs, body, signatures_);
            if (!receiver.has_value()) return;
            const Type& receiver_named = receiver->kind == TypeKind::Reference ? *receiver->pointee : *receiver;
            if (receiver_named.kind != TypeKind::Named) return;
            generic_template_name = receiver_named.name + "_" + expr.name;
            param_offset = 1;
            cloned_method_suffix_prefix = receiver_named.name + "_";
        }
        auto template_it = generic_template_indices_.find(generic_template_name);
        if (template_it == generic_template_indices_.end()) return;
        const bool ordinary_overload_exists = [&]() {
            for (const Function& fn : program_.functions) {
                if (fn.name == generic_template_name && !fn.is_generic_template &&
                    compile_time_dependency_visible(fn, body)) {
                    return true;
                }
            }
            return false;
        }();
        std::vector<std::size_t> visible_template_candidates;
        for (std::size_t candidate_index : template_it->second) {
            if (compile_time_dependency_visible(program_.functions[candidate_index], body)) {
                visible_template_candidates.push_back(candidate_index);
            }
        }
        if (visible_template_candidates.empty()) return;
        if (visible_template_candidates.size() == 1 && !ordinary_overload_exists) {
            const Function& tmpl = program_.functions[visible_template_candidates[0]];
            if (!tmpl.template_params.empty()) {
                monomorphize_generic_function_call(expr, tmpl, body, param_offset, cloned_method_suffix_prefix);
            } else {
                monomorphize_abbreviated_generic_function_call(expr, tmpl, body, param_offset, cloned_method_suffix_prefix);
            }
            return;
        }

        std::vector<std::size_t> matching_candidates;
        for (std::size_t candidate_index : visible_template_candidates) {
            const Function& tmpl = program_.functions[candidate_index];
            if (!tmpl.template_params.empty()) {
                FullHeaderGenericCallResolution resolution;
                if (try_resolve_full_header_generic_function_call(expr, tmpl, body, param_offset, resolution)) {
                    matching_candidates.push_back(candidate_index);
                }
                continue;
            }
            AbbreviatedGenericCallResolution resolution;
            if (try_resolve_abbreviated_generic_function_call(expr, tmpl, body, param_offset, resolution)) {
                matching_candidates.push_back(candidate_index);
            }
        }

        if (matching_candidates.empty()) {
            if (ordinary_overload_exists) return;
            throw DataflowError("no generic overload of '" + generic_template_name + "' matches these argument types",
                                expr.loc);
        }
        if (matching_candidates.size() > 1) {
            throw DataflowError("ambiguous call to overloaded generic function '" + generic_template_name + "'",
                                expr.loc);
        }

        const Function& selected = program_.functions[matching_candidates[0]];
        if (!selected.template_params.empty()) {
            monomorphize_generic_function_call(expr, selected, body, param_offset, cloned_method_suffix_prefix);
        } else {
            monomorphize_abbreviated_generic_function_call(expr, selected, body, param_offset,
                                                           cloned_method_suffix_prefix);
        }
    }

    // ch05 §5.12: resolves a single Lambda expression node in place --
    // performs blanket-capture free-variable analysis if needed,
    // resolves every capture's concrete field type, synthesizes the
    // concrete closure ClassDef + "call" method (injecting both into
    // program_), rewrites the (deep-cloned) body's captured-name
    // references into explicit `this.name` field access, and finally
    // sets `expr.name` to the synthesized class -- the only thing
    // codegen/movecheck need from here on to treat this literal exactly
    // like an ordinary class construction (see codegen's own Lambda
    // case).
    void resolve_lambda(Expr& expr, Body& enclosing_body, const std::optional<Type>& enclosing_this_type) {
        if (expr.lambda_blanket_mode != LambdaCaptureMode::None) {
            std::unordered_set<std::string> excluded;
            for (const Param& p : expr.lambda_params) excluded.insert(p.name);
            for (const LambdaCapture& c : expr.lambda_captures) excluded.insert(c.name);
            if (expr.lambda_body) collect_locally_declared_names(*expr.lambda_body, excluded);
            excluded.insert(known_function_names_.begin(), known_function_names_.end());
            excluded.insert(known_type_names_.begin(), known_type_names_.end());

            std::unordered_set<std::string> free_names;
            if (expr.lambda_body) collect_free_identifiers(*expr.lambda_body, excluded, free_names);
            bool by_reference = expr.lambda_blanket_mode == LambdaCaptureMode::ByReference;
            for (const std::string& name : free_names) {
                // ch05 §5.12's own hard rule: `this` is never implicitly
                // captured by a bare `[=]`/`[&]`, even though it would
                // otherwise look like just another free identifier here
                // -- must be named explicitly (`[this]`/`[=, this]`/
                // `[&, this]`).
                if (name == "this") continue;
                // Not a real local in the enclosing scope -- leave for
                // the usual "use of undeclared variable" error rather
                // than guessing.
                if (!enclosing_body.local_types.contains(name)) continue;
                LambdaCapture capture;
                capture.name = name;
                capture.by_reference = by_reference;
                expr.lambda_captures.push_back(std::move(capture));
            }
        }

        std::vector<Type> field_types;
        field_types.reserve(expr.lambda_captures.size());
        std::unordered_set<std::string> captured_names;
        // ch05 §5.12: every by-*value*-captured name other than `this`
        // (`[*this]`'s own copy semantics are a separate concern --
        // see below) -- used after the loop to reject a direct
        // assignment to one of these inside a non-`mutable` lambda body
        // (reject_write_to_nonmutable_by_value_capture). A by-*reference*
        // capture is deliberately excluded: writing *through* a
        // reference field is always allowed regardless of the closure's
        // own mutability, exactly like real C++ (a reference member's
        // constness is independent of its enclosing object's) -- see
        // `this_type.is_mutable_ref`'s own comment below for why the
        // "call" method's receiver itself is unconditionally mutable.
        std::unordered_set<std::string> by_value_names;
        for (LambdaCapture& capture : expr.lambda_captures) {
            captured_names.insert(capture.name);
            Type captured_type;
            if (capture.name == "this") {
                if (!enclosing_this_type.has_value()) {
                    throw DataflowError(
                        "a lambda captures 'this', but is not itself inside a method body (ch05 §5.12)",
                        expr.loc);
                }
                captured_type = *enclosing_this_type;
            } else if (capture.init) {
                std::optional<Type> t = infer_expr_type(*capture.init, enclosing_body, signatures_);
                if (!t.has_value()) {
                    throw DataflowError("cannot determine the type of init-capture '" + capture.name +
                                             "' (ch05 §5.12)",
                        expr.loc);
                }
                captured_type = std::move(*t);
            } else {
                auto it = enclosing_body.local_types.find(capture.name);
                if (it == enclosing_body.local_types.end()) {
                    throw DataflowError("lambda captures '" + capture.name +
                                             "', which is not a local variable or parameter in this scope (ch05 "
                                             "§5.12)",
                        expr.loc);
                }
                captured_type = it->second;
            }
            if (capture.by_reference) {
                Type ref;
                ref.kind = TypeKind::Reference;
                ref.pointee = std::make_shared<Type>(std::move(captured_type));
                // v1 simplification: every by-reference capture is a
                // mutable reference field, regardless of how the body
                // itself uses it -- ch05 §5.12 doesn't ask for a
                // separate const-vs-mutable capture distinction, and
                // real C++ itself doesn't track per-capture constness
                // this way either (a lambda's own constness -- the
                // `mutable` keyword -- is about by-*value* captures, see
                // `this_param.type.is_mutable_ref` below).
                ref.is_mutable_ref = true;
                field_types.push_back(std::move(ref));
            } else {
                if (capture.name != "this") by_value_names.insert(capture.name);
                field_types.push_back(std::move(captured_type));
            }
        }

        std::string class_name = "__lambda" + std::to_string(lambda_counter_++);
        ClassDef closure_class;
        closure_class.name = class_name;
        closure_class.fields.reserve(expr.lambda_captures.size());
        for (std::size_t i = 0; i < expr.lambda_captures.size(); i++) {
            ClassField field;
            field.name = expr.lambda_captures[i].name;
            field.type = field_types[i];
            field.access = AccessSpecifier::Private;
            closure_class.fields.push_back(std::move(field));
        }
        program_.classes.push_back(std::move(closure_class));
        known_type_names_.insert(class_name);

        Function call_method;
        call_method.name = class_name + "_call";
        call_method.loc = expr.loc;
        Param this_param;
        this_param.name = "this";
        Type this_type;
        this_type.kind = TypeKind::Reference;
        this_type.pointee = std::make_shared<Type>(named_type(class_name));
        // The "call" method's own receiver is unconditionally mutable --
        // *not* gated by `mutable` (unlike a real C++ closure's
        // internally-const `operator()`): the general const-`this`-
        // propagation mechanism this would otherwise rely on
        // (assignment_target_is_read_only) cannot, without full
        // Program-wide field-type information movecheck's Body-based
        // architecture doesn't carry, distinguish "writing to a
        // by-value field" (should require `mutable`) from "writing
        // *through* a by-reference field's own referent" (must always
        // be allowed, matching real C++, where a reference member's
        // constness is independent of its enclosing object's). Instead,
        // "requires `mutable` to modify a by-value capture" is enforced
        // directly and precisely below
        // (reject_write_to_nonmutable_by_value_capture), using the
        // capture-list information only resolve_lambda itself has.
        this_type.is_mutable_ref = true;
        this_param.type = std::move(this_type);
        call_method.params.push_back(std::move(this_param));
        for (const Param& p : expr.lambda_params) call_method.params.push_back(p);
        call_method.is_generic_template =
            std::any_of(expr.lambda_params.begin(), expr.lambda_params.end(),
                        [](const Param& param) { return !param.generic_concept.empty(); });

        call_method.body = expr.lambda_body ? clone_stmt(*expr.lambda_body) : nullptr;
        // ch05 §5.12: "a by-value capture can't be reassigned inside the
        // body" absent `mutable` -- checked on the *original* (pre field-
        // access-rewrite) body, where a captured name is still an
        // ordinary bare Identifier, so no field-type information is
        // needed (see the function's own comment).
        if (call_method.body && !expr.lambda_is_mutable) {
            reject_write_to_nonmutable_by_value_capture(*call_method.body, by_value_names);
        }
        // Return-type inference must likewise run on the *original*
        // (pre-rewrite) body: infer_expr_type has no Program access to
        // resolve a field's type from a `this.name` Member node (see
        // infer_lambda_return_type's own comment), but a captured name
        // is still a plain, resolvable Identifier at this point --
        // exactly like reject_write_to_nonmutable_by_value_capture's
        // identical reasoning just above.
        if (expr.has_lambda_explicit_return_type) {
            call_method.return_type = expr.type;
        } else if (call_method.body) {
            std::unordered_map<std::string, Type> capture_types;
            for (std::size_t i = 0; i < expr.lambda_captures.size(); i++) {
                capture_types[expr.lambda_captures[i].name] = field_types[i];
            }
            call_method.return_type = infer_lambda_return_type(*call_method.body, call_method.params, capture_types);
        } else {
            call_method.return_type = named_type("void");
        }
        if (call_method.body) rewrite_captured_identifiers_as_field_access(*call_method.body, captured_names);

        // scpp requires an explicit `return;` covering every path, even
        // for a `void` function with an otherwise-empty body (verified
        // against this codebase's own existing behavior -- e.g. a bare
        // `Circle() {}` constructor is rejected the same way) -- real
        // C++ lambdas need no such thing (`[](int x) { print_int(x); }`
        // is perfectly valid with no `return` at all), so this
        // synthesis step must compensate by appending one when the
        // resolved return type is `void` and the body doesn't already
        // end with a Return statement (the common case for a body with
        // no explicit `-> Type` and no `return expr;` of its own -- a
        // more complex void body already ending in its own `return;` on
        // every path is left untouched, matching this same "don't guess,
        // defer to the real check" spirit codegen's own is_bare_void
        // helper follows elsewhere (not reusable here directly -- a
        // separate module, see this file's other independently-
        // duplicated helpers, e.g. types_equal).
        bool return_type_is_void =
            call_method.return_type.kind == TypeKind::Named && call_method.return_type.name == "void";
        if (return_type_is_void && call_method.body && call_method.body->kind == StmtKind::Block &&
            (call_method.body->statements.empty() ||
             call_method.body->statements.back()->kind != StmtKind::Return)) {
            auto return_stmt = std::make_unique<Stmt>();
            return_stmt->kind = StmtKind::Return;
            return_stmt->loc = expr.loc;
            call_method.body->statements.push_back(std::move(return_stmt));
        }

        program_.functions.push_back(std::move(call_method));
        Function& synthesized = program_.functions.back();
        known_function_names_.insert(synthesized.name);
        if (synthesized.is_generic_template) {
            generic_template_indices_[synthesized.name].push_back(program_.functions.size() - 1);
        }

        expr.name = class_name;

        // Recurse into the synthesized method's own body (nested generic
        // calls / nested lambdas) using its own freshly-built Body --
        // capture fields are reached via `this.field` (a Member
        // expression, resolved structurally like any other class field,
        // never through body.local_types), so nothing about this
        // recursive walk needs to know about them specially. A
        // synthesized closure's own "call" method is never itself a
        // generic template, so generic-call-monomorphization stays
        // enabled here.
        if (synthesized.body) {
            Body synthesized_body = build_mir(synthesized);
            synthesized_body.program = &program_;
            walk_stmt(*synthesized.body, synthesized_body, this_type_of(synthesized),
                      /*allow_generic_monomorphization=*/!synthesized.is_generic_template);
        }
    }

    // ch05 §5.12: infers a lambda's return type from a single top-level
    // `return expr;` statement when no explicit trailing `-> Type` is
    // given (scpp has no general type inference, so this is a
    // deliberately narrow special case, matching the parser's own
    // comment) -- looks only at the body's own top-level Block
    // statements (not nested inside an If/While), mirroring how
    // narrowly this inference is meant to apply. No qualifying return
    // statement (none at all, or only a bare `return;`) infers `void`.
    // Ambiguous (more than one differently-shaped top-level return, or
    // a top-level return whose own expression type can't be determined
    // structurally) is left as `void` too, rather than guessing -- a
    // genuinely ambiguous case should use an explicit `-> Type` instead;
    // this is intentionally not a general control-flow analysis.
    // `call_params` is the synthesized "call" method's own params
    // (including `this`); `capture_types` maps each captured name to
    // its own resolved field type. Run on the *original* (pre field-
    // access-rewrite) body -- a captured name is still a plain bare
    // Identifier at this point (never yet a `this.field` Member access,
    // which infer_expr_type could not resolve anyway -- it has no
    // Program access to look up a field's type) -- so both a lambda's
    // own params and its captures are plain Identifiers infer_expr_type
    // can resolve directly from a fresh, flat Body (no enclosing
    // Function exists yet to build_mir from).
    [[nodiscard]] Type infer_lambda_return_type(const Stmt& body, const std::vector<Param>& call_params,
                                                 const std::unordered_map<std::string, Type>& capture_types) {
        if (body.kind != StmtKind::Block) return named_type("void");
        Body param_only_body;
        for (const Param& p : call_params) {
            param_only_body.local_types[p.name] = p.type;
        }
        for (const auto& [name, type] : capture_types) {
            param_only_body.local_types[name] = type;
        }
        for (const StmtPtr& stmt : body.statements) {
            if (stmt->kind != StmtKind::Return || !stmt->expr) continue;
            // `[this]() { return this->value; }` (ch05 §5.12): a
            // `this`-capture's own field access -- infer_expr_type's
            // Member case can never resolve this (no Program access),
            // but this function, being a Monomorphizer method, has
            // `program_` directly -- special-cased here rather than
            // widening infer_expr_type's own general contract.
            if (stmt->expr->kind == ExprKind::Member && stmt->expr->lhs->kind == ExprKind::Identifier) {
                auto base_it = param_only_body.local_types.find(stmt->expr->lhs->name);
                if (base_it != param_only_body.local_types.end()) {
                    const Type& base_type = base_it->second;
                    const std::string& class_name =
                        (base_type.kind == TypeKind::Reference ? *base_type.pointee : base_type).name;
                    if (std::optional<Type> field_type = resolve_field_type(class_name, stmt->expr->name)) {
                        return *field_type;
                    }
                }
            }
            std::optional<Type> t = infer_expr_type(*stmt->expr, param_only_body, signatures_);
            if (t.has_value()) return *t;
            return named_type("void");
        }
        return named_type("void");
    }

    // Looks up `class_or_struct_name`'s own declared field `field_name`'s
    // type -- a Monomorphizer method, so it has direct `program_` access
    // (unlike movecheck's own, otherwise Program-less, Body-based
    // machinery -- see DataflowState::class_field_types for the parallel
    // mechanism check_moves needs for the exact same underlying reason).
    [[nodiscard]] std::optional<Type> resolve_field_type(const std::string& class_or_struct_name,
                                                          const std::string& field_name) const {
        for (const ClassDef& def : program_.classes) {
            if (def.name != class_or_struct_name) continue;
            for (const ClassField& field : def.fields) {
                if (field.name == field_name) return field.type;
            }
        }
        for (const StructDef& def : program_.structs) {
            if (def.name != class_or_struct_name) continue;
            for (const StructField& field : def.fields) {
                if (field.name == field_name) return field.type;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool expr_mentions_identifier(const Expr& expr, const std::string& name) const {
        if (expr.kind == ExprKind::Identifier && expr.name == name) return true;
        if (expr.lhs && expr_mentions_identifier(*expr.lhs, name)) return true;
        if (expr.rhs && expr_mentions_identifier(*expr.rhs, name)) return true;
        for (const ExprPtr& arg : expr.args) {
            if (expr_mentions_identifier(*arg, name)) return true;
        }
        if (expr.kind == ExprKind::Lambda) {
            for (const LambdaCapture& capture : expr.lambda_captures) {
                if (capture.init && expr_mentions_identifier(*capture.init, name)) return true;
            }
            if (expr.lambda_body && stmt_mentions_identifier(*expr.lambda_body, name)) return true;
        }
        return false;
    }

    [[nodiscard]] bool stmt_mentions_identifier(const Stmt& stmt, const std::string& name) const {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init && expr_mentions_identifier(*stmt.init, name)) return true;
                for (const ExprPtr& arg : stmt.ctor_args) {
                    if (expr_mentions_identifier(*arg, name)) return true;
                }
                return false;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                return stmt.expr && expr_mentions_identifier(*stmt.expr, name);
            case StmtKind::If:
                return expr_mentions_identifier(*stmt.condition, name) ||
                       stmt_mentions_identifier(*stmt.then_branch, name) ||
                       (stmt.else_branch && stmt_mentions_identifier(*stmt.else_branch, name));
            case StmtKind::While:
                return expr_mentions_identifier(*stmt.condition, name) ||
                       stmt_mentions_identifier(*stmt.then_branch, name);
            case StmtKind::Break:
            case StmtKind::Continue:
                return false;
            case StmtKind::Block:
                for (const StmtPtr& child : stmt.statements) {
                    if (stmt_mentions_identifier(*child, name)) return true;
                }
                return false;
        }
        return false;
    }

    void substitute_identifier_in_expr(Expr& expr, const std::string& from, const std::string& to) {
        if (expr.kind == ExprKind::Identifier && expr.name == from) {
            expr.name = to;
            return;
        }
        if (expr.lhs) substitute_identifier_in_expr(*expr.lhs, from, to);
        if (expr.rhs) substitute_identifier_in_expr(*expr.rhs, from, to);
        for (ExprPtr& arg : expr.args) substitute_identifier_in_expr(*arg, from, to);
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) substitute_identifier_in_expr(*capture.init, from, to);
        }
        if (expr.lambda_body) substitute_identifier_in_stmt(*expr.lambda_body, from, to);
    }

    void substitute_identifier_in_stmt(Stmt& stmt, const std::string& from, const std::string& to) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) substitute_identifier_in_expr(*stmt.init, from, to);
                for (ExprPtr& arg : stmt.ctor_args) substitute_identifier_in_expr(*arg, from, to);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) substitute_identifier_in_expr(*stmt.expr, from, to);
                return;
            case StmtKind::If:
                substitute_identifier_in_expr(*stmt.condition, from, to);
                substitute_identifier_in_stmt(*stmt.then_branch, from, to);
                if (stmt.else_branch) substitute_identifier_in_stmt(*stmt.else_branch, from, to);
                return;
            case StmtKind::While:
                substitute_identifier_in_expr(*stmt.condition, from, to);
                substitute_identifier_in_stmt(*stmt.then_branch, from, to);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) substitute_identifier_in_stmt(*s, from, to);
                return;
        }
    }

    [[nodiscard]] ExprPtr make_fold_identity(BinaryOp op, SourceLocation loc) const {
        auto node = std::make_unique<Expr>();
        node->loc = loc;
        switch (op) {
            case BinaryOp::Add:
                node->kind = ExprKind::IntegerLiteral;
                node->int_value = 0;
                return node;
            case BinaryOp::Mul:
                node->kind = ExprKind::IntegerLiteral;
                node->int_value = 1;
                return node;
            case BinaryOp::And:
                node->kind = ExprKind::BoolLiteral;
                node->bool_value = true;
                return node;
            case BinaryOp::Or:
                node->kind = ExprKind::BoolLiteral;
                node->bool_value = false;
                return node;
            default: return nullptr;
        }
    }

    [[nodiscard]] ExprPtr build_binary_expr(BinaryOp op, ExprPtr lhs, ExprPtr rhs) const {
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Binary;
        node->binary_op = op;
        node->loc = lhs->loc;
        node->lhs = std::move(lhs);
        node->rhs = std::move(rhs);
        return node;
    }

    [[nodiscard]] ExprPtr instantiate_pack_operand(const Expr& pattern, const std::string& pack_name,
                                                   const std::string& concrete_name) {
        ExprPtr result = clone_expr(pattern);
        substitute_identifier_in_expr(*result, pack_name, concrete_name);
        return result;
    }

    [[nodiscard]] ExprPtr expand_fold_for_pack(const Expr& fold_expr, const std::string& pack_name,
                                               const std::vector<std::string>& concrete_names) {
        if (fold_expr.kind != ExprKind::Fold) return clone_expr(fold_expr);
        if (fold_expr.fold_ellipsis_on_left) {
            if (fold_expr.rhs != nullptr) {
                throw DataflowError("binary left folds are not supported in this version", fold_expr.loc);
            }
            if (concrete_names.empty()) {
                ExprPtr identity = make_fold_identity(fold_expr.binary_op, fold_expr.loc);
                if (identity) return identity;
                throw DataflowError("empty fold requires an operator identity this version does not implement",
                                    fold_expr.loc);
            }
            ExprPtr result = instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[0]);
            for (std::size_t i = 1; i < concrete_names.size(); i++) {
                result = build_binary_expr(fold_expr.binary_op, std::move(result),
                                           instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[i]));
            }
            return result;
        }

        if (fold_expr.rhs == nullptr) {
            if (concrete_names.empty()) {
                ExprPtr identity = make_fold_identity(fold_expr.binary_op, fold_expr.loc);
                if (identity) return identity;
                throw DataflowError("empty fold requires an operator identity this version does not implement",
                                    fold_expr.loc);
            }
            ExprPtr result =
                instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[concrete_names.size() - 1]);
            for (std::size_t i = concrete_names.size() - 1; i-- > 0;) {
                result = build_binary_expr(fold_expr.binary_op,
                                           instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[i]),
                                           std::move(result));
            }
            return result;
        }

        bool lhs_mentions = expr_mentions_identifier(*fold_expr.lhs, pack_name);
        bool rhs_mentions = expr_mentions_identifier(*fold_expr.rhs, pack_name);
        if (lhs_mentions == rhs_mentions) {
            throw DataflowError("fold expression must mention the parameter pack on exactly one side of '...'",
                                fold_expr.loc);
        }
        if (concrete_names.empty()) {
            return lhs_mentions ? clone_expr(*fold_expr.rhs) : clone_expr(*fold_expr.lhs);
        }
        if (lhs_mentions) {
            ExprPtr result = clone_expr(*fold_expr.rhs);
            for (std::size_t i = concrete_names.size(); i-- > 0;) {
                result = build_binary_expr(fold_expr.binary_op,
                                           instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[i]),
                                           std::move(result));
            }
            return result;
        }
        ExprPtr result = clone_expr(*fold_expr.lhs);
        for (std::size_t i = 0; i < concrete_names.size(); i++) {
            result = build_binary_expr(fold_expr.binary_op, std::move(result),
                                       instantiate_pack_operand(*fold_expr.rhs, pack_name, concrete_names[i]));
        }
        return result;
    }

    [[nodiscard]] std::vector<ExprPtr> expand_pack_argument(const Expr& expr, const std::string& pack_name,
                                                            const std::vector<std::string>& concrete_names) {
        if (expr.kind != ExprKind::PackExpansion || expr.lhs == nullptr) {
            std::vector<ExprPtr> single;
            single.push_back(clone_expr(expr));
            return single;
        }
        if (!expr_mentions_identifier(*expr.lhs, pack_name)) {
            throw DataflowError("pack expansion does not mention parameter pack '" + pack_name + "'", expr.loc);
        }
        std::vector<ExprPtr> expanded;
        expanded.reserve(concrete_names.size());
        for (const std::string& concrete_name : concrete_names) {
            ExprPtr arg = instantiate_pack_operand(*expr.lhs, pack_name, concrete_name);
            expanded.push_back(std::move(arg));
        }
        return expanded;
    }

    void expand_explicit_template_arg_packs_in_expr(Expr& expr, const std::string& pack_name,
                                                   const std::vector<std::string>& concrete_names) {
        if (expr.lhs) expand_explicit_template_arg_packs_in_expr(*expr.lhs, pack_name, concrete_names);
        if (expr.rhs) expand_explicit_template_arg_packs_in_expr(*expr.rhs, pack_name, concrete_names);
        for (ExprPtr& arg : expr.args) expand_explicit_template_arg_packs_in_expr(*arg, pack_name, concrete_names);
        if (!expr.explicit_template_args.empty()) {
            std::vector<ExplicitTemplateArg> expanded_template_args;
            for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
                if (arg.is_type && arg.type.is_pack_expansion && arg.type.kind == TypeKind::Named && arg.type.name == pack_name) {
                    for (const std::string& concrete_name : concrete_names) {
                        ExplicitTemplateArg expanded_arg;
                        expanded_arg.is_type = true;
                        expanded_arg.type.kind = TypeKind::Named;
                        expanded_arg.type.name = concrete_name;
                        expanded_template_args.push_back(std::move(expanded_arg));
                    }
                    continue;
                }
                expanded_template_args.push_back(std::move(arg));
            }
            expr.explicit_template_args = std::move(expanded_template_args);
        }
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) expand_explicit_template_arg_packs_in_expr(*capture.init, pack_name, concrete_names);
        }
        if (expr.lambda_body) {
            for (StmtPtr& s : expr.lambda_body->statements) {
                switch (s->kind) {
                    case StmtKind::VarDecl:
                        if (s->init) expand_explicit_template_arg_packs_in_expr(*s->init, pack_name, concrete_names);
                        for (ExprPtr& a : s->ctor_args) expand_explicit_template_arg_packs_in_expr(*a, pack_name, concrete_names);
                        break;
                    case StmtKind::Return:
                    case StmtKind::ExprStmt:
                        if (s->expr) expand_explicit_template_arg_packs_in_expr(*s->expr, pack_name, concrete_names);
                        break;
                    case StmtKind::If:
                    case StmtKind::While:
                        expand_explicit_template_arg_packs_in_expr(*s->condition, pack_name, concrete_names);
                        break;
                    case StmtKind::Break:
                    case StmtKind::Continue:
                        break;
                    case StmtKind::Block:
                        break;
                }
            }
        }
    }

    void expand_pack_expansions_in_expr(Expr& expr, const std::string& pack_name,
                                        const std::vector<std::string>& concrete_names) {
        if (expr.lhs) expand_pack_expansions_in_expr(*expr.lhs, pack_name, concrete_names);
        if (expr.rhs) expand_pack_expansions_in_expr(*expr.rhs, pack_name, concrete_names);
        for (ExprPtr& arg : expr.args) expand_pack_expansions_in_expr(*arg, pack_name, concrete_names);
        if (!expr.explicit_template_args.empty()) {
            std::vector<ExplicitTemplateArg> expanded_template_args;
            for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
                if (arg.is_type && arg.type.is_pack_expansion && arg.type.kind == TypeKind::Named && arg.type.name == pack_name) {
                    for (const std::string& concrete_name : concrete_names) {
                        ExplicitTemplateArg expanded_arg;
                        expanded_arg.is_type = true;
                        expanded_arg.type.kind = TypeKind::Named;
                        expanded_arg.type.name = concrete_name;
                        expanded_template_args.push_back(std::move(expanded_arg));
                    }
                    continue;
                }
                expanded_template_args.push_back(std::move(arg));
            }
            expr.explicit_template_args = std::move(expanded_template_args);
        }
        if (!expr.args.empty()) {
            std::vector<ExprPtr> expanded_args;
            for (ExprPtr& arg : expr.args) {
                std::vector<ExprPtr> expanded = expand_pack_argument(*arg, pack_name, concrete_names);
                for (ExprPtr& item : expanded) expanded_args.push_back(std::move(item));
            }
            expr.args = std::move(expanded_args);
        }
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) expand_pack_expansions_in_expr(*capture.init, pack_name, concrete_names);
        }
        if (expr.lambda_body) expand_pack_expansions_in_stmt(*expr.lambda_body, pack_name, concrete_names);
    }

    void expand_explicit_template_arg_packs_in_stmt(Stmt& stmt, const std::string& pack_name,
                                                   const std::vector<std::string>& concrete_names) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) expand_explicit_template_arg_packs_in_expr(*stmt.init, pack_name, concrete_names);
                for (ExprPtr& arg : stmt.ctor_args) expand_explicit_template_arg_packs_in_expr(*arg, pack_name, concrete_names);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) expand_explicit_template_arg_packs_in_expr(*stmt.expr, pack_name, concrete_names);
                return;
            case StmtKind::If:
                expand_explicit_template_arg_packs_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_explicit_template_arg_packs_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                if (stmt.else_branch) expand_explicit_template_arg_packs_in_stmt(*stmt.else_branch, pack_name, concrete_names);
                return;
            case StmtKind::While:
                expand_explicit_template_arg_packs_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_explicit_template_arg_packs_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) expand_explicit_template_arg_packs_in_stmt(*s, pack_name, concrete_names);
                return;
        }
    }

    void expand_pack_folds_in_expr(Expr& expr, const std::string& pack_name, const std::vector<std::string>& concrete_names) {
        if (expr.kind == ExprKind::Fold &&
            (expr_mentions_identifier(expr, pack_name) ||
             (!expr.rhs && expr_mentions_identifier(*expr.lhs, pack_name)))) {
            ExprPtr expanded = expand_fold_for_pack(expr, pack_name, concrete_names);
            expr = std::move(*expanded);
        }
        if (expr.lhs) expand_pack_folds_in_expr(*expr.lhs, pack_name, concrete_names);
        if (expr.rhs) expand_pack_folds_in_expr(*expr.rhs, pack_name, concrete_names);
        for (ExprPtr& arg : expr.args) expand_pack_folds_in_expr(*arg, pack_name, concrete_names);
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) expand_pack_folds_in_expr(*capture.init, pack_name, concrete_names);
        }
        if (expr.lambda_body) expand_pack_folds_in_stmt(*expr.lambda_body, pack_name, concrete_names);
    }

    void expand_pack_folds_in_stmt(Stmt& stmt, const std::string& pack_name, const std::vector<std::string>& concrete_names) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) expand_pack_folds_in_expr(*stmt.init, pack_name, concrete_names);
                for (ExprPtr& arg : stmt.ctor_args) expand_pack_folds_in_expr(*arg, pack_name, concrete_names);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) expand_pack_folds_in_expr(*stmt.expr, pack_name, concrete_names);
                return;
            case StmtKind::If:
                expand_pack_folds_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_pack_folds_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                if (stmt.else_branch) expand_pack_folds_in_stmt(*stmt.else_branch, pack_name, concrete_names);
                return;
            case StmtKind::While:
                expand_pack_folds_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_pack_folds_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) expand_pack_folds_in_stmt(*s, pack_name, concrete_names);
                return;
        }
    }

    void expand_pack_expansions_in_stmt(Stmt& stmt, const std::string& pack_name,
                                        const std::vector<std::string>& concrete_names) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) expand_pack_expansions_in_expr(*stmt.init, pack_name, concrete_names);
                if (!stmt.ctor_args.empty()) {
                    for (ExprPtr& arg : stmt.ctor_args) expand_pack_expansions_in_expr(*arg, pack_name, concrete_names);
                    std::vector<ExprPtr> expanded_args;
                    for (ExprPtr& arg : stmt.ctor_args) {
                        std::vector<ExprPtr> expanded = expand_pack_argument(*arg, pack_name, concrete_names);
                        for (ExprPtr& item : expanded) expanded_args.push_back(std::move(item));
                    }
                    stmt.ctor_args = std::move(expanded_args);
                }
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) expand_pack_expansions_in_expr(*stmt.expr, pack_name, concrete_names);
                return;
            case StmtKind::If:
                expand_pack_expansions_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_pack_expansions_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                if (stmt.else_branch) expand_pack_expansions_in_stmt(*stmt.else_branch, pack_name, concrete_names);
                return;
            case StmtKind::While:
                expand_pack_expansions_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_pack_expansions_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) expand_pack_expansions_in_stmt(*s, pack_name, concrete_names);
                return;
        }
    }

    std::string get_or_create_clone(const Function& tmpl, const std::vector<Type>& concrete_param_types,
                                    const std::vector<std::vector<Type>>& concrete_pack_param_types) {
        std::string cache_key = tmpl.name;
        for (std::size_t i = 0; i < tmpl.params.size(); i++) {
            if (tmpl.params[i].is_parameter_pack) {
                for (const Type& t : concrete_pack_param_types[i]) cache_key += "." + mangle_type_for_clone_name(t);
            } else {
                cache_key += "." + mangle_type_for_clone_name(concrete_param_types[i]);
            }
        }
        auto cached = clone_cache_.find(cache_key);
        if (cached != clone_cache_.end()) return cached->second;
        // Reserve the name *before* recursing (cloning tmpl's own body
        // below never re-enters get_or_create_clone for this exact same
        // key, but keeping this assignment first is simpler to reason
        // about than proving that independently).
        clone_cache_[cache_key] = cache_key;

        Function clone;
        clone.return_type = tmpl.return_type;
        clone.return_lifetime = tmpl.return_lifetime;
        clone.name = cache_key;
        clone.loc = tmpl.loc;
        clone.namespace_path = tmpl.namespace_path;
        // A monomorphized instantiation is always an internal
        // implementation detail of whatever called it -- never itself
        // directly exported (ch11 §11.3 doesn't apply to a compiler-
        // synthesized clone with a compiler-synthesized name).
        clone.is_exported = false;
        clone.is_unsafe = tmpl.is_unsafe;
        clone.is_nodiscard = tmpl.is_nodiscard;
        clone.nodiscard_reason = tmpl.nodiscard_reason;
        clone.owning_module = tmpl.owning_module;
        clone.visibility_module = tmpl.visibility_module.empty() ? tmpl.owning_module : tmpl.visibility_module;
        clone.eval_mode = tmpl.eval_mode;
        clone.member_owner_class = tmpl.member_owner_class;
        clone.is_static = tmpl.is_static;
        clone.access = tmpl.access;
        std::unordered_map<std::string, Type> witness_replacements;
        for (std::size_t i = 0; i < tmpl.params.size() && i < concrete_param_types.size(); i++) {
            if (!tmpl.params[i].generic_concept.empty()) {
                const Type& concrete = concrete_param_types[i].kind == TypeKind::Reference
                                           ? *concrete_param_types[i].pointee
                                           : concrete_param_types[i];
                witness_replacements[tmpl.params[i].generic_concept] = concrete;
            }
        }
        for (const auto& [witness_name, concrete] : witness_replacements) {
            clone.return_type = substitute_type_param(clone.return_type, witness_name, concrete);
        }
        clone.params.reserve(tmpl.params.size());
        std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
        for (std::size_t i = 0; i < tmpl.params.size(); i++) {
            if (tmpl.params[i].is_parameter_pack) {
                pack_param_names[tmpl.params[i].name] = {};
                for (std::size_t j = 0; j < concrete_pack_param_types[i].size(); j++) {
                    Param p = tmpl.params[i];
                    p.name = tmpl.params[i].name + "$" + std::to_string(j);
                    p.type = concrete_pack_param_types[i][j];
                    clone.params.push_back(p);
                    pack_param_names[tmpl.params[i].name].push_back(p.name);
                }
                continue;
            }
            Param p = tmpl.params[i];
            p.name = tmpl.params[i].name;
            p.type = concrete_param_types[i];
            clone.params.push_back(std::move(p));
        }
        clone.body = tmpl.body ? clone_stmt(*tmpl.body) : nullptr;
        if (clone.body) {
            for (const auto& [witness_name, concrete] : witness_replacements) {
                substitute_type_param_in_stmt(*clone.body, witness_name, concrete);
            }
            for (const auto& [pack_name, concrete_names] : pack_param_names) {
                expand_pack_folds_in_stmt(*clone.body, pack_name, concrete_names);
            }
        }
        // is_generic_template stays false (default): the clone is an
        // ordinary, fully concrete function from here on, checked
        // normally by movecheck (see monomorphize_generics's own
        // comment) and compiled normally by codegen.

        program_.functions.push_back(std::move(clone));
        return cache_key;
    }
};


void monomorphize_generics_impl(Program& program) {
    Monomorphizer monomorphizer(program);
    monomorphizer.run();
}

} // namespace scpp
