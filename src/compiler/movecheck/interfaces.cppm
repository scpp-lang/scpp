module;

module scpp.compiler.movecheck:interfaces;

import std;
import scpp.ast;
import :errors;
import :signatures;
import :types;
import :threadsafety;

namespace scpp {

[[nodiscard]] bool type_forms_interface_object(const Type& type, const Program& program);
[[nodiscard]] std::expected<void, DataflowError> validate_class_semantics(const Program& program, const Signatures& signatures);

[[nodiscard]] bool type_forms_interface_object(const Type& type, const Program& program) {
    switch (type.kind) {
        case TypeKind::Named: {
            const ClassDef* def = find_class_def(program, type.name);
            return def != nullptr && def->is_interface;
        }
        case TypeKind::Array: return type.element != nullptr && type_forms_interface_object(*type.element, program);
        default: return false;
    }
}

// This validator is, despite its name, the compiler's only enumeration
// of the positions at which a *type is declared*: a class field, a
// struct field, a parameter, a local variable, a namespace-scope
// variable, and a new-expression. It grew that enumeration for the
// interface-object rules of spec §11.2(5), but the list is not specific
// to them -- so any rule of the form "may an object of this declared
// type exist here?" belongs on the same list rather than on a private
// copy of it. `validate_declared_type` below is the second such rule.
class ClassSemanticsValidator {
public:
    ClassSemanticsValidator(const Program& program, const Signatures& signatures)
        : program_(program), signatures_(signatures) {
        class_defs_.reserve(program_.classes.size());
        for (const ClassDef& def : program_.classes) {
            class_defs_[def.name] = &def;
        }
        for (const Function& fn : program_.functions) {
            if (fn.member_owner_class.empty() || !fn.forwards_to.empty()) continue;
            declared_methods_[fn.member_owner_class].push_back(&fn);
        }
    }

    [[nodiscard]] std::expected<void, DataflowError> run() {
        for (const ClassDef& def : program_.classes) {
            if (should_skip(def)) continue;
            if (auto _r = validate_class_shape(def); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        for (const StructDef& def : program_.structs) {
            if (should_skip_struct(def)) continue;
            if (auto _r = validate_struct_shape(def); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        for (const ClassDef& def : program_.classes) {
            if (should_skip(def)) continue;
            if (auto _r = ensure_analyzed(def.name); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        // Every function's signature, not just every function with a
        // body: a pure virtual method has no body, and its parameter
        // types are declarations exactly as any other function's are.
        // Checking only the ones with bodies is what let
        // `virtual int m(void x) = 0;` reach codegen, where LLVM
        // reported it as "Function arguments must have first-class
        // types!" -- a verifier message about the lowering rather than a
        // diagnostic about the program.
        for (const Function& fn : program_.functions) {
            if (!fn.member_owner_class.empty() && !fn.forwards_to.empty()) continue;
            if (auto _r = validate_function_signature(fn); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        for (const Function& fn : program_.functions) {
            if (!fn.body) continue;
            if (!fn.member_owner_class.empty() && !fn.forwards_to.empty()) continue;
            if (auto _r = validate_function_body(fn, *fn.body); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        // spec §11.2(5) is stated over "any object-forming context", and
        // the list that follows it is explicitly open ("including"). A
        // function body is only one such context: a namespace-scope
        // variable, a class field's default member initializer, a struct
        // field's, and a parameter's default argument all form objects
        // too, and none of them is reachable from any function body, so
        // no loop above visits them.
        //
        // This file has now needed three fixes for that one reason, each
        // time because it kept its own list of those positions and the
        // list was short: globals were added, then class fields, and
        // struct fields and parameter defaults were still missing -- so
        // an interface temporary written in either reached codegen,
        // which assumes such an object cannot exist and segfaults
        // building it. The list is no longer kept here. It comes from
        // for_each_initializer_scope, which movecheck's conversion
        // checks walk as well, so the next position added to the
        // language is added once and both passes see it.
        if (auto _r = validate_initializer_scopes(); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return validate_thread_contracts();
    }

private:
    struct Provider {
        const ClassDef* owner = nullptr;
        const Function* fn = nullptr;
        std::string slot_key;
        std::string name;
    };

    struct Analysis {
        bool computed = false;
        std::unordered_map<std::string, std::vector<Provider>> visible_names;
        std::unordered_map<std::string, Provider> effective_virtual_slots;
        std::unordered_set<std::string> all_virtual_slots;
        std::unordered_set<std::string> reachable_bases;
    };

    const Program& program_;
    const Signatures& signatures_;
    std::unordered_map<std::string, const ClassDef*> class_defs_;
    std::unordered_map<std::string, std::vector<const Function*>> declared_methods_;
    std::unordered_map<std::string, Analysis> analyses_;
    std::unordered_set<std::string> analysis_stack_;

    [[nodiscard]] static bool should_skip(const ClassDef& def) {
        return def.is_forward_declaration || def.is_concept_witness || def.is_synthetic_check_only ||
               !def.template_params.empty() || def.is_variadic_primary_template || def.is_variadic_specialization ||
               def.is_partial_specialization || def.name.rfind("__lambda", 0) == 0;
    }

    // The struct counterpart of should_skip: a template definition has
    // symbolic field types that mean nothing until instantiated, and a
    // forward declaration has no fields at all.
    [[nodiscard]] static bool should_skip_struct(const StructDef& def) {
        return def.is_forward_declaration || !def.template_params.empty();
    }

    // The one judgement passed on a *declaration*, as opposed to on an
    // expression: may an object of this declared type exist here at all?
    //
    // Every caller below is a position that brings an object into being
    // -- a class field, a struct field, a parameter, a local variable, a
    // namespace-scope variable -- and this validator is the only pass in
    // the compiler that enumerates them. That is why the rule lives
    // here: there was no "declared types are validated here" place to
    // put it in, and the absence is exactly why the same question was
    // being answered separately, and differently, in four codegen sites.
    // A function's *return* type is deliberately not a caller: it
    // declares no object, and `void` there is the one place `void` is
    // meaningful.
    [[nodiscard]] static std::expected<void, DataflowError> validate_declared_type(const Type& type,
                                                                                   const std::string& position,
                                                                                   const SourceLocation& loc) {
        if (type_can_have_objects(type)) return {};
        return std::unexpected(DataflowError(
            position + " declares an object of type '" + describe_type_brief(type) +
                "', but 'void' is the type of no object: it may be a function's return type or a pointer's pointee "
                "-- 'void*' -- and nothing else ([basic.types.general], applied unchanged by spec §1(2))",
            loc));
    }

    [[nodiscard]] static std::string non_type_expr_key(const Expr* expr) {
        if (expr == nullptr) return "?";
        switch (expr->kind) {
            case ExprKind::IntegerLiteral: return std::to_string(expr->int_value);
            case ExprKind::Identifier: return expr->name;
            case ExprKind::Binary:
                if (expr->binary_op == BinaryOp::Add) {
                    return non_type_expr_key(expr->lhs.get()) + "+" + non_type_expr_key(expr->rhs.get());
                }
                break;
            default: break;
        }
        return "?";
    }

    [[nodiscard]] static std::string type_key(const Type& type) {
        switch (type.kind) {
            case TypeKind::Named: {
                std::string result = type.is_const_qualified ? std::string("const ") + type.name : type.name;
                if (!type.non_type_args.empty() || !type.template_args.empty()) {
                    result += "<";
                    bool first = true;
                    for (const std::shared_ptr<Expr>& arg : type.non_type_args) {
                        if (!first) result += ",";
                        first = false;
                        result += non_type_expr_key(arg.get());
                    }
                    for (const Type& arg : type.template_args) {
                        if (!first) result += ",";
                        first = false;
                        result += type_key(arg);
                    }
                    result += ">";
                }
                if (type.is_pack_expansion) result += "...";
                return result;
            }
            case TypeKind::Pointer:
                return std::string(type.is_const_qualified ? "const_" : "") +
                       std::string(type.is_mutable_pointee ? "ptr(" : "ptr_const(") +
                       (type.pointee ? type_key(*type.pointee) : std::string("?")) + ")";
            case TypeKind::Reference:
                return std::string(type.is_const_qualified ? "const_" : "") +
                       std::string(type.is_rvalue_ref ? "rvref(" : (type.is_mutable_ref ? "ref(" : "cref(")) +
                       (type.pointee ? type_key(*type.pointee) : std::string("?")) + ")";
            case TypeKind::Array:
                return std::string(type.is_const_qualified ? "const_" : "") +
                       std::string("array(") + (type.element ? type_key(*type.element) : std::string("?")) + ")";
            case TypeKind::Function:
            case TypeKind::FunctionPointer: {
                std::string result = type.is_const_qualified ? "const_" : "";
                result += type.kind == TypeKind::Function ? "fn(" : "fnptr(";
                for (std::size_t i = 0; i < type.function_params.size(); i++) {
                    if (i != 0) result += ",";
                    result += type_key(type.function_params[i]);
                }
                result += ")->";
                result += type.function_return ? type_key(*type.function_return) : std::string("void");
                return result;
            }
            case TypeKind::Span:
                return std::string(type.is_const_qualified ? "const_" : "") +
                       std::string(type.is_mutable_ref ? "span(" : "cspan(") +
                       (type.pointee ? type_key(*type.pointee) : std::string("?")) + ")";
        }
        return "?";
    }

    [[nodiscard]] static bool is_constructor_slot(const Function& fn) { return fn.name.ends_with("_new"); }
    [[nodiscard]] static bool is_destructor_slot(const Function& fn) { return fn.name.ends_with("_delete"); }
    [[nodiscard]] static std::string instantiated_template_source_name(std::string_view class_name) {
        std::size_t dot = class_name.find('.');
        return dot == std::string_view::npos ? std::string() : std::string(class_name.substr(0, dot));
    }

    [[nodiscard]] static std::string lookup_name(const Function& fn) {
        if (is_destructor_slot(fn)) return "~";
        if (fn.name.ends_with("_operator_deref")) return "operator*";
        if (fn.name.ends_with("_operator_arrow")) return "operator->";
        if (fn.name.ends_with("_operator_assign")) return "operator=";
        if (fn.name.ends_with("_operator_equal")) return "operator==";
        if (fn.name.ends_with("_operator_not_equal")) return "operator!=";
        if (!fn.member_owner_class.empty() && fn.name.rfind(fn.member_owner_class + "_", 0) == 0) {
            return fn.name.substr(fn.member_owner_class.size() + 1);
        }
        return fn.name;
    }

    [[nodiscard]] static std::string slot_key(const Function& fn) {
        std::string key = lookup_name(fn);
        key += "(";
        std::size_t start = fn.member_owner_class.empty() ? 0 : 1;
        for (std::size_t i = start; i < fn.params.size(); i++) {
            if (i != start) key += ",";
            key += type_key(fn.params[i].type);
        }
        key += ")";
        if (!fn.member_owner_class.empty() && !fn.params.empty()) {
            key += fn.params[0].type.is_mutable_ref ? "&mut" : "&const";
        }
        switch (fn.receiver_ref_qualifier) {
            case ReceiverRefQualifier::LValue: key += "&"; break;
            case ReceiverRefQualifier::RValue: key += "&&"; break;
            case ReceiverRefQualifier::None: break;
        }
        return key;
    }

    [[nodiscard]] std::vector<const Function*> declared_members_of(const std::string& class_name) const {
        auto it = declared_methods_.find(class_name);
        if (it == declared_methods_.end()) return {};
        return it->second;
    }

    [[nodiscard]] bool type_names_interface(const std::string& name) const {
        auto it = class_defs_.find(name);
        return it != class_defs_.end() && it->second->is_interface;
    }

    [[nodiscard]] bool has_accessible_base_conversion(const std::string& source_name, const std::string& target_name,
                                                      std::string_view current_class) const {
        if (source_name == target_name) return true;
        auto it = class_defs_.find(source_name);
        if (it == class_defs_.end()) return false;
        for (const BaseSpecifier& base : it->second->base_specifiers) {
            if (base.access == AccessSpecifier::Private && current_class != source_name) {
                continue;
            }
            if (base.base_type.name == target_name) return true;
            if (has_accessible_base_conversion(base.base_type.name, target_name, current_class)) return true;
        }
        return false;
    }

    [[nodiscard]] bool named_base_conversion_allowed(const Type& source_type, const Type& target_type,
                                                     std::string_view current_class) const {
        if (source_type.kind != TypeKind::Named || target_type.kind != TypeKind::Named) return false;
        return has_accessible_base_conversion(source_type.name, target_type.name, current_class);
    }

    [[nodiscard]] bool types_compatible_for_base_conversion(const Type& source_type, const Type& target_type,
                                                            std::string_view current_class) const {
        if (types_equal(source_type, target_type)) return true;
        if (target_type.kind == TypeKind::Reference && source_type.kind == TypeKind::Reference &&
            !target_type.is_rvalue_ref && !source_type.is_rvalue_ref && target_type.pointee && source_type.pointee) {
            if (target_type.is_mutable_ref && !source_type.is_mutable_ref) return false;
            return named_base_conversion_allowed(*source_type.pointee, *target_type.pointee, current_class);
        }
        if (target_type.kind == TypeKind::Reference && source_type.kind != TypeKind::Reference && target_type.pointee) {
            return named_base_conversion_allowed(source_type, *target_type.pointee, current_class);
        }
        if (target_type.kind == TypeKind::Pointer && source_type.kind == TypeKind::Pointer && target_type.pointee &&
            source_type.pointee) {
            if (target_type.is_mutable_pointee && !source_type.is_mutable_pointee) return false;
            return named_base_conversion_allowed(*source_type.pointee, *target_type.pointee, current_class);
        }
        return false;
    }

    [[nodiscard]] std::expected<void, DataflowError> validate_class_shape(const ClassDef& def) {
        int ordinary_bases = 0;
        for (const BaseSpecifier& base : def.base_specifiers) {
            const ClassDef* base_def = find_class_def(program_, base.base_type.name);
            if (base_def == nullptr) continue;
            if (base_def->is_interface) {
                if (!base.is_virtual) {
                    return std::unexpected(DataflowError("class '" + def.name + "' directly inherits interface '" + base.base_type.name +
                                        "' without the required 'virtual' (spec §11.3(1))"));
                }
            } else {
                ordinary_bases++;
                if (base.is_virtual) {
                    return std::unexpected(DataflowError("class '" + def.name + "' directly inherits ordinary class '" + base.base_type.name +
                                        "' with forbidden 'virtual' (spec §11.3(2))"));
                }
            }

        }
        if (ordinary_bases > 1) {
            return std::unexpected(DataflowError("class '" + def.name + "' has more than one ordinary direct base class (spec §11.1(6))"));
        }
        if (def.is_interface && !def.fields.empty()) {
            return std::unexpected(DataflowError("interface '" + def.name + "' declares a non-static data member (spec §11.2(1))"));
        }
        if (def.is_interface) {
            std::unordered_set<std::string> visiting;
            if (auto _r = validate_interface_bases(def, visiting); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        if (auto _r = validate_explicit_virtual_destructor(def); !_r.has_value()) return std::unexpected(std::move(_r).error());
        for (const ClassField& field : def.fields) {
            if (auto _r = validate_declared_type(field.type, "class '" + def.name + "' non-static data member '" + field.name + "'",
                                                 field.loc);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
            if (type_forms_interface_object(field.type, program_)) {
                return std::unexpected(DataflowError("class '" + def.name + "' forms an object of interface type in a non-static data member "
                                    "declaration (spec §11.2(5.2))"));
            }
        }
        return {};
    }

    // Struct fields are the sixth declaration position, and until now the
    // only one no frontend pass looked at: a struct's field types were
    // judged solely by codegen's validate_trivial, whose actual question
    // is "may this be a *struct* field?" (it also rejects references,
    // spans, bare function types and class types). `void` was one line
    // inside that struct-only answer, which is why `struct S { void f; };`
    // was rejected with a sensible message while `class K { void f; };`
    // handed a void type to LLVM's layout and segfaulted.
    //
    // Only the declared-type rule is asked here. Its sibling
    // validate_class_shape also asks §11.2(5.2) ("does this field form an
    // object of interface type?"), and a struct field is just as much a
    // data member declaration -- but a struct field of interface type is
    // already rejected today (by validate_trivial, as a class type), so
    // extending §11.2(5.2) here would change a diagnostic's wording, not
    // a program's acceptance. That is a separate rule from this one and
    // gets its own change.
    [[nodiscard]] std::expected<void, DataflowError> validate_struct_shape(const StructDef& def) {
        for (const StructField& field : def.fields) {
            if (auto _r = validate_declared_type(field.type,
                                                 std::string(def.is_union ? "union '" : "struct '") + def.name + "' data member '" +
                                                     field.name + "'",
                                                 field.loc);
                !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, DataflowError> validate_interface_bases(const ClassDef& def, std::unordered_set<std::string>& visiting) {
        if (!visiting.insert(def.name).second) return {};
        for (const BaseSpecifier& base : def.base_specifiers) {
            const ClassDef* base_def = find_class_def(program_, base.base_type.name);
            if (base_def == nullptr) continue;
            if (!base_def->is_interface) {
                return std::unexpected(DataflowError("interface '" + def.name + "' inherits ordinary class '" + base_def->name +
                                    "' through its base graph (spec §11.2(3))"));
            }
            if (auto _r = validate_interface_bases(*base_def, visiting); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        visiting.erase(def.name);
        return {};
    }

    [[nodiscard]] std::expected<void, DataflowError> validate_explicit_virtual_destructor(const ClassDef& def) {
        const Function* destructor = nullptr;
        for (const Function* fn : declared_members_of(def.name)) {
            if (is_destructor_slot(*fn)) {
                destructor = fn;
                break;
            }
        }
        if (destructor == nullptr) {
            return std::unexpected(DataflowError("class '" + def.name + "' must declare an explicit virtual destructor (spec §11.5(1))"));
        }
        bool overrides_base = false;
        std::string dtor_slot = slot_key(*destructor);
        for (const BaseSpecifier& base : def.base_specifiers) {
            if (auto _r = ensure_analyzed(base.base_type.name); !_r.has_value()) return std::unexpected(std::move(_r).error());
            Analysis& base_analysis = analyses_.at(base.base_type.name);
            if (base_analysis.all_virtual_slots.contains(dtor_slot)) {
                overrides_base = true;
                break;
            }
        }
        if (!destructor->is_virtual && !overrides_base) {
            std::string template_source = instantiated_template_source_name(def.name);
            if (!template_source.empty()) {
                for (const Function* fn : declared_members_of(template_source)) {
                    if (is_destructor_slot(*fn) && (fn->is_virtual || fn->is_override)) return {};
                }
            }
        }
        if (!destructor->is_virtual && !overrides_base) {
            return std::unexpected(DataflowError("destructor of class '" + def.name + "' must be declared virtual or override a base "
                                "virtual destructor (spec §11.5(1)-(3))",
                                destructor->loc));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, DataflowError> ensure_analyzed(const std::string& class_name) {
        Analysis& result = analyses_[class_name];
        if (result.computed) return {};
        if (!analysis_stack_.insert(class_name).second) {
            return std::unexpected(DataflowError("cyclic class inheritance involving '" + class_name + "'"));
        }
        const ClassDef* def = class_defs_.at(class_name);

        std::unordered_map<std::string, std::vector<Provider>> base_visible_candidates;
        std::unordered_map<std::string, std::unordered_set<std::string>> base_visible_contributors;
        std::unordered_map<std::string, std::vector<Provider>> base_virtual_candidates;
        for (const BaseSpecifier& base : def->base_specifiers) {
            result.reachable_bases.insert(base.base_type.name);
            if (auto _r = ensure_analyzed(base.base_type.name); !_r.has_value()) return std::unexpected(std::move(_r).error());
            Analysis& base_analysis = analyses_.at(base.base_type.name);
            result.reachable_bases.insert(base_analysis.reachable_bases.begin(), base_analysis.reachable_bases.end());
            for (const auto& [name, providers] : base_analysis.visible_names) {
                auto& dest = base_visible_candidates[name];
                dest.insert(dest.end(), providers.begin(), providers.end());
                base_visible_contributors[name].insert(base.base_type.name);
            }
            for (const auto& [slot, provider] : base_analysis.effective_virtual_slots) {
                base_virtual_candidates[slot].push_back(provider);
                result.all_virtual_slots.insert(slot);
            }
            result.all_virtual_slots.insert(base_analysis.all_virtual_slots.begin(), base_analysis.all_virtual_slots.end());
        }

        std::unordered_map<std::string, std::vector<const Function*>> own_names;
        std::unordered_map<std::string, Provider> own_virtual_slots;
        for (const Function* fn : declared_members_of(def->name)) {
            if (is_constructor_slot(*fn)) continue;
            std::string name = lookup_name(*fn);
            if (name != "~" && !fn->is_static) own_names[name].push_back(fn);
            std::string slot = slot_key(*fn);
            bool overrides = result.all_virtual_slots.contains(slot);
            if (overrides && !fn->is_override) {
                return std::unexpected(DataflowError("member '" + name + "' of class '" + def->name +
                                    "' overrides a base virtual member but omits 'override' (spec §11.5(4))",
                                    fn->loc));
            }
            if (!overrides && fn->is_override) {
                return std::unexpected(DataflowError("member '" + name + "' of class '" + def->name +
                                    "' is marked 'override' but does not override any base virtual member (spec §11.5(5))",
                                    fn->loc));
            }
            bool is_effectively_virtual = fn->is_virtual || overrides;
            if (is_effectively_virtual) {
                own_virtual_slots[slot] = Provider{def, fn, slot, name};
                result.all_virtual_slots.insert(slot);
            }
        }

        std::unordered_map<std::string, std::vector<Provider>> using_names;
        for (const ClassUsingDeclaration& using_decl : def->using_declarations) {
            if (!result.reachable_bases.contains(using_decl.base_name)) {
                return std::unexpected(DataflowError("class '" + def->name + "' names non-base class '" + using_decl.base_name +
                                    "' in a using-declaration (spec §11.4)"));
            }
            if (auto _r = ensure_analyzed(using_decl.base_name); !_r.has_value()) return std::unexpected(std::move(_r).error());
            Analysis& target_analysis = analyses_.at(using_decl.base_name);
            auto base_it = target_analysis.visible_names.find(using_decl.member_name);
            if (base_it == target_analysis.visible_names.end() || base_it->second.empty()) {
                return std::unexpected(DataflowError("class '" + def->name + "' names missing member '" + using_decl.member_name +
                                    "' in using " + using_decl.base_name + "::" + using_decl.member_name + "'"));
            }
            auto& dest = using_names[using_decl.member_name];
            dest.insert(dest.end(), base_it->second.begin(), base_it->second.end());
        }

        std::unordered_set<std::string> all_names;
        for (const auto& [name, _] : base_visible_candidates) all_names.insert(name);
        for (const auto& [name, _] : own_names) all_names.insert(name);
        for (const auto& [name, _] : using_names) all_names.insert(name);
        for (const std::string& name : all_names) {
            if (own_names.contains(name)) {
                std::vector<Provider> providers;
                for (const Function* fn : own_names.at(name)) {
                    providers.push_back(Provider{def, fn, slot_key(*fn), name});
                }
                result.visible_names[name] = std::move(providers);
                continue;
            }
            if (using_names.contains(name)) {
                result.visible_names[name] = using_names.at(name);
                continue;
            }
            auto candidates_it = base_visible_candidates.find(name);
            if (candidates_it == base_visible_candidates.end()) continue;
            if (base_visible_contributors[name].size() > 1) {
                return std::unexpected(DataflowError("class '" + def->name + "' inherits ambiguous member name '" + name +
                                    "' from multiple bases without an overriding declaration or using-declaration "
                                    "(spec §11.4(1)-(4))"));
            }
            result.visible_names[name] = candidates_it->second;
        }

        for (const auto& [slot, provider] : own_virtual_slots) {
            result.effective_virtual_slots[slot] = provider;
        }
        for (const auto& [slot, providers] : base_virtual_candidates) {
            if (result.effective_virtual_slots.contains(slot)) continue;
            std::unordered_set<std::string> distinct_owners;
            Provider chosen;
            bool have_chosen = false;
            for (const Provider& provider : providers) {
                if (!distinct_owners.insert(provider.owner->name).second) continue;
                if (!have_chosen) {
                    chosen = provider;
                    have_chosen = true;
                }
            }
            if (distinct_owners.size() > 1) {
                return std::unexpected(DataflowError("class '" + def->name +
                                    "' needs its own overriding declaration to provide a unique final overrider for '" +
                                    chosen.name + "' (spec §11.4(5)-(6))"));
            }
            if (have_chosen) result.effective_virtual_slots[slot] = chosen;
        }

        result.computed = true;
        analysis_stack_.erase(class_name);
        return {};
    }

    [[nodiscard]] std::expected<void, DataflowError> validate_thread_contracts() {
        for (const ClassDef& def : program_.classes) {
            if (should_skip(def) || def.is_interface) continue;
            std::unordered_set<std::string> interfaces;
            collect_interfaces(def.name, interfaces);
            for (const std::string& interface_name : interfaces) {
                const ClassDef* iface = find_class_def(program_, interface_name);
                if (iface == nullptr) continue;
                Type self = named_type(def.name);
                if (iface->thread_movable_override) {
                    auto _r = thread_movable_of(self, program_);
                    if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                    if (!_r.value()) {
                        return std::unexpected(DataflowError("class '" + def.name + "' violates inherited thread-movable contract of interface '" +
                                            interface_name + "' (spec §8.5(2)-(5))"));
                    }
                }
                if (iface->thread_shareable_override) {
                    auto _r = thread_shareable_of(self, program_);
                    if (!_r.has_value()) return std::unexpected(std::move(_r).error());
                    if (!_r.value()) {
                        return std::unexpected(DataflowError("class '" + def.name +
                                            "' violates inherited thread-shareable contract of interface '" +
                                            interface_name + "' (spec §8.5(3)-(5))"));
                    }
                }
            }
        }
        return {};
    }

    void collect_interfaces(const std::string& class_name, std::unordered_set<std::string>& out) const {
        auto it = class_defs_.find(class_name);
        if (it == class_defs_.end()) return;
        for (const BaseSpecifier& base : it->second->base_specifiers) {
            const ClassDef* base_def = find_class_def(program_, base.base_type.name);
            if (base_def == nullptr) continue;
            if (base_def->is_interface) out.insert(base_def->name);
            collect_interfaces(base.base_type.name, out);
        }
    }

    [[nodiscard]] std::expected<void, DataflowError> validate_function_signature(const Function& fn) {
        const bool is_template = fn.is_generic_template || !fn.template_params.empty();
        for (std::size_t i = 0; i < fn.params.size(); i++) {
            if (i == 0 && fn.params[i].name == "this") continue;
            // Inside an uninstantiated template a parameter's type is
            // symbolic, and a concept witness substitutes a `void`
            // sentinel for an unconstrained one -- the same "a `void`
            // answer can mean unconstrained here" abstention
            // check_expression_yields_a_value makes (calls.cppm).
            if (!is_template) {
                if (auto _r = validate_declared_type(fn.params[i].type,
                                                     "function '" + fn.name + "' parameter '" + fn.params[i].name + "'", fn.loc);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            if (type_forms_interface_object(fn.params[i].type, program_)) {
                return std::unexpected(DataflowError("function '" + fn.name + "' forms an object of interface type in a by-value "
                                    "parameter declaration (spec §11.2(5.6))",
                                    fn.loc));
            }
        }
        if (type_forms_interface_object(fn.return_type, program_)) {
            return std::unexpected(DataflowError("function '" + fn.name + "' returns an interface type by value (spec §11.2(5.7))",
                                fn.loc));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, DataflowError> validate_function_body(const Function& fn, const Stmt& body_stmt) {
        Body body = build_mir(fn);
        body.program = &program_;
        // A constructor's `: base{...}, field{...}` list is part of the
        // constructor, but it is not part of its body Stmt, so walking
        // only body_stmt has never looked at it -- the same "walks the
        // body, skips the list" blindness already fixed in MIR lowering
        // (mem-init lists were invisible to move checking) and in
        // monomorphize (a lambda there was never given a closure class).
        // build_mir already deep-copies the list into the Body precisely
        // so a pass can walk it with identifiers resolved, so this walks
        // that copy rather than fn.member_initializers: infer_expr_type
        // needs body.local_of() to answer, and only the Body's own copy
        // carries the resolved ids.
        for (const MemberInitializer& init : body.owned_member_initializers) {
            if (auto _r = walk_initializer(init.initializer, body); !_r.has_value()) {
                return std::unexpected(std::move(_r).error());
            }
        }
        return walk_stmt(body_stmt, body);
    }

    // Every expression an Initializer can hold, in either of its two
    // spellings (`= expr` and `{args...}`), for the one position that
    // still carries an Initializer of its own: a constructor's mem-init
    // list, which is walked with the constructor's Body because it can
    // name the constructor's parameters.
    [[nodiscard]] std::expected<void, DataflowError> walk_initializer(const Initializer& init, const Body& body) {
        if (init.expr) {
            if (auto _r = walk_expr(*init.expr, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        for (const ExprPtr& arg : init.brace_args) {
            if (arg == nullptr) continue;
            if (auto _r = walk_expr(*arg, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        return {};
    }

    // Every expression position in the program that is not inside a
    // function body, with the rules applied uniformly to all of them.
    //
    // §11.2(5.1)/(5.3) are about a *declaration*, so they apply only
    // where the position declares a variable -- a namespace-scope one
    // here (a field's own type is checked as a member declaration by
    // validate_class_shape, §11.2(5.2); a parameter's by
    // validate_function_signature, §11.2(5.6)). §11.2(5.4)/(5.5) are
    // about an *expression* and so apply to every position without
    // exception, which is the part that was missing.
    [[nodiscard]] std::expected<void, DataflowError> validate_initializer_scopes() {
        return for_each_initializer_scope(program_, [&, this](const InitializerScope& scope)
                                                        -> std::expected<void, DataflowError> {
            if (scope.declares_namespace_scope_variable) {
                if (auto _r = validate_declared_type(*scope.declared_type, "global variable '" + scope.name + "'", scope.loc);
                    !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
                if (type_forms_interface_object(*scope.declared_type, program_)) {
                    return std::unexpected(DataflowError("a global variable definition forms an object of interface type (spec §11.2(5.1))",
                                        scope.loc));
                }
            }
            if (scope.expr != nullptr) {
                if (auto _r = walk_expr(*scope.expr, scope.body); !_r.has_value()) return std::unexpected(std::move(_r).error());
            }
            if (scope.brace_args != nullptr) {
                for (const ExprPtr& arg : *scope.brace_args) {
                    if (arg == nullptr) continue;
                    if (auto _r = walk_expr(*arg, scope.body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                }
            }
            return {};
        });
    }

    [[nodiscard]] std::expected<void, DataflowError> walk_stmt(const Stmt& stmt, const Body& body) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (!body.function_is_generic_template) {
                    if (auto _r = validate_declared_type(stmt.type, "local variable '" + stmt.var_name + "'", stmt.loc);
                        !_r.has_value()) {
                        return std::unexpected(std::move(_r).error());
                    }
                }
                if (type_forms_interface_object(stmt.type, program_)) {
                    return std::unexpected(DataflowError("a local variable definition forms an object of interface type (spec §11.2(5.1))",
                                        stmt.loc));
                }
                if (stmt.init) {
                    if (auto _r = walk_expr(*stmt.init, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                }
                for (const ExprPtr& arg : stmt.ctor_args) {
                    if (auto _r = walk_expr(*arg, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                }
                return {};
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) return walk_expr(*stmt.expr, body);
                return {};
            case StmtKind::If: {
                if (auto _r = walk_expr(*stmt.condition, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                if (auto _r = walk_stmt(*stmt.then_branch, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                if (stmt.else_branch) return walk_stmt(*stmt.else_branch, body);
                return {};
            }
            case StmtKind::While: {
                if (auto _r = walk_expr(*stmt.condition, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                return walk_stmt(*stmt.then_branch, body);
            }
            case StmtKind::Switch: {
                if (auto _r = walk_expr(*stmt.condition, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                for (const SwitchCase& switch_case : stmt.switch_cases) {
                    if (switch_case.value) {
                        if (auto _r = walk_expr(*switch_case.value, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                    }
                    for (const StmtPtr& nested : switch_case.statements) {
                        if (auto _r = walk_stmt(*nested, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                    }
                }
                return {};
            }
            case StmtKind::Block:
                for (const StmtPtr& nested : stmt.statements) {
                    if (auto _r = walk_stmt(*nested, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
                }
                return {};
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Fallthrough:
                return {};
        }
        return {};
    }

    [[nodiscard]] std::expected<void, DataflowError> walk_expr(const Expr& expr, const Body& body) {
        if (expr.lhs) {
            if (auto _r = walk_expr(*expr.lhs, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        if (expr.rhs) {
            if (auto _r = walk_expr(*expr.rhs, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        if (expr.third) {
            if (auto _r = walk_expr(*expr.third, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        for (const ExprPtr& arg : expr.args) {
            if (auto _r = walk_expr(*arg, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        for (const LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) {
                if (auto _r = walk_expr(*capture.init, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
            }
        }
        if (expr.lambda_body) {
            if (auto _r = walk_stmt(*expr.lambda_body, body); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        if (expr.kind == ExprKind::New) {
            // A new-expression is the one *expression* that declares a
            // type rather than producing a value of one, so the
            // declaration rule applies to it exactly as to a variable.
            if (!body.function_is_generic_template) {
                if (auto _r = validate_declared_type(expr.type, "a new-expression", expr.loc); !_r.has_value()) {
                    return std::unexpected(std::move(_r).error());
                }
            }
            if (type_forms_interface_object(expr.type, program_)) {
                return std::unexpected(DataflowError("a new-expression forms an object whose most-derived type is an interface (spec §11.2(5.4))",
                                    expr.loc));
            }
        }
        if (expr.kind == ExprKind::Call || expr.kind == ExprKind::Cast) {
            std::optional<Type> inferred = infer_expr_type(expr, body, signatures_);
            if (inferred.has_value() && type_forms_interface_object(*inferred, program_)) {
                return std::unexpected(DataflowError("an expression forms a temporary object whose most-derived type is an interface "
                                    "(spec §11.2(5.5))",
                                    expr.loc));
            }
        }
        return {};
    }
};

// ch05 §5.11: a deep (recursive) copy of an Expr/Stmt tree -- needed
// only for monomorphization (below), which must inject an independent
// clone of a generic template's body per concrete instantiation (Stmt/
// Expr trees use unique_ptr children with no copy constructor of their
// own, by design -- see Expr/Stmt's own comments in ast.cppm).


[[nodiscard]] std::expected<void, DataflowError> validate_class_semantics(const Program& program, const Signatures& signatures) {
    return ClassSemanticsValidator(program, signatures).run();
}

} // namespace scpp
