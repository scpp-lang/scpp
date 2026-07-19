module;

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Support/raw_ostream.h>

module scpp.compiler.codegen:orchestration;

import std;
import :api;

namespace scpp {

    Codegen::Codegen(const std::string& module_name, std::string source_path , bool emit_debug_info)
        : context_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>(module_name, *context_)),
          builder_(std::make_unique<llvm::IRBuilder<>>(*context_)),
          source_path_(std::move(source_path)),
          emit_debug_info_(emit_debug_info)
{}


    void Codegen::set_target(const llvm::Triple& triple, const llvm::DataLayout& data_layout)
{
        module_->setTargetTriple(triple);
        module_->setDataLayout(data_layout);
    }


    llvm::Module& Codegen::generate(const Program& program)
{
        // Structs are declared first (validated + turned into named LLVM
        // struct types) since function signatures and locals may reference
        // them. The single-pass parser only allows a struct to reference
        // itself via pointer or an *earlier* struct by value, so processing
        // program.structs in declaration order is always sufficient (no
        // separate opaque-then-setBody phase is needed: LLVM pointers are
        // opaque, so pointer fields never need the pointee's type up front).
        // Classes (ch04 §4.2) are declared next, after every struct: a
        // class field may be a trivial struct by value (never the other
        // way around -- a struct field can never be a class, since a class
        // isn't guaranteed trivial), and, like structs among themselves,
        // the single-pass parser already guarantees one class only ever
        // references an *earlier* class by value.
        program_ = &program;
        initialize_debug_info();
        // ch05 §5.11: a concept's hidden witness class (ClassDef::
        // is_concept_witness) is never a real, instantiable type -- it
        // exists purely so a generic function's own body-check has
        // something to resolve method calls against (see
        // ClassDef::is_concept_witness's own comment); skipped from
        // declare_class entirely. Its name is recorded here so the
        // Function loops below can likewise skip its (also bodyless,
        // never-compiled) methods, found via their own `this` parameter.
        //
        // ch05 §5.14: a generic class/struct *template* (ClassDef/
        // StructDef::template_params non-empty) is likewise never real
        // -- its own fields/methods still literally reference "T", never
        // a concrete type -- movecheck's Monomorphizer synthesizes a
        // separate, fully concrete class/struct (and, for a class, one
        // concrete method clone) per real instantiation instead (see
        // resolve_generic_types); and a "checking class" (ClassDef::
        // is_synthetic_check_only) is a purely internal, witness-
        // substituted artifact synthesized only so movecheck can check
        // one generic method's body once, abstractly, never meant to be
        // emitted either (see check_generic_type_methods_once).
        std::unordered_set<std::string> witness_class_names;
        std::unordered_set<std::string> generic_type_template_names;
        for (const StructDef& def : program.structs) {
            if (!def.template_params.empty()) {
                generic_type_template_names.insert(def.name);
                continue;
            }
            declare_struct(def);
        }
        for (const ClassDef& def : program.classes) {
            if (def.is_concept_witness) {
                witness_class_names.insert(def.name);
                continue;
            }
            if (def.is_variadic_specialization) {
                generic_type_template_names.insert(def.name);
                continue;
            }
            if (!def.template_params.empty()) {
                generic_type_template_names.insert(def.name);
                continue;
            }
            if (def.is_synthetic_check_only) continue;
            declare_class(def);
        }
        for (const GlobalVar& global : program.globals) {
            if (global.decl == nullptr) continue;
            llvm::Type* llvm_type = to_llvm_type(global.decl->type);
            auto* init = llvm::Constant::getNullValue(llvm_type);
            auto* variable =
                new llvm::GlobalVariable(*module_, llvm_type, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
                                         init, mangle_global_symbol_name(global.decl->var_name));
            if (global.decl->resolved_alignment != 0) variable->setAlignment(llvm::Align(global.decl->resolved_alignment));
            globals_.emplace(global.decl->var_name, GlobalSlot{variable, global.decl->type,
                                                               global.decl->is_const || global.decl->is_constexpr});
        }
        build_overload_names();
        auto is_never_compiled = [&](const Function& fn) {
            // consteval functions/constructors are compile-time-only by
            // definition; every surviving use site is lowered through the
            // constexpr engine (or rejected earlier), never by calling an
            // emitted runtime symbol.
            if (fn.eval_mode == FunctionEvalMode::Consteval) return true;
            // A generic template is checked once, abstractly, by
            // movecheck (ch05 §5.11) -- only its concrete monomorphized
            // clones (ordinary Functions by the time codegen sees them,
            // injected by movecheck's own monomorphize_generics) ever
            // actually compile.
            if (fn.is_generic_template) return true;
            // A witness class's own method is bodyless and never
            // called (every real call site resolves to a concrete
            // type's own real method instead, see
            // type_satisfies_concept/monomorphization) -- purely a
            // signature for the generic template's own body-check to
            // resolve against.
            if (!fn.member_owner_class.empty() && witness_class_names.contains(fn.member_owner_class)) {
                return true;
            }
            // ch05 §5.14: a generic class template's own, not-yet-
            // resolved method (its `this` parameter names the template
            // directly, e.g. "Vec", never a concrete instantiation like
            // "Vec_int") -- "T" is never a real type anywhere in the
            // program for these; only check_generic_type_methods_once's
            // own witness-substituted clones (is_generic_template,
            // already excluded above) and resolve_generic_types' own
            // concrete-instantiation clones (ordinary functions by now)
            // are ever compiled.
            return !fn.member_owner_class.empty() && generic_type_template_names.contains(fn.member_owner_class);
        };
        for (const Function& fn : program.functions) {
            if (is_never_compiled(fn)) continue;
            declare_function(fn);
        }
        if (!program.globals.empty()) define_global_initializers(program);
        for (const Function& fn : program.functions) {
            if (is_never_compiled(fn)) continue;
            if (fn.body != nullptr) {
                define_function(fn);
            } else if (fn.is_defaulted) {
                define_defaulted_function(fn);
            } else if (!fn.forwards_to.empty()) {
                // ch05 §5.14: an inherited method's own forwarding stub
                // (synthesize_inherited_method_forwards) -- has no
                // scpp-level AST body at all, just a thin codegen-only
                // wrapper.
                define_forwarding_function(fn);
            }
            // Otherwise: a bodyless `extern "C"` declaration (ch02
            // §2.1) already got its LLVM `declare` from declare_function
            // above; there's no body to lower.
        }
        finalize_debug_info();
        std::string error;
        llvm::raw_string_ostream error_stream(error);
        if (llvm::verifyModule(*module_, &error_stream)) {
            throw CodegenError("module verification failed: " + error,
                current_loc_);
        }
        return *module_;
    }


    std::string Codegen::module_ir() const
{
        std::string ir;
        llvm::raw_string_ostream stream(ir);
        module_->print(stream, nullptr);
        return ir;
    }


    std::unique_ptr<llvm::LLVMContext> Codegen::take_context()
{ return std::move(context_); }


    std::unique_ptr<llvm::Module> Codegen::take_module()
{ return std::move(module_); }


    [[nodiscard]] const std::vector<std::string>& Codegen::current_lookup_namespace_path() const
{
        if (!current_global_namespace_path_.empty()) return current_global_namespace_path_;
        if (current_function_def_ != nullptr) return current_function_def_->namespace_path;
        static const std::vector<std::string> empty;
        return empty;
    }


    [[nodiscard]] const Codegen::GlobalSlot* Codegen::find_visible_global_slot(const std::string& name,
                                                                      bool explicit_global_qualification) const
{
        if (program_ == nullptr) return nullptr;
        const GlobalVar* global =
            find_visible_global(program_, current_lookup_namespace_path(), name, explicit_global_qualification);
        if (global == nullptr || global->decl == nullptr) return nullptr;
        auto it = globals_.find(global->decl->var_name);
        return it == globals_.end() ? nullptr : &it->second;
    }


    [[nodiscard]] std::string Codegen::mangle_global_symbol_name(const std::string& name) const
{
        std::string result = "__scpp_global.";
        for (char ch : name) result += ch == ':' ? '.' : ch;
        return result;
    }


    [[nodiscard]] std::string Codegen::mangle_type(const Type& type)
{
        switch (type.kind) {
            case TypeKind::Named:
                if (type.template_args.empty()) return type.name;
                {
                    std::string result = type.name;
                    for (const Type& arg : type.template_args) result += "_" + mangle_type(arg);
                    return result;
                }
            case TypeKind::Pointer: return mangle_type(*type.pointee) + (type.is_mutable_pointee ? "_ptr" : "_cptr");
            case TypeKind::Function: {
                std::string result = mangle_type(*type.function_return) + "_fntype";
                for (const Type& param : type.function_params) result += "_" + mangle_type(param);
                if (type.is_const_function) result += "_const";
                if (type.function_ref_qualifier == ReceiverRefQualifier::LValue) result += "_lrefq";
                if (type.function_ref_qualifier == ReceiverRefQualifier::RValue) result += "_rrefq";
                return result;
            }
            case TypeKind::FunctionPointer: {
                std::string result = mangle_type(*type.function_return) +
                                     (type.is_unsafe_function_pointer ? "_ufnptr" : "_fnptr");
                for (const Type& param : type.function_params) result += "_" + mangle_type(param);
                return result;
            }
            case TypeKind::Reference:
                return mangle_type(*type.pointee) +
                       (type.is_rvalue_ref ? "_rref" : (type.is_mutable_ref ? "_ref" : "_cref"));
            case TypeKind::Span: return mangle_type(*type.pointee) + (type.is_mutable_ref ? "_span" : "_cspan");
            case TypeKind::Array: return mangle_type(*type.element) + "_arr" + std::to_string(type.array_size);
        }
        return "?";
    }


    [[nodiscard]] std::string Codegen::method_lookup_name(const Function& fn)
{
        if (fn.name.ends_with("_delete")) return "~";
        if (fn.name.ends_with("_operator_deref")) return "operator*";
        if (fn.name.ends_with("_operator_arrow")) return "operator->";
        if (fn.name.ends_with("_operator_assign")) return "operator=";
        if (!fn.member_owner_class.empty() && fn.name.rfind(fn.member_owner_class + "_", 0) == 0) {
            return fn.name.substr(fn.member_owner_class.size() + 1);
        }
        return fn.name;
    }


    [[nodiscard]] std::string Codegen::interface_method_slot_key(const Function& fn)
{
        std::string key = method_lookup_name(fn);
        key += "(";
        std::size_t start = fn.member_owner_class.empty() ? 0 : 1;
        for (std::size_t i = start; i < fn.params.size(); i++) {
            if (i != start) key += ",";
            key += mangle_type(fn.params[i].type);
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


    [[nodiscard]] Type Codegen::function_pointer_type_from_signature(const Type& return_type,
                                                                   const std::vector<Type>& param_types,
                                                                   bool is_unsafe)
{
        Type type;
        type.kind = TypeKind::FunctionPointer;
        type.function_return = std::make_shared<Type>(return_type);
        type.function_params = param_types;
        type.is_unsafe_function_pointer = is_unsafe;
        return type;
    }


    [[nodiscard]] bool Codegen::same_function_pointer_shape_ignoring_unsafe(const Type& a, const Type& b)
{
        if (a.kind != TypeKind::FunctionPointer || b.kind != TypeKind::FunctionPointer ||
            !types_equal(*a.function_return, *b.function_return) || a.function_params.size() != b.function_params.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.function_params.size(); i++) {
            if (!types_equal(a.function_params[i], b.function_params[i])) return false;
        }
        return true;
    }


    [[nodiscard]] std::optional<Type> Codegen::resolve_function_designator_type(const Expr& expr,
                                                                       const std::optional<Type>& target_type)
{
        const Expr* source = &expr;
        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::AddressOf && expr.lhs) source = expr.lhs.get();
        if (source->kind != ExprKind::Identifier ||
            (!source->explicit_global_qualification && locals_.contains(source->name))) {
            return std::nullopt;
        }
        std::optional<Type> result;
        for (const Function& fn : program_->functions) {
            if (fn.name != source->name) continue;
            Type candidate =
                function_pointer_type_from_signature(fn.return_type, [&]() {
                    std::vector<Type> params;
                    params.reserve(fn.params.size());
                    for (const Param& param : fn.params) params.push_back(param.type);
                    return params;
                }(),
                    fn.is_unsafe || (fn.is_extern_c && fn.body == nullptr));
            if (target_type.has_value()) {
                if (same_function_pointer_shape_ignoring_unsafe(candidate, *target_type)) return candidate;
                continue;
            }
            if (result.has_value()) return std::nullopt;
            result = std::move(candidate);
        }
        return result;
    }


    [[nodiscard]] llvm::Value* Codegen::codegen_function_pointer_value_for_target(const Expr& expr, const Type& target_type)
{
        std::optional<Type> source_type = resolve_function_designator_type(expr, target_type);
        if (!source_type.has_value()) return nullptr;
        const Expr* source = &expr;
        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::AddressOf && expr.lhs) source = expr.lhs.get();
        for (const Function& fn : program_->functions) {
            if (fn.name != source->name) continue;
            Type candidate = function_pointer_type_from_signature(fn.return_type, [&]() {
                std::vector<Type> params;
                params.reserve(fn.params.size());
                for (const Param& param : fn.params) params.push_back(param.type);
                return params;
            }(), fn.is_unsafe || (fn.is_extern_c && fn.body == nullptr));
            if (!same_function_pointer_shape_ignoring_unsafe(candidate, target_type)) continue;
            auto name_it = overload_names_.find(&fn);
            if (name_it == overload_names_.end()) continue;
            llvm::Function* callee = module_->getFunction(name_it->second);
            if (callee == nullptr) continue;
            return callee;
        }
        return nullptr;
    }


    [[nodiscard]] std::string Codegen::verbatim_type_spelling(const Type& type)
{
        switch (type.kind) {
            case TypeKind::Named:
                if (type.template_args.empty()) return type.name;
                {
                    std::string result = type.name + "<";
                    for (std::size_t i = 0; i < type.template_args.size(); i++) {
                        if (i > 0) result += ", ";
                        result += verbatim_type_spelling(type.template_args[i]);
                    }
                    result += ">";
                    return result;
                }
            case TypeKind::Pointer:
                return (type.is_mutable_pointee ? std::string() : std::string("const ")) +
                       verbatim_type_spelling(*type.pointee) + "*";
            case TypeKind::Function: {
                std::string result = verbatim_type_spelling(*type.function_return) + "(";
                for (std::size_t i = 0; i < type.function_params.size(); i++) {
                    if (i > 0) result += ", ";
                    result += verbatim_type_spelling(type.function_params[i]);
                }
                result += ")";
                if (type.is_const_function) result += " const";
                if (type.function_ref_qualifier == ReceiverRefQualifier::LValue) result += " &";
                if (type.function_ref_qualifier == ReceiverRefQualifier::RValue) result += " &&";
                return result;
            }
            case TypeKind::FunctionPointer: {
                std::string result = verbatim_type_spelling(*type.function_return) + " (*";
                if (type.is_unsafe_function_pointer) result += " [[scpp::unsafe]]";
                result += ")(";
                for (std::size_t i = 0; i < type.function_params.size(); i++) {
                    if (i > 0) result += ", ";
                    result += verbatim_type_spelling(type.function_params[i]);
                }
                result += ")";
                return result;
            }
            case TypeKind::Reference:
                if (type.is_rvalue_ref) return verbatim_type_spelling(*type.pointee) + "&&";
                return (type.is_mutable_ref ? std::string() : std::string("const ")) +
                       verbatim_type_spelling(*type.pointee) + "&";
            case TypeKind::Span:
                return "std::span<" + (type.is_mutable_ref ? std::string() : std::string("const ")) +
                       verbatim_type_spelling(*type.pointee) + ">";
            case TypeKind::Array:
                return verbatim_type_spelling(*type.element) + "[" + std::to_string(type.array_size) + "]";
        }
        return "?";
    }


    [[nodiscard]] std::vector<std::string> Codegen::split_dotted(const std::string& dotted)
{
        std::vector<std::string> segments;
        std::size_t start = 0;
        while (start <= dotted.size()) {
            std::size_t dot = dotted.find('.', start);
            if (dot == std::string::npos) {
                segments.push_back(dotted.substr(start));
                break;
            }
            segments.push_back(dotted.substr(start, dot - start));
            start = dot + 1;
        }
        return segments;
    }


    [[nodiscard]] std::string Codegen::mangle_exported_symbol(const Function& fn) const
{
        const std::string& effective_module = fn.owning_module.empty() ? program_->module_name : fn.owning_module;
        std::string mangled = "_scppM" + std::to_string(effective_module.size()) + "_" + effective_module;
        // Namespace nesting *beyond* the module's own required prefix
        // (ch11 §11.5 already requires every *exported* symbol's
        // namespace to start with the module's dotted name -- module
        // names use '.', namespace paths use '::', translated
        // segment-for-segment -- so that shared prefix doesn't need
        // re-encoding here). A *non-exported* declaration carries no
        // such guarantee at all (ch11 §11.6: "may live in any namespace
        // (or none)"), so its namespace_path can be shorter than, the
        // same length as, or a completely different set of segments
        // than the module's own name -- only the segments that actually
        // match the module name one-for-one from the start are the
        // module's own prefix; comparing by *length* alone (as opposed
        // to content) would silently skip -- and drop from the mangled
        // name entirely -- an unrelated same-length-or-shorter namespace
        // (e.g. a private `namespace bar { ... }` function inside module
        // `foo`), colliding with any other declaration of the same bare
        // name that also happens to drop the same number of segments
        // (e.g. a top-level one, dropping zero).
        std::vector<std::string> module_segments = split_dotted(effective_module);
        std::size_t shared_prefix = 0;
        while (shared_prefix < module_segments.size() && shared_prefix < fn.namespace_path.size() &&
               module_segments[shared_prefix] == fn.namespace_path[shared_prefix]) {
            shared_prefix++;
        }
        for (std::size_t i = shared_prefix; i < fn.namespace_path.size(); i++) {
            const std::string& segment = fn.namespace_path[i];
            mangled += "N" + std::to_string(segment.size()) + "_" + segment;
        }
        // fn.name is already fully namespace-qualified (e.g.
        // "std::string_new", see the parser's qualify_name) -- the
        // mangled symbol's own <function name bytes> segment is just the
        // last "::"-separated piece (namespace nesting is separately
        // encoded by the N blocks above).
        std::string bare_name = fn.name;
        std::size_t last_separator = bare_name.rfind("::");
        if (last_separator != std::string::npos) bare_name = bare_name.substr(last_separator + 2);
        mangled += "F" + std::to_string(bare_name.size()) + "_" + bare_name;
        mangled += "Q" + std::to_string(static_cast<int>(fn.receiver_ref_qualifier)) + "_";
        mangled += "P" + std::to_string(fn.params.size()) + "_";
        for (const Param& param : fn.params) {
            std::string spelling = verbatim_type_spelling(param.type);
            mangled += std::to_string(spelling.size()) + "_" + spelling;
        }
        return mangled;
    }


    void Codegen::build_overload_names()
{
        std::unordered_map<std::string, std::vector<const Function*>> by_name;
        for (const Function& fn : program_->functions) {
            by_name[fn.name].push_back(&fn);
        }
        for (const auto& [name, fns] : by_name) {
            if (!fns.empty() && fns[0]->is_extern_c) {
                if (fns.size() != 1) {
                    throw CodegenError("'" + name +
                                        "' cannot be overloaded: 'extern \"C\"' functions share real C's own "
                                        "lack of a function-overloading concept, so every 'extern \"C\"' "
                                        "declaration of the same name must have an identical signature",
                        current_loc_);
                }
                overload_names_[fns[0]] = name;
                continue;
            }
            bool recovered_from_elsewhere = !fns[0]->owning_module.empty();
            bool defined_in_this_module = !program_->module_name.empty();
            if (recovered_from_elsewhere || defined_in_this_module) {
                for (const Function* fn : fns) {
                    overload_names_[fn] = mangle_exported_symbol(*fn);
                }
                continue;
            }
            if (fns.size() == 1) {
                overload_names_[fns[0]] = name;
                continue;
            }
            // `extern "C"` functions can never be overloaded: C itself
            // has no such concept, and mangling one's symbol name here
            // would silently break its link against the *real* C symbol
            // of the same plain name (e.g. libc's own `puts`) that this
            // declaration exists to describe -- unlike an ordinary scpp
            // function, there is no "give it a different LLVM name" fix
            // available at all, so this must be a hard error instead.
            for (const Function* fn : fns) {
                if (fn->is_extern_c) {
                    throw CodegenError("'" + name +
                                        "' cannot be overloaded: 'extern \"C\"' functions share real C's own "
                                        "lack of a function-overloading concept, so every 'extern \"C\"' "
                                        "declaration of the same name must have an identical signature",
                        current_loc_);
                }
            }
            for (const Function* fn : fns) {
                std::string mangled = name;
                if (fn->receiver_ref_qualifier == ReceiverRefQualifier::LValue) mangled += ".lrefq";
                if (fn->receiver_ref_qualifier == ReceiverRefQualifier::RValue) mangled += ".rrefq";
                for (const Param& param : fn->params) {
                    mangled += "." + mangle_type(param.type);
                }
                overload_names_[fn] = mangled;
            }
        }
    }


    void Codegen::define_global_initializers(const Program& program)
{
        bool needs_initializer = false;
        for (const GlobalVar& global : program.globals) {
            if (global.decl != nullptr && (global.decl->init != nullptr || global.decl->has_ctor_args)) {
                needs_initializer = true;
                break;
            }
        }
        if (!needs_initializer) return;

        auto* fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), false);
        auto* init_fn = llvm::Function::Create(fn_type, llvm::GlobalValue::InternalLinkage, "__scpp_global_init", module_.get());
        auto* entry = llvm::BasicBlock::Create(*context_, "entry", init_fn);
        builder_->SetInsertPoint(entry);
        current_function_def_ = nullptr;
        current_global_namespace_path_.clear();

        for (const GlobalVar& global : program.globals) {
            if (global.decl == nullptr) continue;
            auto it = globals_.find(global.decl->var_name);
            if (it == globals_.end()) continue;
            current_global_namespace_path_ = global.namespace_path;
            if (global.decl->has_ctor_args) {
                throw CodegenError("global constructor-call initialization is not supported in this version", global.decl->loc);
            }
            if (global.decl->init == nullptr) continue;
            if (global.decl->type.kind == TypeKind::Reference) {
                llvm::Value* referent = codegen_lvalue(*global.decl->init).ptr;
                create_store(referent, it->second.global, std::nullopt);
                continue;
            }
            create_store(codegen_value_for_target(*global.decl->init, global.decl->type), it->second.global,
                         global.decl->resolved_alignment != 0 ? std::optional<llvm::Align>(llvm::Align(global.decl->resolved_alignment))
                                                              : alignment_for_type(global.decl->type));
        }

        builder_->CreateRetVoid();
        current_global_namespace_path_.clear();

        auto* ctor_ty = llvm::StructType::get(llvm::Type::getInt32Ty(*context_), llvm::PointerType::getUnqual(*context_),
                                              llvm::PointerType::getUnqual(*context_));
        auto* ctor_entry = llvm::ConstantStruct::get(
            ctor_ty, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 65535),
            llvm::ConstantExpr::getBitCast(init_fn, llvm::PointerType::getUnqual(*context_)),
            llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)));
        auto* array_ty = llvm::ArrayType::get(ctor_ty, 1);
        new llvm::GlobalVariable(*module_, array_ty, /*isConstant=*/true, llvm::GlobalValue::AppendingLinkage,
                                 llvm::ConstantArray::get(array_ty, {ctor_entry}), "llvm.global_ctors");
    }

} // namespace scpp
