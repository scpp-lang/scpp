#include "interfaces.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "movecheck.h"
#include "ast.h"
#include "signatures.h"
#include "types.h"
#include "threadsafety.h"

namespace scpp {

[[maybe_unused]] [[nodiscard]] const StructDef* find_struct_def(const Program& program, const std::string& struct_name) {
    for (const StructDef& def : program.structs) {
        if (def.name == struct_name) return &def;
    }
    return nullptr;
}

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

    void run() {
        for (const ClassDef& def : program_.classes) {
            if (should_skip(def)) continue;
            validate_class_shape(def);
        }
        for (const ClassDef& def : program_.classes) {
            if (should_skip(def)) continue;
            (void)analyze(def.name);
        }
        for (const Function& fn : program_.functions) {
            if (!fn.body) continue;
            if (!fn.member_owner_class.empty() && !fn.forwards_to.empty()) continue;
            validate_function_signature(fn);
            validate_function_body(fn, *fn.body);
        }
        validate_thread_contracts();
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
                std::string result = type.name;
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
                return std::string(type.is_mutable_pointee ? "ptr(" : "ptr_const(") +
                       (type.pointee ? type_key(*type.pointee) : std::string("?")) + ")";
            case TypeKind::Reference:
                return std::string(type.is_rvalue_ref ? "rvref(" : (type.is_mutable_ref ? "ref(" : "cref(")) +
                       (type.pointee ? type_key(*type.pointee) : std::string("?")) + ")";
            case TypeKind::Array:
                return std::string("array(") + (type.element ? type_key(*type.element) : std::string("?")) + ")";
            case TypeKind::Function:
            case TypeKind::FunctionPointer: {
                std::string result = type.kind == TypeKind::Function ? "fn(" : "fnptr(";
                for (size_t i = 0; i < type.function_params.size(); i++) {
                    if (i != 0) result += ",";
                    result += type_key(type.function_params[i]);
                }
                result += ")->";
                result += type.function_return ? type_key(*type.function_return) : std::string("void");
                return result;
            }
            case TypeKind::Span:
                return std::string(type.is_mutable_ref ? "span(" : "cspan(") +
                       (type.pointee ? type_key(*type.pointee) : std::string("?")) + ")";
        }
        return "?";
    }

    [[nodiscard]] static bool is_constructor_slot(const Function& fn) { return fn.name.ends_with("_new"); }
    [[nodiscard]] static bool is_destructor_slot(const Function& fn) { return fn.name.ends_with("_delete"); }
    [[nodiscard]] static std::string instantiated_template_source_name(std::string_view class_name) {
        size_t dot = class_name.find('.');
        return dot == std::string_view::npos ? std::string() : std::string(class_name.substr(0, dot));
    }

    [[nodiscard]] static std::string lookup_name(const Function& fn) {
        if (is_destructor_slot(fn)) return "~";
        if (fn.name.ends_with("_operator_deref")) return "operator*";
        if (fn.name.ends_with("_operator_assign")) return "operator=";
        if (!fn.member_owner_class.empty() && fn.name.rfind(fn.member_owner_class + "_", 0) == 0) {
            return fn.name.substr(fn.member_owner_class.size() + 1);
        }
        return fn.name;
    }

    [[nodiscard]] static std::string slot_key(const Function& fn) {
        std::string key = lookup_name(fn);
        key += "(";
        size_t start = fn.member_owner_class.empty() ? 0 : 1;
        for (size_t i = start; i < fn.params.size(); i++) {
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

    void validate_class_shape(const ClassDef& def) {
        int ordinary_bases = 0;
        for (const BaseSpecifier& base : def.base_specifiers) {
            const ClassDef* base_def = find_class_def(program_, base.base_type.name);
            if (base_def == nullptr) continue;
            if (base_def->is_interface) {
                if (!base.is_virtual) {
                    throw DataflowError("class '" + def.name + "' directly inherits interface '" + base.base_type.name +
                                        "' without the required 'virtual' (spec §11.3(1))");
                }
            } else {
                ordinary_bases++;
                if (base.is_virtual) {
                    throw DataflowError("class '" + def.name + "' directly inherits ordinary class '" + base.base_type.name +
                                        "' with forbidden 'virtual' (spec §11.3(2))");
                }
            }

        }
        if (ordinary_bases > 1) {
            throw DataflowError("class '" + def.name + "' has more than one ordinary direct base class (spec §11.1(6))");
        }
        if (def.is_interface && !def.fields.empty()) {
            throw DataflowError("interface '" + def.name + "' declares a non-static data member (spec §11.2(1))");
        }
        if (def.is_interface) {
            std::unordered_set<std::string> visiting;
            validate_interface_bases(def, visiting);
        }
        validate_explicit_virtual_destructor(def);
        for (const ClassField& field : def.fields) {
            if (type_forms_interface_object(field.type, program_)) {
                throw DataflowError("class '" + def.name + "' forms an object of interface type in a non-static data member "
                                    "declaration (spec §11.2(5.2))");
            }
        }
    }

    void validate_interface_bases(const ClassDef& def, std::unordered_set<std::string>& visiting) {
        if (!visiting.insert(def.name).second) return;
        for (const BaseSpecifier& base : def.base_specifiers) {
            const ClassDef* base_def = find_class_def(program_, base.base_type.name);
            if (base_def == nullptr) continue;
            if (!base_def->is_interface) {
                throw DataflowError("interface '" + def.name + "' inherits ordinary class '" + base_def->name +
                                    "' through its base graph (spec §11.2(3))");
            }
            validate_interface_bases(*base_def, visiting);
        }
        visiting.erase(def.name);
    }

    void validate_explicit_virtual_destructor(const ClassDef& def) {
        const Function* destructor = nullptr;
        for (const Function* fn : declared_members_of(def.name)) {
            if (is_destructor_slot(*fn)) {
                destructor = fn;
                break;
            }
        }
        if (destructor == nullptr) {
            throw DataflowError("class '" + def.name + "' must declare an explicit virtual destructor (spec §11.5(1))");
        }
        bool overrides_base = false;
        std::string dtor_slot = slot_key(*destructor);
        for (const BaseSpecifier& base : def.base_specifiers) {
            Analysis& base_analysis = analyze(base.base_type.name);
            if (base_analysis.all_virtual_slots.contains(dtor_slot)) {
                overrides_base = true;
                break;
            }
        }
        if (!destructor->is_virtual && !overrides_base) {
            std::string template_source = instantiated_template_source_name(def.name);
            if (!template_source.empty()) {
                for (const Function* fn : declared_members_of(template_source)) {
                    if (is_destructor_slot(*fn) && (fn->is_virtual || fn->is_override)) return;
                }
            }
        }
        if (!destructor->is_virtual && !overrides_base) {
            throw DataflowError("destructor of class '" + def.name + "' must be declared virtual or override a base "
                                "virtual destructor (spec §11.5(1)-(3))",
                                destructor->loc);
        }
    }

    Analysis& analyze(const std::string& class_name) {
        Analysis& result = analyses_[class_name];
        if (result.computed) return result;
        if (!analysis_stack_.insert(class_name).second) {
            throw DataflowError("cyclic class inheritance involving '" + class_name + "'");
        }
        const ClassDef* def = class_defs_.at(class_name);

        std::unordered_map<std::string, std::vector<Provider>> base_visible_candidates;
        std::unordered_map<std::string, std::unordered_set<std::string>> base_visible_contributors;
        std::unordered_map<std::string, std::vector<Provider>> base_virtual_candidates;
        for (const BaseSpecifier& base : def->base_specifiers) {
            result.reachable_bases.insert(base.base_type.name);
            Analysis& base_analysis = analyze(base.base_type.name);
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
                throw DataflowError("member '" + name + "' of class '" + def->name +
                                    "' overrides a base virtual member but omits 'override' (spec §11.5(4))",
                                    fn->loc);
            }
            if (!overrides && fn->is_override) {
                throw DataflowError("member '" + name + "' of class '" + def->name +
                                    "' is marked 'override' but does not override any base virtual member (spec §11.5(5))",
                                    fn->loc);
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
                throw DataflowError("class '" + def->name + "' names non-base class '" + using_decl.base_name +
                                    "' in a using-declaration (spec §11.4)");
            }
            Analysis& target_analysis = analyze(using_decl.base_name);
            auto base_it = target_analysis.visible_names.find(using_decl.member_name);
            if (base_it == target_analysis.visible_names.end() || base_it->second.empty()) {
                throw DataflowError("class '" + def->name + "' names missing member '" + using_decl.member_name +
                                    "' in using " + using_decl.base_name + "::" + using_decl.member_name + "'");
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
                throw DataflowError("class '" + def->name + "' inherits ambiguous member name '" + name +
                                    "' from multiple bases without an overriding declaration or using-declaration "
                                    "(spec §11.4(1)-(4))");
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
                throw DataflowError("class '" + def->name +
                                    "' needs its own overriding declaration to provide a unique final overrider for '" +
                                    chosen.name + "' (spec §11.4(5)-(6))");
            }
            if (have_chosen) result.effective_virtual_slots[slot] = chosen;
        }

        result.computed = true;
        analysis_stack_.erase(class_name);
        return result;
    }

    void validate_thread_contracts() {
        for (const ClassDef& def : program_.classes) {
            if (should_skip(def) || def.is_interface) continue;
            std::unordered_set<std::string> interfaces;
            collect_interfaces(def.name, interfaces);
            for (const std::string& interface_name : interfaces) {
                const ClassDef* iface = find_class_def(program_, interface_name);
                if (iface == nullptr) continue;
                Type self = named_type(def.name);
                if (iface->thread_movable_override && !thread_movable_of(self, program_)) {
                    throw DataflowError("class '" + def.name + "' violates inherited thread-movable contract of interface '" +
                                        interface_name + "' (spec §8.5(2)-(5))");
                }
                if (iface->thread_shareable_override && !thread_shareable_of(self, program_)) {
                    throw DataflowError("class '" + def.name +
                                        "' violates inherited thread-shareable contract of interface '" +
                                        interface_name + "' (spec §8.5(3)-(5))");
                }
            }
        }
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

    void validate_function_signature(const Function& fn) {
        for (size_t i = 0; i < fn.params.size(); i++) {
            if (i == 0 && fn.params[i].name == "this") continue;
            if (type_forms_interface_object(fn.params[i].type, program_)) {
                throw DataflowError("function '" + fn.name + "' forms an object of interface type in a by-value "
                                    "parameter declaration (spec §11.2(5.6))",
                                    fn.loc);
            }
        }
        if (type_forms_interface_object(fn.return_type, program_)) {
            throw DataflowError("function '" + fn.name + "' returns an interface type by value (spec §11.2(5.7))",
                                fn.loc);
        }
    }

    void validate_function_body(const Function& fn, const Stmt& body_stmt) {
        Body body = build_mir(fn);
        body.program = &program_;
        walk_stmt(body_stmt, body);
    }

    void walk_stmt(const Stmt& stmt, const Body& body) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (type_forms_interface_object(stmt.type, program_)) {
                    throw DataflowError("a local variable definition forms an object of interface type (spec §11.2(5.1))",
                                        stmt.loc);
                }
                if (stmt.init) walk_expr(*stmt.init, body);
                for (const ExprPtr& arg : stmt.ctor_args) walk_expr(*arg, body);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) walk_expr(*stmt.expr, body);
                return;
            case StmtKind::If:
                walk_expr(*stmt.condition, body);
                walk_stmt(*stmt.then_branch, body);
                if (stmt.else_branch) walk_stmt(*stmt.else_branch, body);
                return;
            case StmtKind::While:
                walk_expr(*stmt.condition, body);
                walk_stmt(*stmt.then_branch, body);
                return;
            case StmtKind::Block:
                for (const StmtPtr& nested : stmt.statements) walk_stmt(*nested, body);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
        }
    }

    void walk_expr(const Expr& expr, const Body& body) {
        if (expr.lhs) walk_expr(*expr.lhs, body);
        if (expr.rhs) walk_expr(*expr.rhs, body);
        if (expr.third) walk_expr(*expr.third, body);
        for (const ExprPtr& arg : expr.args) walk_expr(*arg, body);
        for (const LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) walk_expr(*capture.init, body);
        }
        if (expr.lambda_body) walk_stmt(*expr.lambda_body, body);
        if (expr.kind == ExprKind::New && type_forms_interface_object(expr.type, program_)) {
            throw DataflowError("a new-expression forms an object whose most-derived type is an interface (spec §11.2(5.4))",
                                expr.loc);
        }
        if (expr.kind == ExprKind::Call || expr.kind == ExprKind::Cast) {
            std::optional<Type> inferred = infer_expr_type(expr, body, signatures_);
            if (inferred.has_value() && type_forms_interface_object(*inferred, program_)) {
                throw DataflowError("an expression forms a temporary object whose most-derived type is an interface "
                                    "(spec §11.2(5.5))",
                                    expr.loc);
            }
        }
    }
};

// ch05 §5.11: a deep (recursive) copy of an Expr/Stmt tree -- needed
// only for monomorphization (below), which must inject an independent
// clone of a generic template's body per concrete instantiation (Stmt/
// Expr trees use unique_ptr children with no copy constructor of their
// own, by design -- see Expr/Stmt's own comments in ast.cppm).


void validate_class_semantics(const Program& program, const Signatures& signatures) {
    ClassSemanticsValidator(program, signatures).run();
}

} // namespace scpp
