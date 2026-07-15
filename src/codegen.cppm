module;

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

export module scpp.codegen;

import scpp.ast;
import scpp.constexpr_engine;

export namespace scpp {

struct CodegenError : std::runtime_error {
    explicit CodegenError(const std::string& message, SourceLocation loc = {})
        : std::runtime_error(message), loc(loc) {}
    SourceLocation loc;
};

[[nodiscard]] bool is_scalar_type_name(const std::string& name) {
    static const std::unordered_set<std::string> scalar_names = {
        "bool", "char", "int", "long", "unsigned int", "unsigned long", "int8_t", "int16_t", "int32_t",
        "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "float", "double", "float32_t", "float64_t",
        "size_t", "ptrdiff_t"};
    return scalar_names.contains(name);
}

[[nodiscard]] const EnumDef* find_enum_def(const Program* program, const std::string& name) {
    if (program == nullptr) return nullptr;
    for (const EnumDef& def : program->enums) {
        if (def.name == name) return &def;
    }
    return nullptr;
}

[[nodiscard]] const EnumVariant* find_enum_variant(const Program* program, const std::string& name,
                                                   const EnumDef** owning_enum = nullptr) {
    if (program == nullptr) return nullptr;
    for (const EnumDef& def : program->enums) {
        for (const EnumVariant& variant : def.variants) {
            if (variant.name == name) {
                if (owning_enum != nullptr) *owning_enum = &def;
                return &variant;
            }
        }
    }
    return nullptr;
}

// Lowers the M1/M2 AST subset (scalars + locals + control flow + functions +
// trivial structs, no borrow/move checks yet) directly to LLVM IR.
class Codegen {
public:
    explicit Codegen(const std::string& module_name, std::string source_path = {}, bool emit_debug_info = false)
        : context_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>(module_name, *context_)),
          builder_(std::make_unique<llvm::IRBuilder<>>(*context_)),
          source_path_(std::move(source_path)),
          emit_debug_info_(emit_debug_info) {}

    // Sets the target triple and data layout on the module. Must be called
    // (if at all) before generate(), since generate() may need
    // target-accurate type sizes (e.g. for std::make_unique's malloc
    // call). Optional: without it, the module keeps LLVM's default,
    // non-target-specific data layout, which is fine for callers (like
    // codegen_test) that only inspect the generated IR text and don't need
    // bit-perfect target sizing.
    void set_target(const llvm::Triple& triple, const llvm::DataLayout& data_layout) {
        module_->setTargetTriple(triple);
        module_->setDataLayout(data_layout);
    }

    llvm::Module& generate(const Program& program) {
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

    // Renders the generated module as LLVM IR text. Exposed so callers (and
    // tests) don't need to include LLVM headers directly.
    std::string module_ir() const {
        std::string ir;
        llvm::raw_string_ostream stream(ir);
        module_->print(stream, nullptr);
        return ir;
    }

    std::unique_ptr<llvm::LLVMContext> take_context() { return std::move(context_); }
    std::unique_ptr<llvm::Module> take_module() { return std::move(module_); }

private:
    struct StructInfo {
        llvm::StructType* llvm_type = nullptr;
        std::vector<std::string> field_names;
        std::vector<Type> field_types;
        bool is_union = false;
        bool is_packed = false;
        llvm::Align abi_align = llvm::Align(1);

        // ch05 §5.14: finds `name`'s own index in `field_names`, searching
        // from the *end* backwards -- needed since a derived class's
        // flattened (base-fields-first) layout may have a base's own
        // field name *shadowed* by a same-named field the derived level
        // itself declares (e.g. a variadic Tuple-style type's own
        // recursive-inheritance chain, where every level names its field
        // identically, "value"/"head" -- ch05 §5.14's own TupleImpl
        // example). Searching from the end always finds *this* level's
        // own field first (the last one appended, see declare_class),
        // matching real C++'s own "the most-derived declaration shadows
        // a base's same-named member" rule for unqualified `.field`
        // access. Harmless/no-op for the overwhelmingly common
        // non-shadowed case (a name appearing only once has the same
        // first-match and last-match index).
        [[nodiscard]] std::optional<size_t> find_field_index(const std::string& name) const {
            for (size_t i = field_names.size(); i > 0; i--) {
                if (field_names[i - 1] == name) return i - 1;
            }
            return std::nullopt;
        }
    };

    // A storage location: an LLVM pointer plus the scpp-level Type stored
    // there. Needed (rather than just an llvm::Value*) so Member/Subscript
    // chains can resolve field indices and element types as they walk down
    // (e.g. `p.inner.x` needs to know `p.inner`'s struct type to find `x`).
    struct LValue {
        llvm::Value* ptr;
        Type type;
        std::optional<llvm::Align> alignment;
    };

    // codegen_call's result: the raw LLVM call value, plus the resolved
    // callee's own AST-level Function (its return type is what codegen_
    // expr/codegen_lvalue's own Call cases need next, e.g. to decide
    // whether to auto-dereference a reference-returning result -- see
    // codegen_call's own comment for why a method call's receiver can
    // only ever be resolved once, so both must come from a single call).
    struct CallResult {
        llvm::Value* value;
        const Function* callee_def; // nullptr only if truly unknown (defensive; codegen_call already
                                     // required a matching LLVM function to exist)
    };

    [[nodiscard]] static bool is_enum_cast_store_builtin_name(const std::string& name) {
        return name == "scpp::__enum_cast_store" || name.rfind("scpp::__enum_cast_store.", 0) == 0;
    }

    struct LocalSlot {
        llvm::AllocaInst* alloca;
        Type type;
        bool is_const = false;
        // spec §6.4: non-null only for a class-typed local whose own
        // class has a destructor (see create_moved_flag_if_has_destructor)
        // -- an extra `i1` slot, initialized false at declaration, set
        // true by codegen_expr's Move case exactly when this local is
        // the source of a `std::move(...)`. Consulted only at scope-exit
        // (see codegen_call_destructor_unless_moved) so a moved-out
        // instance's destructor is correctly never invoked for it (spec
        // §6.3/§6.4) -- a class with no destructor at all needs no such
        // tracking, since there's nothing to conditionally skip; null
        // for every non-class-typed local for the same reason.
        llvm::AllocaInst* moved_flag = nullptr;
    };

    const Program* program_ = nullptr;
    // The AST-level Function currently being lowered by define_function,
    // consulted by StmtKind::Return to tell whether *this* function's own
    // return type is a reference (see codegen_stmt's Return case).
    const Function* current_function_def_ = nullptr;
    // `unsafe { }` nesting depth (ch01 §1.3), mirroring movecheck's own
    // DataflowState::unsafe_depth: 0 outside any unsafe context, > 0
    // directly or transitively inside one. Initialized to 0 per-function
    // in define_function (every function is checked by default, ch01 --
    // there's no per-function way to start already unsafe), then
    // incremented/decremented around an `unsafe { }` Block in
    // codegen_stmt. Consulted only by arithmetic codegen so far (ch05
    // §5.8: `+`/`-`/`*` are overflow-checked by default, plain
    // guaranteed-wrapping inside `unsafe`) -- every other check that
    // reads unsafe-ness (raw pointer deref, calling an `extern "C"`
    // function) is movecheck's job, not codegen's, since by the time a
    // program reaches codegen it has already been accepted (see
    // codegen_lvalue's Deref case).
    int unsafe_depth_ = 0;
    // Where the statement/expression currently being lowered begins in
    // the source (see SourceLocation, ast.cppm) -- refreshed as
    // codegen_stmt/codegen_expr/codegen_lvalue recurse, purely so a
    // thrown CodegenError can report a location; never consulted by any
    // actual codegen decision.
    SourceLocation current_loc_;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::unique_ptr<llvm::DIBuilder> dibuilder_;
    llvm::DICompileUnit* compile_unit_ = nullptr;
    llvm::DIFile* compile_unit_file_ = nullptr;
    llvm::DIScope* current_debug_scope_ = nullptr;
    llvm::DISubprogram* current_subprogram_ = nullptr;
    std::string source_path_;
    bool emit_debug_info_ = false;
    std::map<std::string, LocalSlot> locals_;
    std::unordered_map<std::string, StructInfo> structs_;
    std::unordered_set<std::string> declaring_aggregates_;
    // ch05 §5.10: each Function's actual LLVM symbol name -- the plain
    // `fn.name` unchanged for the overwhelmingly common case (exactly one
    // function under that name; critically, this is what keeps `main`/
    // `extern "C"` functions' names exactly as declared, since LLVM
    // requires every function in a module to have a unique name but
    // these are never intentionally duplicated), or, for 2+ functions
    // genuinely sharing a name (overloading), a name disambiguated with
    // each parameter's type (see mangle_type) -- built once up front by
    // build_overload_names, keyed by AST node identity (not by name:
    // that's exactly the one-to-many relationship this map exists to
    // resolve).
    std::unordered_map<const Function*, std::string> overload_names_;
    // A stack of block scopes, each holding the names declared directly in
    // that block (in declaration order). Pushed/popped around every Block,
    // and around the (possibly brace-less) branches of if/while, so a
    // unique_ptr local is dropped at the end of *its own* scope rather
    // than only at function return -- this is what makes e.g. a
    // unique_ptr re-declared on every loop iteration not leak the
    // previous iteration's allocation. Function parameters are not part
    // of any pushed scope; they live for the whole function and are only
    // freed at Return, same as before.
    std::vector<std::vector<std::string>> scope_stack_;
    struct LoopFrame {
        llvm::BasicBlock* cond_block;
        llvm::BasicBlock* end_block;
        size_t scope_depth;
    };
    std::vector<LoopFrame> loop_stack_;
    std::unordered_map<std::string, llvm::DIType*> debug_type_cache_;
    std::unordered_map<std::string, llvm::DIFile*> debug_file_cache_;
    llvm::StructType* interface_representation_llvm_type_ = nullptr;
    std::unordered_map<std::string, llvm::ArrayType*> interface_dispatch_table_types_;
    std::unordered_map<std::string, std::vector<const Function*>> interface_dispatch_methods_cache_;
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> interface_slot_indices_cache_;
    std::unordered_map<std::string, llvm::GlobalVariable*> interface_dispatch_tables_;
    std::unordered_map<std::string, llvm::Function*> interface_dispatch_thunks_;

    [[nodiscard]] std::string default_debug_source_path() const {
        return source_path_.empty() ? (std::filesystem::current_path() / "memory.scpp").string() : source_path_;
    }

    [[nodiscard]] llvm::DIFile* debug_file_for_path(const std::string& path) {
        if (!emit_debug_info_) return nullptr;
        auto it = debug_file_cache_.find(path);
        if (it != debug_file_cache_.end()) return it->second;
        std::filesystem::path source(path);
        llvm::DIFile* file = dibuilder_->createFile(source.filename().string(), source.parent_path().string());
        debug_file_cache_.emplace(path, file);
        return file;
    }

    [[nodiscard]] llvm::DIFile* debug_file_for_program() {
        if (!emit_debug_info_) return nullptr;
        if (compile_unit_file_ != nullptr) return compile_unit_file_;
        compile_unit_file_ = debug_file_for_path(default_debug_source_path());
        return compile_unit_file_;
    }

    [[nodiscard]] llvm::DIFile* debug_file_for_loc(const SourceLocation& loc) {
        return debug_file_for_path(loc.has_source_path() ? loc.source_path_text() : default_debug_source_path());
    }

    void initialize_debug_info() {
        if (!emit_debug_info_) return;
        dibuilder_ = std::make_unique<llvm::DIBuilder>(*module_);
        llvm::DIFile* file = debug_file_for_program();
        compile_unit_ = dibuilder_->createCompileUnit(llvm::dwarf::DW_LANG_C_plus_plus_17, file, "scpp", false, "", 0,
                                                      "", llvm::DICompileUnit::FullDebug);
        module_->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        module_->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
    }

    void finalize_debug_info() {
        if (!emit_debug_info_ || !dibuilder_) return;
        dibuilder_->finalize();
    }

    [[nodiscard]] llvm::DIType* debug_type_for(const Type& type) {
        if (!emit_debug_info_) return nullptr;
        std::string key = mangle_type(type);
        auto it = debug_type_cache_.find(key);
        if (it != debug_type_cache_.end()) return it->second;
        llvm::DIType* result = nullptr;
        switch (type.kind) {
            case TypeKind::Named: {
                auto basic = [&](llvm::dwarf::TypeKind encoding) -> llvm::DIType* {
                    return dibuilder_->createBasicType(type.name, module_->getDataLayout().getTypeSizeInBits(to_llvm_type(type)),
                                                       encoding);
                };
                if (type.name == "bool") result = basic(llvm::dwarf::DW_ATE_boolean);
                else if (type.name == "char") result = basic(llvm::dwarf::DW_ATE_signed_char);
                else if (is_float_scalar_type_name(type.name)) result = basic(llvm::dwarf::DW_ATE_float);
                else if (is_scalar_type_name(type.name)) {
                    result = basic(is_unsigned_scalar_type_name(type.name) ? llvm::dwarf::DW_ATE_unsigned
                                                                            : llvm::dwarf::DW_ATE_signed);
                } else if (const EnumDef* enum_def = find_enum_def(program_, type.name)) {
                    const std::string& underlying = enum_def->underlying_type.name;
                    result = basic(underlying == "char"                ? llvm::dwarf::DW_ATE_signed_char
                                   : is_unsigned_scalar_type_name(underlying) ? llvm::dwarf::DW_ATE_unsigned
                                                                               : llvm::dwarf::DW_ATE_signed);
                } else {
                    result = dibuilder_->createUnspecifiedType(type.name);
                }
                break;
            }
            case TypeKind::Pointer:
            case TypeKind::Reference: {
                if (is_interface_representation_type(type)) {
                    result = dibuilder_->createUnspecifiedType(type.kind == TypeKind::Pointer ? "interface_ptr"
                                                                                             : "interface_ref");
                    break;
                }
                llvm::DIType* pointee = type.pointee ? debug_type_for(*type.pointee) : nullptr;
                result = dibuilder_->createPointerType(
                    pointee, module_->getDataLayout().getPointerSizeInBits(),
                    module_->getDataLayout().getPointerABIAlignment(0).value() * 8);
                break;
            }
            case TypeKind::Array: {
                llvm::DIType* element = debug_type_for(*type.element);
                auto subscripts = dibuilder_->getOrCreateArray(
                    {dibuilder_->getOrCreateSubrange(0, type.array_size)});
                llvm::Type* llvm_type = to_llvm_type(type);
                result = dibuilder_->createArrayType(module_->getDataLayout().getTypeSizeInBits(llvm_type),
                                                     module_->getDataLayout().getABITypeAlign(llvm_type).value() * 8,
                                                     element, subscripts);
                break;
            }
            case TypeKind::Span:
                result = dibuilder_->createUnspecifiedType("std::span");
                break;
            case TypeKind::Function:
            case TypeKind::FunctionPointer: {
                std::vector<llvm::Metadata*> elems;
                elems.push_back(type.function_return ? debug_type_for(*type.function_return) : nullptr);
                for (const Type& param : type.function_params) elems.push_back(debug_type_for(param));
                llvm::DISubroutineType* subroutine =
                    dibuilder_->createSubroutineType(dibuilder_->getOrCreateTypeArray(elems));
                result = type.kind == TypeKind::FunctionPointer
                             ? dibuilder_->createPointerType(subroutine, module_->getDataLayout().getPointerSizeInBits(),
                                                            module_->getDataLayout().getPointerABIAlignment(0).value() * 8)
                             : static_cast<llvm::DIType*>(subroutine);
                break;
            }
        }
        debug_type_cache_[key] = result;
        return result;
    }

    void refresh_debug_location(SourceLocation loc) {
        current_loc_ = loc;
        if (!emit_debug_info_ || current_debug_scope_ == nullptr || !loc.is_known()) {
            builder_->SetCurrentDebugLocation(llvm::DebugLoc());
            return;
        }
        builder_->SetCurrentDebugLocation(llvm::DILocation::get(*context_, loc.line, std::max(loc.column, 1),
                                                                current_debug_scope_));
    }

    void maybe_emit_parameter_debug_decl(const Param& param, llvm::AllocaInst* slot, unsigned index) {
        if (!emit_debug_info_ || current_subprogram_ == nullptr) return;
        llvm::DIType* type = debug_type_for(param.type);
        if (type == nullptr) return;
        llvm::DILocalVariable* var =
            dibuilder_->createParameterVariable(current_subprogram_, param.name, index, debug_file_for_loc(current_function_def_->loc),
                                                std::max(current_function_def_->loc.line, 1), type, true);
        dibuilder_->insertDeclare(slot, var, dibuilder_->createExpression(),
                                  llvm::DILocation::get(*context_, std::max(current_function_def_->loc.line, 1), 1,
                                                        current_subprogram_),
                                  builder_->GetInsertBlock());
    }

    void maybe_emit_local_debug_decl(const std::string& name, const Type& type, llvm::AllocaInst* slot, SourceLocation loc) {
        if (!emit_debug_info_ || current_debug_scope_ == nullptr) return;
        llvm::DIType* debug_type = debug_type_for(type);
        if (debug_type == nullptr) return;
        llvm::DILocalVariable* var =
            dibuilder_->createAutoVariable(current_debug_scope_, name, debug_file_for_loc(loc), std::max(loc.line, 1),
                                           debug_type, true);
        dibuilder_->insertDeclare(slot, var, dibuilder_->createExpression(),
                                  llvm::DILocation::get(*context_, std::max(loc.line, 1), std::max(loc.column, 1),
                                                        current_debug_scope_),
                                  builder_->GetInsertBlock());
    }

    // Hoists named local-variable storage to the function entry block so
    // LLVM can describe it with one stable frame-base location even when
    // the declaration itself lives in a nested scope whose initializer
    // emits its own control flow (e.g. checked arithmetic overflow
    // diamonds). Preserves declaration order among existing entry-block
    // allocas for tests/IR readability.
    llvm::AllocaInst* create_entry_block_alloca(llvm::Type* type, const std::string& name) {
        llvm::BasicBlock* current_block = builder_->GetInsertBlock();
        if (current_block == nullptr) return builder_->CreateAlloca(type, nullptr, name);
        llvm::IRBuilderBase::InsertPoint saved_ip = builder_->saveIP();
        llvm::DebugLoc saved_dbg = builder_->getCurrentDebugLocation();
        llvm::BasicBlock& entry = current_block->getParent()->getEntryBlock();
        llvm::BasicBlock::iterator insert_it = entry.getFirstInsertionPt();
        while (insert_it != entry.end() && llvm::isa<llvm::AllocaInst>(*insert_it)) ++insert_it;
        builder_->SetInsertPoint(&entry, insert_it);
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        llvm::AllocaInst* slot = builder_->CreateAlloca(type, nullptr, name);
        builder_->restoreIP(saved_ip);
        builder_->SetCurrentDebugLocation(saved_dbg);
        return slot;
    }

    void attach_debug_subprogram(llvm::Function* llvm_fn, const Function& fn) {
        if (!emit_debug_info_) return;
        std::vector<llvm::Metadata*> type_elems;
        type_elems.push_back(debug_type_for(fn.return_type));
        for (const Param& param : fn.params) type_elems.push_back(debug_type_for(param.type));
        llvm::DISubroutineType* fn_type =
            dibuilder_->createSubroutineType(dibuilder_->getOrCreateTypeArray(type_elems));
        llvm::DISubprogram* subprogram = dibuilder_->createFunction(
            debug_file_for_loc(fn.loc), fn.name, llvm_fn->getName(), debug_file_for_loc(fn.loc),
            std::max(fn.loc.line, 1), fn_type, std::max(fn.loc.line, 1),
            llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
        llvm_fn->setSubprogram(subprogram);
        current_subprogram_ = subprogram;
        current_debug_scope_ = subprogram;
    }

    const StructDef* find_struct_def(const std::string& name) const {
        for (const StructDef& def : program_->structs) {
            if (def.name == name) return &def;
        }
        return nullptr;
    }

    const ClassDef* find_class_def(const std::string& name) const {
        for (const ClassDef& def : program_->classes) {
            if (def.name == name) return &def;
        }
        return nullptr;
    }

    const Function* find_function_def(const std::string& name) const {
        for (const Function& fn : program_->functions) {
            if (fn.name == name) return &fn;
        }
        return nullptr;
    }

    [[nodiscard]] bool type_names_interface(const std::string& name) const {
        const ClassDef* def = find_class_def(name);
        return def != nullptr && def->is_interface;
    }

    [[nodiscard]] bool is_interface_named_type(const Type& type) const {
        return type.kind == TypeKind::Named && type_names_interface(type.name);
    }

    [[nodiscard]] bool is_interface_pointer_type(const Type& type) const {
        return type.kind == TypeKind::Pointer && type.pointee != nullptr && is_interface_named_type(*type.pointee);
    }

    [[nodiscard]] bool is_interface_reference_type(const Type& type) const {
        return type.kind == TypeKind::Reference && type.pointee != nullptr && is_interface_named_type(*type.pointee);
    }

    [[nodiscard]] bool is_interface_representation_type(const Type& type) const {
        return is_interface_pointer_type(type) || is_interface_reference_type(type);
    }

    [[nodiscard]] std::string current_enclosing_class_name() const {
        return current_function_def_ == nullptr ? std::string() : current_function_def_->member_owner_class;
    }

    [[nodiscard]] llvm::StructType* interface_representation_type() {
        if (interface_representation_llvm_type_ != nullptr) return interface_representation_llvm_type_;
        llvm::Type* ptr_type = llvm::PointerType::getUnqual(*context_);
        interface_representation_llvm_type_ =
            llvm::StructType::get(*context_, {ptr_type, ptr_type});
        return interface_representation_llvm_type_;
    }

    [[nodiscard]] llvm::Value* build_interface_value(llvm::Value* object_ptr, llvm::Value* dispatch_ptr) {
        llvm::Value* value = llvm::UndefValue::get(interface_representation_type());
        value = builder_->CreateInsertValue(value, object_ptr, {0}, "iface.obj");
        value = builder_->CreateInsertValue(value, dispatch_ptr, {1}, "iface.dispatch");
        return value;
    }

    [[nodiscard]] llvm::Value* extract_interface_object_ptr(llvm::Value* interface_value) {
        return builder_->CreateExtractValue(interface_value, {0}, "iface.obj");
    }

    [[nodiscard]] llvm::Value* extract_interface_dispatch_ptr(llvm::Value* interface_value) {
        return builder_->CreateExtractValue(interface_value, {1}, "iface.dispatch");
    }

    [[nodiscard]] bool has_accessible_interface_base_conversion(const std::string& source_name, const std::string& target_name,
                                                                std::string_view current_class) const {
        if (source_name == target_name) return true;
        const ClassDef* def = find_class_def(source_name);
        if (def == nullptr) return false;
        for (const BaseSpecifier& base : def->base_specifiers) {
            if (base.kind == BaseClassKind::Interface && base.access == AccessSpecifier::Private &&
                current_class != source_name) {
                continue;
            }
            if (base.base_type.name == target_name) return true;
            if (has_accessible_interface_base_conversion(base.base_type.name, target_name, current_class)) return true;
        }
        return false;
    }

    [[nodiscard]] bool types_compatible_with_interface_conversion(const Type& source_type, const Type& target_type,
                                                                  std::string_view current_class) const {
        if (types_equal(source_type, target_type)) return true;
        if (target_type.kind == TypeKind::Reference && source_type.kind == TypeKind::Reference &&
            !target_type.is_rvalue_ref && !source_type.is_rvalue_ref && target_type.pointee && source_type.pointee) {
            if (target_type.is_mutable_ref && !source_type.is_mutable_ref) return false;
            if (types_equal(*source_type.pointee, *target_type.pointee)) return true;
            return target_type.pointee->kind == TypeKind::Named && source_type.pointee->kind == TypeKind::Named &&
                   type_names_interface(target_type.pointee->name) &&
                   has_accessible_interface_base_conversion(source_type.pointee->name, target_type.pointee->name,
                                                            current_class);
        }
        if (target_type.kind == TypeKind::Reference && source_type.kind != TypeKind::Reference && target_type.pointee) {
            if (types_equal(source_type, *target_type.pointee)) return true;
            return target_type.pointee->kind == TypeKind::Named && source_type.kind == TypeKind::Named &&
                   type_names_interface(target_type.pointee->name) &&
                   has_accessible_interface_base_conversion(source_type.name, target_type.pointee->name, current_class);
        }
        if (target_type.kind == TypeKind::Pointer && source_type.kind == TypeKind::Pointer && target_type.pointee &&
            source_type.pointee) {
            if (target_type.is_mutable_pointee && !source_type.is_mutable_pointee) return false;
            if (types_equal(*source_type.pointee, *target_type.pointee)) return true;
            return target_type.pointee->kind == TypeKind::Named && source_type.pointee->kind == TypeKind::Named &&
                   type_names_interface(target_type.pointee->name) &&
                   has_accessible_interface_base_conversion(source_type.pointee->name, target_type.pointee->name,
                                                            current_class);
        }
        return false;
    }

    ExprPtr clone_expr(const Expr& expr) const {
        auto clone = std::make_unique<Expr>();
        clone->kind = expr.kind;
        clone->loc = expr.loc;
        clone->int_value = expr.int_value;
        clone->float_value = expr.float_value;
        clone->bool_value = expr.bool_value;
        clone->name = expr.name;
        clone->explicit_global_qualification = expr.explicit_global_qualification;
        clone->binary_op = expr.binary_op;
        clone->unary_op = expr.unary_op;
        clone->fold_ellipsis_on_left = expr.fold_ellipsis_on_left;
        if (expr.lhs) clone->lhs = clone_expr(*expr.lhs);
        if (expr.rhs) clone->rhs = clone_expr(*expr.rhs);
        if (expr.third) clone->third = clone_expr(*expr.third);
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
        return clone;
    }

    [[nodiscard]] const Function* resolve_converting_constructor_by_type(const std::string& class_name, const Expr& arg) {
        return find_single_argument_converting_constructor(class_name, arg);
    }

    void store_constexpr_value_into(llvm::Value* dest_ptr, const Type& dest_type, const ConstexprValue& value) {
        if (is_scalar_type_name(dest_type.name)) {
            if (dest_type.kind == TypeKind::Named && dest_type.name == "bool") {
                create_store(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), value.bool_value ? 1 : 0), dest_ptr,
                             std::nullopt);
                return;
            }
            if (dest_type.kind == TypeKind::Named && dest_type.name == "char") {
                create_store(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), value.int_value, false), dest_ptr,
                             std::nullopt);
                return;
            }
            if (dest_type.kind == TypeKind::Named && dest_type.name == "double") {
                create_store(llvm::ConstantFP::get(llvm::Type::getDoubleTy(*context_), value.double_value), dest_ptr,
                             std::nullopt);
                return;
            }
            create_store(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), value.int_value, true), dest_ptr,
                         std::nullopt);
            return;
        }
        if (dest_type.kind == TypeKind::Pointer && dest_type.pointee &&
            dest_type.pointee->kind == TypeKind::Named && dest_type.pointee->name == "char" && !dest_type.is_mutable_pointee &&
            value.kind == ConstexprValueKind::StringLiteralPointer) {
            create_store(builder_->CreateGlobalString(value.string_value, "cexprstr"), dest_ptr, std::nullopt);
            return;
        }
        if (dest_type.kind == TypeKind::Array && dest_type.element && value.kind == ConstexprValueKind::Array) {
            for (size_t i = 0; i < value.elements.size(); ++i) {
                llvm::Value* elem_ptr = builder_->CreateConstGEP2_32(to_llvm_type(dest_type), dest_ptr, 0,
                                                                     static_cast<unsigned>(i));
                store_constexpr_value_into(elem_ptr, *dest_type.element, value.elements[i]);
            }
            return;
        }
        if (dest_type.kind == TypeKind::Named && find_class_def(dest_type.name) != nullptr &&
            value.kind == ConstexprValueKind::Object) {
            const StructInfo& info = structs_.at(dest_type.name);
            for (size_t i = 0; i < info.field_names.size(); ++i) {
                auto it = std::find_if(value.object_fields.begin(), value.object_fields.end(),
                                       [&](const auto& field) { return field.first == info.field_names[i]; });
                if (it == value.object_fields.end()) continue;
                llvm::Value* field_ptr = builder_->CreateStructGEP(info.llvm_type, dest_ptr, i, info.field_names[i]);
                store_constexpr_value_into(field_ptr, info.field_types[i], *it->second);
            }
            return;
        }
        throw CodegenError("unsupported constexpr class materialization for type '" + dest_type.name + "'", current_loc_);
    }

    llvm::Value* codegen_consteval_class_value(const Expr& expr, const std::string& class_name) {
        ConstexprValue value = evaluate_immediate_expr(*program_, expr);
        llvm::Type* llvm_type = to_llvm_type(named_type(class_name));
        llvm::AllocaInst* temp = create_entry_block_alloca(llvm_type, "constevalclasstmp");
        zero_initialize_storage(temp, named_type(class_name));
        store_constexpr_value_into(temp, named_type(class_name), value);
        return builder_->CreateLoad(llvm_type, temp, "constevalclass.value");
    }

    llvm::Value* codegen_constructed_class_value(const std::string& class_name, const std::vector<ExprPtr>& args,
                                                 const Function* ctor_def, const Expr* original_expr = nullptr) {
        llvm::Type* llvm_type = to_llvm_type(named_type(class_name));
        llvm::AllocaInst* temp = create_entry_block_alloca(llvm_type, "classtmp");
        LValue target{temp, named_type(class_name), std::nullopt};
        zero_initialize_storage(target.ptr, target.type, target.alignment);
        if (try_initialize_class_storage_from_same_type_source(target, args)) {
            return builder_->CreateLoad(llvm_type, temp, "classtmp.value");
        }
        if (ctor_def != nullptr) {
            if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                ExprPtr ctor_expr;
                if (original_expr != nullptr) {
                    ctor_expr = clone_expr(*original_expr);
                } else {
                    ctor_expr = std::make_unique<Expr>();
                    ctor_expr->kind = ExprKind::Call;
                    ctor_expr->loc = current_loc_;
                    ctor_expr->name = class_name;
                    ctor_expr->has_paren_init = true;
                    for (const ExprPtr& arg : args) ctor_expr->args.push_back(clone_expr(*arg));
                }
                ConstexprValue value = evaluate_immediate_expr(*program_, *ctor_expr);
                store_constexpr_value_into(target.ptr, target.type, value);
            } else {
                llvm::Function* ctor = module_->getFunction(overload_names_.at(ctor_def));
                if (ctor == nullptr) {
                    throw CodegenError("class '" + class_name + "' has no constructor matching this call", current_loc_);
                }
                std::vector<llvm::Value*> ctor_args = codegen_call_args(args, ctor_def, /*param_offset=*/1);
                ctor_args.insert(ctor_args.begin(), target.ptr);
                builder_->CreateCall(ctor, ctor_args);
            }
        } else if (args.empty()) {
            const ClassDef* class_def = find_class_def(class_name);
            if (class_def != nullptr && !class_has_any_constructor(class_name)) {
                emit_default_initializers_for_class_storage(target.ptr, *class_def);
            }
        }
        return builder_->CreateLoad(llvm_type, temp, "classtmp.value");
    }

    // Infers `expr`'s scpp type, for function-overload resolution
    // purposes only (ch05 §5.10) -- mirrors movecheck's own
    // infer_expr_type (same overall shape and non-exhaustiveness: this
    // is not a general type-checker), but, unlike movecheck, this *does*
    // support Member/Subscript: codegen already has full struct/class
    // field-type info via structs_ (declare_struct/declare_class),
    // unlike movecheck's Body, which only ever tracked per-local types.
    // Never has side effects (never calls codegen_expr/codegen_lvalue),
    // so it's safe to call before actually generating any code for
    // `expr` -- needed since resolving which overload a call targets
    // must happen *before* generating its arguments (codegen_call_args
    // needs to already know the callee to decide value-vs-address per
    // parameter).
    [[nodiscard]] bool is_for_range_size_builtin(const Expr& expr) const {
        return expr.kind == ExprKind::Call && expr.lhs == nullptr && expr.name == "$for_range_size" && expr.args.size() == 1;
    }

    std::optional<Type> infer_type(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return named_type("int");
            case ExprKind::FloatLiteral: return named_type("double");
            case ExprKind::BoolLiteral: return named_type("bool");
            case ExprKind::Sizeof: return named_type("size_t");
            case ExprKind::TypeTrait: return named_type("bool");
            case ExprKind::CharLiteral: return named_type("char");
            case ExprKind::StringLiteral: {
                Type result;
                result.kind = TypeKind::Pointer;
                result.pointee = std::make_shared<Type>(named_type("char"));
                result.is_mutable_pointee = false;
                return result;
            }

            case ExprKind::Identifier: {
                auto it = expr.explicit_global_qualification ? locals_.end() : locals_.find(expr.name);
                if (it != locals_.end()) return it->second.type;
                if (const EnumDef* def = [&]() {
                        const EnumDef* enum_def = nullptr;
                        [[maybe_unused]] const EnumVariant* variant = find_enum_variant(program_, expr.name, &enum_def);
                        return enum_def;
                    }()) {
                    return named_type(def->name);
                }
                return resolve_function_designator_type(expr);
            }

            case ExprKind::Move: {
                if (expr.lhs->kind != ExprKind::Identifier) return std::nullopt;
                auto it = locals_.find(expr.lhs->name);
                return it == locals_.end() ? std::nullopt : std::optional<Type>(it->second.type);
            }

            case ExprKind::New: {
                Type result;
                result.kind = TypeKind::Pointer;
                result.pointee = std::make_shared<Type>(expr.type);
                result.is_mutable_pointee = true;
                return result;
            }

            case ExprKind::Delete:
            case ExprKind::Destroy:
                return named_type("void");

            case ExprKind::Lambda: {
                // ch05 §5.12: once resolved (movecheck's closure-
                // resolution pass), `expr.name` holds the synthesized
                // closure class's own name -- its type is exactly that
                // class, by value (matching MakeUnique's identical shape
                // just above: a fresh, concretely-typed value).
                if (expr.name.empty()) return std::nullopt;
                return named_type(expr.name);
            }

            case ExprKind::Member: {
                std::optional<Type> base = infer_type(*expr.lhs);
                if (!base) return std::nullopt;
                // See codegen_lvalue's Identifier case: a Reference-typed
                // base (e.g. `this`) auto-dereferences to its pointee.
                const Type& base_named = base->kind == TypeKind::Reference ? *base->pointee : *base;
                if (base_named.kind != TypeKind::Named) return std::nullopt;
                auto struct_it = structs_.find(base_named.name);
                if (struct_it == structs_.end()) return std::nullopt;
                const StructInfo& info = struct_it->second;
                std::optional<size_t> field_index = info.find_field_index(expr.name);
                if (!field_index.has_value()) return std::nullopt;
                const Type& field_type = info.field_types[*field_index];
                // ch05 §5.12: a Reference-typed field (e.g. a closure's
                // own by-reference capture) auto-dereferences to its
                // pointee too, exactly like codegen_lvalue's own
                // (matching) Member-case fix -- `this.b`'s *type* is the
                // referent's type, not "a reference to it".
                return field_type.kind == TypeKind::Reference ? *field_type.pointee : field_type;
            }

            case ExprKind::Subscript: {
                std::optional<Type> base = infer_type(*expr.lhs);
                if (!base) return std::nullopt;
                const Type& effective = base->kind == TypeKind::Reference && base->pointee ? *base->pointee : *base;
                if (effective.kind == TypeKind::Array) return *effective.element;
                if (effective.kind == TypeKind::Span) return *effective.pointee;
                if (effective.kind == TypeKind::Pointer) return *effective.pointee;
                return std::nullopt;
            }

            case ExprKind::Unary:
                switch (expr.unary_op) {
                    case UnaryOp::Not: return named_type("bool");
                    case UnaryOp::Neg: return infer_type(*expr.lhs);
                    case UnaryOp::AddressOf: {
                        if (std::optional<Type> fn_ptr = resolve_function_designator_type(expr)) return fn_ptr;
                        std::optional<Type> operand = infer_type(*expr.lhs);
                        if (!operand) return std::nullopt;
                        Type result;
                        result.kind = TypeKind::Pointer;
                        result.pointee = std::make_shared<Type>(std::move(*operand));
                        result.is_mutable_pointee = true; // &expr always yields a mutable T* (ch05 §5.7)
                        return result;
                    }
                    case UnaryOp::Deref: {
                        std::optional<Type> operand = infer_type(*expr.lhs);
                        if (!operand) return std::nullopt;
                        if (expr.lhs->kind == ExprKind::Identifier && expr.lhs->name == "this" &&
                            operand->kind == TypeKind::Reference && operand->pointee) {
                            return *operand->pointee;
                        }
                        if (operand->kind == TypeKind::FunctionPointer) return *operand;
                        const Type& underlying =
                            operand->kind == TypeKind::Reference && operand->pointee ? *operand->pointee : *operand;
                        if (underlying.kind == TypeKind::Named) {
                            std::vector<ExprPtr> no_args;
                            bool receiver_is_mutable = !(operand->kind == TypeKind::Reference && !operand->is_mutable_ref);
                            if (const Function* callee =
                                    resolve_overload_by_type(underlying.name + "_operator_deref", no_args, 1,
                                                         receiver_is_mutable, expr.lhs.get())) {
                                return callee->return_type.kind == TypeKind::Reference
                                           ? std::optional<Type>(*callee->return_type.pointee)
                                           : std::optional<Type>(callee->return_type);
                            }
                        }
                        if (operand->kind != TypeKind::Pointer) {
                            return std::nullopt;
                        }
                        return *operand->pointee;
                    }
                }
                return std::nullopt;

            // `static_cast<T>(expr)`/`(T)expr` (ch06 §6): the cast's own
            // declared target type, unconditionally -- that *is* the
            // whole point of an explicit cast (movecheck's own Cast
            // handling is what actually validates the source/target
            // pairing is legal in the first place).
            case ExprKind::Cast: return expr.type;

            case ExprKind::Binary:
                switch (expr.binary_op) {
                    case BinaryOp::Add:
                        if (std::optional<Type> lhs = infer_type(*expr.lhs), rhs = infer_type(*expr.rhs);
                            lhs.has_value() && rhs.has_value()) {
                            if (std::optional<Type> result = pointer_arithmetic_result_type(expr.binary_op, *lhs, *rhs)) {
                                return result;
                            }
                        }
                        [[fallthrough]];
                    case BinaryOp::Sub:
                        if (expr.binary_op == BinaryOp::Sub) {
                            if (std::optional<Type> lhs = infer_type(*expr.lhs), rhs = infer_type(*expr.rhs);
                                lhs.has_value() && rhs.has_value()) {
                                if (std::optional<Type> result = pointer_arithmetic_result_type(expr.binary_op, *lhs, *rhs)) {
                                    return result;
                                }
                            }
                        }
                        [[fallthrough]];
                    case BinaryOp::Mul:
                    case BinaryOp::Div:
                    case BinaryOp::Assign:
                        return infer_type(*expr.lhs);
                    case BinaryOp::Eq:
                    case BinaryOp::Ne:
                    case BinaryOp::Lt:
                    case BinaryOp::Gt:
                    case BinaryOp::Le:
                    case BinaryOp::Ge:
                    case BinaryOp::And:
                    case BinaryOp::Or:
                        return named_type("bool");
                }
                return std::nullopt;

            case ExprKind::Conditional: {
                std::optional<Type> then_type = infer_type(*expr.rhs);
                std::optional<Type> else_type = infer_type(*expr.third);
                if (!then_type.has_value() || !else_type.has_value()) return std::nullopt;
                return types_equal(*then_type, *else_type) ? then_type : std::nullopt;
            }

            case ExprKind::Fold:
            case ExprKind::PackExpansion:
                // Fold expressions are expanded away during generic-call
                // monomorphization; no concrete codegen path should ever
                // see one. Same for a raw `args...` pack expansion.
                return std::nullopt;

            case ExprKind::Call: {
                if (is_for_range_size_builtin(expr)) return named_type("int");
                if (expr.lhs == nullptr) {
                    if (structs_.contains(expr.name)) return named_type(expr.name);
                }
                if (expr.lhs != nullptr && expr.name.empty()) {
                    const Expr* callee_expr = expr.lhs.get();
                    if (callee_expr->kind == ExprKind::Unary && callee_expr->unary_op == UnaryOp::Deref &&
                        callee_expr->lhs != nullptr) {
                        callee_expr = callee_expr->lhs.get();
                    }
                    std::optional<Type> callee_type = infer_type(*callee_expr);
                    if (callee_type.has_value() && callee_type->kind == TypeKind::FunctionPointer) {
                        return *callee_type->function_return;
                    }
                    return std::nullopt;
                }
                if (expr.lhs == nullptr && !expr.explicit_global_qualification && locals_.contains(expr.name) &&
                    locals_.at(expr.name).type.kind == TypeKind::FunctionPointer) {
                    return *locals_.at(expr.name).type.function_return;
                }
                std::string callee_name = expr.name;
                size_t param_offset = 0;
                bool receiver_is_mutable = true;
                if (expr.lhs != nullptr) {
                    std::optional<Type> receiver = infer_type(*expr.lhs);
                    if (!receiver) return std::nullopt;
                    const Type& receiver_named =
                        receiver->kind == TypeKind::Reference ? *receiver->pointee : *receiver;
                    if (receiver_named.kind != TypeKind::Named) return std::nullopt;
                    callee_name = receiver_named.name + "_" + expr.name;
                    param_offset = 1;
                    receiver_is_mutable = !is_read_only_place(*expr.lhs);
                }
                const Function* callee =
                    resolve_overload_by_type(callee_name, expr.args, param_offset, receiver_is_mutable, expr.lhs.get());
                return callee == nullptr ? std::nullopt : std::optional<Type>(callee->return_type);
            }
        }
        return std::nullopt;
    }

    // Whether `arg` produces a genuine rvalue of exactly `expected_type`
    // -- mirrors movecheck's own produces_rvalue_of_type (ch03/ch05
    // §5.11), used only for the `T&&`/`Concept auto&&` branch of
    // argument_matches_parameter just below (a monomorphized `Concept
    // auto&&` call site is, by the time codegen sees it, an ordinary
    // `T&&` parameter of a concrete type -- see the concept-
    // monomorphization pass -- so this needs no concept-specific logic
    // of its own).
    bool produces_rvalue_of_type(const Expr& arg, const Type& expected_type) {
        switch (arg.kind) {
            case ExprKind::Move:
            case ExprKind::New:
            case ExprKind::IntegerLiteral:
            case ExprKind::FloatLiteral:
            case ExprKind::BoolLiteral:
            case ExprKind::CharLiteral:
            case ExprKind::StringLiteral:
            case ExprKind::Sizeof:
            case ExprKind::Lambda:
                break;
            case ExprKind::Call: {
                std::optional<Type> t = infer_type(arg);
                if (!t.has_value() || t->kind == TypeKind::Reference) return false;
                break;
            }
            default:
                return false;
        }
        std::optional<Type> arg_type = infer_type(arg);
        if (!arg_type.has_value()) return false;
        if (types_equal(*arg_type, expected_type)) return true;
        if (arg.kind == ExprKind::Move && arg_type->kind == TypeKind::Reference && arg_type->pointee != nullptr) {
            return types_equal(*arg_type->pointee, expected_type);
        }
        return false;
    }

    bool const_reference_binds_materialized_temporary(const Expr& arg, const Type& param_type) {
        if (param_type.kind != TypeKind::Reference || param_type.is_rvalue_ref || param_type.is_mutable_ref ||
            param_type.pointee == nullptr) {
            return false;
        }
        if (produces_rvalue_of_type(arg, *param_type.pointee)) return true;
        return param_type.pointee->kind == TypeKind::Named && find_class_def(param_type.pointee->name) != nullptr &&
               find_single_argument_converting_constructor(param_type.pointee->name, arg) != nullptr;
    }

    [[nodiscard]] TargetLayoutInfo current_target_layout_info() const {
        return TargetLayoutInfo{module_->getDataLayout().getPointerSize(),
                                module_->getDataLayout().getPointerABIAlignment(0).value()};
    }

    llvm::Value* codegen_sizeof_value(const Expr& expr) {
        Type queried_type;
        if (expr.sizeof_operand_is_type) {
            queried_type = expr.type;
        } else {
            std::optional<Type> inferred = infer_type(*expr.lhs);
            if (!inferred.has_value()) {
                throw CodegenError("cannot apply 'sizeof' to this expression: its type could not be inferred", current_loc_);
            }
            queried_type = *inferred;
        }
        if (program_ == nullptr) throw CodegenError("internal error: sizeof requires program type information", current_loc_);
        std::optional<TypeLayoutInfo> layout = layout_of_type(*program_, queried_type, current_target_layout_info());
        if (!layout.has_value()) {
            throw CodegenError("cannot apply 'sizeof' to this type in this version", current_loc_);
        }
        return llvm::ConstantInt::get(to_llvm_type(named_type("size_t")), layout->size_bytes, /*isSigned=*/false);
    }

    [[nodiscard]] bool is_lvalue_copy_source_shape(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::Identifier:
                return true;
            case ExprKind::Member:
            case ExprKind::Subscript:
                return expr.lhs != nullptr && is_lvalue_copy_source_shape(*expr.lhs);
            default:
                return false;
        }
    }

    [[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type) {
        if (!is_lvalue_copy_source_shape(expr)) return false;
        std::optional<Type> expr_type = infer_type(expr);
        if (!expr_type.has_value()) return false;
        if (types_equal(*expr_type, target_type)) return true;
        return expr_type->kind == TypeKind::Reference && !expr_type->is_rvalue_ref &&
               expr_type->pointee != nullptr && types_equal(*expr_type->pointee, target_type);
    }

    [[nodiscard]] bool is_implicit_move_return_source(const Expr& expr, const Type& target_type) {
        if (expr.kind != ExprKind::Identifier || expr.explicit_global_qualification) return false;
        auto it = locals_.find(expr.name);
        return it != locals_.end() && types_equal(it->second.type, target_type);
    }

    // Whether `arg` is a legitimate argument for a candidate overload's
    // parameter declared as `param_type` -- mirrors movecheck's own
    // argument_matches_parameter (ch05 §5.10) exactly, just phrased over
    // codegen's own infer_type/types_equal instead of movecheck's.
    const Function* find_single_argument_converting_constructor(const std::string& class_name, const Expr& arg) {
        if (arg.kind != ExprKind::StringLiteral) return nullptr;
        std::vector<const Function*> matches;
        for (const Function& fn : program_->functions) {
            if (fn.name != class_name + "_new" || fn.params.size() != 2) continue;
            const Type& ctor_param_type = fn.params[1].type;
            if (types_equal(ctor_param_type, named_type(class_name)) ||
                (ctor_param_type.kind == TypeKind::Reference && ctor_param_type.pointee != nullptr &&
                 types_equal(*ctor_param_type.pointee, named_type(class_name)))) {
                continue;
            }
            if (argument_matches_parameter(arg, fn.params[1].type)) matches.push_back(&fn);
        }
        if (matches.empty()) return nullptr;
        return matches[0];
    }

    bool argument_matches_parameter(const Expr& arg, const Type& param_type) {
        auto argument_type_matches_parameter = [&](const Type& arg_type, const Type& candidate_param_type) {
            if (candidate_param_type.kind == TypeKind::Reference) {
                if (arg_type.kind == TypeKind::Reference) {
                    if (arg_type.pointee == nullptr || candidate_param_type.pointee == nullptr) return false;
                    return types_equal(*arg_type.pointee, *candidate_param_type.pointee) &&
                           (!candidate_param_type.is_mutable_ref || arg_type.is_mutable_ref);
                }
                return candidate_param_type.pointee != nullptr && types_equal(arg_type, *candidate_param_type.pointee);
            }
            if (arg_type.kind == TypeKind::Reference) {
                return arg_type.pointee != nullptr && types_equal(*arg_type.pointee, candidate_param_type);
            }
            return types_equal(arg_type, candidate_param_type);
        };
        auto argument_type_matches_or_converts = [&](const Type& arg_type, const Type& candidate_param_type) {
            return argument_type_matches_parameter(arg_type, candidate_param_type) ||
                   types_compatible_with_interface_conversion(arg_type, candidate_param_type, current_enclosing_class_name());
        };
        if (param_type.kind == TypeKind::Reference && param_type.is_rvalue_ref) {
            // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- mirror image of
            // the ordinary-reference case just below.
            return produces_rvalue_of_type(arg, *param_type.pointee);
        }
        if (param_type.kind == TypeKind::Reference) {
            // ch05 §5.x: a *const* reference may bind either to a
            // genuine rvalue of the exact pointee type, or to a freshly
            // materialized temporary built through a converting
            // constructor such as `std::string{"..."}` from a string
            // literal.
            if (const_reference_binds_materialized_temporary(arg, param_type)) {
                return true;
            }
            if (arg.kind == ExprKind::Move || arg.kind == ExprKind::New ||
                arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::FloatLiteral ||
                arg.kind == ExprKind::BoolLiteral ||
                arg.kind == ExprKind::CharLiteral || arg.kind == ExprKind::StringLiteral) {
                return false;
            }
            std::optional<Type> arg_type = infer_type(arg);
            return arg_type.has_value() && argument_type_matches_or_converts(*arg_type, param_type);
        }
        std::optional<Type> arg_type = infer_type(arg);
        if (!arg_type.has_value()) return false;
        if (!argument_type_matches_or_converts(*arg_type, param_type)) {
            if (param_type.kind == TypeKind::Named && find_class_def(param_type.name) != nullptr &&
                find_single_argument_converting_constructor(param_type.name, arg) != nullptr) {
                return true;
            }
            return false;
        }
        if (param_type.kind == TypeKind::Named && find_class_def(param_type.name) != nullptr) {
            return (is_bare_same_type_copy_source(arg, param_type) && is_copy_constructible(param_type.name)) ||
                   produces_rvalue_of_type(arg, param_type);
        }
        return true;
    }

    // Whether `expr` (a method-call receiver or reference-parameter
    // argument) is only reachable *read-only* -- mirrors movecheck's own
    // is_read_only_reachable (same overall shape/scope), needed here
    // purely to resolve which overload a call targets when a const/
    // non-const method pair (or an ordinary T&/const T& overload pair)
    // makes the receiver's/argument's own mutability the only
    // distinguishing factor (ch05 §5.10).
    bool is_read_only_place(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::Identifier: {
                auto it = locals_.find(expr.name);
                if (it == locals_.end()) return false;
                return it->second.is_const || (it->second.type.kind == TypeKind::Reference && !it->second.type.is_mutable_ref);
            }
            case ExprKind::Member:
            case ExprKind::Subscript:
                return is_read_only_place(*expr.lhs);
            case ExprKind::Unary:
                if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) return false;
                {
                    auto it = locals_.find(expr.lhs->name);
                    return it != locals_.end() && it->second.type.kind == TypeKind::Pointer &&
                           !it->second.type.is_mutable_pointee;
                }
            case ExprKind::Call: {
                std::optional<Type> t = infer_type(expr);
                return t.has_value() && t->kind == TypeKind::Reference && !t->is_mutable_ref;
            }
            default:
                return false;
        }
    }

    bool receiver_matches_method_qualifier(const Expr& receiver_expr, const Function& fn) {
        if (fn.params.empty() || fn.params[0].type.kind != TypeKind::Reference || fn.params[0].type.pointee == nullptr) {
            return true;
        }
        bool receiver_is_rvalue = produces_rvalue_of_type(receiver_expr, *fn.params[0].type.pointee);
        switch (fn.receiver_ref_qualifier) {
            case ReceiverRefQualifier::None: return true;
            case ReceiverRefQualifier::LValue: return !receiver_is_rvalue;
            case ReceiverRefQualifier::RValue: return receiver_is_rvalue;
        }
        return true;
    }

    // Resolves a call/constructor-call's callee among the (possibly
    // several) Functions sharing `callee_name`'s exact name -- ch05
    // §5.10, mirroring movecheck's own resolve_overload (see its much
    // more detailed comment): exact type match only, with a
    // single-candidate shortcut (the overwhelmingly common,
    // non-overloaded case: no filtering at all, so an argument shape
    // infer_type can't resolve -- e.g. a Member/Subscript chain codegen
    // itself *can* usually resolve, but needn't be relied on -- never
    // wrongly breaks an ordinary call). Movecheck has already fully
    // validated the whole program by the time codegen runs, so a genuine
    // ambiguity here would mean a movecheck/codegen resolution
    // disagreement -- codegen falls back to the first candidate rather
    // than crashing, same conservative choice as movecheck's own. Takes
    // the raw argument list (not a whole Call Expr) so this is equally
    // usable for an ordinary call (expr.args) and a `ClassName name{args};`
    // constructor-call VarDecl (stmt.ctor_args), which has no Expr of its
    // own to hand over. `receiver_is_mutable` is the method-call
    // receiver's own mutability (ch05 §5.9's implicit `this`
    // parameter) -- meaningless when `param_offset` is 0 (an ordinary
    // free-function/constructor call, no receiver at all), and always
    // `true` for a constructor call (there's no *existing* object yet
    // for read-only-reachability to apply to).
    const Function* resolve_overload_by_type(const std::string& callee_name, const std::vector<ExprPtr>& args,
                                              size_t param_offset, bool receiver_is_mutable = true,
                                              const Expr* receiver_expr = nullptr) {
        std::vector<const Function*> candidates;
        for (const Function& fn : program_->functions) {
            if (fn.name == callee_name) candidates.push_back(&fn);
        }
        if (candidates.empty()) return nullptr;
        if (candidates.size() == 1) {
            if (param_offset == 1 && receiver_expr != nullptr) {
                if (candidates[0]->params.size() > 0 && candidates[0]->params[0].type.kind == TypeKind::Reference &&
                    candidates[0]->params[0].type.is_mutable_ref && !receiver_is_mutable) {
                    return nullptr;
                }
                if (!receiver_matches_method_qualifier(*receiver_expr, *candidates[0])) return nullptr;
            }
            return candidates[0];
        }

        std::vector<const Function*> matches;
        for (const Function* fn : candidates) {
            if (fn->params.size() != args.size() + param_offset) continue;
            // The receiver (`this`): viable only if the candidate's own
            // `this` mutability doesn't demand more than the receiver
            // place can actually provide.
            if (param_offset == 1 && fn->params[0].type.is_mutable_ref && !receiver_is_mutable) continue;
            if (param_offset == 1 && receiver_expr != nullptr &&
                !receiver_matches_method_qualifier(*receiver_expr, *fn)) {
                continue;
            }
            bool all_match = true;
            for (size_t i = 0; all_match && i < args.size(); i++) {
                all_match = argument_matches_parameter(*args[i], fn->params[i + param_offset].type);
            }
            if (all_match) matches.push_back(fn);
        }
        if (matches.empty()) return nullptr;
        if (matches.size() == 1) return matches[0];

        // Tie-break ("T& beats const T& for a mutable lvalue", ch05
        // §5.10): prefer whichever match has the most mutable-reference
        // parameters (including `this`) among positions where the
        // argument/receiver is itself a mutable place. Falls back to the
        // first match if that still doesn't produce a unique winner.
        auto mutable_ref_score = [&](const Function* fn) {
            int score = 0;
            if (param_offset == 1 && fn->params[0].type.is_mutable_ref && receiver_is_mutable) score++;
            if (param_offset == 1 && receiver_expr != nullptr && fn->params[0].type.pointee != nullptr) {
                bool receiver_is_rvalue = produces_rvalue_of_type(*receiver_expr, *fn->params[0].type.pointee);
                if ((receiver_is_rvalue && fn->receiver_ref_qualifier == ReceiverRefQualifier::RValue) ||
                    (!receiver_is_rvalue && fn->receiver_ref_qualifier == ReceiverRefQualifier::LValue)) {
                    score += 2;
                }
            }
            for (size_t i = 0; i < args.size(); i++) {
                const Type& param_type = fn->params[i + param_offset].type;
                if (param_type.kind == TypeKind::Reference && param_type.is_mutable_ref && !is_read_only_place(*args[i])) {
                    score++;
                }
            }
            return score;
        };
        const Function* best = matches[0];
        int best_score = mutable_ref_score(best);
        bool unique_best = true;
        for (size_t i = 1; i < matches.size(); i++) {
            int score = mutable_ref_score(matches[i]);
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

    const Function* resolve_constructor_overload_exact(const std::string& class_name, const std::vector<ExprPtr>& args) {
        std::vector<const Function*> matches;
        for (const Function& fn : program_->functions) {
            if (fn.name != class_name + "_new") continue;
            if (fn.params.size() != args.size() + 1) continue;
            bool all_match = true;
            for (size_t i = 0; all_match && i < args.size(); i++) {
                all_match = argument_matches_parameter(*args[i], fn.params[i + 1].type);
            }
            if (all_match) matches.push_back(&fn);
        }
        if (matches.empty()) return nullptr;
        if (matches.size() == 1) return matches[0];
        auto mutable_ref_score = [&](const Function* fn) {
            int score = 0;
            for (size_t i = 0; i < args.size(); i++) {
                const Type& param_type = fn->params[i + 1].type;
                if (param_type.kind == TypeKind::Reference && param_type.is_mutable_ref && !is_read_only_place(*args[i])) {
                    score++;
                }
            }
            return score;
        };
        const Function* best = matches[0];
        int best_score = mutable_ref_score(best);
        bool unique_best = true;
        for (size_t i = 1; i < matches.size(); i++) {
            int score = mutable_ref_score(matches[i]);
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

    // Recursively verifies a type is trivial per the language spec (ch04):
    // scalars, raw pointers (any pointee), fixed-size arrays of trivial
    // types, and structs/unions whose fields are themselves all trivial.
    // `in_progress` detects a struct/union containing itself *by value*, which
    // must be rejected (as in C, this would be an infinitely-sized type);
    // self-reference via pointer is fine since pointers don't recurse here.
    void validate_trivial(const Type& type, std::vector<std::string>& in_progress) {
        switch (type.kind) {
            case TypeKind::Pointer:
            case TypeKind::FunctionPointer:
                return;
            case TypeKind::Function:
                throw CodegenError("a bare function type cannot be a struct field in this version", current_loc_);
            case TypeKind::Reference:
                throw CodegenError("a reference cannot be a struct field in this version",
                    current_loc_);
            case TypeKind::Span:
                throw CodegenError("a std::span cannot be a struct field in this version (it is a "
                                    "lifetime-checked borrowed view; use class instead)",
                    current_loc_);
            case TypeKind::Array:
                validate_trivial(*type.element, in_progress);
                return;
            case TypeKind::Named: {
                if (is_scalar_type_name(type.name)) return;
                if (find_enum_def(program_, type.name) != nullptr) return;
                if (type.name == "std::storage_for") return;
                if (type.name == "void") {
                    throw CodegenError("'void' cannot be a struct field (only a return type or a "
                                        "pointer's pointee -- 'void*' -- may be 'void')",
                        current_loc_);
                }
                if (find_class_def(type.name) != nullptr) {
                    throw CodegenError("a class type '" + type.name + "' cannot be a struct field; use class instead",
                        current_loc_);
                }
                const StructDef* def = find_struct_def(type.name);
                if (def == nullptr) {
                    throw CodegenError("unknown type '" + type.name + "'",
                        current_loc_);
                }
                if (std::find(in_progress.begin(), in_progress.end(), type.name) != in_progress.end()) {
                    throw CodegenError("struct '" + type.name + "' cannot contain itself by value "
                                                                 "(did you mean a pointer '" +
                                        type.name + "*'?)",
                        current_loc_);
                }
                in_progress.push_back(type.name);
                for (const StructField& field : def->fields) {
                    validate_trivial(field.type, in_progress);
                }
                in_progress.pop_back();
                return;
            }
        }
    }

    void declare_struct(const StructDef& def) {
        if (declaring_aggregates_.contains(def.name)) return;
        declaring_aggregates_.insert(def.name);
        std::vector<std::string> in_progress;
        for (const StructField& field : def.fields) {
            try {
                validate_trivial(field.type, in_progress);
            } catch (const CodegenError& e) {
                throw CodegenError(std::string(def.is_union ? "union '" : "struct '") + def.name + "' field '" +
                                        field.name + "': " + e.what() +
                                        " (only scalars, pointers, trivial structs/unions, and fixed-size arrays "
                                        "of trivial types are allowed here; see spec ch04)",
                    current_loc_);
            }
        }

        StructInfo info;
        info.is_union = def.is_union;
        info.is_packed = def.is_packed;
        std::vector<llvm::Type*> llvm_field_types;
        llvm_field_types.reserve(def.fields.size());
        for (const StructField& field : def.fields) {
            info.field_names.push_back(field.name);
            info.field_types.push_back(field.type);
            llvm_field_types.push_back(to_llvm_type(field.type));
        }
        if (!def.is_union) {
            info.llvm_type = llvm::StructType::create(*context_, llvm_field_types, "struct." + def.name, def.is_packed);
        } else {
            if (llvm_field_types.empty()) {
                throw CodegenError("union '" + def.name + "' must declare at least one field",
                    current_loc_);
            }
            size_t align_value = def.is_packed ? 1 : 0;
            size_t max_size = 0;
            size_t max_rep_align = 0;
            llvm::Type* rep_type = llvm_field_types[0];
            size_t rep_size = module_->getDataLayout().getTypeAllocSize(rep_type);
            for (llvm::Type* field_type : llvm_field_types) {
                size_t field_size = module_->getDataLayout().getTypeAllocSize(field_type);
                size_t field_align = def.is_packed ? 1 : module_->getDataLayout().getABITypeAlign(field_type).value();
                if (field_size > max_size) max_size = field_size;
                if (field_align > align_value) align_value = field_align;
                if (field_align > max_rep_align ||
                    (field_align == max_rep_align && field_size > rep_size)) {
                    rep_type = field_type;
                    rep_size = field_size;
                    max_rep_align = field_align;
                }
            }
            if (align_value == 0) align_value = 1;
            size_t union_size = ((max_size + align_value - 1) / align_value) * align_value;
            std::vector<llvm::Type*> storage_fields;
            storage_fields.push_back(rep_type);
            if (union_size > rep_size) {
                storage_fields.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), union_size - rep_size));
            }
            info.llvm_type = llvm::StructType::create(*context_, storage_fields, "union." + def.name, def.is_packed);
        }
        info.abi_align = module_->getDataLayout().getABITypeAlign(info.llvm_type);
        structs_[def.name] = std::move(info);
        declaring_aggregates_.erase(def.name);
    }

    // Registers a `class`'s layout the same way declare_struct does for a
    // `struct` (a named LLVM struct type, keyed into the same `structs_`
    // map -- ch04 §4.2's `class` and `struct` are both fixed-layout
    // aggregates at the codegen level; only field access control and
    // participation in move/borrow/lifetime checking, both handled
    // elsewhere, tell them apart). Unlike declare_struct, a class field is
    // *not* required to be trivial (ch04 §4.2 explicitly allows
    // unique_ptr/span/other class members, or any other type carrying
    // ownership/lifetime semantics), so this skips validate_trivial
    // entirely.
    //
    // ch05 §5.14: a class with an ordinary direct base gets a
    // *flattened* layout -- the base's own StructInfo (already
    // registered: the parser requires a base to be declared, and
    // `generate()`'s own declare_class loop processes program.classes in
    // that same declaration order) is copied in first, then this class's
    // own fields appended -- rather than nesting the base as a single
    // sub-struct element. This is the same memory layout real single
    // inheritance already produces (the base subobject's fields occupy
    // the leading bytes either way), but flattening means every existing
    // field-access/GEP path (codegen_lvalue's Member case, a simple
    // linear search through field_names) needs no inheritance-specific
    // logic at all, and a most-derived instance's own leading bytes are
    // trivially reinterpretable as its base type (needed later for
    // base-class-deduction, ch05 §5.14's indexed-access pattern) via a
    // plain bitcast, with no pointer adjustment.
    void declare_class(const ClassDef& def) {
        if (declaring_aggregates_.contains(def.name)) return;
        declaring_aggregates_.insert(def.name);
        StructInfo info;
        if (const BaseSpecifier* base = def.direct_ordinary_base()) {
            const StructInfo& base_info = structs_.at(base->base_type.name);
            info.field_names = base_info.field_names;
            info.field_types = base_info.field_types;
        }
        std::vector<llvm::Type*> llvm_field_types;
        llvm_field_types.reserve(info.field_names.size() + def.fields.size());
        for (const Type& t : info.field_types) llvm_field_types.push_back(to_llvm_type(t));
        for (const ClassField& field : def.fields) {
            info.field_names.push_back(field.name);
            info.field_types.push_back(field.type);
            llvm_field_types.push_back(to_llvm_type(field.type));
        }
        info.llvm_type = llvm::StructType::create(*context_, llvm_field_types, "class." + def.name);
        info.abi_align = module_->getDataLayout().getABITypeAlign(info.llvm_type);
        structs_[def.name] = std::move(info);
        declaring_aggregates_.erase(def.name);
    }

    [[nodiscard]] llvm::Type* storage_for_llvm_type(const Type& type) {
        if (type.template_args.empty()) {
            throw CodegenError("'std::storage_for' requires at least one type argument", current_loc_);
        }
        size_t max_size = 0;
        size_t max_align = 1;
        llvm::Type* rep_type = nullptr;
        size_t rep_size = 0;
        size_t rep_align = 0;
        for (const Type& candidate : type.template_args) {
            llvm::Type* candidate_llvm = to_llvm_type(candidate);
            size_t candidate_size = module_->getDataLayout().getTypeAllocSize(candidate_llvm);
            size_t candidate_align = module_->getDataLayout().getABITypeAlign(candidate_llvm).value();
            max_size = std::max(max_size, candidate_size);
            max_align = std::max(max_align, candidate_align);
            if (rep_type == nullptr || candidate_align > rep_align || (candidate_align == rep_align && candidate_size > rep_size)) {
                rep_type = candidate_llvm;
                rep_size = candidate_size;
                rep_align = candidate_align;
            }
        }
        size_t storage_size = ((max_size + max_align - 1) / max_align) * max_align;
        std::vector<llvm::Type*> storage_fields;
        storage_fields.push_back(rep_type);
        if (storage_size > rep_size) {
            storage_fields.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), storage_size - rep_size));
        }
        return llvm::StructType::get(*context_, storage_fields);
    }

    llvm::Type* to_llvm_type(const Type& type) {
        switch (type.kind) {
            case TypeKind::Function:
                throw CodegenError("a bare function type cannot be lowered directly; only function pointers are runtime values",
                                   current_loc_);
            case TypeKind::Pointer:
            case TypeKind::Reference:
                if (is_interface_representation_type(type)) {
                    return interface_representation_type();
                }
                // A unique_ptr is just a possibly-null owning pointer at the
                // ABI/codegen level in v0.1: no destructor/drop codegen
                // exists yet (that's M3's "drop insertion"), so it lowers
                // identically to a raw pointer. The move checker is what
                // enforces the ownership discipline on top of this.
                // A reference is likewise ABI-identical to a pointer (same
                // as Clang lowers C++ references): the frontend is what
                // makes it auto-dereference on every use (see
                // codegen_lvalue's Identifier case) and enforces borrow
                // discipline (scpp.movecheck), not the IR shape itself.
                return llvm::PointerType::getUnqual(*context_);
            case TypeKind::FunctionPointer: {
                std::vector<llvm::Type*> params;
                params.reserve(type.function_params.size());
                for (const Type& param : type.function_params) params.push_back(to_llvm_type(param));
                return llvm::PointerType::get(*context_, 0);
            }
            case TypeKind::Span:
                // A non-owning {data pointer, element count} pair -- a
                // literal (unnamed) two-word LLVM struct, not registered
                // in `structs_` (span isn't a user-visible aggregate the
                // way a `struct` is; Member/Subscript access to it is
                // special-cased directly on TypeKind::Span in
                // codegen_lvalue, not routed through StructInfo). LLVM
                // deduplicates identical literal struct types itself, so
                // there's no need to cache this beyond calling
                // StructType::get each time.
                return llvm::StructType::get(*context_,
                                              {llvm::PointerType::getUnqual(*context_), llvm::Type::getInt64Ty(*context_)});
            case TypeKind::Array:
                return llvm::ArrayType::get(to_llvm_type(*type.element), type.array_size);
            case TypeKind::Named:
                if (type.name == "int") return llvm::Type::getInt32Ty(*context_);
                // A full byte (i8), matching real C++'s sizeof(bool)==1
                // and the spec's false=0/true=1 invariant (ch06) -- not
                // i1, even though LLVM's own icmp/br/select instructions
                // require i1. See bool_to_i1/i1_to_bool below for the
                // narrow choke point that bridges the two: every
                // consumer that needs a branch/select condition truncates
                // down to i1 first, and every comparison/logical result
                // gets zext'd back up to i8 before being stored, passed,
                // or returned as an ordinary bool value.
                if (type.name == "bool") return llvm::Type::getInt8Ty(*context_);
                // A signed 8-bit scalar, matching the common (e.g.
                // x86-64 Linux/Clang) default for plain `char` -- no
                // implicit promotion to/from `int` exists yet (matching
                // the same pre-existing lack of promotion between `bool`
                // and `int`), so this is the type's only representation.
                if (type.name == "char") return llvm::Type::getInt8Ty(*context_);
                // ch06 §6: the rest of the numeric family -- LLVM natively
                // supports arbitrary-width integers, so every fixed-width
                // signed/unsigned pair just maps to the same-width
                // integer type (LLVM itself draws no signed/unsigned
                // distinction at the type level, only at the
                // instruction level -- see is_unsigned_scalar_type/
                // codegen_checked_arith's own signedness dispatch).
                // `long`/`unsigned long` are always 64-bit regardless of
                // target (ch06's own deliberate anti-LP64/LLP64-pitfall
                // fix), unlike `size_t`/`ptrdiff_t` below, which are
                // meant to track the pointer width.
                if (type.name == "int8_t" || type.name == "uint8_t") return llvm::Type::getInt8Ty(*context_);
                if (type.name == "int16_t" || type.name == "uint16_t") return llvm::Type::getInt16Ty(*context_);
                if (type.name == "int32_t" || type.name == "uint32_t" || type.name == "unsigned int") {
                    return llvm::Type::getInt32Ty(*context_);
                }
                if (type.name == "int64_t" || type.name == "uint64_t" || type.name == "long" ||
                    type.name == "unsigned long") {
                    return llvm::Type::getInt64Ty(*context_);
                }
                if (type.name == "float" || type.name == "float32_t") return llvm::Type::getFloatTy(*context_);
                if (type.name == "double" || type.name == "float64_t") return llvm::Type::getDoubleTy(*context_);
                if (type.name == "size_t" || type.name == "ptrdiff_t") {
                    return llvm::Type::getIntNTy(*context_, module_->getDataLayout().getPointerSizeInBits());
                }
                if (type.name == "std::storage_for") return storage_for_llvm_type(type);
                if (const EnumDef* enum_def = find_enum_def(program_, type.name)) {
                    return to_llvm_type(enum_def->underlying_type);
                }
                // `void` (ch02 §2.1): only meaningful as a function return
                // type or a pointer's pointee (`void*`, whose own
                // to_llvm_type case above never even inspects the
                // pointee, so nothing further is needed there). Callers
                // that would put it somewhere nonsensical (a bare local,
                // parameter, struct field, or array element) reject it
                // *before* reaching here -- see declare_function's
                // parameter loop and codegen_stmt's VarDecl case -- so
                // this is never actually asked to allocate storage for a
                // void value.
                if (type.name == "void") return llvm::Type::getVoidTy(*context_);
                {
                    auto it = structs_.find(type.name);
                    if (it != structs_.end()) return it->second.llvm_type;
                }
                if (!declaring_aggregates_.contains(type.name) && program_ != nullptr) {
                    for (const StructDef& def : program_->structs) {
                        if (def.name == type.name && def.template_params.empty()) {
                            declare_struct(def);
                            auto it = structs_.find(type.name);
                            if (it != structs_.end()) return it->second.llvm_type;
                        }
                    }
                    for (const ClassDef& def : program_->classes) {
                        if (def.name == type.name && def.template_params.empty() && !def.is_synthetic_check_only &&
                            !def.is_concept_witness) {
                            declare_class(def);
                            auto it = structs_.find(type.name);
                            if (it != structs_.end()) return it->second.llvm_type;
                        }
                    }
                }
                throw CodegenError("unsupported type '" + type.name + "'",
                    current_loc_);
        }
        throw CodegenError("unhandled type kind",
            current_loc_);
    }

    [[nodiscard]] std::optional<llvm::Align> alignment_for_type(const Type& type) const {
        if (type.kind != TypeKind::Named) return std::nullopt;
        if (type.name == "std::storage_for") {
            llvm::Type* llvm_type = const_cast<Codegen*>(this)->to_llvm_type(type);
            return module_->getDataLayout().getABITypeAlign(llvm_type);
        }
        auto it = structs_.find(type.name);
        if (it == structs_.end()) return std::nullopt;
        return it->second.abi_align;
    }

    llvm::LoadInst* create_load(llvm::Type* type, llvm::Value* ptr, std::optional<llvm::Align> alignment,
                                const llvm::Twine& name = "") {
        llvm::LoadInst* load = builder_->CreateLoad(type, ptr, name);
        if (alignment.has_value()) load->setAlignment(*alignment);
        return load;
    }

    llvm::StoreInst* create_store(llvm::Value* value, llvm::Value* ptr, std::optional<llvm::Align> alignment) {
        llvm::StoreInst* store = builder_->CreateStore(value, ptr);
        if (alignment.has_value()) store->setAlignment(*alignment);
        return store;
    }

    void zero_initialize_storage(llvm::Value* ptr, const Type& type, std::optional<llvm::Align> alignment = std::nullopt) {
        llvm::Type* llvm_type = to_llvm_type(type);
        switch (type.kind) {
            case TypeKind::Named:
            case TypeKind::Array:
            case TypeKind::Span: {
                llvm::TypeSize size = module_->getDataLayout().getTypeAllocSize(llvm_type);
                builder_->CreateMemSet(ptr,
                                       llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), 0),
                                       size,
                                       alignment.value_or(llvm::Align(1)));
                return;
            }
            default:
                create_store(llvm::Constant::getNullValue(llvm_type), ptr, alignment);
                return;
        }
    }

    // A reference's referent may not itself be another reference:
    // reference-to-reference aliasing analysis is still out of scope for
    // v0.1's intraprocedural, first-order borrow checking.
    void validate_reference_pointee(const Type& pointee) {
        if (pointee.kind == TypeKind::Reference) {
            throw CodegenError("a reference to a reference is not supported",
                current_loc_);
        }
    }

    // Structural counterpart to movecheck's resolve_elided_param_index
    // (spec ch05.3's elision rule): a function may only declare a
    // Reference return type if it has *exactly* one reference-typed
    // parameter (this language has no `this`/method-receiver concept at
    // all yet, so that half of the rule never applies), with compatible
    // mutability (a `const T&` parameter can't license a `T&` return --
    // that would manufacture a mutable alias out of a shared one).
    // Codegen doesn't need the resolved parameter itself the way
    // movecheck does (it never traces which expression a `return`
    // statement's value came from -- that's movecheck's per-return
    // dangling check, which runs before codegen in the driver's
    // pipeline; see driver.cppm), so this only has to reject a
    // structurally-invalid signature up front.
    void validate_reference_return_elision(const Function& fn) {
        // ch04 §4.2/ch05 §5.9/spec §6.5: `this` (always params[0] when
        // present) is always the elision source when the function is a
        // method, regardless of how many other reference parameters it
        // also takes -- see movecheck's resolve_elided_param_index for
        // the full rationale (this is its structural codegen-side
        // counterpart, kept in sync).
        if (!fn.params.empty() && fn.params[0].name == "this" && fn.params[0].type.kind == TypeKind::Reference) {
            if (fn.return_type.is_mutable_ref && !fn.params[0].type.is_mutable_ref) {
                throw CodegenError("function '" + fn.name +
                                    "' returns a mutable reference ('T&') but its 'this' is a read-only "
                                    "('const') receiver",
                    current_loc_);
            }
            return;
        }
        const Param* found = nullptr;
        for (const Param& param : fn.params) {
            // ch03/ch05 §5.11: exclude a `T&&` parameter -- see
            // movecheck's resolve_elided_param_index (this function's
            // structural counterpart) for why it's never a sound elision
            // source.
            if (param.type.kind != TypeKind::Reference || param.type.is_rvalue_ref) continue;
            if (found != nullptr) {
                throw CodegenError("function '" + fn.name +
                                    "' returns a reference but has more than one reference parameter; scpp "
                                    "v0.1 can only infer a returned reference's lifetime when there is exactly "
                                    "one (spec ch05.3)",
                    current_loc_);
            }
            found = &param;
        }
        if (found == nullptr) {
            throw CodegenError("function '" + fn.name +
                                "' returns a reference but has no reference parameter to infer its lifetime "
                                "from (spec ch05.3)",
                current_loc_);
        }
        if (fn.return_type.is_mutable_ref && !found->type.is_mutable_ref) {
            throw CodegenError("function '" + fn.name +
                                "' returns a mutable reference ('T&') but its sole reference parameter '" +
                                found->name + "' is a shared reference ('const T&')",
                current_loc_);
        }
    }

    [[nodiscard]] static bool is_bare_void(const Type& type) {
        return type.kind == TypeKind::Named && type.name == "void";
    }

    // ch02 §2.1: an `extern "C"` signature's parameter and return types
    // must have a defined C representation. Allowed: scalars (`int`/
    // `bool`) and `void` (return-type position only, checked by the
    // caller before this is reached -- see declare_function); raw
    // pointers `T*` (any pointee, including `void*` -- to_llvm_type never
    // inspects a pointer's pointee, so nothing further to check there);
    // `struct` by value or by pointer (already guaranteed Clang-ABI-
    // compatible layout, ch04.3). A fixed-size array `T[N]` written in
    // parameter position is already decayed to `T*` by the parser (see
    // parse_function) -- exactly as in ordinary C++, and matching this
    // rule's own allowance for it -- so `TypeKind::Array` itself can never
    // actually reach here (return types have no array syntax at all,
    // ch03; a plain local/struct-field array never becomes a signature
    // type). Rejected: `T&`/`const T&`, `std::unique_ptr`, `std::span` --
    // none of these have a defined C representation
    // (`std::string`/`std::vector`/`std::shared_ptr`/`[[scpp::lifetime]]`
    // don't exist in scpp at all yet, so there's nothing to reject for
    // those -- see ch02 §2.1's own note on this).
    void validate_c_abi_compatible(const Type& type, const std::string& fn_name,
                                    const std::string& context_description) {
        if (is_interface_representation_type(type)) {
            throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                " cannot be an interface-typed pointer or reference -- it has no defined C "
                                "representation (spec ch02 §2.1 / §11.3)",
                current_loc_);
        }
        switch (type.kind) {
            case TypeKind::Function:
                throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be a bare function type -- use a function pointer instead (spec ch02 §2.1)",
                    current_loc_);
            case TypeKind::Named: {
                if (find_class_def(type.name) != nullptr) {
                    throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                        " cannot be class type '" + type.name +
                                        "' -- class types have no defined C representation "
                                        "(spec ch02 §2.1)",
                        current_loc_);
                }
                if (is_scalar_type_name(type.name)) return;
            }
            case TypeKind::Pointer:
            case TypeKind::FunctionPointer:
            case TypeKind::Array:
                return;
            case TypeKind::Reference:
                throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be a reference -- it has no defined C representation (spec "
                                    "ch02 §2.1)",
                    current_loc_);
            case TypeKind::Span:
                throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be std::span -- it has no defined C representation (spec "
                                    "ch02 §2.1)",
                    current_loc_);
        }
    }

    // Structural (deep) equality between two Types -- see movecheck's own
    // types_equal (this file has no access to it: separate module,
    // separate data model) for why a naive `=default` comparison of
    // Type's shared_ptr pointee/element wouldn't work. Used only for
    // function-overload resolution (ch05 §5.10)'s exact-type-match rule.
    // Reference additionally requires is_rvalue_ref to match: `T&`/
    // `const T&` and `T&&` (ch03) are distinct parameter types, never
    // interchangeable -- meaningless for Span.
    [[nodiscard]] static bool types_equal(const Type& a, const Type& b) {
        if (a.kind != b.kind) return false;
        switch (a.kind) {
            case TypeKind::Named:
                if (a.name != b.name || a.template_args.size() != b.template_args.size()) return false;
                for (size_t i = 0; i < a.template_args.size(); i++) {
                    if (!types_equal(a.template_args[i], b.template_args[i])) return false;
                }
                return true;
            case TypeKind::Pointer: return a.is_mutable_pointee == b.is_mutable_pointee && types_equal(*a.pointee, *b.pointee);
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if ((a.kind == TypeKind::FunctionPointer && a.is_unsafe_function_pointer != b.is_unsafe_function_pointer) ||
                    (a.kind == TypeKind::Function &&
                     (a.is_const_function != b.is_const_function ||
                      a.function_ref_qualifier != b.function_ref_qualifier)) ||
                    !types_equal(*a.function_return, *b.function_return) ||
                    a.function_params.size() != b.function_params.size()) {
                    return false;
                }

                for (size_t i = 0; i < a.function_params.size(); i++) {
                    if (!types_equal(a.function_params[i], b.function_params[i])) return false;
                }
                return true;
            case TypeKind::Reference:
                return a.is_mutable_ref == b.is_mutable_ref && a.is_rvalue_ref == b.is_rvalue_ref &&
                       types_equal(*a.pointee, *b.pointee);
            case TypeKind::Span:
                return a.is_mutable_ref == b.is_mutable_ref && types_equal(*a.pointee, *b.pointee);
            case TypeKind::Array: return a.array_size == b.array_size && types_equal(*a.element, *b.element);
        }
        return false;
    }

    [[nodiscard]] static const Type& binary_operand_type(const Type& type) {
        return type.kind == TypeKind::Reference ? *type.pointee : type;
    }

    [[nodiscard]] static bool is_pointer_arithmetic_offset_type(const Type& type) {
        return type.kind == TypeKind::Named && type.name != "bool" && is_integral_scalar_type_name(type.name);
    }

    [[nodiscard]] bool pointer_supports_arithmetic(const Type& type) const {
        return type.kind == TypeKind::Pointer && type.pointee != nullptr && !is_interface_pointer_type(type) &&
               !(type.pointee->kind == TypeKind::Named && type.pointee->name == "void");
    }

    [[nodiscard]] std::optional<Type> pointer_arithmetic_result_type(BinaryOp op, const Type& lhs, const Type& rhs) const {
        const Type& lhs_operand = binary_operand_type(lhs);
        const Type& rhs_operand = binary_operand_type(rhs);
        if (op == BinaryOp::Add) {
            if (pointer_supports_arithmetic(lhs_operand) && is_pointer_arithmetic_offset_type(rhs_operand)) {
                return lhs_operand;
            }
            if (is_pointer_arithmetic_offset_type(lhs_operand) && pointer_supports_arithmetic(rhs_operand)) {
                return rhs_operand;
            }
            return std::nullopt;
        }
        if (op == BinaryOp::Sub) {
            if (pointer_supports_arithmetic(lhs_operand) && is_pointer_arithmetic_offset_type(rhs_operand)) {
                return lhs_operand;
            }
            if (pointer_supports_arithmetic(lhs_operand) && pointer_supports_arithmetic(rhs_operand) &&
                types_equal(lhs_operand, rhs_operand)) {
                return named_type("ptrdiff_t");
            }
        }
        return std::nullopt;
    }

    // A short, LLVM-identifier-safe (alphanumeric/underscore only, no
    // spaces) encoding of `type`, used only to build a *disambiguating*
    // symbol-name suffix for a *local* (non-cross-module) overloaded
    // function (see build_overload_names) -- unlike ch11 §11.9's real
    // mangling scheme (used for symbols crossing a module boundary, see
    // mangle_exported_symbol below), this only has to be unique *within
    // this one compiled file*, so a compact tag scheme is simpler and
    // just as correct for the one job this specific helper needs it for.
    [[nodiscard]] static std::string mangle_type(const Type& type) {
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

    [[nodiscard]] static std::string method_lookup_name(const Function& fn) {
        if (fn.name.ends_with("_delete")) return "~";
        if (fn.name.ends_with("_operator_deref")) return "operator*";
        if (fn.name.ends_with("_operator_assign")) return "operator=";
        if (!fn.member_owner_class.empty() && fn.name.rfind(fn.member_owner_class + "_", 0) == 0) {
            return fn.name.substr(fn.member_owner_class.size() + 1);
        }
        return fn.name;
    }

    [[nodiscard]] static std::string interface_method_slot_key(const Function& fn) {
        std::string key = method_lookup_name(fn);
        key += "(";
        size_t start = fn.member_owner_class.empty() ? 0 : 1;
        for (size_t i = start; i < fn.params.size(); i++) {
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

    [[nodiscard]] const std::vector<const Function*>& interface_dispatch_methods(const std::string& interface_name) {
        auto cached = interface_dispatch_methods_cache_.find(interface_name);
        if (cached != interface_dispatch_methods_cache_.end()) return cached->second;
        const ClassDef* def = find_class_def(interface_name);
        if (def == nullptr || !def->is_interface) {
            throw CodegenError("unknown interface '" + interface_name + "' for dispatch table generation", current_loc_);
        }
        std::vector<const Function*> methods;
        std::unordered_map<std::string, size_t> slot_indices;
        for (const BaseSpecifier& base : def->base_specifiers) {
            if (base.kind != BaseClassKind::Interface) continue;
            for (const Function* method : interface_dispatch_methods(base.base_type.name)) {
                std::string slot_key = interface_method_slot_key(*method);
                if (!slot_indices.contains(slot_key)) {
                    slot_indices.emplace(slot_key, methods.size());
                    methods.push_back(method);
                }
            }
        }
        for (const Function& fn : program_->functions) {
            if (fn.member_owner_class != interface_name || fn.is_static || !fn.is_virtual || !fn.forwards_to.empty()) continue;
            if (fn.name.ends_with("_new") || fn.name.ends_with("_delete")) continue;
            std::string slot_key = interface_method_slot_key(fn);
            auto slot_it = slot_indices.find(slot_key);
            if (slot_it == slot_indices.end()) {
                slot_indices.emplace(slot_key, methods.size());
                methods.push_back(&fn);
            } else {
                methods[slot_it->second] = &fn;
            }
        }
        interface_slot_indices_cache_.emplace(interface_name, std::move(slot_indices));
        auto [it, _] = interface_dispatch_methods_cache_.emplace(interface_name, std::move(methods));
        return it->second;
    }

    [[nodiscard]] llvm::ArrayType* interface_dispatch_table_type(const std::string& interface_name) {
        auto it = interface_dispatch_table_types_.find(interface_name);
        if (it != interface_dispatch_table_types_.end()) return it->second;
        llvm::ArrayType* type =
            llvm::ArrayType::get(llvm::PointerType::getUnqual(*context_),
                                 interface_dispatch_methods(interface_name).size());
        interface_dispatch_table_types_.emplace(interface_name, type);
        return type;
    }

    [[nodiscard]] std::optional<size_t> interface_method_slot_index(const std::string& interface_name,
                                                                    const Function& method) {
        (void)interface_dispatch_methods(interface_name);
        auto cache_it = interface_slot_indices_cache_.find(interface_name);
        if (cache_it == interface_slot_indices_cache_.end()) return std::nullopt;
        auto slot_it = cache_it->second.find(interface_method_slot_key(method));
        if (slot_it == cache_it->second.end()) return std::nullopt;
        return slot_it->second;
    }

    [[nodiscard]] const Function* find_direct_method_by_slot(const std::string& class_name, const std::string& slot_key) const {
        for (const Function& fn : program_->functions) {
            if (fn.member_owner_class != class_name || fn.is_static || !fn.forwards_to.empty()) continue;
            if (interface_method_slot_key(fn) == slot_key) return &fn;
        }
        return nullptr;
    }

    [[nodiscard]] const Function* resolve_interface_slot_provider(const std::string& class_name, const std::string& slot_key) const {
        if (const Function* direct = find_direct_method_by_slot(class_name, slot_key)) return direct;
        const ClassDef* def = find_class_def(class_name);
        if (def == nullptr) return nullptr;
        const Function* chosen = nullptr;
        for (const BaseSpecifier& base : def->base_specifiers) {
            const Function* candidate = resolve_interface_slot_provider(base.base_type.name, slot_key);
            if (candidate == nullptr) continue;
            if (chosen == nullptr) {
                chosen = candidate;
                continue;
            }
            if (chosen != candidate && overload_names_.at(chosen) != overload_names_.at(candidate)) {
                throw CodegenError("ambiguous interface dispatch provider for slot '" + slot_key + "' in class '" + class_name + "'",
                                   current_loc_);
            }
        }
        return chosen;
    }

    [[nodiscard]] llvm::FunctionType* interface_dispatch_function_type(const Function& method) {
        std::vector<llvm::Type*> params;
        params.reserve(method.params.size());
        params.push_back(llvm::PointerType::getUnqual(*context_));
        for (size_t i = 1; i < method.params.size(); i++) {
            params.push_back(to_llvm_type(method.params[i].type));
        }
        return llvm::FunctionType::get(to_llvm_type(method.return_type), params, /*isVarArg=*/false);
    }

    [[nodiscard]] bool interface_destructor_uses_raw_this(const Function& fn) const {
        return fn.name.ends_with("_delete") && !fn.member_owner_class.empty() && type_names_interface(fn.member_owner_class);
    }

    [[nodiscard]] llvm::Type* llvm_param_type_for_function(const Function& fn, const Param& param, size_t index) {
        if (index == 0 && interface_destructor_uses_raw_this(fn)) {
            return llvm::PointerType::getUnqual(*context_);
        }
        return to_llvm_type(param.type);
    }

    [[nodiscard]] llvm::Function* get_or_create_interface_dispatch_thunk(const std::string& concrete_class_name,
                                                                         const Function& target) {
        std::string cache_key = concrete_class_name + "|" + overload_names_.at(&target);
        auto it = interface_dispatch_thunks_.find(cache_key);
        if (it != interface_dispatch_thunks_.end()) return it->second;
        const Type& this_type = target.params.front().type;
        const std::string interface_name = this_type.pointee->name;
        llvm::FunctionType* thunk_type = interface_dispatch_function_type(target);
        llvm::Function* thunk = llvm::Function::Create(thunk_type, llvm::GlobalValue::PrivateLinkage,
                                                       "__scpp_iface_thunk." + cache_key, *module_);
        interface_dispatch_thunks_.emplace(cache_key, thunk);
        llvm::IRBuilderBase::InsertPoint saved_ip = builder_->saveIP();
        llvm::DebugLoc saved_dbg = builder_->getCurrentDebugLocation();
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", thunk);
        builder_->SetInsertPoint(entry);
        llvm::Value* raw_this = thunk->arg_begin();
        llvm::Value* dispatch_ptr = get_or_create_interface_dispatch_table(concrete_class_name, interface_name);
        llvm::Value* fat_this = build_interface_value(raw_this, dispatch_ptr);
        std::vector<llvm::Value*> args;
        args.reserve(target.params.size());
        args.push_back(fat_this);
        for (auto arg_it = std::next(thunk->arg_begin()); arg_it != thunk->arg_end(); ++arg_it) {
            args.push_back(&*arg_it);
        }
        llvm::Function* target_fn = module_->getFunction(overload_names_.at(&target));
        llvm::Value* result = builder_->CreateCall(target_fn, args);
        if (target.return_type.kind == TypeKind::Named && target.return_type.name == "void") {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(result);
        }
        builder_->restoreIP(saved_ip);
        builder_->SetCurrentDebugLocation(saved_dbg);
        return thunk;
    }

    [[nodiscard]] llvm::Constant* interface_dispatch_entry_for(const std::string& concrete_class_name, const Function& method) {
        const Function* provider = resolve_interface_slot_provider(concrete_class_name, interface_method_slot_key(method));
        if (provider == nullptr) {
            throw CodegenError("class '" + concrete_class_name + "' has no final overrider for interface method '" +
                               method_lookup_name(method) + "'",
                current_loc_);
        }
        if (is_interface_reference_type(provider->params.front().type)) {
            return get_or_create_interface_dispatch_thunk(concrete_class_name, *provider);
        }
        llvm::Function* fn = module_->getFunction(overload_names_.at(provider));
        if (fn == nullptr) {
            throw CodegenError("missing LLVM declaration for interface dispatch target '" + provider->name + "'",
                current_loc_);
        }
        return fn;
    }

    [[nodiscard]] llvm::GlobalVariable* get_or_create_interface_dispatch_table(const std::string& concrete_class_name,
                                                                               const std::string& interface_name) {
        std::string cache_key = concrete_class_name + "->" + interface_name;
        auto it = interface_dispatch_tables_.find(cache_key);
        if (it != interface_dispatch_tables_.end()) return it->second;
        llvm::ArrayType* table_type = interface_dispatch_table_type(interface_name);
        auto* global = new llvm::GlobalVariable(*module_, table_type, /*isConstant=*/true,
                                                llvm::GlobalValue::PrivateLinkage,
                                                llvm::ConstantAggregateZero::get(table_type),
                                                "__scpp_iface_table." + cache_key);
        interface_dispatch_tables_.emplace(cache_key, global);
        std::vector<llvm::Constant*> entries;
        for (const Function* method : interface_dispatch_methods(interface_name)) {
            entries.push_back(interface_dispatch_entry_for(concrete_class_name, *method));
        }
        global->setInitializer(llvm::ConstantArray::get(table_type, entries));
        return global;
    }

    [[nodiscard]] static Type function_pointer_type_from_signature(const Type& return_type,
                                                                   const std::vector<Type>& param_types,
                                                                   bool is_unsafe) {
        Type type;
        type.kind = TypeKind::FunctionPointer;
        type.function_return = std::make_shared<Type>(return_type);
        type.function_params = param_types;
        type.is_unsafe_function_pointer = is_unsafe;
        return type;
    }

    [[nodiscard]] static bool same_function_pointer_shape_ignoring_unsafe(const Type& a, const Type& b) {
        if (a.kind != TypeKind::FunctionPointer || b.kind != TypeKind::FunctionPointer ||
            !types_equal(*a.function_return, *b.function_return) || a.function_params.size() != b.function_params.size()) {
            return false;
        }
        for (size_t i = 0; i < a.function_params.size(); i++) {
            if (!types_equal(a.function_params[i], b.function_params[i])) return false;
        }
        return true;
    }

    [[nodiscard]] std::optional<Type> resolve_function_designator_type(const Expr& expr,
                                                                       const std::optional<Type>& target_type = std::nullopt) {
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

    [[nodiscard]] llvm::Value* codegen_function_pointer_value_for_target(const Expr& expr, const Type& target_type) {
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

    // The full, human-readable-spelled-out type text ch11 §11.9's real
    // mangling scheme requires (e.g. "int", "const int&",
    // "std::unique_ptr<int>") -- mirrors cli.cppm's own type_to_string
    // (a separate module, so duplicated rather than shared; both are
    // small, stable, one-purpose functions).
    [[nodiscard]] static std::string verbatim_type_spelling(const Type& type) {
        switch (type.kind) {
            case TypeKind::Named:
                if (type.template_args.empty()) return type.name;
                {
                    std::string result = type.name + "<";
                    for (size_t i = 0; i < type.template_args.size(); i++) {
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
                for (size_t i = 0; i < type.function_params.size(); i++) {
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
                for (size_t i = 0; i < type.function_params.size(); i++) {
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

    // Splits a dotted module name ("org.lotx.cmath") into segments --
    // codegen's own copy of the parser's split_dotted_name (separate
    // module, no shared code; both are tiny, stable helpers).
    [[nodiscard]] static std::vector<std::string> split_dotted(const std::string& dotted) {
        std::vector<std::string> segments;
        size_t start = 0;
        while (start <= dotted.size()) {
            size_t dot = dotted.find('.', start);
            if (dot == std::string::npos) {
                segments.push_back(dotted.substr(start));
                break;
            }
            segments.push_back(dotted.substr(start, dot - start));
            start = dot + 1;
        }
        return segments;
    }

    // ch11 §11.9: the real cross-module mangled symbol name for an
    // exported function -- `_scppM<len>_<module>N<len>_<ns segment>...
    // F<len>_<name>P<count>_<len>_<type>...`, e.g. `sin` exported from
    // `org.lotx.cmath` under an extra `trig` nesting level mangles to
    // `_scppM14_org.lotx.cmathN4_trigF3_sinP0_`. `effective_module` is
    // `fn.owning_module` for a function recovered from an *imported*
    // module, or the current Program's own `module_name` for a function
    // this module exports itself -- either way, the segment this
    // function's own separate compilation (now or originally) produces
    // is identical, which is the entire point: an importer's `declare`
    // must match the exporter's `define` byte-for-byte.
    [[nodiscard]] std::string mangle_exported_symbol(const Function& fn) const {
        const std::string& effective_module = fn.owning_module.empty() ? program_->module_name : fn.owning_module;
        std::string mangled = "_scppM" + std::to_string(effective_module.size()) + "_" + effective_module;
        // Namespace nesting *beyond* the module's own required prefix
        // (ch11 §11.5 already requires every exported symbol's namespace
        // to start with the module's dotted name -- module names use
        // '.', namespace paths use '::', translated segment-for-segment
        // -- so that shared prefix doesn't need re-encoding here).
        std::vector<std::string> module_segments = split_dotted(effective_module);
        for (size_t i = module_segments.size(); i < fn.namespace_path.size(); i++) {
            const std::string& segment = fn.namespace_path[i];
            mangled += "N" + std::to_string(segment.size()) + "_" + segment;
        }
        // fn.name is already fully namespace-qualified (e.g.
        // "std::string_new", see the parser's qualify_name) -- the
        // mangled symbol's own <function name bytes> segment is just the
        // last "::"-separated piece (namespace nesting is separately
        // encoded by the N blocks above).
        std::string bare_name = fn.name;
        size_t last_separator = bare_name.rfind("::");
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

    // Builds overload_names_ for every function in the program. Three
    // cases, in priority order:
    //   1. Cross-module (ch11 §11.9): either exported from *this*
    //      Program's own module (fn.owning_module empty, but
    //      program_->module_name isn't, and fn.is_exported), or recovered
    //      from a *different*, already-separately-compiled module
    //      (fn.owning_module non-empty -- see the parser's
    //      merge_imported_module). Always gets the real mangled name,
    //      external linkage; already globally unique per (module,
    //      namespace, name, param types), so no extra "which overload"
    //      bookkeeping is needed on top of the mangled name itself.
    //   2. A solitary function under a name (the overwhelmingly common
    //      case for everything else) keeps its name unchanged -- this is
    //      what keeps `main`/`extern "C"` functions' names exactly as
    //      declared, and (new) what gives a module-private, non-exported
    //      function's the plain, un-mangled name ch11 §11.9 says it
    //      needs (internal linkage never risks a cross-file collision).
    //   3. 2+ functions genuinely sharing a name within this same file,
    //      *none* of which are cross-module (ch05 §5.10, local
    //      overloading) -- each gets a compact, file-local disambiguating
    //      suffix (mangle_type).
    void build_overload_names() {
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

    void declare_function(const Function& fn) {
        if (fn.return_type.kind == TypeKind::Reference) {
            validate_reference_return_elision(fn);
            validate_reference_pointee(*fn.return_type.pointee);
        }
        if (fn.is_extern_c) {
            validate_c_abi_compatible(fn.return_type, fn.name, "return type");
        }
        std::vector<llvm::Type*> param_types;
        param_types.reserve(fn.params.size());
        for (size_t i = 0; i < fn.params.size(); ++i) {
            const Param& param = fn.params[i];
            if (param.type.kind == TypeKind::Reference) {
                validate_reference_pointee(*param.type.pointee);
            }
            if (fn.is_extern_c) {
                validate_c_abi_compatible(param.type, fn.name, "parameter '" + param.name + "'");
            }
            if (is_bare_void(param.type)) {
                throw CodegenError("function '" + fn.name + "': parameter '" + param.name +
                                    "' cannot have type 'void' (only a return type or a pointer's pointee "
                                    "-- 'void*' -- may be 'void')",
                    current_loc_);
            }
            param_types.push_back(llvm_param_type_for_function(fn, param, i));
        }
        llvm::FunctionType* fn_type =
            llvm::FunctionType::get(to_llvm_type(fn.return_type), param_types, fn.has_varargs);
        // ch11 §11.9: a module-private (non-exported) function *defined*
        // in this same translation unit never needs to be visible
        // outside it -- LLVM internal linkage (the same mechanism as C's
        // `static`) guarantees zero risk of colliding with an unrelated
        // module's own same-named private helper, with no mangling
        // needed at all. A bodyless declaration (extern "C", bare
        // `extern` awaiting a separate implementation unit, or a
        // function recovered from an *imported* module -- see the
        // parser's merge_imported_module, which always clears the
        // cloned Function's body) always keeps external linkage: LLVM
        // requires a definition for internal linkage, and there's
        // nothing here to hide regardless. Every other case (a
        // non-module file's own function, or an exported one, handled
        // via overload_names_'s mangled name already) is unaffected --
        // external linkage, exactly as before this chapter.
        llvm::Function::LinkageTypes linkage = llvm::Function::ExternalLinkage;
        // ch05 §5.14: a forwarding stub (Function::forwards_to) gets a
        // real, defined body too (define_forwarding_function), just
        // never an scpp-level AST one -- eligible for the same internal
        // linkage as an ordinary defined function.
        bool has_definition = fn.body != nullptr || fn.is_defaulted || !fn.forwards_to.empty();
        if (has_definition && !fn.is_exported && !fn.is_extern_c &&
            (!fn.owning_module.empty() || !program_->module_name.empty() || fn.is_compile_time_dependency)) {
            linkage = llvm::Function::InternalLinkage;
        }
        llvm::Function::Create(fn_type, linkage, overload_names_.at(&fn), *module_);
    }

    void define_function(const Function& fn) {
        llvm::Function* llvm_fn = module_->getFunction(overload_names_.at(&fn));
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition",
                current_loc_);
        }

        current_function_def_ = &fn;
        // Mirrors movecheck's entry_state.unsafe_depth (ch01 §1.2/§1.3):
        // every function is checked by default and starts outside any
        // unsafe context, *except* one whose own declaration carries the
        // function-level `[[scpp::unsafe]]` marker (fn.is_unsafe) --
        // its entire body is an unsafe context throughout, exactly as if
        // wrapped in one `[[scpp::unsafe]] { }` block, so overflow/
        // bounds-check codegen is skipped throughout it too (see
        // codegen_binary_op/codegen_span_subscript below). Otherwise,
        // unsafe_depth_ only increases via an explicit, lexically nested
        // `[[scpp::unsafe]] { }` block within that function's own body
        // (the old "native function = implicitly unsafe everywhere"
        // concept is fully retired).
        unsafe_depth_ = fn.is_unsafe ? 1 : 0;
        attach_debug_subprogram(llvm_fn, fn);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", llvm_fn);
        builder_->SetInsertPoint(entry);
        current_loc_ = fn.loc;
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());

        locals_.clear();
        scope_stack_.clear();
        size_t index = 0;
        for (auto& arg : llvm_fn->args()) {
            const Param& param = fn.params[index++];
            arg.setName(param.name);
            llvm::AllocaInst* slot = nullptr;
            if (index == 1 && interface_destructor_uses_raw_this(fn)) {
                slot = builder_->CreateAlloca(to_llvm_type(param.type), nullptr, param.name);
                llvm::Value* fat_this = build_interface_value(
                    &arg, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)));
                create_store(fat_this, slot, alignment_for_type(param.type));
            } else {
                slot = builder_->CreateAlloca(arg.getType(), nullptr, param.name);
                builder_->CreateStore(&arg, slot);
            }
            locals_[param.name] = LocalSlot{slot, param.type};
            maybe_emit_parameter_debug_decl(param, slot, static_cast<unsigned>(index));
            if (param.type.kind == TypeKind::Named && find_class_def(param.type.name) != nullptr) {
                locals_[param.name].moved_flag = create_moved_flag_if_has_destructor(param.type.name);
            }
        }

        emit_constructor_member_initializers(fn);
        codegen_stmt(*fn.body, llvm_fn);

        // Every well-formed M1 function must return on all paths; if the
        // generated block has no terminator (e.g. missing trailing return),
        // that is a user error we surface instead of emitting invalid IR.
        if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
            throw CodegenError("function '" + fn.name + "' does not return on all paths",
                current_loc_);
        }
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }

    void define_defaulted_function(const Function& fn) {
        if (!fn.is_defaulted) {
            throw CodegenError("internal error: asked to define a non-defaulted function without a body", current_loc_);
        }
        if (!fn.name.ends_with("_delete") || fn.params.size() != 1) {
            throw CodegenError("only defaulted destructors are code-generated in this version", fn.loc);
        }

        llvm::Function* llvm_fn = module_->getFunction(overload_names_.at(&fn));
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition", fn.loc);
        }

        current_function_def_ = &fn;
        unsafe_depth_ = fn.is_unsafe ? 1 : 0;
        attach_debug_subprogram(llvm_fn, fn);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", llvm_fn);
        builder_->SetInsertPoint(entry);
        current_loc_ = fn.loc;
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());

        locals_.clear();
        scope_stack_.clear();
        size_t index = 0;
        for (auto& arg : llvm_fn->args()) {
            const Param& param = fn.params[index++];
            arg.setName(param.name);
            llvm::AllocaInst* slot = nullptr;
            if (index == 1 && interface_destructor_uses_raw_this(fn)) {
                slot = builder_->CreateAlloca(to_llvm_type(param.type), nullptr, param.name);
                llvm::Value* fat_this = build_interface_value(
                    &arg, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)));
                create_store(fat_this, slot, alignment_for_type(param.type));
            } else {
                slot = builder_->CreateAlloca(arg.getType(), nullptr, param.name);
                builder_->CreateStore(&arg, slot);
            }
            locals_[param.name] = LocalSlot{slot, param.type};
            maybe_emit_parameter_debug_decl(param, slot, static_cast<unsigned>(index));
            if (param.type.kind == TypeKind::Named && find_class_def(param.type.name) != nullptr) {
                locals_[param.name].moved_flag = create_moved_flag_if_has_destructor(param.type.name);
            }
        }

        const Type& this_type = fn.params[0].type;
        if (this_type.kind != TypeKind::Reference || this_type.pointee == nullptr || this_type.pointee->kind != TypeKind::Named) {
            throw CodegenError("defaulted destructor '" + fn.name + "' has an invalid this parameter", fn.loc);
        }
        const std::string& class_name = this_type.pointee->name;
        auto info_it = structs_.find(class_name);
        if (info_it == structs_.end()) {
            throw CodegenError("defaulted destructor '" + fn.name + "' names unknown class '" + class_name + "'", fn.loc);
        }

        llvm::Type* this_llvm_type = to_llvm_type(fn.params[0].type);
        llvm::Value* this_ptr = builder_->CreateLoad(this_llvm_type, locals_.at("this").alloca, "thisptr");
        const StructInfo& info = info_it->second;
        for (size_t i = info.field_types.size(); i > 0; --i) {
            const Type& field_type = info.field_types[i - 1];
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                llvm::Value* field_ptr =
                    builder_->CreateStructGEP(info.llvm_type, this_ptr, i - 1, info.field_names[i - 1]);
                codegen_destroy_old_class_state_for_move_assign(field_ptr, field_type.name);
            }
        }

        builder_->CreateRetVoid();
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }

    // ch05 §5.14: emits a thin, codegen-only wrapper body for a
    // Function::forwards_to stub (an inherited method a derived class
    // doesn't itself override, synthesized by movecheck's
    // synthesize_inherited_method_forwards) -- calls the real target
    // function directly, forwarding every LLVM argument (including
    // `this`) completely unchanged: the derived class's own flattened,
    // base-first layout (declare_class) already makes its leading bytes
    // byte-identical to the base's own full layout, so no pointer
    // adjustment/bitcast is needed at all (LLVM's opaque pointers carry
    // no type information to begin with). `fn.body` is always null for
    // one of these (see Function::forwards_to's own comment) -- this is
    // the *only* place that ever runs for it, never codegen_stmt/
    // codegen_expr.
    void define_forwarding_function(const Function& fn) {
        llvm::Function* llvm_fn = module_->getFunction(overload_names_.at(&fn));
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition",
                current_loc_);
        }
        // Finds the exact base method this stub forwards to: `name`
        // alone isn't necessarily unique (ch05 §5.10 method
        // overloading), but this stub's own params[1:] were copied
        // verbatim from that exact overload at synthesis time, so
        // matching on both name and every non-`this` parameter's type
        // is unambiguous.
        const Function* target = nullptr;
        for (const Function& candidate : program_->functions) {
            if (candidate.name != fn.forwards_to || candidate.params.size() != fn.params.size()) continue;
            bool params_match = true;
            for (size_t i = 1; i < fn.params.size() && params_match; i++) {
                params_match = types_equal(candidate.params[i].type, fn.params[i].type);
            }
            if (params_match) {
                target = &candidate;
                break;
            }
        }
        if (target == nullptr) {
            throw CodegenError("forwarding stub '" + fn.name + "' names an unknown target '" + fn.forwards_to + "'",
                current_loc_);
        }
        llvm::Function* target_llvm = module_->getFunction(overload_names_.at(target));

        attach_debug_subprogram(llvm_fn, fn);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", llvm_fn);
        builder_->SetInsertPoint(entry);
        current_loc_ = fn.loc;
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        std::vector<llvm::Value*> args;
        args.reserve(llvm_fn->arg_size());
        for (auto& arg : llvm_fn->args()) args.push_back(&arg);
        llvm::Value* call_result = nullptr;
        if (!fn.params.empty() && is_interface_reference_type(fn.params.front().type)) {
            std::optional<size_t> slot_index = interface_method_slot_index(fn.member_owner_class, fn);
            if (!slot_index.has_value()) {
                throw CodegenError("missing interface dispatch slot for forwarding stub '" + fn.name + "'", current_loc_);
            }
            llvm::Value* receiver_value = args.front();
            llvm::Value* dispatch_ptr = extract_interface_dispatch_ptr(receiver_value);
            llvm::ArrayType* table_type = interface_dispatch_table_type(fn.member_owner_class);
            llvm::Value* table_ptr = builder_->CreateBitCast(dispatch_ptr, llvm::PointerType::get(*context_, 0),
                                                             "ifacetable");
            llvm::Value* slot_ptr =
                builder_->CreateGEP(table_type, table_ptr,
                                    {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_),
                                                           static_cast<unsigned>(*slot_index))},
                                    "ifaceslot");
            llvm::Value* target_ptr =
                create_load(llvm::PointerType::getUnqual(*context_), slot_ptr, std::nullopt, "ifacemethod");
            std::vector<llvm::Value*> dispatch_args;
            dispatch_args.reserve(args.size());
            dispatch_args.push_back(extract_interface_object_ptr(receiver_value));
            for (size_t i = 1; i < args.size(); ++i) dispatch_args.push_back(args[i]);
            call_result = builder_->CreateCall(interface_dispatch_function_type(*target), target_ptr, dispatch_args);
        } else if (!fn.params.empty() && !target->params.empty() && is_interface_reference_type(target->params.front().type)) {
            const std::string& concrete_class_name = fn.params.front().type.pointee->name;
            const std::string& target_interface_name = target->params.front().type.pointee->name;
            llvm::Value* fat_receiver =
                build_interface_value(args.front(), get_or_create_interface_dispatch_table(concrete_class_name,
                                                                                           target_interface_name));
            std::vector<llvm::Value*> direct_args;
            direct_args.reserve(args.size());
            direct_args.push_back(fat_receiver);
            for (size_t i = 1; i < args.size(); ++i) direct_args.push_back(args[i]);
            call_result = builder_->CreateCall(target_llvm, direct_args);
        } else {
            call_result = builder_->CreateCall(target_llvm, args);
        }
        if (is_bare_void(fn.return_type)) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(call_result);
        }
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }

    void codegen_stmt(const Stmt& stmt, llvm::Function* current_function) {
        // Refreshed on every call (including each recursive call for a
        // nested statement) so a CodegenError thrown while handling
        // `stmt` points at `stmt` itself -- see current_loc_ and
        // codegen_expr's identical opening comment.
        refresh_debug_location(stmt.loc);
        switch (stmt.kind) {
            case StmtKind::Block:
                push_scope();
                // ch01 §1.3 / ch05 §5.8: an `unsafe { }` block raises the
                // depth counter for its own statements (and anything
                // transitively nested inside it) so codegen_binary knows
                // to emit plain, guaranteed-wrapping arithmetic instead of
                // the overflow-checked form -- mirrors movecheck's own
                // UnsafeEnter/UnsafeExit MIR statements.
                if (stmt.is_unsafe) unsafe_depth_++;
                for (const auto& s : stmt.statements) {
                    // Once a block has a terminator (return), skip anything
                    // after it: unreachable code shouldn't be lowered.
                    if (builder_->GetInsertBlock()->getTerminator() != nullptr) break;
                    codegen_stmt(*s, current_function);
                }
                if (stmt.is_unsafe) unsafe_depth_--;
                pop_scope();
                return;

            case StmtKind::VarDecl: {
                if (stmt.type.kind == TypeKind::Reference) {
                    // Real C++ references must be bound at declaration
                    // (there's no such thing as a later-bound or
                    // "null" reference) -- unlike every other type, which
                    // zero-initializes when no initializer is given.
                    if (!stmt.init) {
                        throw CodegenError("reference '" + stmt.var_name +
                                            "' must be initialized (bound to a variable) at declaration",
                            current_loc_);
                    }
                    if (is_interface_reference_type(stmt.type)) {
                        llvm::AllocaInst* slot = create_entry_block_alloca(to_llvm_type(stmt.type), stmt.var_name);
                        create_store(codegen_interface_value_for_target(*stmt.init, stmt.type), slot, alignment_for_type(stmt.type));
                        locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                        locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                        maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(stmt.var_name);
                        }
                        return;
                    }
                    validate_reference_pointee(*stmt.type.pointee);
                    // ch05 §5.x: a *const* reference may bind directly to
                    // a fresh rvalue initializer (a literal, std::move/
                    // std::make_unique, a lambda literal, or a call not
                    // itself returning by reference) -- movecheck has
                    // already validated this (produces_rvalue_of_type,
                    // only ever for a non-mutable reference), so it only
                    // remains to materialize a temporary and use *its*
                    // address, exactly like codegen_call_args' identical
                    // handling of the same shapes for a reference call
                    // argument. Otherwise (the overwhelmingly common
                    // case): codegen_lvalue on the initializer gives the
                    // address directly, and also enforces it resolves to
                    // a real, addressable place (a plain variable, or a
                    // further member/subscript chain off one).
                    llvm::Value* referent_addr =
                        !stmt.type.is_mutable_ref && produces_rvalue_of_type(*stmt.init, *stmt.type.pointee)
                            ? codegen_materialize_rvalue_reference_source(*stmt.init)
                            : codegen_lvalue(*stmt.init).ptr;
                    llvm::AllocaInst* slot =
                        create_entry_block_alloca(llvm::PointerType::getUnqual(*context_), stmt.var_name);
                    builder_->CreateStore(referent_addr, slot);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                    locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                    maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    return;
                }

                if (stmt.type.kind == TypeKind::Span) {
                    // Like a reference, a std::span<T> must be bound to a
                    // place at declaration -- v0.1 only supports
                    // constructing one from a fixed-size array (spec
                    // ch06/M6; std::vector doesn't exist yet).
                    if (!stmt.init) {
                        throw CodegenError("span '" + stmt.var_name +
                                            "' must be initialized (bound to an array) at declaration",
                            current_loc_);
                    }
                    llvm::Type* span_type = to_llvm_type(stmt.type);
                    llvm::Value* span_value = codegen_span_value_for_target(*stmt.init, stmt.type);
                    llvm::AllocaInst* slot = create_entry_block_alloca(span_type, stmt.var_name);
                    builder_->CreateStore(span_value, slot);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                    locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                    maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    return;
                }

                if (is_bare_void(stmt.type)) {
                    throw CodegenError("variable '" + stmt.var_name +
                                        "' cannot have type 'void' (only a return type or a pointer's "
                                        "pointee -- 'void*' -- may be 'void')",
                        current_loc_);
                }

                // ch05 §5.12: `auto f = [...];` -- the only spelling
                // that gives a class-typed VarDecl a plain `= expr`
                // initializer rather than `ClassName name{args};`'s own
                // constructor-call syntax (movecheck's closure-
                // resolution pass gives a synthesized closure class no
                // constructor at all). A Lambda literal's own codegen
                // (codegen_construct_lambda) already allocates and fully
                // populates its own fresh instance -- exactly the
                // storage `f` itself should use -- so `f` is aliased
                // directly to that address rather than allocating a
                // *second*, separate slot and trying to copy into it
                // (which would be wrong regardless: a class-typed
                // value's own codegen representation is always its
                // address, never a loadable/storable flat value, unlike
                // every scalar/struct/array/pointer type the general
                // path below handles).
                if (stmt.init && stmt.init->kind == ExprKind::Lambda) {
                    llvm::AllocaInst* closure_ptr = create_entry_block_alloca(to_llvm_type(stmt.type), stmt.var_name);
                    codegen_construct_lambda(*stmt.init, closure_ptr);
                    locals_[stmt.var_name] = LocalSlot{closure_ptr, stmt.type};
                    locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                    maybe_emit_local_debug_decl(stmt.var_name, stmt.type, closure_ptr, stmt.loc);
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    return;
                }

                llvm::Type* llvm_type = to_llvm_type(stmt.type);
                llvm::AllocaInst* slot = create_entry_block_alloca(llvm_type, stmt.var_name);
                if (stmt.has_ctor_args) {
                    // `ClassName name{args};` (ch04 §4.2 / spec §6.1):
                    // direct-
                    // initialization via an explicit constructor call.
                    // Storage is zero-initialized first -- same as every
                    // other VarDecl with no initializer at all (scpp has
                    // no concept of "uninitialized" memory, ch05.4) --
                    // then the synthesized `ClassName_new(&name, args...)`
                    // constructor runs in place: the same caller-
                    // allocates/constructor-initializes-in-place ABI shape
                    // real C++ itself already uses, so this needs no new
                    // storage-layout logic beyond what every other
                    // Named-type VarDecl already does above.
                    zero_initialize_storage(slot, stmt.type);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                    locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                    if (stmt.type.kind == TypeKind::Named) {
                        locals_[stmt.var_name].moved_flag = create_moved_flag_if_has_destructor(stmt.type.name);
                    }
                    maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    if (stmt.type.kind != TypeKind::Named || !structs_.contains(stmt.type.name)) {
                        initialize_storage_from_brace_args(LValue{slot, stmt.type, std::nullopt}, stmt.ctor_args);
                        return;
                    }
                    if (try_initialize_class_storage_from_same_type_source(LValue{slot, stmt.type, std::nullopt},
                                                                           stmt.ctor_args)) {
                        return;
                    }
                    std::string ctor_name = stmt.type.name + "_new";
                    // ch05 §5.10: a class may declare multiple
                    // constructors (all synthesized as "ClassName_new"),
                    // resolved by exact argument-type match exactly like
                    // any other overloaded name.
                    const Function* ctor_def = resolve_overload_by_type(ctor_name, stmt.ctor_args, /*param_offset=*/1);
                    if (ctor_def == nullptr) {
                        const ClassDef* class_def = find_class_def(stmt.type.name);
                        if (stmt.ctor_args.empty() && class_def == nullptr) {
                            return;
                        }
                        if (stmt.ctor_args.empty() && class_def != nullptr && !class_has_any_constructor(stmt.type.name)) {
                            emit_default_initializers_for_class_storage(slot, *class_def);
                            return;
                        }
                        // spec §6.5: `ClassName y{x};` with no matching
                        // user-declared constructor found by ordinary
                        // resolution just above (which would already
                        // have found a user-declared copy constructor,
                        // if one exists, since it's registered like any
                        // other overload) -- if this is a bare (non-
                        // move) same-type single argument and the class
                        // is copy-constructible, synthesize the
                        // compiler-provided recursive memberwise copy
                        // directly, exactly like move construction's own
                        // analogous fallback above.
                        throw CodegenError("class '" + stmt.type.name + "' has no constructor matching this call",
                            current_loc_);
                    }
                    if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                        llvm::Value* value = codegen_constructed_class_value(stmt.type.name, stmt.ctor_args, ctor_def);
                        create_store(value, slot, std::nullopt);
                        return;
                    }
                    llvm::Function* ctor = module_->getFunction(overload_names_.at(ctor_def));
                    if (ctor == nullptr) {
                        throw CodegenError("class '" + stmt.type.name + "' has no constructor matching this call",
                            current_loc_);
                    }
                    std::vector<llvm::Value*> args = codegen_call_args(stmt.ctor_args, ctor_def, /*param_offset=*/1);
                    args.insert(args.begin(), slot);
                    builder_->CreateCall(ctor, args);
                    return;
                }
                if (stmt.init) {
                    if (stmt.type.kind == TypeKind::Named && structs_.contains(stmt.type.name) &&
                        stmt.init->kind == ExprKind::Identifier) {
                        // spec §6.5: `ClassName y = x;` -- copy
                        // construction (movecheck has already verified
                        // `x` is the exact same class type and that the
                        // class is copy-constructible). Dispatch to the
                        // user-declared copy constructor if one exists
                        // (a real function call, so any side effects --
                        // e.g. incrementing a reference count, spec
                        // §6.5's own worked example -- actually run,
                        // unlike a blind byte copy); otherwise the
                        // compiler-provided recursive memberwise copy.
                        LValue src = codegen_lvalue(*stmt.init);
                        if (const Function* user_ctor = find_user_declared_copy_ctor_ast(stmt.type.name)) {
                            llvm::Function* ctor = module_->getFunction(overload_names_.at(user_ctor));
                            builder_->CreateCall(ctor, {slot, src.ptr});
                        } else {
                            codegen_memberwise_copy_construct(slot, src.ptr, stmt.type.name);
                        }
                        locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                        locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                        locals_[stmt.var_name].moved_flag = create_moved_flag_if_has_destructor(stmt.type.name);
                        maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                        if (!scope_stack_.empty()) {
                            scope_stack_.back().push_back(stmt.var_name);
                        }
                        return;
                    }
                    llvm::Value* init_value = codegen_value_for_target(*stmt.init, stmt.type);
                    // Refresh to `stmt`'s own position: codegen_expr just
                    // recursed through `stmt.init` (possibly a compound
                    // expression like `a + b`), leaving current_loc_ at
                    // whichever sub-expression it last visited rather
                    // than the statement check_store_type is actually
                    // about.
                    refresh_debug_location(stmt.loc);
                    check_store_type(init_value, llvm_type, "variable '" + stmt.var_name + "'");
                    builder_->CreateStore(init_value, slot);
                } else {
                    // scpp has no concept of an uninitialized variable: a
                    // local declared without an initializer is always
                    // zero-initialized (0 / false / null / all-zero
                    // fields), for every type -- scalars and raw pointers
                    // included, not just struct/array/unique_ptr.
                    zero_initialize_storage(slot, stmt.type);
                }
                locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                locals_[stmt.var_name].is_const = stmt.is_const || stmt.is_constexpr;
                if (stmt.type.kind == TypeKind::Named) {
                    locals_[stmt.var_name].moved_flag = create_moved_flag_if_has_destructor(stmt.type.name);
                }
                maybe_emit_local_debug_decl(stmt.var_name, stmt.type, slot, stmt.loc);
                if (!scope_stack_.empty()) {
                    scope_stack_.back().push_back(stmt.var_name);
                }
                return;
            }

            case StmtKind::Return: {
                // Evaluate the return value *before* freeing owned locals:
                // `return std::move(a);` nulls out `a`'s slot as a side
                // effect of the move, so by the time we free every
                // unique_ptr local below, an already-moved-from one is
                // safely a no-op (free(NULL) is well-defined) while a
                // still-owning one is correctly released.
                //
                // When *this* function's own return type is a reference,
                // the returned expression is an addressable place
                // (movecheck's dangling check -- see
                // resolve_borrow_source_root -- only allows an
                // Identifier/Member/Subscript chain here), and returning
                // it means returning that address, not its current value
                // -- codegen_lvalue, not codegen_expr (which would
                // auto-dereference it, same as any other read).
                llvm::Value* value = nullptr;
                if (stmt.expr) {
                    if (current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                        current_function_def_->return_type.name == "void") {
                        codegen_expr(*stmt.expr);
                    } else {
                        value = current_function_def_ != nullptr && is_interface_reference_type(current_function_def_->return_type)
                                    ? codegen_interface_value_for_target(*stmt.expr, current_function_def_->return_type)
                                : current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Reference
                                    ? codegen_lvalue(*stmt.expr).ptr
                                    : current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                                          find_class_def(current_function_def_->return_type.name) != nullptr &&
                                          is_implicit_move_return_source(*stmt.expr, current_function_def_->return_type)
                                          ? [&]() {
                                                Expr implicit_move;
                                                implicit_move.kind = ExprKind::Move;
                                                implicit_move.loc = stmt.expr->loc;
                                                implicit_move.lhs = clone_expr(*stmt.expr);
                                                return codegen_expr(implicit_move);
                                            }()
                                    : current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Named &&
                                          find_class_def(current_function_def_->return_type.name) != nullptr
                                          ? codegen_class_value_for_boundary(*stmt.expr, current_function_def_->return_type)
                                    : current_function_def_ != nullptr
                                          ? codegen_value_for_target(*stmt.expr, current_function_def_->return_type)
                                          : codegen_expr(*stmt.expr);
                    }
                }
                free_unique_ptr_locals();
                if (value != nullptr) {
                    builder_->CreateRet(value);
                } else {
                    builder_->CreateRetVoid();
                }
                return;
            }

            case StmtKind::ExprStmt:
                if (stmt.expr && stmt.expr->kind == ExprKind::Delete) {
                    codegen_delete_expr(*stmt.expr);
                    return;
                }
                if (stmt.expr && stmt.expr->kind == ExprKind::Destroy) {
                    codegen_destroy_expr(*stmt.expr);
                    return;
                }
                codegen_expr(*stmt.expr);
                return;

            case StmtKind::If: {
                // `stmt.condition` is a `bool` expression, stored/passed
                // as i8 (see to_llvm_type) -- CreateCondBr needs a 1-bit
                // condition, so narrow it right here (see bool_to_i1).
                llvm::Value* cond = codegen_contextual_bool_i1(*stmt.condition);
                llvm::BasicBlock* then_block = llvm::BasicBlock::Create(*context_, "if.then", current_function);
                llvm::BasicBlock* else_block = llvm::BasicBlock::Create(*context_, "if.else", current_function);
                llvm::BasicBlock* merge_block = llvm::BasicBlock::Create(*context_, "if.end", current_function);

                builder_->CreateCondBr(cond, then_block, else_block);

                // then/else each get their own scope so a unique_ptr
                // declared in one branch (with or without braces -- a
                // bare `if (c) unique_ptr<T> x = ...;` is valid grammar,
                // same as real C++) is dropped at the end of *that*
                // branch, not left dangling in the flat locals_ map.
                builder_->SetInsertPoint(then_block);
                push_scope();
                codegen_stmt(*stmt.then_branch, current_function);
                pop_scope();
                if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
                    builder_->CreateBr(merge_block);
                }

                builder_->SetInsertPoint(else_block);
                push_scope();
                if (stmt.else_branch) {
                    codegen_stmt(*stmt.else_branch, current_function);
                }
                pop_scope();
                if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
                    builder_->CreateBr(merge_block);
                }

                builder_->SetInsertPoint(merge_block);
                if (merge_block->hasNPredecessorsOrMore(1)) {
                    return;
                }
                // Both branches terminated (e.g. returned), so this merge
                // point is unreachable; give it a terminator anyway since
                // every basic block must end with one, and let the caller
                // see the *original* branches' terminators, not this dead
                // block, when checking "does this path return?".
                builder_->CreateUnreachable();
                return;
            }

            case StmtKind::While: {
                llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(*context_, "while.cond", current_function);
                llvm::BasicBlock* body_block = llvm::BasicBlock::Create(*context_, "while.body", current_function);
                llvm::BasicBlock* end_block = llvm::BasicBlock::Create(*context_, "while.end", current_function);

                builder_->CreateBr(cond_block);

                builder_->SetInsertPoint(cond_block);
                // Same bool_to_i1 narrowing as the If case above.
                llvm::Value* cond = codegen_contextual_bool_i1(*stmt.condition);
                builder_->CreateCondBr(cond, body_block, end_block);

                // The body's scope is popped (and its unique_ptr locals
                // dropped) at the end of *every* iteration, right before
                // jumping back to re-check the condition -- so a
                // unique_ptr re-declared each iteration doesn't leak the
                // previous iteration's allocation.
                builder_->SetInsertPoint(body_block);
                push_scope();
                loop_stack_.push_back(LoopFrame{cond_block, end_block, scope_stack_.size()});
                codegen_stmt(*stmt.then_branch, current_function);
                pop_scope();
                loop_stack_.pop_back();
                if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
                    builder_->CreateBr(cond_block);
                }

                builder_->SetInsertPoint(end_block);
                return;
            }

            case StmtKind::Break:
                if (!loop_stack_.empty()) {
                    emit_scope_cleanup_to_depth(loop_stack_.back().scope_depth);
                    builder_->CreateBr(loop_stack_.back().end_block);
                }
                return;

            case StmtKind::Continue:
                if (!loop_stack_.empty()) {
                    emit_scope_cleanup_to_depth(loop_stack_.back().scope_depth);
                    builder_->CreateBr(loop_stack_.back().cond_block);
                }
                return;
        }
    }

    // Builds and emits the actual `call` instruction for `expr` (a Call
    // expression naming a real, non-builtin function -- callers handle
    // `print_int`/`print_bool` themselves before reaching here), binding
    // each reference-typed argument to its address rather than its
    // value. The raw LLVM result: if the callee returns a reference,
    // that result is still just the *address* at this point (see
    // to_llvm_type's Reference case) -- it's up to the caller to decide
    // whether to dereference it (codegen_expr, for a value context) or
    // hand the address on as-is (codegen_lvalue, for a reference-
    // returning call used itself as a further borrow source -- see
    // resolve_borrow_source_root in movecheck.cppm). Also returns the
    // resolved callee's own Function record (see CallResult) so both
    // call sites can answer that question without re-resolving anything.
    //
    // Method call handling (ch05 §5.9): if `expr.lhs` is non-null
    // (`obj.method(args)`), the receiver `obj` is resolved *once* here
    // (codegen_lvalue has real side effects -- e.g. a span subscript's
    // bounds check -- so it must never run twice for the same syntactic
    // receiver) to find its static type, which supplies `ClassName` for
    // the synthesized `ClassName_method` symbol (scpp has no real C++
    // name mangling; this recomputes the identical deterministic scheme
    // parse_class_def used to create the method in the first place --
    // see ClassDef's own comment) -- then `&obj` is passed as the
    // implicit first (`this`) argument, ahead of the explicit ones.
    CallResult codegen_call(const Expr& expr) {
        if (expr.lhs != nullptr && !expr.name.empty() && expr.lhs->kind != ExprKind::Lambda) {
            std::optional<Type> receiver_type = infer_type(*expr.lhs);
            if (receiver_type.has_value()) {
                const Type& receiver_named =
                    receiver_type->kind == TypeKind::Reference && receiver_type->pointee ? *receiver_type->pointee : *receiver_type;
                if (receiver_named.kind == TypeKind::Named && type_names_interface(receiver_named.name)) {
                    const Function* callee =
                        resolve_overload_by_type(receiver_named.name + "_" + expr.name, expr.args, /*param_offset=*/1,
                                                 !is_read_only_place(*expr.lhs), expr.lhs.get());
                    if (callee == nullptr) {
                        throw CodegenError("call to unknown function '" + receiver_named.name + "_" + expr.name + "'",
                                           current_loc_);
                    }
                    llvm::Value* receiver_value = codegen_expr(*expr.lhs);
                    if (!callee->is_virtual) {
                        llvm::Function* target = module_->getFunction(overload_names_.at(callee));
                        std::vector<llvm::Value*> args = codegen_call_args(expr.args, callee, /*param_offset=*/1);
                        args.insert(args.begin(), receiver_value);
                        return CallResult{builder_->CreateCall(target, args), callee};
                    }
                    std::optional<size_t> slot_index = interface_method_slot_index(receiver_named.name, *callee);
                    if (!slot_index.has_value()) {
                        throw CodegenError("missing interface dispatch slot for '" + callee->name + "'", current_loc_);
                    }
                    llvm::Value* dispatch_ptr = extract_interface_dispatch_ptr(receiver_value);
                    llvm::ArrayType* table_type = interface_dispatch_table_type(receiver_named.name);
                    llvm::Value* table_ptr =
                        builder_->CreateBitCast(dispatch_ptr, llvm::PointerType::get(*context_, 0), "ifacetable");
                    llvm::Value* slot_ptr =
                        builder_->CreateGEP(table_type, table_ptr,
                                            {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                                             llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_),
                                                                   static_cast<unsigned>(*slot_index))},
                                            "ifaceslot");
                    llvm::Value* target_ptr =
                        create_load(llvm::PointerType::getUnqual(*context_), slot_ptr, std::nullopt, "ifacemethod");
                    std::vector<llvm::Value*> args = codegen_call_args(expr.args, callee, /*param_offset=*/1);
                    args.insert(args.begin(), extract_interface_object_ptr(receiver_value));
                    return CallResult{builder_->CreateCall(interface_dispatch_function_type(*callee), target_ptr, args), callee};
                }
            }
            LValue base = codegen_lvalue(*expr.lhs);
            if (base.type.kind == TypeKind::Named && structs_.contains(base.type.name)) {
                const StructInfo& info = structs_.at(base.type.name);
                std::optional<size_t> field_index_opt = info.find_field_index(expr.name);
                if (field_index_opt.has_value() &&
                    info.field_types[*field_index_opt].kind == TypeKind::FunctionPointer) {
                    const Type& member_type = info.field_types[*field_index_opt];
                    llvm::Value* field_ptr = info.is_union
                                                 ? builder_->CreateBitCast(base.ptr,
                                                                           llvm::PointerType::get(
                                                                               to_llvm_type(member_type)->getContext(),
                                                                               0),
                                                                           expr.name + ".fnptr")
                                                 : builder_->CreateStructGEP(info.llvm_type, base.ptr, *field_index_opt,
                                                                             expr.name + ".fnptr");
                    llvm::Value* callee_value =
                        create_load(to_llvm_type(member_type), field_ptr,
                                    info.is_union ? base.alignment
                                                  : (info.is_packed ? std::optional<llvm::Align>(llvm::Align(1))
                                                                    : std::nullopt),
                                    expr.name + ".fn");
                    std::vector<llvm::Value*> args =
                        codegen_call_args_for_types(expr.args, member_type.function_params);
                    auto* fn_type =
                        llvm::FunctionType::get(to_llvm_type(*member_type.function_return),
                                                [&]() {
                                                    std::vector<llvm::Type*> params;
                                                    params.reserve(member_type.function_params.size());
                                                    for (const Type& param : member_type.function_params) {
                                                        params.push_back(to_llvm_type(param));
                                                    }
                                                    return params;
                                                }(),
                                                /*isVarArg=*/false);
                    return CallResult{builder_->CreateCall(fn_type, callee_value, args), nullptr};
                }
            }
            if (receiver_type.has_value() && receiver_type->kind == TypeKind::FunctionPointer) {
                llvm::Value* callee_value = codegen_expr(*expr.lhs);
                std::vector<llvm::Value*> args = codegen_call_args_for_types(expr.args, receiver_type->function_params);
                auto* fn_type =
                    llvm::FunctionType::get(to_llvm_type(*receiver_type->function_return),
                                            [&]() {
                                                std::vector<llvm::Type*> params;
                                                params.reserve(receiver_type->function_params.size());
                                                for (const Type& param : receiver_type->function_params) {
                                                    params.push_back(to_llvm_type(param));
                                                }
                                                return params;
                                            }(),
                                            /*isVarArg=*/false);
                return CallResult{builder_->CreateCall(fn_type, callee_value, args), nullptr};
            }
        }
        if (expr.lhs != nullptr && expr.name.empty()) {
            const Expr* callee_expr = expr.lhs.get();
            if (callee_expr->kind == ExprKind::Unary && callee_expr->unary_op == UnaryOp::Deref && callee_expr->lhs) {
                callee_expr = callee_expr->lhs.get();
            }
            std::optional<Type> callee_type = infer_type(*callee_expr);
            if (!callee_type.has_value() || callee_type->kind != TypeKind::FunctionPointer) {
                throw CodegenError("indirect call requires a function pointer value", current_loc_);
            }
            llvm::Value* callee_value = codegen_expr(*callee_expr);
            std::vector<llvm::Value*> args = codegen_call_args_for_types(expr.args, callee_type->function_params);
            auto* fn_type =
                llvm::FunctionType::get(to_llvm_type(*callee_type->function_return),
                                        [&]() {
                                            std::vector<llvm::Type*> params;
                                            params.reserve(callee_type->function_params.size());
                                            for (const Type& param : callee_type->function_params) {
                                                params.push_back(to_llvm_type(param));
                                            }
                                            return params;
                                        }(),
                                        /*isVarArg=*/false);
            return CallResult{builder_->CreateCall(fn_type, callee_value, args), nullptr};
        }
        if (expr.lhs == nullptr) {
            if (const Function* builtin_callee = resolve_overload_by_type(expr.name, expr.args, /*param_offset=*/0);
                builtin_callee != nullptr && is_enum_cast_store_builtin_name(builtin_callee->name)) {
                return codegen_enum_cast_store_builtin(expr, *builtin_callee);
            }
            if (find_class_def(expr.name) != nullptr) {
                const Function* ctor_def = nullptr;
                if (!expr.args.empty() || expr.has_paren_init) {
                    std::string ctor_name = expr.name + "_new";
                    ctor_def = resolve_overload_by_type(ctor_name, expr.args, /*param_offset=*/1);
                    if (ctor_def == nullptr) {
                        if (expr.args.empty()) {
                            return CallResult{codegen_constructed_class_value(expr.name, expr.args, nullptr, &expr), nullptr};
                        }
                        throw CodegenError("class '" + expr.name + "' has no constructor matching this call", current_loc_);
                    }
                }
                return CallResult{codegen_constructed_class_value(expr.name, expr.args, ctor_def, &expr), nullptr};
            }
            auto local_it = expr.explicit_global_qualification ? locals_.end() : locals_.find(expr.name);
            if (local_it != locals_.end() && local_it->second.type.kind == TypeKind::FunctionPointer) {
                llvm::Value* callee_value = builder_->CreateLoad(to_llvm_type(local_it->second.type), local_it->second.alloca,
                                                                 expr.name + ".fnptr");
                std::vector<llvm::Value*> args = codegen_call_args_for_types(expr.args, local_it->second.type.function_params);
                auto* fn_type =
                    llvm::FunctionType::get(to_llvm_type(*local_it->second.type.function_return),
                                            [&]() {
                                                std::vector<llvm::Type*> params;
                                                params.reserve(local_it->second.type.function_params.size());
                                                for (const Type& param : local_it->second.type.function_params) {
                                                    params.push_back(to_llvm_type(param));
                                                }
                                                return params;
                                            }(),
                                            /*isVarArg=*/false);
                return CallResult{builder_->CreateCall(fn_type, callee_value, args), nullptr};
            }
        }
        std::string callee_name = expr.name;
        llvm::Value* this_arg = nullptr;
        size_t param_offset = 0;
        bool receiver_is_mutable = true;
        if (expr.lhs != nullptr) {
            LValue receiver = codegen_lvalue(*expr.lhs);
            if (receiver.type.kind != TypeKind::Named) {
                throw CodegenError("method call '." + expr.name + "(...)' is only supported on a class type",
                    current_loc_);
            }
            callee_name = receiver.type.name + "_" + expr.name;
            this_arg = receiver.ptr;
            param_offset = 1;
            receiver_is_mutable = !is_read_only_place(*expr.lhs);
        }

        // ch05 §5.10: resolve the specific overload this call targets
        // (movecheck has already independently confirmed exactly one
        // overload matches, so this is expected to agree with it -- see
        // resolve_overload_by_type's own comment) *before* generating
        // this call's own arguments below: codegen_call_args needs
        // `callee_def` already in hand to decide value-vs-address per
        // parameter.
        const Function* callee_def =
            resolve_overload_by_type(callee_name, expr.args, param_offset, receiver_is_mutable, expr.lhs.get());
        if (callee_def == nullptr) {
            throw CodegenError("call to unknown function '" + callee_name + "'",
                current_loc_);
        }
        llvm::Function* callee = module_->getFunction(overload_names_.at(callee_def));
        if (callee == nullptr) {
            throw CodegenError("call to unknown function '" + callee_name + "'",
                current_loc_);
        }
        std::vector<llvm::Value*> args = codegen_call_args(expr.args, callee_def, param_offset);
        if (this_arg != nullptr) {
            if (!callee_def->params.empty() && is_interface_reference_type(callee_def->params.front().type)) {
                args.insert(args.begin(), codegen_interface_value_for_target(*expr.lhs, callee_def->params.front().type));
            } else {
                args.insert(args.begin(), this_arg);
            }
        }
        return CallResult{builder_->CreateCall(callee, args), callee_def};
    }

    // Builds the LLVM argument list for a call to `callee_def` (nullable
    // -- an unresolvable callee still needs *some* argument list, just
    // with no reference-parameter information to consult), given how many
    // leading parameters are already spoken for before `args[0]` --
    // `param_offset` is 1 when an implicit `this` occupies params[0] (a
    // method call or a constructor call, ch04 §4.2/ch05 §5.9), 0
    // otherwise. Shared by codegen_call and the VarDecl constructor-call
    // case below so both resolve "is this argument's target parameter a
    // reference" identically.
    // Materializes a temporary holding `expr`'s own *value* and returns
    // its address -- what a reference binds to when its source is a
    // genuine rvalue (a literal, std::move/std::make_unique, a lambda
    // literal, or a call that doesn't itself return by reference) rather
    // than an existing addressable place, exactly like real C++'s own
    // temporary materialization. Shared by codegen_call_args (a `T&&`
    // parameter, unconditionally, or a *const* `T&`/`Concept auto&`
    // parameter bound directly to an rvalue argument, ch05 §5.x) and the
    // VarDecl BindReference case below (`const T& r = <rvalue>;`) -- both
    // already move-checked (produces_rvalue_of_type) to guarantee `expr`
    // is one of the shapes handled here. A Lambda literal is special-
    // cased: its own codegen (codegen_construct_lambda, via codegen_expr's
    // Lambda case) already allocates a fresh temporary and returns *its*
    // address directly (a class value is always represented/passed by
    // address in this codebase, never as a bare aggregate SSA value) --
    // using that address as-is avoids double-wrapping it in yet another
    // temporary, which would produce "a pointer to a pointer to the
    // closure" instead of "a pointer to the closure".
    llvm::Value* codegen_materialize_rvalue_reference_source(const Expr& expr) {
        if (expr.kind == ExprKind::Lambda) return codegen_expr(expr);
        // Also reuses std::move's own codegen unchanged, including its
        // "null out the source slot" side effect when the moved value is
        // itself a std::unique_ptr/class.
        llvm::Value* value = codegen_expr(expr);
        llvm::AllocaInst* temp = create_entry_block_alloca(value->getType(), "rvaluetmp");
        builder_->CreateStore(value, temp);
        return temp;
    }

    llvm::Value* codegen_materialize_const_reference_source(const Expr& expr, const Type& target_type) {
        if (produces_rvalue_of_type(expr, target_type)) {
            return codegen_materialize_rvalue_reference_source(expr);
        }
        llvm::Type* llvm_type = to_llvm_type(target_type);
        llvm::AllocaInst* temp = create_entry_block_alloca(llvm_type, "constreftmp");
        if (target_type.kind == TypeKind::Named && find_class_def(target_type.name) != nullptr) {
            create_store(codegen_class_value_for_boundary(expr, target_type), temp, alignment_for_type(target_type));
        } else {
            create_store(codegen_value_for_target(expr, target_type), temp, alignment_for_type(target_type));
        }
        return temp;
    }

    void codegen_copy_construct_class(llvm::Value* dest_ptr, llvm::Value* src_ptr, const std::string& class_name) {
        if (const Function* user_ctor = find_user_declared_copy_ctor_ast(class_name)) {
            llvm::Function* ctor = module_->getFunction(overload_names_.at(user_ctor));
            builder_->CreateCall(ctor, {dest_ptr, src_ptr});
        } else {
            codegen_memberwise_copy_construct(dest_ptr, src_ptr, class_name);
        }
    }

    [[nodiscard]] bool is_constructor_function(const Function& fn) const {
        if (fn.member_owner_class.empty() || !fn.name.ends_with("_new") || fn.params.empty()) return false;
        const Type& this_param = fn.params[0].type;
        return this_param.kind == TypeKind::Reference && this_param.pointee != nullptr &&
               this_param.pointee->kind == TypeKind::Named && this_param.pointee->name == fn.member_owner_class;
    }

    [[nodiscard]] std::string unqualified_template_base_name(std::string_view class_name) const {
        size_t scope = class_name.rfind("::");
        std::string_view tail = scope == std::string_view::npos ? class_name : class_name.substr(scope + 2);
        size_t dot = tail.find('.');
        if (dot != std::string_view::npos) tail = tail.substr(0, dot);
        return std::string(tail);
    }

    [[nodiscard]] bool names_direct_base(const std::string& member_name, const ClassDef& def) const {
        const BaseSpecifier* base = def.direct_ordinary_base();
        if (base == nullptr || base->base_type.name.empty()) return false;
        return member_name == base->base_type.name || member_name == unqualified_template_base_name(base->base_type.name);
    }

    [[nodiscard]] bool names_base(const std::string& member_name, const BaseSpecifier& base) const {
        return member_name == base.base_type.name || member_name == unqualified_template_base_name(base.base_type.name);
    }

    [[nodiscard]] llvm::Value* load_this_object_ptr() {
        auto this_it = locals_.find("this");
        if (this_it == locals_.end()) {
            throw CodegenError("constructor/member initialization needs 'this' in scope", current_loc_);
        }
        return create_load(llvm::PointerType::getUnqual(*context_), this_it->second.alloca, std::nullopt, "this.obj");
    }

    [[nodiscard]] LValue codegen_raw_member_storage(llvm::Value* object_ptr, const std::string& class_name,
                                                    const ClassField& field) {
        auto info_it = structs_.find(class_name);
        if (info_it == structs_.end()) {
            throw CodegenError("unknown class '" + class_name + "'", current_loc_);
        }
        const StructInfo& info = info_it->second;
        std::optional<size_t> field_index = info.find_field_index(field.name);
        if (!field_index.has_value()) {
            throw CodegenError("class '" + class_name + "' has no field '" + field.name + "'", current_loc_);
        }
        llvm::Value* field_ptr = builder_->CreateStructGEP(info.llvm_type, object_ptr, *field_index, field.name);
        return LValue{field_ptr, field.type, alignment_for_type(field.type)};
    }

    void initialize_reference_storage(const LValue& target, const Expr& expr) {
        if (target.type.kind != TypeKind::Reference || target.type.pointee == nullptr) {
            throw CodegenError("internal error: reference initializer target is not a reference", current_loc_);
        }
        if (is_interface_reference_type(target.type)) {
            create_store(codegen_interface_value_for_target(expr, target.type), target.ptr, target.alignment);
            return;
        }
        validate_reference_pointee(*target.type.pointee);
        llvm::Value* referent_addr =
            const_reference_binds_materialized_temporary(expr, target.type)
                ? codegen_materialize_const_reference_source(expr, *target.type.pointee)
                : codegen_lvalue(expr).ptr;
        create_store(referent_addr, target.ptr, target.alignment);
    }

    void initialize_span_storage(const LValue& target, const Expr& expr) {
        if (target.type.kind != TypeKind::Span || target.type.pointee == nullptr) {
            throw CodegenError("internal error: span initializer target is not a span", current_loc_);
        }
        llvm::Value* span_value = codegen_span_value_for_target(expr, target.type);
        create_store(span_value, target.ptr, target.alignment);
    }

    // Direct-initializing a fresh class object from another prvalue of the
    // exact same type (`T x{f()};`, `new T(f())`, `T(f())`) materializes the
    // source object directly into the destination storage instead of routing
    // back through ordinary constructor overload resolution.
    bool try_initialize_class_storage_from_same_type_source(const LValue& target, const std::vector<ExprPtr>& args) {
        if (target.type.kind != TypeKind::Named || find_class_def(target.type.name) == nullptr || args.size() != 1) {
            return false;
        }
        if (produces_rvalue_of_type(*args[0], target.type)) {
            create_store(codegen_expr(*args[0]), target.ptr, target.alignment);
            return true;
        }
        if (!is_bare_same_type_copy_source(*args[0], target.type) || !is_copy_constructible(target.type.name)) {
            return false;
        }
        LValue src = codegen_lvalue(*args[0]);
        if (!types_equal(src.type, target.type)) return false;
        codegen_copy_construct_class(target.ptr, src.ptr, target.type.name);
        return true;
    }

    void initialize_storage_from_expr(const LValue& target, const Expr& expr) {
        if (target.type.kind == TypeKind::Reference) {
            initialize_reference_storage(target, expr);
            return;
        }
        if (target.type.kind == TypeKind::Span) {
            initialize_span_storage(target, expr);
            return;
        }
        if (target.type.kind == TypeKind::Named && find_class_def(target.type.name) != nullptr) {
            llvm::Value* value = codegen_class_value_for_boundary(expr, target.type);
            create_store(value, target.ptr, target.alignment);
            return;
        }
        llvm::Value* init_value = codegen_value_for_target(expr, target.type);
        check_store_type(init_value, to_llvm_type(target.type), "member initializer");
        create_store(init_value, target.ptr, target.alignment);
    }

    void initialize_storage_from_brace_args(const LValue& target, const std::vector<ExprPtr>& args) {
        if (target.type.kind == TypeKind::Reference) {
            if (args.size() != 1) {
                throw CodegenError("a reference member must be initialized with exactly one expression", current_loc_);
            }
            initialize_reference_storage(target, *args[0]);
            return;
        }
        if (target.type.kind == TypeKind::Span) {
            if (args.size() != 1) {
                throw CodegenError("a span member must be initialized with exactly one array expression", current_loc_);
            }
            initialize_span_storage(target, *args[0]);
            return;
        }
        if (target.type.kind == TypeKind::Named && find_class_def(target.type.name) != nullptr) {
            zero_initialize_storage(target.ptr, target.type, target.alignment);
            if (try_initialize_class_storage_from_same_type_source(target, args)) return;
            const Function* ctor_def = resolve_overload_by_type(target.type.name + "_new", args, /*param_offset=*/1);
            if (ctor_def == nullptr) {
                const ClassDef* class_def = find_class_def(target.type.name);
                if (args.empty() && class_def != nullptr && !class_has_any_constructor(target.type.name)) {
                    emit_default_initializers_for_class_storage(target.ptr, *class_def);
                    return;
                }
                throw CodegenError("class '" + target.type.name + "' has no constructor matching this call", current_loc_);
            }
            if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                llvm::Value* value = codegen_constructed_class_value(target.type.name, args, ctor_def);
                create_store(value, target.ptr, target.alignment);
                return;
            }
            llvm::Function* ctor = module_->getFunction(overload_names_.at(ctor_def));
            if (ctor == nullptr) {
                throw CodegenError("class '" + target.type.name + "' has no constructor matching this call", current_loc_);
            }
            std::vector<llvm::Value*> ctor_args = codegen_call_args(args, ctor_def, /*param_offset=*/1);
            ctor_args.insert(ctor_args.begin(), target.ptr);
            builder_->CreateCall(ctor, ctor_args);
            return;
        }
        if (args.empty()) {
            zero_initialize_storage(target.ptr, target.type, target.alignment);
            return;
        }
        if (args.size() != 1) {
            throw CodegenError("brace-initialization of this member requires exactly one expression", current_loc_);
        }
        initialize_storage_from_expr(target, *args[0]);
    }

    void initialize_storage(const LValue& target, const Initializer& init) {
        if (init.has_brace_args) {
            initialize_storage_from_brace_args(target, init.brace_args);
            return;
        }
        if (init.expr) {
            initialize_storage_from_expr(target, *init.expr);
            return;
        }
        zero_initialize_storage(target.ptr, target.type, target.alignment);
    }

    void emit_constructor_member_initializers(const Function& fn) {
        if (!is_constructor_function(fn)) return;
        const ClassDef* class_def = find_class_def(fn.member_owner_class);
        if (class_def == nullptr) {
            throw CodegenError("unknown constructor owner class '" + fn.member_owner_class + "'", current_loc_);
        }
        llvm::Value* object_ptr = load_this_object_ptr();
        if (const BaseSpecifier* base = class_def->direct_ordinary_base()) {
            const MemberInitializer* explicit_base_init = nullptr;
            for (const MemberInitializer& init : fn.member_initializers) {
                if (names_direct_base(init.member_name, *class_def)) {
                    explicit_base_init = &init;
                    break;
                }
            }
            const ClassDef* base_def = find_class_def(base->base_type.name);
            if (base_def == nullptr) {
                throw CodegenError("unknown base class '" + base->base_type.name + "'", current_loc_);
            }
            static const std::vector<ExprPtr> no_base_args;
            const std::vector<ExprPtr>* base_args = explicit_base_init != nullptr ? &explicit_base_init->initializer.brace_args
                                                                                  : nullptr;
            const Function* base_ctor =
                resolve_constructor_overload_exact(base->base_type.name, base_args != nullptr ? *base_args : no_base_args);
            if (base_ctor != nullptr) {
                std::vector<llvm::Value*> ctor_args =
                    codegen_call_args(base_args != nullptr ? *base_args : no_base_args, base_ctor, /*param_offset=*/1);
                ctor_args.insert(ctor_args.begin(), object_ptr);
                builder_->CreateCall(module_->getFunction(overload_names_.at(base_ctor)), ctor_args);
            } else if (base_args == nullptr || base_args->empty()) {
                emit_default_initializers_for_class_storage(object_ptr, *base_def);
            } else {
                throw CodegenError("base-class initializer for '" + base->base_type.name +
                                       "' does not match any constructor of that class",
                                   current_loc_);
            }
        }
        for (const BaseSpecifier& base : class_def->base_specifiers) {
            if (base.kind != BaseClassKind::Interface) continue;
            const MemberInitializer* explicit_base_init = nullptr;
            for (const MemberInitializer& init : fn.member_initializers) {
                if (names_base(init.member_name, base)) {
                    explicit_base_init = &init;
                    break;
                }
            }
            if (explicit_base_init == nullptr) continue;
            const Function* base_ctor = resolve_constructor_overload_exact(base.base_type.name,
                                                                           explicit_base_init->initializer.brace_args);
            if (base_ctor == nullptr) {
                throw CodegenError("base-class initializer for '" + base.base_type.name +
                                       "' does not match any constructor of that class",
                                   current_loc_);
            }
            std::vector<llvm::Value*> ctor_args =
                codegen_call_args(explicit_base_init->initializer.brace_args, base_ctor, /*param_offset=*/1);
            llvm::Value* fat_this =
                build_interface_value(object_ptr, get_or_create_interface_dispatch_table(class_def->name, base.base_type.name));
            ctor_args.insert(ctor_args.begin(), fat_this);
            builder_->CreateCall(module_->getFunction(overload_names_.at(base_ctor)), ctor_args);
        }
        for (const ClassField& field : class_def->fields) {
            const Initializer* selected_init = nullptr;
            for (const MemberInitializer& init : fn.member_initializers) {
                if (init.member_name == field.name) {
                    selected_init = &init.initializer;
                    break;
                }
            }
            if (selected_init == nullptr && field.default_initializer) selected_init = &*field.default_initializer;
            if (selected_init == nullptr) continue;
            LValue field_storage = codegen_raw_member_storage(object_ptr, class_def->name, field);
            initialize_storage(field_storage, *selected_init);
        }
    }

    [[nodiscard]] bool class_has_any_constructor(const std::string& class_name) const {
        return std::any_of(program_->functions.begin(), program_->functions.end(),
                           [&](const Function& fn) { return is_constructor_function(fn) && fn.member_owner_class == class_name; });
    }

    void emit_default_initializers_for_class_storage(llvm::Value* object_ptr, const ClassDef& class_def) {
        if (const BaseSpecifier* base = class_def.direct_ordinary_base()) {
            const ClassDef* base_def = find_class_def(base->base_type.name);
            if (base_def == nullptr) {
                throw CodegenError("unknown base class '" + base->base_type.name + "'", current_loc_);
            }
            const Function* base_ctor = resolve_constructor_overload_exact(base->base_type.name, {});
            if (base_ctor != nullptr) {
                builder_->CreateCall(module_->getFunction(overload_names_.at(base_ctor)), {object_ptr});
            } else if (!class_has_any_constructor(base->base_type.name)) {
                emit_default_initializers_for_class_storage(object_ptr, *base_def);
            } else {
                throw CodegenError("class '" + class_def.name + "' cannot be implicitly default-constructed because base class '" +
                                      base->base_type.name + "' has no accessible default constructor",
                                   current_loc_);
            }
        }
        for (const ClassField& field : class_def.fields) {
            if (!field.default_initializer) continue;
            LValue field_storage = codegen_raw_member_storage(object_ptr, class_def.name, field);
            initialize_storage(field_storage, *field.default_initializer);
        }
    }

    llvm::Value* codegen_class_value_for_boundary(const Expr& expr, const Type& target_type) {
        llvm::Type* llvm_type = to_llvm_type(target_type);
        if (is_bare_same_type_copy_source(expr, target_type) && is_copy_constructible(target_type.name)) {
            auto src_it = locals_.find(expr.name);
            llvm::AllocaInst* temp = create_entry_block_alloca(llvm_type, "classtransport");
            codegen_copy_construct_class(temp, src_it->second.alloca, target_type.name);
            return builder_->CreateLoad(llvm_type, temp, "classtransport.value");
        }
        if (expr.kind == ExprKind::Lambda) {
            llvm::Value* temp = codegen_expr(expr);
            return builder_->CreateLoad(llvm_type, temp, "classtransport.lambda");
        }
        if (produces_rvalue_of_type(expr, target_type)) {
            return codegen_expr(expr);
        }
        if (const Function* converting_ctor = resolve_converting_constructor_by_type(target_type.name, expr);
            converting_ctor != nullptr) {
            std::vector<ExprPtr> ctor_args;
            ctor_args.push_back(clone_expr(expr));
            return codegen_constructed_class_value(target_type.name, ctor_args, converting_ctor);
        }
        return codegen_expr(expr);
    }

    llvm::Value* codegen_interface_value_for_target(const Expr& expr, const Type& target_type) {
        std::optional<Type> source_type = infer_type(expr);
        if (!source_type.has_value()) {
            throw CodegenError("cannot determine interface conversion source type", current_loc_);
        }
        if (types_equal(*source_type, target_type)) return codegen_expr(expr);
        if (target_type.kind == TypeKind::Reference && target_type.pointee != nullptr &&
            target_type.pointee->kind == TypeKind::Named) {
            if (source_type->kind == TypeKind::Named) {
                if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr) {
                    std::optional<Type> operand_type = infer_type(*expr.lhs);
                    if (operand_type.has_value() && is_interface_pointer_type(*operand_type)) return codegen_expr(expr);
                }
                llvm::Value* object_ptr = codegen_lvalue(expr).ptr;
                llvm::Value* table_ptr =
                    get_or_create_interface_dispatch_table(source_type->name, target_type.pointee->name);
                return build_interface_value(object_ptr, table_ptr);
            }
            if (source_type->kind == TypeKind::Reference && source_type->pointee != nullptr &&
                source_type->pointee->kind == TypeKind::Named && !type_names_interface(source_type->pointee->name)) {
                llvm::Value* object_ptr = codegen_lvalue(expr).ptr;
                llvm::Value* table_ptr =
                    get_or_create_interface_dispatch_table(source_type->pointee->name, target_type.pointee->name);
                return build_interface_value(object_ptr, table_ptr);
            }
        }
        if (target_type.kind == TypeKind::Pointer && target_type.pointee != nullptr &&
            target_type.pointee->kind == TypeKind::Named) {
            if (source_type->kind == TypeKind::Pointer && source_type->pointee != nullptr &&
                source_type->pointee->kind == TypeKind::Named && !type_names_interface(source_type->pointee->name)) {
                llvm::Value* object_ptr = codegen_expr(expr);
                llvm::Value* table_ptr =
                    get_or_create_interface_dispatch_table(source_type->pointee->name, target_type.pointee->name);
                return build_interface_value(object_ptr, table_ptr);
            }
        }
        if (source_type->kind == TypeKind::Reference && source_type->pointee != nullptr &&
            target_type.kind == TypeKind::Reference && target_type.pointee != nullptr &&
            types_equal(*source_type->pointee, *target_type.pointee)) {
            return codegen_expr(expr);
        }
        throw CodegenError("unsupported interface conversion at code generation time", current_loc_);
    }

    llvm::Value* codegen_span_value_for_target(const Expr& expr, const Type& target_type) {
        if (target_type.kind != TypeKind::Span || target_type.pointee == nullptr) {
            throw CodegenError("internal error: span conversion target is not a span", current_loc_);
        }
        if (std::optional<Type> source_type = infer_type(expr); source_type.has_value() && types_equal(*source_type, target_type)) {
            return codegen_expr(expr);
        }
        LValue source = codegen_lvalue(expr);
        if (source.type.kind != TypeKind::Array) {
            throw CodegenError("std::span<T> can currently only be constructed from a fixed-size array in this version",
                               current_loc_);
        }
        if (to_llvm_type(*source.type.element) != to_llvm_type(*target_type.pointee)) {
            throw CodegenError("array element type does not match the span's element type", current_loc_);
        }
        llvm::Type* span_type = to_llvm_type(target_type);
        llvm::Value* size_value = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), source.type.array_size);
        llvm::Value* span_value = llvm::UndefValue::get(span_type);
        span_value = builder_->CreateInsertValue(span_value, source.ptr, {0});
        span_value = builder_->CreateInsertValue(span_value, size_value, {1});
        return span_value;
    }

    llvm::Value* codegen_contextual_bool_value(const Expr& expr) {
        std::optional<Type> expr_type = infer_type(expr);
        if (expr_type.has_value() && is_interface_pointer_type(*expr_type)) {
            llvm::Value* interface_value = codegen_expr(expr);
            llvm::Value* object_ptr = extract_interface_object_ptr(interface_value);
            return i1_to_bool(builder_->CreateICmpNE(
                object_ptr, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)), "ifacenotnull"));
        }
        return codegen_expr(expr);
    }

    llvm::Value* codegen_contextual_bool_i1(const Expr& expr) {
        return bool_to_i1(codegen_contextual_bool_value(expr));
    }

    std::vector<llvm::Value*> codegen_call_args(const std::vector<ExprPtr>& args, const Function* callee_def,
                                                  size_t param_offset) {
        std::vector<llvm::Value*> result;
        result.reserve(args.size());
        for (size_t i = 0; i < args.size(); i++) {
            bool param_is_reference = callee_def != nullptr && i + param_offset < callee_def->params.size() &&
                                       callee_def->params[i + param_offset].type.kind == TypeKind::Reference;
            const Type* ref_param_type =
                param_is_reference ? &callee_def->params[i + param_offset].type : nullptr;
            bool param_is_interface_reference = param_is_reference && is_interface_reference_type(*ref_param_type);
            bool param_is_rvalue_reference = param_is_reference && ref_param_type->is_rvalue_ref;
            // ch05 §5.x: a *const* (non-rvalue, non-mutable) reference
            // parameter may also bind directly to a fresh rvalue argument
            // -- movecheck's own argument_matches_parameter/
            // apply_reference_argument already gate this identically
            // (produces_rvalue_of_type), only ever for a const reference
            // (real C++ itself forbids binding a *mutable* lvalue
            // reference to a temporary).
            bool param_is_const_reference_bound_to_rvalue =
                param_is_reference && const_reference_binds_materialized_temporary(*args[i], *ref_param_type);
            if (param_is_interface_reference) {
                result.push_back(codegen_interface_value_for_target(*args[i], *ref_param_type));
            } else if (param_is_rvalue_reference || param_is_const_reference_bound_to_rvalue) {
                // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- the move
                // checker has already verified this argument produces a
                // genuine rvalue (produces_rvalue_of_type), which may not
                // itself be an addressable place (a literal, a fresh
                // std::make_unique<T>(...)/call result, ...).
                result.push_back(param_is_rvalue_reference ? codegen_materialize_rvalue_reference_source(*args[i])
                                                           : codegen_materialize_const_reference_source(
                                                                 *args[i], *ref_param_type->pointee));
            } else if (param_is_reference) {
                // Bind the reference parameter to the argument's address
                // rather than passing its value, exactly like a local
                // reference's own VarDecl.
                result.push_back(codegen_lvalue(*args[i]).ptr);
            } else {
                // ch06 §6: a bare literal argument adapts directly to
                // its target parameter's own declared scalar type (see
                // codegen_value_for_target) -- exactly like a VarDecl
                // initializer/plain assignment's identical treatment,
                // rather than defaulting to `int`/`double` and failing
                // the callee's own parameter-type check.
                if (callee_def != nullptr && i + param_offset < callee_def->params.size()) {
                    const Type& param_type = callee_def->params[i + param_offset].type;
                    if (param_type.kind == TypeKind::Named && find_class_def(param_type.name) != nullptr) {
                        result.push_back(codegen_class_value_for_boundary(*args[i], param_type));
                    } else {
                        result.push_back(codegen_value_for_target(*args[i], param_type));
                    }
                } else {
                    result.push_back(codegen_expr(*args[i]));
                }
            }
        }
        return result;
    }

    std::vector<llvm::Value*> codegen_call_args_for_types(const std::vector<ExprPtr>& args,
                                                          const std::vector<Type>& param_types) {
        std::vector<llvm::Value*> result;
        result.reserve(args.size());
        for (size_t i = 0; i < args.size(); i++) {
            bool param_is_reference = i < param_types.size() && param_types[i].kind == TypeKind::Reference;
            const Type* ref_param_type = param_is_reference ? &param_types[i] : nullptr;
            bool param_is_interface_reference = param_is_reference && is_interface_reference_type(*ref_param_type);
            bool param_is_rvalue_reference = param_is_reference && ref_param_type->is_rvalue_ref;
            bool param_is_const_reference_bound_to_rvalue =
                param_is_reference && const_reference_binds_materialized_temporary(*args[i], *ref_param_type);
            if (param_is_interface_reference) {
                result.push_back(codegen_interface_value_for_target(*args[i], *ref_param_type));
            } else if (param_is_rvalue_reference || param_is_const_reference_bound_to_rvalue) {
                result.push_back(param_is_rvalue_reference ? codegen_materialize_rvalue_reference_source(*args[i])
                                                           : codegen_materialize_const_reference_source(
                                                                 *args[i], *ref_param_type->pointee));
            } else if (param_is_reference) {
                result.push_back(codegen_lvalue(*args[i]).ptr);
            } else if (i < param_types.size()) {
                if (param_types[i].kind == TypeKind::Named && find_class_def(param_types[i].name) != nullptr) {
                    result.push_back(codegen_class_value_for_boundary(*args[i], param_types[i]));
                } else {
                    result.push_back(codegen_value_for_target(*args[i], param_types[i]));
                }
            } else {
                result.push_back(codegen_expr(*args[i]));
            }
        }
        return result;
    }

    // Reads `lv`'s value for use as an ordinary rvalue. An array decays to
    // its own address instead of being loaded as a giant aggregate: a
    // bare array is never meaningfully read "by value" as a whole (there
    // is no array-copy/array-comparison support, and never will be
    // without a real generics story) -- every place a bare array
    // identifier/subscript/field is used where a *value* is expected is
    // exactly the C/C++ array-to-pointer decay (e.g. `char* p = buf;`, or
    // passing `buf` as a `char*` argument), matching how a raw pointer's
    // own storage already holds an *address* rather than "the pointee's
    // bytes" at this level (see to_llvm_type's Pointer case). Shared by
    // every "resolve an lvalue, then read its value" call site below
    // (Identifier/Subscript, Member's struct-field fallback) so all of
    // them treat an array-typed result the same way.
    llvm::Value* load_value(const LValue& lv) {
        if (lv.type.kind == TypeKind::Array) {
            return lv.ptr;
        }
        return create_load(to_llvm_type(lv.type), lv.ptr, lv.alignment, "loadtmp");
    }

    // `bool` is stored/passed/returned as a full byte (i8; see
    // to_llvm_type), but LLVM's branch/select instructions require a
    // 1-bit condition -- this narrows an i8 bool value (guaranteed by
    // the false=0/true=1 invariant, ch06, to already be exactly 0 or 1)
    // down to i1 right before such a use. Requires the input to actually
    // *be* i8 (not just any integer width truncated down to its lowest
    // bit): ch06 explicitly forbids implicit scalar-to-bool conversion,
    // including in if/while conditions (unlike real C++, where e.g.
    // `if (5)` is legal) -- without this check, a plain `CreateTrunc`
    // would silently accept `if (5)` (5's lowest bit happens to be 1)
    // instead of rejecting it the way an explicit-cast-only language must.
    // Known gap: `char` is *also* i8 (see to_llvm_type), and this check
    // has no way to tell "an i8 that started life as a bool" from "an i8
    // that started life as a char" -- catching `if (some_char)` would
    // need real expression-type inference, which doesn't exist anywhere
    // in this codebase yet (a much bigger undertaking than this narrow
    // width check). Left as a known limitation rather than expanded scope.
    llvm::Value* bool_to_i1(llvm::Value* v) {
        if (!v->getType()->isIntegerTy(8)) {
            throw CodegenError(
                "expected a 'bool' value here (e.g. an if/while condition, or an '&&'/'||' operand); "
                "scpp requires an explicit cast for any scalar-to-bool conversion, unlike real C++ "
                "(spec ch06)",
                current_loc_);
        }
        return builder_->CreateTrunc(v, llvm::Type::getInt1Ty(*context_), "tobool");
    }

    // The inverse of bool_to_i1: widens an i1 (an icmp result, or another
    // logical operation already in the 1-bit domain) back up to the i8
    // representation every ordinary bool value uses -- the choke point
    // every comparison/logical operator goes through before its result
    // is stored, passed, or returned as an actual `bool`.
    llvm::Value* i1_to_bool(llvm::Value* v) {
        return builder_->CreateZExt(v, llvm::Type::getInt8Ty(*context_), "boolext");
    }

    [[nodiscard]] bool enum_value_fits_source_type(const Type& source_type, long long enum_value) {
        if (source_type.kind != TypeKind::Named || !is_integral_scalar_type_name(source_type.name)) return false;
        auto* integer_type = llvm::dyn_cast<llvm::IntegerType>(to_llvm_type(source_type));
        if (integer_type == nullptr) return false;
        unsigned bits = integer_type->getBitWidth();
        bool source_is_unsigned = is_unsigned_for_cast(source_type.name);
        if (source_is_unsigned) {
            if (enum_value < 0) return false;
            if (bits >= 64) return true;
            std::uint64_t max_value = (std::uint64_t{1} << bits) - 1;
            return static_cast<std::uint64_t>(enum_value) <= max_value;
        }
        if (bits >= 64) return true;
        long long min_value = -(std::int64_t{1} << (bits - 1));
        long long max_value = (std::int64_t{1} << (bits - 1)) - 1;
        return enum_value >= min_value && enum_value <= max_value;
    }

    llvm::Value* build_integral_enum_match(llvm::Value* source, const Type& source_type, long long enum_value) {
        auto* source_integer_type = llvm::dyn_cast<llvm::IntegerType>(source->getType());
        if (source_integer_type == nullptr || !enum_value_fits_source_type(source_type, enum_value)) {
            return llvm::ConstantInt::getFalse(*context_);
        }
        if (is_unsigned_for_cast(source_type.name)) {
            return builder_->CreateICmpEQ(
                source, llvm::ConstantInt::get(source_integer_type, static_cast<std::uint64_t>(enum_value), false),
                "enumcastcmp");
        }
        return builder_->CreateICmpEQ(source, llvm::ConstantInt::getSigned(source_integer_type, enum_value),
                                      "enumcastcmp");
    }

    llvm::ConstantInt* enum_variant_constant(llvm::Type* enum_storage_type, const Type& underlying_type, long long enum_value) {
        auto* integer_type = llvm::cast<llvm::IntegerType>(enum_storage_type);
        if (is_unsigned_for_cast(underlying_type.name)) {
            return llvm::ConstantInt::get(integer_type, static_cast<std::uint64_t>(enum_value), false);
        }
        return llvm::ConstantInt::getSigned(integer_type, enum_value);
    }

    CallResult codegen_enum_cast_store_builtin(const Expr& expr, const Function& callee_def) {
        if (expr.args.size() != 2 || callee_def.params.size() != 2) {
            throw CodegenError("internal error: malformed scpp::__enum_cast_store call", current_loc_);
        }
        const Type& source_type = callee_def.params[0].type;
        const Type& out_param_type = callee_def.params[1].type;
        if (source_type.kind != TypeKind::Named || !is_integral_scalar_type_name(source_type.name)) {
            throw CodegenError("scpp::enum_cast<T>(value) requires an integral source value", current_loc_);
        }
        if (out_param_type.kind != TypeKind::Reference || out_param_type.pointee == nullptr ||
            out_param_type.pointee->kind != TypeKind::Named) {
            throw CodegenError("scpp::enum_cast<T>(value) requires T to be an enum class", current_loc_);
        }
        const EnumDef* enum_def = find_enum_def(program_, out_param_type.pointee->name);
        if (enum_def == nullptr) {
            throw CodegenError("scpp::enum_cast<T>(value) requires T to be an enum class", current_loc_);
        }

        llvm::Value* source_value = codegen_value_for_target(*expr.args[0], source_type);
        LValue out = codegen_lvalue(*expr.args[1]);
        llvm::Type* enum_storage_type = to_llvm_type(*out_param_type.pointee);
        llvm::Value* matched = llvm::ConstantInt::getFalse(*context_);
        llvm::Value* selected =
            enum_variant_constant(enum_storage_type, enum_def->underlying_type, 0);
        for (const EnumVariant& variant : enum_def->variants) {
            llvm::Value* variant_matches = build_integral_enum_match(source_value, source_type, variant.value);
            matched = builder_->CreateOr(matched, variant_matches, "enumcastmatch");
            selected = builder_->CreateSelect(
                variant_matches, enum_variant_constant(enum_storage_type, enum_def->underlying_type, variant.value), selected,
                "enumcastselect");
        }
        create_store(selected, out.ptr, out.alignment);
        return CallResult{i1_to_bool(matched), &callee_def};
    }

    // ch06 §6: a bare numeric literal (Integer/Float) has no fixed type
    // of its own the way a named variable does -- exactly like real
    // C++'s own literal-suffix rules (and, more directly, how Rust
    // treats an unsuffixed integer/float literal as unconstrained until
    // context picks a concrete type): generates the constant directly in
    // `target_type`'s own LLVM representation when the source shape is a
    // literal and the target is a scalar Named type, so `int64_t x = 5;`
    // needs no separate cast at all -- this is *type inference for an
    // otherwise-untyped constant*, not an implicit conversion of an
    // already-typed value (ch06's "no implicit conversions" rule is
    // about the latter; see check_store_type's own scope). Falls back to
    // plain codegen_expr for every other expression shape (an existing
    // variable, a call, an arithmetic expression, ...), which already
    // has a real, fixed type of its own to check via check_store_type
    // exactly as before. Shared by every site that already knows its own
    // target type up front: a VarDecl initializer, a plain assignment's
    // RHS, and std::make_unique<T>(...)'s scalar argument.
    llvm::Value* codegen_value_for_target(const Expr& expr, const Type& target_type) {
        if (is_interface_representation_type(target_type)) {
            return codegen_interface_value_for_target(expr, target_type);
        }
        // `-100`/`-1.5` (a negated literal, ExprKind::Unary/Neg over a
        // bare literal) is just as untyped as the bare literal itself --
        // real C++ itself treats a unary-minus-literal as a single
        // token for exactly this reason (a negative literal, not "minus
        // applied to a positive one"). Recurses once, with the negation
        // folded into the literal's own value, rather than falling
        // through to plain codegen_expr (which would infer a fixed
        // int/double type for the un-negated literal, then apply `-` in
        // that type, defeating the point).
        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Neg) {
            if (expr.lhs->kind == ExprKind::IntegerLiteral) {
                Expr negated;
                negated.kind = ExprKind::IntegerLiteral;
                negated.loc = expr.loc;
                negated.int_value = -expr.lhs->int_value;
                return codegen_value_for_target(negated, target_type);
            }
            if (expr.lhs->kind == ExprKind::FloatLiteral) {
                Expr negated;
                negated.kind = ExprKind::FloatLiteral;
                negated.loc = expr.loc;
                negated.float_value = -expr.lhs->float_value;
                return codegen_value_for_target(negated, target_type);
            }
        }
        if (target_type.kind == TypeKind::Named) {
            if (expr.kind == ExprKind::IntegerLiteral) {
                if (is_float_scalar_type_name(target_type.name)) {
                    return llvm::ConstantFP::get(to_llvm_type(target_type), static_cast<double>(expr.int_value));
                }
                if (target_type.name != "bool" && target_type.name != "char") {
                    return llvm::ConstantInt::get(to_llvm_type(target_type), expr.int_value,
                                                   /*isSigned=*/!is_unsigned_scalar_type_name(target_type.name));
                }
            } else if (expr.kind == ExprKind::FloatLiteral && is_float_scalar_type_name(target_type.name)) {
                return llvm::ConstantFP::get(to_llvm_type(target_type), expr.float_value);
            }
        }
        if (target_type.kind == TypeKind::FunctionPointer) {
            if (llvm::Value* fn = codegen_function_pointer_value_for_target(expr, target_type)) return fn;
        }
        if (target_type.kind == TypeKind::Span) {
            return codegen_span_value_for_target(expr, target_type);
        }
        return codegen_expr(expr);
    }

    // Verifies `value`'s LLVM type exactly matches `expected` before it's
    // stored into a place declared as `expected` (a VarDecl initializer,
    // a plain assignment's RHS, or std::make_unique<T>(...)'s scalar
    // argument) -- scpp has no implicit conversion between distinct
    // scalar types (bool/char/int are all separate, ch06), and, unlike a
    // mismatched call argument/return value/binary operand (all already
    // rejected by LLVM's own module verifier), a *store*'s address
    // operand is an opaque `ptr` with no embedded pointee type for the
    // verifier to check against -- an unchecked width-mismatched store
    // would instead silently corrupt whatever memory follows the
    // undersized slot (a stack buffer overflow, or reading back
    // uninitialized garbage from an oversized heap allocation) rather
    // than failing cleanly. Same narrow width-check philosophy as
    // bool_to_i1 above (not full expression-type inference, which
    // doesn't exist anywhere in this codebase): this catches every
    // *differently-sized* mismatch (bool/int, char/int, ...) but can't
    // tell apart two scalar types that happen to share a width (bool vs.
    // char, both i8) -- a known, accepted limitation, not a soundness
    // gap, since same-width scalars can never corrupt memory this way.
    void check_store_type(llvm::Value* value, llvm::Type* expected, const std::string& what) {
        if (value->getType() != expected) {
            throw CodegenError("type mismatch initializing/assigning " + what +
                                ": scpp has no implicit conversion between distinct scalar types (e.g. "
                                "bool/char/int are all distinct, spec ch06) -- an explicit cast would be "
                                "required, but cast expressions aren't implemented in this version yet",
                current_loc_);
        }
    }

    llvm::Value* codegen_expr(const Expr& expr) {
        // Refreshed on every call (including each recursive call for a
        // child sub-expression), same reasoning as codegen_stmt above --
        // so a CodegenError thrown while examining `expr` itself (before
        // or after recursing into any children) reports `expr`'s own
        // position, not whichever child was most recently visited.
        refresh_debug_location(expr.loc);
        switch (expr.kind) {
            case ExprKind::IntegerLiteral:
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), expr.int_value, /*isSigned=*/true);

            case ExprKind::FloatLiteral:
                // Defaults to `double` (ch06 §6, real C++'s own
                // no-suffix default) -- adapted to a narrower/other float
                // type by context wherever the target type is known
                // instead (VarDecl/Assign/call argument/return -- see
                // codegen_value_for_target), exactly like an
                // IntegerLiteral's own default-to-`int` treatment.
                return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*context_), expr.float_value);

            case ExprKind::BoolLiteral:
            case ExprKind::TypeTrait:
                // `bool` is stored as a full byte (i8; see to_llvm_type
                // and its false=0/true=1 invariant, ch06) -- a literal's
                // value is already exactly 0 or 1, so no i1_to_bool
                // widening is needed here (unlike a comparison/logical
                // result, which starts out as a genuine i1).
                return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), expr.bool_value ? 1 : 0);

            case ExprKind::CharLiteral:
                // `char` is its own distinct 1-byte type (ch06) -- not an
                // alias for any fixed-width integer type, so it takes no
                // stance on signedness at all (no implicit arithmetic or
                // cross-type comparison exists for it to matter for);
                // `expr.int_value` already holds the decoded ordinal
                // value 0-255 (see parser's decode_char_literal), which
                // fits identically in the 8 bits either way.
                return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context_), expr.int_value, /*isSigned=*/false);

            case ExprKind::Sizeof:
                return codegen_sizeof_value(expr);

            case ExprKind::StringLiteral:
                // A read-only global byte array (null-terminated, like a
                // real C string literal), decaying directly to a pointer
                // to its first byte -- there is no backing local
                // variable/place for a literal, so (unlike an array-typed
                // identifier's load_value decay) this needs no separate
                // lvalue-then-decay step; CreateGlobalString itself
                // returns the pointer. Reuses the exact mechanism already
                // used for print_bool's "true"/"false" constants.
                return builder_->CreateGlobalString(expr.name, "str");

            case ExprKind::Conditional: {
                llvm::Value* cond = codegen_contextual_bool_i1(*expr.lhs);
                llvm::Function* current_function = builder_->GetInsertBlock()->getParent();
                llvm::BasicBlock* then_block = llvm::BasicBlock::Create(*context_, "cond.then", current_function);
                llvm::BasicBlock* else_block = llvm::BasicBlock::Create(*context_, "cond.else", current_function);
                llvm::BasicBlock* merge_block = llvm::BasicBlock::Create(*context_, "cond.end", current_function);
                builder_->CreateCondBr(cond, then_block, else_block);

                builder_->SetInsertPoint(then_block);
                llvm::Value* then_value = codegen_expr(*expr.rhs);
                builder_->CreateBr(merge_block);
                llvm::BasicBlock* then_end = builder_->GetInsertBlock();

                builder_->SetInsertPoint(else_block);
                llvm::Value* else_value = codegen_expr(*expr.third);
                builder_->CreateBr(merge_block);
                llvm::BasicBlock* else_end = builder_->GetInsertBlock();

                builder_->SetInsertPoint(merge_block);
                if (then_value->getType() != else_value->getType()) {
                    throw CodegenError("conditional operator requires both arms to have the same type", current_loc_);
                }
                llvm::PHINode* phi = builder_->CreatePHI(then_value->getType(), 2, "condtmp");
                phi->addIncoming(then_value, then_end);
                phi->addIncoming(else_value, else_end);
                return phi;
            }

            case ExprKind::Cast: {
                // ch06 §6 / spec §5.1(5.2): `static_cast<T>(expr)`/`(T)expr`
                // converts either between scalar types, or between raw
                // pointer types (movecheck already enforces the latter's
                // unsafe-context requirement). With LLVM opaque pointers,
                // every raw pointer lowers to the same `ptr` type, so a
                // pointer-to-pointer cast is a codegen no-op.
                std::optional<Type> source_type = infer_type(*expr.lhs);
                if (!source_type.has_value()) {
                    throw CodegenError("cast operand has no inferable type", current_loc_);
                }
                if (is_interface_representation_type(*source_type) || is_interface_representation_type(expr.type)) {
                    throw CodegenError("casts involving interface-typed pointers or references are not supported",
                                       current_loc_);
                }
                if (source_type->kind == TypeKind::Pointer && expr.type.kind == TypeKind::Pointer) {
                    return codegen_value_for_target(*expr.lhs, *source_type);
                }
                if (source_type->kind != TypeKind::Named || expr.type.kind != TypeKind::Named) {
                    throw CodegenError("cast is only supported between scalar types or raw pointer types in this version",
                                       current_loc_);
                }
                if (is_integral_scalar_type_name(source_type->name) && find_enum_def(program_, expr.type.name) != nullptr) {
                    throw CodegenError("cannot cast an integer value to enum class '" + expr.type.name +
                                           "'; use scpp::enum_cast<" + expr.type.name + ">(value) instead",
                                       current_loc_);
                }
                bool source_is_scalar_or_enum =
                    is_scalar_type_name(source_type->name) || find_enum_def(program_, source_type->name) != nullptr;
                bool target_is_scalar_or_enum =
                    is_scalar_type_name(expr.type.name) || find_enum_def(program_, expr.type.name) != nullptr;
                if (!source_is_scalar_or_enum || !target_is_scalar_or_enum) {
                    throw CodegenError(
                        "cast is only supported between builtin scalar types or between an enum class and its "
                        "underlying integer type in this version",
                        current_loc_);
                }
                llvm::Value* operand = codegen_value_for_target(*expr.lhs, *source_type);
                return codegen_scalar_cast(operand, *source_type, expr.type);
            }

            case ExprKind::Identifier: {
                if (expr.explicit_global_qualification || !locals_.contains(expr.name)) {
                    const EnumDef* enum_def = nullptr;
                    const EnumVariant* enum_variant = find_enum_variant(program_, expr.name, &enum_def);
                    if (enum_variant != nullptr) {
                        return llvm::ConstantInt::get(to_llvm_type(named_type(enum_def->name)), enum_variant->value,
                                                      /*isSigned=*/!is_unsigned_scalar_type_name(
                                                          enum_def->underlying_type.name));
                    }
                    if (std::optional<Type> fn_type = resolve_function_designator_type(expr)) {
                        if (llvm::Value* fn = codegen_function_pointer_value_for_target(expr, *fn_type)) return fn;
                    }
                    if (expr.explicit_global_qualification) {
                        throw CodegenError("use of undeclared global name '" + expr.name + "'", current_loc_);
                    }
                }
                LValue lv = codegen_lvalue(expr);
                return load_value(lv);
            }

            case ExprKind::Subscript: {
                LValue lv = codegen_lvalue(expr);
                return load_value(lv);
            }

            case ExprKind::Member: {
                // `s.size` on a std::span<T> is a computed, read-only
                // property (there's no backing storage to take the
                // address of at the *scpp* type level -- it's an i64
                // internally but exposed as a plain `int`, see
                // to_llvm_type's Span case) -- codegen_lvalue's own
                // Member case rejects it outright for that reason, so it
                // has to be handled here instead, before falling back to
                // the ordinary lvalue-then-load pattern used for a real
                // struct field.
                LValue base = codegen_lvalue(*expr.lhs);
                if (base.type.kind == TypeKind::Span && expr.name == "size") {
                    llvm::Value* size_ptr = builder_->CreateStructGEP(to_llvm_type(base.type), base.ptr, 1, "sizeptr");
                    llvm::Value* size64 = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), size_ptr, "size64");
                    return builder_->CreateTrunc(size64, llvm::Type::getInt32Ty(*context_), "size");
                }
                LValue lv = codegen_lvalue(expr);
                return load_value(lv);
            }

            case ExprKind::Unary: {
                if (expr.unary_op == UnaryOp::Deref) {
                    if (std::optional<Type> operand_type = infer_type(*expr.lhs);
                        operand_type.has_value() && is_interface_pointer_type(*operand_type)) {
                        return codegen_expr(*expr.lhs);
                    }
                    if (std::optional<Type> operand_type = infer_type(*expr.lhs);
                        operand_type.has_value() && operand_type->kind == TypeKind::FunctionPointer) {
                        return codegen_expr(*expr.lhs);
                    }
                    // Same lvalue-then-load pattern as Identifier/Member/
                    // Subscript above: codegen_lvalue resolves *what*
                    // `*p` addresses (see its own Unary case), this just
                    // reads the value stored there.
                    LValue lv = codegen_lvalue(expr);
                    return create_load(to_llvm_type(lv.type), lv.ptr, lv.alignment, "loadtmp");
                }
                if (expr.unary_op == UnaryOp::AddressOf) {
                    if (std::optional<Type> operand_type = infer_type(*expr.lhs); operand_type.has_value()) {
                        if (is_interface_reference_type(*operand_type)) {
                            return codegen_expr(*expr.lhs);
                        }
                        if (expr.lhs->kind == ExprKind::Unary && expr.lhs->unary_op == UnaryOp::Deref && expr.lhs->lhs != nullptr) {
                            std::optional<Type> inner = infer_type(*expr.lhs->lhs);
                            if (inner.has_value() && is_interface_pointer_type(*inner)) {
                                return codegen_expr(*expr.lhs->lhs);
                            }
                        }
                    }
                    if (std::optional<Type> fn_type = resolve_function_designator_type(expr)) {
                        if (llvm::Value* fn = codegen_function_pointer_value_for_target(expr, *fn_type)) return fn;
                    }
                    // `&expr` (ch05 §5.7) -- the mirror image of Deref
                    // just above: codegen_lvalue already resolves
                    // expr.lhs's address (its `.ptr`); returning that
                    // pointer directly as this expression's value --
                    // instead of loading through it -- is the entire
                    // codegen difference between reading a `T&`/
                    // `const T&` (which loads) and creating a raw `T*`
                    // (which doesn't). No new address-computation logic
                    // needed; movecheck (apply_address_of) has already
                    // verified expr.lhs resolves to a real place.
                    return codegen_lvalue(*expr.lhs).ptr;
                }
                if (expr.unary_op == UnaryOp::Neg) {
                    llvm::Value* operand = codegen_expr(*expr.lhs);
                    std::optional<Type> operand_type = infer_type(*expr.lhs);
                    bool is_float = operand_type.has_value() && is_float_scalar_type_name(operand_type->name);
                    return is_float ? builder_->CreateFNeg(operand, "fnegtmp") : builder_->CreateNeg(operand, "negtmp");
                }
                llvm::Value* operand = codegen_contextual_bool_value(*expr.lhs);
                // Not (`!`) -- `operand` is a `bool` value (i8; see
                // to_llvm_type), so this goes through the i1 domain
                // rather than a raw bitwise-not directly on the i8: NOT
                // on the byte `0x01` gives `0xFE`, not the canonical
                // false=`0x00` the ch06 invariant requires (every other
                // bool-producing operation -- comparisons, `&&`/`||` --
                // is careful to only ever produce 0 or 1; this must be
                // too, or a later `== false` on the result would wrongly
                // disagree with `!` itself).
                return i1_to_bool(builder_->CreateNot(bool_to_i1(operand), "nottmp"));
            }

            case ExprKind::Binary:
                return codegen_binary(expr);

            case ExprKind::Call: {
                if (is_for_range_size_builtin(expr)) {
                    std::optional<Type> range_type = infer_type(*expr.args[0]);
                    if (!range_type.has_value()) {
                        throw CodegenError("cannot determine range-for operand type", current_loc_);
                    }
                    const Type& unwrapped = range_type->kind == TypeKind::Reference && range_type->pointee != nullptr
                                                ? *range_type->pointee
                                                : *range_type;
                    if (unwrapped.kind == TypeKind::Array) {
                        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), unwrapped.array_size);
                    }
                    if (unwrapped.kind == TypeKind::Span) {
                        auto size_expr = std::make_unique<Expr>();
                        size_expr->kind = ExprKind::Member;
                        size_expr->loc = expr.loc;
                        size_expr->lhs = clone_expr(*expr.args[0]);
                        size_expr->name = "size";
                        return codegen_expr(*size_expr);
                    }
                    throw CodegenError("range-for requires a fixed-size array or std::span operand", current_loc_);
                }
                if (expr.name == "print_int" || expr.name == "print_bool" || expr.name == "print_char") {
                    return codegen_builtin_print(expr);
                }
                CallResult result = codegen_call(expr);
                if (result.callee_def != nullptr && is_interface_reference_type(result.callee_def->return_type)) {
                    return result.value;
                }
                if (result.callee_def != nullptr && result.callee_def->return_type.kind == TypeKind::Reference) {
                    // The callee returns a reference -- an address,
                    // lowered identically to a pointer (see
                    // to_llvm_type) -- so using the call's result as a
                    // *value* here means auto-dereferencing it, exactly
                    // like a reference local's own read (see
                    // codegen_lvalue's Identifier case).
                    return builder_->CreateLoad(to_llvm_type(*result.callee_def->return_type.pointee), result.value,
                                                 "derefcalltmp");
                }
                return result.value;
            }

            case ExprKind::Move: {
                // The move checker has already verified `expr.lhs` is a
                // plain, currently-Initialized unique_ptr or class-typed
                // variable. At the IR level a move is: read the old
                // value, then null out the source slot -- so even code
                // that (incorrectly) bypassed the move checker would
                // observe a null pointer rather than an aliased/
                // duplicated one. For a class-typed source with a
                // destructor, also set its own moved_flag (spec §6.3/
                // §6.4: the destructor is never invoked for a moved-out
                // object) -- see codegen_call_destructor_unless_moved.
                LValue lv = codegen_lvalue(*expr.lhs);
                llvm::Type* llvm_type = to_llvm_type(lv.type);
                llvm::Value* old_value = create_load(llvm_type, lv.ptr, lv.alignment, "movetmp");
                zero_initialize_storage(lv.ptr, lv.type, lv.alignment);
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto local_it = locals_.find(expr.lhs->name);
                    if (local_it != locals_.end() && local_it->second.moved_flag != nullptr) {
                        builder_->CreateStore(llvm::ConstantInt::getTrue(*context_), local_it->second.moved_flag);
                    }
                }
                return old_value;
            }

            case ExprKind::New:
                return codegen_new_expr(expr);

            case ExprKind::Delete:
            case ExprKind::Destroy:
                throw CodegenError("'delete' and explicit destructor calls are only supported as standalone statements "
                                   "in this version",
                    current_loc_);

            case ExprKind::Fold:
            case ExprKind::PackExpansion:
                throw CodegenError("fold expression should have been expanded before codegen",
                    current_loc_);

            case ExprKind::Lambda:
                return codegen_construct_lambda(expr);
        }
        throw CodegenError("unhandled expression kind",
            current_loc_);
    }

    // ch05 §5.12: constructs a resolved Lambda literal's own closure
    // value -- allocates a fresh temporary, then directly stores each
    // capture into its own field slot, bypassing the ordinary "call a
    // synthesized constructor" convention entirely (movecheck's
    // closure-resolution pass gives this class no constructor at all).
    // A by-value capture stores the captured value; a by-reference
    // capture stores the captured variable's own ADDRESS (via
    // codegen_lvalue, exactly like a local `T& r = expr;` reference
    // declaration's own binding -- correctly handling both an ordinary
    // local, whose own alloca is the address, and an already-reference-
    // typed local like `this`, whose own current pointer *value* is the
    // address, since codegen_lvalue's Identifier case already
    // distinguishes the two). This sidesteps a real, separate pre-
    // existing gap where plain Member-access assignment on a
    // Reference-typed field does not auto-dereference the way
    // Identifier access already does -- storing the address directly
    // here avoids ever going through that path. Returns the closure's
    // own address (an `llvm::AllocaInst*`, like any other class-typed
    // value in this codebase -- see codegen_expr's Lambda case, and
    // codegen_lvalue's own Lambda case for an IIFE's receiver, ch05
    // §5.12's `[](...){...}()`).
    llvm::AllocaInst* codegen_construct_lambda(const Expr& expr, llvm::AllocaInst* existing_storage = nullptr) {
        const StructInfo& info = structs_.at(expr.name);
        llvm::AllocaInst* closure =
            existing_storage != nullptr ? existing_storage : create_entry_block_alloca(info.llvm_type, "lambdatmp");
        for (size_t i = 0; i < expr.lambda_captures.size(); i++) {
            const LambdaCapture& capture = expr.lambda_captures[i];
            const Type& field_type = info.field_types[i];
            llvm::Value* field_ptr = builder_->CreateStructGEP(info.llvm_type, closure, i, capture.name);
            if (capture.by_reference) {
                Expr ident;
                ident.kind = ExprKind::Identifier;
                ident.loc = expr.loc;
                ident.name = capture.name;
                llvm::Value* address = codegen_lvalue(ident).ptr;
                create_store(address, field_ptr, std::nullopt);
                continue;
            }
            Expr ident;
            ident.kind = ExprKind::Identifier;
            ident.loc = expr.loc;
            ident.name = capture.name;
            const Expr& source = capture.init ? *capture.init : ident;
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name) &&
                source.kind == ExprKind::Identifier) {
                auto src_it = locals_.find(source.name);
                if (src_it != locals_.end() && types_equal(src_it->second.type, field_type) &&
                    is_copy_constructible(field_type.name)) {
                    codegen_copy_construct_class(field_ptr, src_it->second.alloca, field_type.name);
                    continue;
                }
            }
            llvm::Value* value = codegen_value_for_target(source, field_type);
            check_store_type(value, to_llvm_type(field_type), "capture '" + capture.name + "'");
            create_store(value, field_ptr, std::nullopt);
        }
        return closure;
    }

    llvm::Value* codegen_new_expr(const Expr& expr) {
        llvm::Type* element_type = to_llvm_type(expr.type);
        llvm::Value* heap_ptr = nullptr;
        if (expr.lhs) {
            heap_ptr = codegen_expr(*expr.lhs);
        } else {
            llvm::Function* malloc_fn = get_or_declare_malloc();
            uint64_t size_in_bytes = module_->getDataLayout().getTypeAllocSize(element_type);
            llvm::Value* size_arg = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), size_in_bytes);
            heap_ptr = builder_->CreateCall(malloc_fn, {size_arg}, "newptr");
        }

        if (expr.type.kind == TypeKind::Named && structs_.contains(expr.type.name)) {
            LValue target{heap_ptr, expr.type, std::nullopt};
            zero_initialize_storage(target.ptr, target.type, target.alignment);
            if (!expr.args.empty() || expr.has_paren_init) {
                if (try_initialize_class_storage_from_same_type_source(target, expr.args)) return heap_ptr;
                std::string ctor_name = expr.type.name + "_new";
                const Function* ctor_def = resolve_overload_by_type(ctor_name, expr.args, /*param_offset=*/1);
                if (ctor_def == nullptr) {
                    if (expr.args.empty()) return heap_ptr;
                    throw CodegenError("class '" + expr.type.name + "' has no constructor matching this call",
                        current_loc_);
                }
                if (ctor_def->eval_mode == FunctionEvalMode::Consteval) {
                    llvm::Value* value = codegen_constructed_class_value(expr.type.name, expr.args, ctor_def);
                    builder_->CreateStore(value, heap_ptr);
                    return heap_ptr;
                }
                llvm::Function* ctor = module_->getFunction(overload_names_.at(ctor_def));
                if (ctor == nullptr) {
                    if (expr.args.empty()) return heap_ptr;
                    throw CodegenError("class '" + expr.type.name + "' has no constructor matching this call",
                        current_loc_);
                }
                std::vector<llvm::Value*> args = codegen_call_args(expr.args, ctor_def, /*param_offset=*/1);
                args.insert(args.begin(), target.ptr);
                builder_->CreateCall(ctor, args);
            }
            return heap_ptr;
        }

        llvm::Value* initial_value = llvm::Constant::getNullValue(element_type);
        if (!expr.args.empty()) {
            if (expr.args.size() != 1) {
                throw CodegenError("'new T(args...)' for a non-class type currently requires exactly one argument",
                    current_loc_);
            }
            initial_value = codegen_expr(*expr.args[0]);
            refresh_debug_location(expr.loc);
            check_store_type(initial_value, element_type, "'new " + expr.type.name + "(...)' argument");
        }
        builder_->CreateStore(initial_value, heap_ptr);
        return heap_ptr;
    }

    void codegen_delete_expr(const Expr& expr) {
        llvm::Value* ptr = codegen_expr(*expr.lhs);
        std::optional<Type> operand_type = infer_type(*expr.lhs);
        if (!operand_type.has_value() || operand_type->kind != TypeKind::Pointer || operand_type->pointee == nullptr) {
            throw CodegenError("'delete' requires a raw pointer operand in this version", current_loc_);
        }
        if (is_interface_pointer_type(*operand_type)) {
            throw CodegenError("'delete' does not support interface pointers in this version", current_loc_);
        }
        const Type& pointee = *operand_type->pointee;
        if (pointee.kind == TypeKind::Named) {
            if (class_has_destructor_in_chain(pointee.name)) {
                codegen_call_destructor_chain_unless_moved(pointee.name, ptr, nullptr);
            }
        }
        builder_->CreateCall(get_or_declare_free(), {ptr});
    }

    void codegen_destroy_expr(const Expr& expr) {
        if (!expr.destroy_through_pointer) {
            throw CodegenError("explicit destructor calls currently require the pointer form 'ptr->~T()'",
                               current_loc_);
        }
        llvm::Value* ptr = codegen_expr(*expr.lhs);
        if (expr.destroy_through_pointer) {
            std::optional<Type> operand_type = infer_type(*expr.lhs);
            if (operand_type.has_value() && is_interface_pointer_type(*operand_type)) {
                throw CodegenError("explicit destructor calls do not support interface pointers in this version",
                                   current_loc_);
            }
        }
        if (expr.type.kind == TypeKind::Named) {
            if (class_has_destructor_in_chain(expr.type.name)) {
                codegen_call_destructor_chain_unless_moved(expr.type.name, ptr, nullptr);
            }
        }
    }

    // Returns `class_name`'s destructor function, if it has one (see
    // parse_class_def's `ClassName_delete` synthesized-name scheme) --
    // nullptr if `class_name` isn't a class, or is one with no destructor
    // defined. A class with no destructor needs no cleanup at scope exit
    // at all (same as a plain struct); this is deliberately *not* an
    // error, unlike a missing constructor for constructor-call syntax
    // (VarDecl's own check) -- a destructor is optional, a constructor
    // call always names one explicitly.
    llvm::Function* find_destructor(const std::string& class_name) {
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_delete") || fn.params.size() != 1) continue;
            const Type& this_param = fn.params[0].type;
            if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
                this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
                continue;
            }
            return module_->getFunction(overload_names_.at(&fn));
        }
        return nullptr;
    }

    // spec §6.5: codegen's own counterpart to movecheck's identically-
    // named helpers (has_user_declared_copy_ctor/copy_assign/dtor and
    // is_copy_constructible/is_copy_assignable) -- kept as a small,
    // separately-duplicated copy per module (the established pattern
    // already used for types_equal, rather than a shared
    // cross-module utility), since codegen has direct Program access
    // (`program_`) rather than movecheck's Body-only architecture.
    [[nodiscard]] const Function* find_user_declared_copy_ctor_ast(const std::string& class_name) {
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_new") || fn.params.size() != 2) continue;
            const Type& this_param = fn.params[0].type;
            if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
                this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
                continue;
            }
            const Type& p = fn.params[1].type;
            if (p.kind == TypeKind::Reference && !p.is_rvalue_ref && !p.is_mutable_ref && p.pointee &&
                p.pointee->kind == TypeKind::Named && p.pointee->name == class_name) {
                return &fn;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Function* find_user_declared_copy_assign_ast(const std::string& class_name) {
        for (const Function& fn : program_->functions) {
            if (!fn.name.ends_with("_operator_assign") || fn.params.size() != 2) continue;
            const Type& this_param = fn.params[0].type;
            if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
                this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
                continue;
            }
            const Type& p = fn.params[1].type;
            if (p.kind == TypeKind::Reference && !p.is_rvalue_ref && !p.is_mutable_ref && p.pointee &&
                p.pointee->kind == TypeKind::Named && p.pointee->name == class_name) {
                return &fn;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool has_user_declared_dtor(const std::string& class_name) {
        return find_destructor(class_name) != nullptr;
    }

    [[nodiscard]] bool is_copy_constructible(const std::string& class_name) {
        if (find_user_declared_copy_ctor_ast(class_name) != nullptr) return true;
        if (has_user_declared_dtor(class_name) || find_user_declared_copy_assign_ast(class_name) != nullptr) {
            return false;
        }
        auto it = structs_.find(class_name);
        if (it == structs_.end()) return false;
        for (const Type& field_type : it->second.field_types) {
            if (!is_field_copy_constructible(field_type)) return false;
        }
        return true;
    }

    [[nodiscard]] bool is_copy_assignable(const std::string& class_name) {
        if (find_user_declared_copy_assign_ast(class_name) != nullptr) return true;
        if (has_user_declared_dtor(class_name) || find_user_declared_copy_ctor_ast(class_name) != nullptr) {
            return false;
        }
        auto it = structs_.find(class_name);
        if (it == structs_.end()) return false;
        for (const Type& field_type : it->second.field_types) {
            if (field_type.kind == TypeKind::Reference) return false;
            if (!is_field_copy_assignable(field_type)) return false;
        }
        return true;
    }

    [[nodiscard]] bool is_field_copy_constructible(const Type& type) {
        if (type.kind == TypeKind::Reference) return true;
        if (type.kind == TypeKind::Array) return is_field_copy_constructible(*type.element);
        if (type.kind == TypeKind::Named && structs_.contains(type.name)) return is_copy_constructible(type.name);
        return true;
    }

    [[nodiscard]] bool is_field_copy_assignable(const Type& type) {
        if (type.kind == TypeKind::Reference) return false;
        if (type.kind == TypeKind::Array) return is_field_copy_assignable(*type.element);
        if (type.kind == TypeKind::Named && structs_.contains(type.name)) return is_copy_assignable(type.name);
        return true;
    }

    // spec §6.5(5): the compiler-provided copy constructor -- a *real*
    // recursive memberwise copy (unlike move construction's whole-
    // aggregate-load shortcut, codegen_stmt's VarDecl case): a nested
    // class-typed field's own copy constructor must actually run (it
    // may be user-declared, with arbitrary side effects -- e.g. spec
    // §6.5's own RefCounted example incrementing a reference count --
    // that a blind byte copy would silently skip), never just copied as
    // bytes. Scalar/struct/raw-pointer/reference fields are bitwise-
    // copied directly (struct is always bitwise-copyable per ch04 §4.1
    // regardless of its own fields; a reference field is simply rebound
    // to the same referent the source has, exactly like move
    // construction's own identical reasoning).
    void codegen_memberwise_copy_construct(llvm::Value* dest_ptr, llvm::Value* src_ptr,
                                            const std::string& class_name) {
        const StructInfo& info = structs_.at(class_name);
        for (size_t i = 0; i < info.field_names.size(); i++) {
            const Type& field_type = info.field_types[i];
            llvm::Value* dest_field = builder_->CreateStructGEP(info.llvm_type, dest_ptr, i, info.field_names[i]);
            llvm::Value* src_field = builder_->CreateStructGEP(info.llvm_type, src_ptr, i, info.field_names[i]);
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                if (const Function* user_ctor = find_user_declared_copy_ctor_ast(field_type.name)) {
                    llvm::Function* ctor = module_->getFunction(overload_names_.at(user_ctor));
                    builder_->CreateCall(ctor, {dest_field, src_field});
                } else {
                    codegen_memberwise_copy_construct(dest_field, src_field, field_type.name);
                }
            } else {
                llvm::Type* llvm_field_type = to_llvm_type(field_type);
                llvm::Value* value = builder_->CreateLoad(llvm_field_type, src_field, "copiedfield");
                create_store(value, dest_field, std::nullopt);
            }
        }
    }

    // spec §6.5(6): the compiler-provided copy assignment operator --
    // symmetric to codegen_memberwise_copy_construct, calling each
    // class-typed field's own copy-assignment operator (never a
    // destructor -- real C++'s own implicitly-defined copy assignment
    // operator recursively copy-*assigns* each member, it never
    // destroys-then-reconstructs one) recursively. No std::unique_ptr
    // field can be present here at all (is_copy_assignable's own
    // eligibility check already precludes it, since std::unique_ptr is
    // never copy-assignable), so there is nothing to release first the
    // way move assignment's own codegen_release_nested_unique_ptrs needs
    // to.
    void codegen_memberwise_copy_assign(llvm::Value* dest_ptr, llvm::Value* src_ptr, const std::string& class_name) {
        const StructInfo& info = structs_.at(class_name);
        for (size_t i = 0; i < info.field_names.size(); i++) {
            const Type& field_type = info.field_types[i];
            llvm::Value* dest_field = builder_->CreateStructGEP(info.llvm_type, dest_ptr, i, info.field_names[i]);
            llvm::Value* src_field = builder_->CreateStructGEP(info.llvm_type, src_ptr, i, info.field_names[i]);
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                if (const Function* user_assign = find_user_declared_copy_assign_ast(field_type.name)) {
                    llvm::Function* op = module_->getFunction(overload_names_.at(user_assign));
                    builder_->CreateCall(op, {dest_field, src_field});
                } else {
                    codegen_memberwise_copy_assign(dest_field, src_field, field_type.name);
                }
            } else {
                llvm::Type* llvm_field_type = to_llvm_type(field_type);
                llvm::Value* value = builder_->CreateLoad(llvm_field_type, src_field, "copiedfield");
                create_store(value, dest_field, std::nullopt);
            }
        }
    }

    [[nodiscard]] bool class_has_destructor_in_chain(const std::string& class_name) {
        std::string current = class_name;
        while (!current.empty()) {
            if (find_destructor(current) != nullptr) return true;
            const ClassDef* def = find_class_def(current);
            if (def == nullptr) break;
            const BaseSpecifier* base = def->direct_ordinary_base();
            if (base == nullptr) break;
            current = base->base_type.name;
        }
        return false;
    }

    void emit_destructor_chain_calls(const std::string& class_name, llvm::Value* object_ptr) {
        if (llvm::Function* dtor = find_destructor(class_name)) {
            builder_->CreateCall(dtor, {object_ptr});
        }
        const ClassDef* def = find_class_def(class_name);
        if (def != nullptr) {
            if (const BaseSpecifier* base = def->direct_ordinary_base()) emit_destructor_chain_calls(base->base_type.name, object_ptr);
        }
    }

    // spec §6.4: a fresh, zero-initialized (`false`) `i1` slot for
    // tracking whether a class-typed local has been moved-out, so
    // scope-exit cleanup (codegen_call_destructor_chain_unless_moved) can
    // correctly skip invoking its destructor chain if so (spec §6.3/§6.4:
    // "its destructor, if declared, is not invoked for it"). Returns
    // null when neither the class nor any of its bases declares a
    // destructor at all.
    llvm::AllocaInst* create_moved_flag_if_has_destructor(const std::string& class_name) {
        if (!class_has_destructor_in_chain(class_name)) return nullptr;
        llvm::AllocaInst* flag = create_entry_block_alloca(llvm::Type::getInt1Ty(*context_), "movedflag");
        builder_->CreateStore(llvm::ConstantInt::getFalse(*context_), flag);
        return flag;
    }

    // spec §6.3/§6.4/ch05 §5.14: emits the whole most-derived-to-base
    // destructor chain guarded by `!moved_flag` when present, matching
    // real C++'s reverse-of-construction destruction order for a derived
    // object's base subobjects.
    void codegen_call_destructor_chain_unless_moved(const std::string& class_name, llvm::Value* object_ptr,
                                                    llvm::AllocaInst* moved_flag) {
        if (moved_flag == nullptr) {
            emit_destructor_chain_calls(class_name, object_ptr);
            return;
        }
        llvm::Value* was_moved = builder_->CreateLoad(llvm::Type::getInt1Ty(*context_), moved_flag, "wasmoved");
        llvm::Function* current_fn = builder_->GetInsertBlock()->getParent();
        llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(*context_, "dtorcall", current_fn);
        llvm::BasicBlock* merge_bb = llvm::BasicBlock::Create(*context_, "dtorskip", current_fn);
        builder_->CreateCondBr(was_moved, merge_bb, then_bb);
        builder_->SetInsertPoint(then_bb);
        emit_destructor_chain_calls(class_name, object_ptr);
        builder_->CreateBr(merge_bb);
        builder_->SetInsertPoint(merge_bb);
    }

    // spec §6.4(5): move-assignment's own "replace the value of each
    // member" step needs the *destination*'s old state torn down first
    // (the book's "destroy the old value" step) before the moved-in bytes
    // overwrite it. Move-construction has no analogous step because its
    // destination is always freshly zero-initialized storage (see
    // codegen_stmt's VarDecl path), so there is nothing old there to
    // release. For a class with a user-declared destructor, that
    // destructor *is* the old-state teardown logic and must run here too;
    // otherwise, recurse through its flattened field layout and tear down
    // only the owning subobjects the compiler itself knows how to manage
    // (builtin std::unique_ptr fields, or nested class fields that in turn
    // need teardown by the same rules). `moved_flag`, when non-null, is
    // the local-slot flag for the *whole* object being overwritten: a
    // self-move-assignment (`x = std::move(x)`) sets it true while
    // evaluating the RHS Move expression, so the old-value teardown
    // correctly becomes a no-op exactly as the spec/book require.
    void codegen_destroy_old_class_state_for_move_assign(llvm::Value* ptr, const std::string& class_name,
                                                         llvm::AllocaInst* moved_flag = nullptr) {
        if (class_has_destructor_in_chain(class_name)) {
            codegen_call_destructor_chain_unless_moved(class_name, ptr, moved_flag);
            return;
        }
        auto struct_it = structs_.find(class_name);
        if (struct_it == structs_.end()) return;
        const StructInfo& info = struct_it->second;
        (void)get_or_declare_free();
        for (size_t i = 0; i < info.field_types.size(); i++) {
            const Type& field_type = info.field_types[i];
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                llvm::Value* field_ptr = builder_->CreateStructGEP(info.llvm_type, ptr, i, info.field_names[i]);
                codegen_destroy_old_class_state_for_move_assign(field_ptr, field_type.name);
            }
        }
    }

    // Releases every *currently in-scope* unique_ptr local's owned
    // resource, and runs every currently-in-scope class-typed local's
    // destructor (ch04 §4.2), if it has one. Called right before each
    // `return` (see StmtKind::Return). `free(NULL)` is a well-defined
    // no-op in C, so unconditionally freeing whatever value is
    // *currently* in each unique_ptr slot is always correct, regardless
    // of whether that local still owns a value, was moved-out (its slot
    // was nulled by Move's codegen), or was never assigned past its own
    // zero-init. A class-typed local has no such "moved-out" state to
    // account for in this version (movecheck disallows reassigning or
    // moving one after construction -- ch04 §4.2's checking is
    // deliberately minimal, see ClassDef's own comment), so its
    // destructor unconditionally runs exactly once, passing the local's
    // own address (not a loaded value -- unlike unique_ptr, the object
    // itself lives directly in the alloca, there is no separate heap
    // allocation this local merely points at). Locals whose block scope
    // already closed before reaching this return were already dropped by
    // pop_scope() and removed from `locals_`, so they aren't double-
    // freed/double-destructed here.
    void free_unique_ptr_locals() {
        for (const auto& [name, slot] : locals_) {
            if (slot.type.kind == TypeKind::Named) {
                if (class_has_destructor_in_chain(slot.type.name)) {
                    codegen_call_destructor_chain_unless_moved(slot.type.name, slot.alloca, slot.moved_flag);
                }
            }
        }
    }

    void push_scope() { scope_stack_.emplace_back(); }

    // Drops every unique_ptr declared directly in the scope being popped,
    // and runs the destructor of every class-typed local declared
    // directly in it that has one (in reverse declaration order, matching
    // C++/Rust destruction order), then removes all of that scope's names
    // from `locals_` so they're correctly treated as out-of-scope
    // afterward (e.g. a variable declared only inside an `if` branch can
    // no longer be referenced once that branch ends). If the current
    // block already has a terminator (e.g. the scope ended in `return`,
    // which already freed/destructed everything via
    // free_unique_ptr_locals), no drop instructions are emitted here --
    // there both to avoid inserting unreachable code after a terminator
    // and to avoid a double free/destruction.
    void pop_scope() {
        std::vector<std::string> names = std::move(scope_stack_.back());
        scope_stack_.pop_back();

        bool already_terminated = builder_->GetInsertBlock()->getTerminator() != nullptr;
        if (!already_terminated) {
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                auto slot_it = locals_.find(*it);
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.type.kind == TypeKind::Named) {
                    if (class_has_destructor_in_chain(slot_it->second.type.name)) {
                        codegen_call_destructor_chain_unless_moved(slot_it->second.type.name, slot_it->second.alloca,
                                                                   slot_it->second.moved_flag);
                    }
                }
            }
        }
        for (const std::string& name : names) {
            locals_.erase(name);
        }
    }

    void emit_scope_cleanup_to_depth(size_t target_depth) {
        for (size_t depth = scope_stack_.size(); depth > target_depth; depth--) {
            const std::vector<std::string>& names = scope_stack_[depth - 1];
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                auto slot_it = locals_.find(*it);
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.type.kind == TypeKind::Named) {
                    if (class_has_destructor_in_chain(slot_it->second.type.name)) {
                        codegen_call_destructor_chain_unless_moved(slot_it->second.type.name, slot_it->second.alloca,
                                                                   slot_it->second.moved_flag);
                    }
                }
            }
        }
    }

    llvm::Function* get_or_declare_malloc() {
        if (llvm::Function* existing = module_->getFunction("malloc")) {
            return existing;
        }
        llvm::PointerType* ptr_type = llvm::PointerType::getUnqual(*context_);
        llvm::FunctionType* malloc_type =
            llvm::FunctionType::get(ptr_type, {llvm::Type::getInt64Ty(*context_)}, /*isVarArg=*/false);
        return llvm::Function::Create(malloc_type, llvm::Function::ExternalLinkage, "malloc", *module_);
    }

    llvm::Function* get_or_declare_free() {
        if (llvm::Function* existing = module_->getFunction("free")) {
            return existing;
        }
        llvm::PointerType* ptr_type = llvm::PointerType::getUnqual(*context_);
        llvm::FunctionType* free_type =
            llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {ptr_type}, /*isVarArg=*/false);
        return llvm::Function::Create(free_type, llvm::Function::ExternalLinkage, "free", *module_);
    }

    llvm::Function* get_or_declare_abort() {
        if (llvm::Function* existing = module_->getFunction("abort")) {
            return existing;
        }
        llvm::FunctionType* abort_type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), /*isVarArg=*/false);
        llvm::Function* fn = llvm::Function::Create(abort_type, llvm::Function::ExternalLinkage, "abort", *module_);
        // libc's abort() never returns -- telling LLVM this lets it treat
        // the code right after a call to it as unreachable, same as real
        // Clang does.
        fn->addFnAttr(llvm::Attribute::NoReturn);
        return fn;
    }

    // Emits a runtime bounds check for a `std::span<T>` subscript (spec
    // ch08's "insert runtime bounds checks by default" decision): if
    // `index` is negative or `>= size`, calls libc's `abort()` (v0.1's
    // panic model, per ch08) instead of proceeding. Splits the current
    // block into a `bounds.fail` block (unreachable after the call) and a
    // `bounds.ok` block, leaving the builder's insert point at the latter
    // so the caller can continue emitting the actual element access.
    // Skipped entirely inside `unsafe { }` (unsafe_depth_ > 0, ch01
    // §1.3) -- same treatment, and for the same reason, as
    // codegen_checked_arith/codegen_checked_div: a scpp-inserted
    // *runtime* check, not an otherwise-illegal operation, so skipping
    // it carries none of the "corrupted bookkeeping leaking into
    // surrounding checked code" risk that keeps movecheck's own checks
    // unconditional.
    void emit_span_bounds_check(llvm::Value* index, llvm::Value* size) {
        if (unsafe_depth_ > 0) return;

        llvm::Function* current_function = builder_->GetInsertBlock()->getParent();
        llvm::BasicBlock* fail_block = llvm::BasicBlock::Create(*context_, "bounds.fail", current_function);
        llvm::BasicBlock* ok_block = llvm::BasicBlock::Create(*context_, "bounds.ok", current_function);

        llvm::Value* index64 = builder_->CreateSExt(index, llvm::Type::getInt64Ty(*context_), "idx64");
        llvm::Value* too_low =
            builder_->CreateICmpSLT(index64, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), 0), "toolow");
        llvm::Value* too_high = builder_->CreateICmpSGE(index64, size, "toohigh");
        llvm::Value* out_of_bounds = builder_->CreateOr(too_low, too_high, "oob");
        builder_->CreateCondBr(out_of_bounds, fail_block, ok_block);

        builder_->SetInsertPoint(fail_block);
        builder_->CreateCall(get_or_declare_abort(), {});
        builder_->CreateUnreachable();

        builder_->SetInsertPoint(ok_block);
    }

    // ch06 §6: the numeric family's own signed/unsigned/floating
    // classification, by scpp type name -- LLVM draws no signed/
    // unsigned distinction at the *type* level (only i8/i16/i32/i64),
    // so every integer arithmetic/comparison/division instruction that
    // cares about signedness (sdiv vs udiv, icmp slt vs icmp ult, ...)
    // needs this to pick the right one. `bool`/`char` are deliberately
    // excluded from both is_unsigned/is_float (neither is ever true for
    // them) *and* is_checked_arithmetic_scalar below (ch06: no
    // arithmetic is defined for them yet, pre-existing, unchanged
    // scope) -- every other named scalar (the numeric family proper) is
    // checked.
    [[nodiscard]] static bool is_float_scalar_type_name(const std::string& name) {
        return name == "float" || name == "double" || name == "float32_t" || name == "float64_t";
    }
    [[nodiscard]] static bool is_integral_scalar_type_name(const std::string& name) {
        return name == "char" || name == "int" || name == "long" || name == "unsigned int" ||
               name == "unsigned long" || name == "int8_t" || name == "int16_t" || name == "int32_t" ||
               name == "int64_t" || name == "uint8_t" || name == "uint16_t" || name == "uint32_t" ||
               name == "uint64_t" || name == "size_t" || name == "ptrdiff_t";
    }
    [[nodiscard]] static bool is_unsigned_scalar_type_name(const std::string& name) {
        return name == "unsigned int" || name == "unsigned long" || name == "uint8_t" || name == "uint16_t" ||
               name == "uint32_t" || name == "uint64_t" || name == "size_t";
    }
    [[nodiscard]] static bool is_checked_arithmetic_scalar_type_name(const std::string& name) {
        return name != "bool" && name != "char";
    }

    // ch06 §6: whether `name`'s own values should be treated as unsigned
    // when *converting* to/from it (a cast, never an arithmetic
    // operation -- see is_unsigned_scalar_type_name above for that
    // narrower, arithmetic-only question) -- widens is_unsigned_scalar_
    // type_name to also cover `bool`/`char`: both are always
    // zero-extended when widened (a bool is always the bit pattern 0/1;
    // char's ordinal value 0-255 already matches how every other part of
    // this codebase treats it, e.g. CharLiteral's own codegen), even
    // though neither counts as "unsigned" for arithmetic/overflow-
    // checking purposes (ch06: no arithmetic is defined for either yet).
    [[nodiscard]] static bool is_unsigned_for_cast(const std::string& name) {
        return name == "bool" || name == "char" || is_unsigned_scalar_type_name(name);
    }

    [[nodiscard]] std::string scalar_name_for_cast(const Type& type) const {
        if (type.kind != TypeKind::Named) return {};
        if (const EnumDef* def = find_enum_def(program_, type.name)) return def->underlying_type.name;
        return type.name;
    }

    // ch06 §6: the actual conversion instruction for `static_cast<T>(expr)`/
    // `(T)expr`, given `value` already evaluated as `source_type`'s own
    // LLVM representation -- dispatches on the (source, target) type
    // pair exactly like a real compiler's own cast-kind selection:
    // int<->int (sext/zext for widening -- signedness from the *source*
    // type, matching real C++'s own conversion rules; trunc for
    // narrowing; a bare bitcast-free no-op when both are the exact same
    // width, e.g. int8_t<->uint8_t<->char<->bool, all i8), float<->float
    // (fpext widening / fptrunc narrowing), and float<->int (sitofp/
    // uitofp, fptosi/fptoui, signedness from whichever side is the
    // integer one).
    llvm::Value* codegen_scalar_cast(llvm::Value* value, const Type& source_type, const Type& target_type) {
        llvm::Type* target_llvm = to_llvm_type(target_type);
        if (value->getType() == target_llvm) return value;
        std::string source_name = scalar_name_for_cast(source_type);
        std::string target_name = scalar_name_for_cast(target_type);
        bool source_is_float = is_float_scalar_type_name(source_name);
        bool target_is_float = is_float_scalar_type_name(target_name);
        if (source_is_float && target_is_float) {
            return value->getType()->getScalarSizeInBits() < target_llvm->getScalarSizeInBits()
                       ? builder_->CreateFPExt(value, target_llvm, "fpexttmp")
                       : builder_->CreateFPTrunc(value, target_llvm, "fptrunctmp");
        }
        if (source_is_float) {
            return is_unsigned_for_cast(target_name) ? builder_->CreateFPToUI(value, target_llvm, "fptouitmp")
                                                     : builder_->CreateFPToSI(value, target_llvm, "fptositmp");
        }
        if (target_is_float) {
            return is_unsigned_for_cast(source_name) ? builder_->CreateUIToFP(value, target_llvm, "uitofptmp")
                                                     : builder_->CreateSIToFP(value, target_llvm, "sitofptmp");
        }
        // int -> int: same width already returned `value` unchanged
        // above (e.g. int8_t <-> uint8_t <-> char <-> bool).
        if (value->getType()->getScalarSizeInBits() < target_llvm->getScalarSizeInBits()) {
            return is_unsigned_for_cast(source_name) ? builder_->CreateZExt(value, target_llvm, "zexttmp")
                                                     : builder_->CreateSExt(value, target_llvm, "sexttmp");
        }
        return builder_->CreateTrunc(value, target_llvm, "trunctmp");
    }

    // `+`/`-`/`*` on a floating-point type (ch06 §6): IEEE-754 arithmetic
    // has no UB-on-overflow concept to guard against at all (overflow
    // produces +/-infinity, underflow a signed zero or subnormal, both
    // well-defined by the standard itself) -- so, unlike the integer
    // path below, there is nothing to check regardless of unsafe
    // context; always the plain fadd/fsub/fmul instruction.
    llvm::Value* codegen_float_arith(BinaryOp op, llvm::Value* lhs, llvm::Value* rhs) {
        switch (op) {
            case BinaryOp::Add: return builder_->CreateFAdd(lhs, rhs, "faddtmp");
            case BinaryOp::Sub: return builder_->CreateFSub(lhs, rhs, "fsubtmp");
            case BinaryOp::Mul: return builder_->CreateFMul(lhs, rhs, "fmultmp");
            default: throw CodegenError("unhandled floating-point arithmetic operator",
                current_loc_);
        }
    }

    // `+`/`-`/`*` (ch05 §5.8): overflow-checked by default (aborting,
    // via the same panic mechanism as emit_span_bounds_check, on
    // overflow -- both signed and unsigned per the spec), or a plain,
    // guaranteed-wrapping (never UB) operation inside `unsafe { }`
    // (unsafe_depth_ > 0) -- achieved simply by using the plain
    // CreateAdd/CreateSub/CreateMul instructions, which (unlike real
    // Clang's) never get an `nsw`/`nuw` flag anywhere in this codebase, so
    // they're already well-defined, wrapping LLVM IR on their own.
    // `is_checked` is false for `bool`/`char` (ch06: no arithmetic is
    // defined for either yet, pre-existing, unchanged scope) -- every
    // other integer width, signed or unsigned, is checked regardless of
    // width (generalized from this codebase's original int-only, i32-only
    // scope now that the rest of the numeric family exists).
    llvm::Value* codegen_checked_arith(BinaryOp op, llvm::Value* lhs, llvm::Value* rhs, bool is_unsigned,
                                        bool is_checked) {
        const char* name = op == BinaryOp::Add ? "addtmp" : op == BinaryOp::Sub ? "subtmp" : "multmp";
        if (unsafe_depth_ > 0 || !is_checked) {
            switch (op) {
                case BinaryOp::Add: return builder_->CreateAdd(lhs, rhs, name);
                case BinaryOp::Sub: return builder_->CreateSub(lhs, rhs, name);
                case BinaryOp::Mul: return builder_->CreateMul(lhs, rhs, name);
                default: throw CodegenError("unhandled checked-arithmetic operator",
                    current_loc_);
            }
        }

        llvm::Intrinsic::ID intrinsic_id =
            op == BinaryOp::Add
                ? (is_unsigned ? llvm::Intrinsic::uadd_with_overflow : llvm::Intrinsic::sadd_with_overflow)
            : op == BinaryOp::Sub
                ? (is_unsigned ? llvm::Intrinsic::usub_with_overflow : llvm::Intrinsic::ssub_with_overflow)
                : (is_unsigned ? llvm::Intrinsic::umul_with_overflow : llvm::Intrinsic::smul_with_overflow);
        llvm::Function* intrinsic =
            llvm::Intrinsic::getOrInsertDeclaration(module_.get(), intrinsic_id, {lhs->getType()});
        llvm::Value* pair = builder_->CreateCall(intrinsic, {lhs, rhs}, name);
        llvm::Value* result = builder_->CreateExtractValue(pair, 0, name);
        llvm::Value* overflowed = builder_->CreateExtractValue(pair, 1, "overflow");

        llvm::Function* current_function = builder_->GetInsertBlock()->getParent();
        llvm::BasicBlock* fail_block = llvm::BasicBlock::Create(*context_, "overflow.fail", current_function);
        llvm::BasicBlock* ok_block = llvm::BasicBlock::Create(*context_, "overflow.ok", current_function);
        builder_->CreateCondBr(overflowed, fail_block, ok_block);

        builder_->SetInsertPoint(fail_block);
        builder_->CreateCall(get_or_declare_abort(), {});
        builder_->CreateUnreachable();

        builder_->SetInsertPoint(ok_block);
        return result;
    }

    // `/` (ch05 §5.8): `b == 0` always abort() -- unconditionally, in
    // *every* context, whether inside `unsafe { }` or not, unlike +/-/*
    // above: division traps at the hardware level (x86 #DE) with no
    // wrapped result for the hardware to fall back on, so there is no
    // "unsafe and still defined" variant to fall back to here. The one
    // *signed*-only extra trap, `a == MIN && b == -1` (signed division's
    // own overflow case -- unsigned division has no analogous overflow
    // at all, MIN there is just 0 and 0/-1 isn't even representable as
    // -1), generalized from this codebase's original int32-only scope to
    // any integer width via getBitWidth()/getSignedMinValue() rather
    // than a hardcoded int32_t::min(). `is_checked` is false for
    // `bool`/`char` -- see codegen_checked_arith's identical reasoning.
    llvm::Value* codegen_checked_div(llvm::Value* lhs, llvm::Value* rhs, bool is_unsigned, bool is_checked) {
        if (!is_checked) {
            return is_unsigned ? builder_->CreateUDiv(lhs, rhs, "divtmp") : builder_->CreateSDiv(lhs, rhs, "divtmp");
        }

        llvm::IntegerType* int_ty = llvm::cast<llvm::IntegerType>(lhs->getType());
        llvm::Value* zero = llvm::ConstantInt::get(int_ty, 0);
        llvm::Value* divides_by_zero = builder_->CreateICmpEQ(rhs, zero, "divzero");
        llvm::Value* traps = divides_by_zero;
        if (!is_unsigned) {
            llvm::APInt min_value = llvm::APInt::getSignedMinValue(int_ty->getBitWidth());
            llvm::Value* int_min = llvm::ConstantInt::get(*context_, min_value);
            llvm::Value* neg_one = llvm::ConstantInt::getSigned(int_ty, -1);
            llvm::Value* overflows = builder_->CreateAnd(builder_->CreateICmpEQ(lhs, int_min, "isintmin"),
                                                           builder_->CreateICmpEQ(rhs, neg_one, "isnegone"),
                                                           "divoverflow");
            traps = builder_->CreateOr(divides_by_zero, overflows, "divtraps");
        }

        llvm::Function* current_function = builder_->GetInsertBlock()->getParent();
        llvm::BasicBlock* fail_block = llvm::BasicBlock::Create(*context_, "div.fail", current_function);
        llvm::BasicBlock* ok_block = llvm::BasicBlock::Create(*context_, "div.ok", current_function);
        builder_->CreateCondBr(traps, fail_block, ok_block);

        builder_->SetInsertPoint(fail_block);
        builder_->CreateCall(get_or_declare_abort(), {});
        builder_->CreateUnreachable();

        builder_->SetInsertPoint(ok_block);
        return is_unsigned ? builder_->CreateUDiv(lhs, rhs, "divtmp") : builder_->CreateSDiv(lhs, rhs, "divtmp");
    }

    llvm::Value* codegen_pointer_offset(llvm::Value* base_ptr, llvm::Value* offset, const Type& pointer_type, bool negate_offset) {
        llvm::Value* gep_offset = negate_offset ? builder_->CreateNeg(offset, "ptroffset") : offset;
        return builder_->CreateGEP(to_llvm_type(*pointer_type.pointee), base_ptr, {gep_offset}, "ptrarith");
    }

    llvm::Value* codegen_pointer_difference(llvm::Value* lhs_ptr, llvm::Value* rhs_ptr, const Type& pointer_type) {
        llvm::Type* diff_type = to_llvm_type(named_type("ptrdiff_t"));
        llvm::Value* lhs_int = builder_->CreatePtrToInt(lhs_ptr, diff_type, "lhsint");
        llvm::Value* rhs_int = builder_->CreatePtrToInt(rhs_ptr, diff_type, "rhsint");
        llvm::Value* byte_diff = builder_->CreateSub(lhs_int, rhs_int, "ptrbytes");
        uint64_t elem_size = module_->getDataLayout().getTypeAllocSize(to_llvm_type(*pointer_type.pointee));
        if (elem_size == 1) return byte_diff;
        llvm::Value* elem_size_value = llvm::ConstantInt::get(diff_type, elem_size, /*isSigned=*/false);
        return builder_->CreateSDiv(byte_diff, elem_size_value, "ptrdifftmp");
    }

    // Computes the storage location (pointer + scpp Type) of an lvalue
    // expression, i.e. anything that can appear on the left of `=` or be
    // read via a plain load: a variable, or a chain of `.field`/`[index]`
    // off of one. Member-of-call-result (e.g. `f().x` where f returns a
    // struct by value) is intentionally not supported yet since it has no
    // backing storage to take a pointer to; that is deferred to whenever
    // by-value struct temporaries need addressable storage.
    LValue codegen_lvalue(const Expr& expr) {
        // Same refresh discipline as codegen_expr above.
        refresh_debug_location(expr.loc);
        switch (expr.kind) {
            case ExprKind::Identifier: {
                auto it = locals_.find(expr.name);
                if (it == locals_.end()) {
                    throw CodegenError("use of undeclared variable '" + expr.name + "'",
                        current_loc_);
                }
                if (it->second.type.kind == TypeKind::Reference) {
                    if (is_interface_reference_type(it->second.type)) {
                        return LValue{it->second.alloca, it->second.type, alignment_for_type(it->second.type)};
                    }
                    // A reference-typed local's own alloca just holds the
                    // address it's bound to (see the VarDecl case below,
                    // and how a Reference parameter arrives already as
                    // that address): auto-dereference once so every
                    // caller (reads, writes-through, and Member/Subscript
                    // base resolution) transparently operates on the
                    // referent, exactly like a real C++ reference.
                    llvm::Value* referent_ptr =
                        create_load(llvm::PointerType::getUnqual(*context_), it->second.alloca, std::nullopt, "deref");
                    return LValue{referent_ptr, *it->second.type.pointee, alignment_for_type(*it->second.type.pointee)};
                }
                return LValue{it->second.alloca, it->second.type, alignment_for_type(it->second.type)};
            }

            case ExprKind::Member: {
                LValue base = codegen_lvalue(*expr.lhs);
                if (base.type.kind != TypeKind::Named || !structs_.contains(base.type.name)) {
                    throw CodegenError("member access '." + expr.name + "' on a non-struct type",
                        current_loc_);
                }
                const StructInfo& info = structs_.at(base.type.name);
                std::optional<size_t> field_index_opt = info.find_field_index(expr.name);
                if (!field_index_opt.has_value()) {
                    throw CodegenError(std::string(info.is_union ? "union '" : "struct '") + base.type.name +
                                           "' has no field '" + expr.name + "'",
                        current_loc_);
                }
                size_t field_index = *field_index_opt;
                const Type& field_type = info.field_types[field_index];
                std::optional<llvm::Align> field_alignment =
                    info.is_union ? (base.alignment.has_value() ? base.alignment : alignment_for_type(base.type))
                                  : (info.is_packed ? std::optional<llvm::Align>(llvm::Align(1))
                                                    : alignment_for_type(field_type));
                llvm::Value* field_ptr = info.is_union
                                             ? builder_->CreateBitCast(base.ptr, llvm::PointerType::get(
                                                                                          to_llvm_type(field_type)->getContext(),
                                                                                          0),
                                                                       expr.name + ".unionfield")
                                             : builder_->CreateStructGEP(info.llvm_type, base.ptr, field_index, expr.name);
                if (field_type.kind == TypeKind::Reference) {
                    if (is_interface_reference_type(field_type)) {
                        return LValue{field_ptr, field_type, field_alignment};
                    }
                    // ch05 §5.12: a Reference-typed field (e.g. a
                    // closure's own by-reference capture) stores just
                    // the address it's bound to, exactly like a
                    // Reference-typed local's own alloca (see the
                    // Identifier case above) -- auto-dereference once so
                    // every caller (reads, writes-through, and further
                    // Member/Subscript base resolution) transparently
                    // operates on the referent, not the field's own
                    // storage slot.
                    llvm::Value* referent_ptr =
                        create_load(llvm::PointerType::getUnqual(*context_), field_ptr, field_alignment, "fieldderef");
                    return LValue{referent_ptr, *field_type.pointee, alignment_for_type(*field_type.pointee)};
                }
                return LValue{field_ptr, field_type, field_alignment};
            }

            case ExprKind::Subscript: {
                LValue base = codegen_lvalue(*expr.lhs);
                if (base.type.kind == TypeKind::Span) {
                    llvm::Type* span_type = to_llvm_type(base.type);
                    llvm::Value* size_ptr = builder_->CreateStructGEP(span_type, base.ptr, 1, "sizeptr");
                    llvm::Value* size = builder_->CreateLoad(llvm::Type::getInt64Ty(*context_), size_ptr, "size");
                    llvm::Value* data_ptr = builder_->CreateStructGEP(span_type, base.ptr, 0, "dataptr");
                    llvm::Value* data = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), data_ptr, "data");
                    llvm::Value* index = codegen_expr(*expr.rhs);
                    // Runtime bounds check (spec ch08: checked by default,
                    // bounds checks inserted unconditionally) -- unlike a
                    // fixed-size array's subscript below, a span's length is
                    // only known at runtime, so there's no way to reject an
                    // out-of-bounds constant index at compile time.
                    emit_span_bounds_check(index, size);
                    llvm::Value* elem_ptr =
                        builder_->CreateGEP(to_llvm_type(*base.type.pointee), data, {index}, "elemtmp");
                    return LValue{elem_ptr, *base.type.pointee, alignment_for_type(*base.type.pointee)};
                }
                if (base.type.kind == TypeKind::Pointer) {
                    llvm::Value* data = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), base.ptr, "data");
                    llvm::Value* index = codegen_expr(*expr.rhs);
                    llvm::Value* elem_ptr =
                        builder_->CreateGEP(to_llvm_type(*base.type.pointee), data, {index}, "elemtmp");
                    return LValue{elem_ptr, *base.type.pointee, alignment_for_type(*base.type.pointee)};
                }
                if (base.type.kind != TypeKind::Array) {
                    throw CodegenError("subscript on a non-array type",
                        current_loc_);
                }
                llvm::Value* index = codegen_expr(*expr.rhs);
                llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                llvm::Value* elem_ptr =
                    builder_->CreateGEP(to_llvm_type(base.type), base.ptr, {zero, index}, "elemtmp");
                return LValue{elem_ptr, *base.type.element, alignment_for_type(*base.type.element)};
            }

            case ExprKind::Call: {
                // Reachable whenever a call to a reference-returning
                // function is itself used as a reference-binding source
                // (`T& r = f(x);`), a reference argument (`g(f(x))`), or
                // forwarded in a `return` -- see
                // resolve_borrow_source_root in movecheck.cppm.
                // codegen_call's raw result is already the referent's
                // address in that case -- no load needed, unlike
                // codegen_expr's own Call case. Validity (must actually
                // be reference-returning) is checked *after* codegen_call
                // returns rather than before, unlike the pre-method-call
                // version of this code -- codegen_call must run first
                // regardless, to resolve a possible method-call receiver
                // exactly once; an invalid program reaching this far
                // would already have been rejected by movecheck, so
                // emitting (and then discarding, via the throw below) a
                // few extra instructions first is harmless.
                CallResult result = codegen_call(expr);
                if (result.callee_def == nullptr || result.callee_def->return_type.kind != TypeKind::Reference) {
                    throw CodegenError("expression is not assignable",
                        current_loc_);
                }
                if (is_interface_reference_type(result.callee_def->return_type)) {
                    llvm::AllocaInst* slot =
                        create_entry_block_alloca(to_llvm_type(result.callee_def->return_type), "ifacereftmp");
                    create_store(result.value, slot, alignment_for_type(result.callee_def->return_type));
                    return LValue{slot, result.callee_def->return_type, alignment_for_type(result.callee_def->return_type)};
                }
                return LValue{result.value, *result.callee_def->return_type.pointee,
                              alignment_for_type(*result.callee_def->return_type.pointee)};
            }

            case ExprKind::Lambda: {
                // ch05 §5.12: an IIFE's receiver (`[](...){...}(args)`,
                // parser.cppm's own Lambda-followed-by-`(` case) needs
                // the constructed closure's *address* to invoke its
                // "call" method on -- exactly like an ordinary method
                // call's receiver (codegen_call's own `expr.lhs != nullptr`
                // branch calls codegen_lvalue on it uniformly, regardless
                // of receiver shape).
                llvm::Value* ptr = codegen_construct_lambda(expr);
                return LValue{ptr, named_type(expr.name),
                              alignment_for_type(named_type(expr.name))};
            }

            case ExprKind::Move:
                return codegen_lvalue(*expr.lhs);

            case ExprKind::Cast: {
                if (expr.type.kind != TypeKind::Pointer) {
                    throw CodegenError("expression is not assignable", current_loc_);
                }
                llvm::Value* value = codegen_expr(expr);
                llvm::AllocaInst* slot = create_entry_block_alloca(to_llvm_type(expr.type), "castptrtmp");
                create_store(value, slot, alignment_for_type(expr.type));
                return LValue{slot, expr.type, alignment_for_type(expr.type)};
            }

            case ExprKind::Unary: {
                // Only `*p` (Deref) is addressable; Neg/Not produce a
                // plain value with no backing storage.
                if (expr.unary_op != UnaryOp::Deref) {
                    throw CodegenError("expression is not assignable",
                        current_loc_);
                }
                if (expr.lhs->kind == ExprKind::Identifier && expr.lhs->name == "this") {
                    // parser/movecheck model `this` as a reference-typed
                    // pseudo-parameter, but ch05 §5.9 keeps the real-C++
                    // `(*this).x` spelling valid at expression level. That
                    // makes `*this` just an explicit spelling of the same
                    // referent codegen_lvalue(Identifier "this") already
                    // resolves.
                    return codegen_lvalue(*expr.lhs);
                }
                LValue operand = codegen_lvalue(*expr.lhs);
                if (operand.type.kind == TypeKind::Named) {
                    std::vector<ExprPtr> no_args;
                    bool receiver_is_mutable = !is_read_only_place(*expr.lhs);
                    if (const Function* callee_def =
                            resolve_overload_by_type(operand.type.name + "_operator_deref", no_args, 1,
                                                     receiver_is_mutable, expr.lhs.get())) {
                        llvm::Function* callee = module_->getFunction(overload_names_.at(callee_def));
                        if (callee == nullptr) {
                            throw CodegenError("call to unknown function '" + operand.type.name + "_operator_deref'",
                                current_loc_);
                        }
                        llvm::Value* referent_ptr = builder_->CreateCall(callee, {operand.ptr});
                        if (callee_def->return_type.kind != TypeKind::Reference) {
                            throw CodegenError("operator* on class '" + operand.type.name +
                                                   "' must return a reference to be assignable",
                                current_loc_);
                        }
                        return LValue{referent_ptr, *callee_def->return_type.pointee,
                                      alignment_for_type(*callee_def->return_type.pointee)};
                    }
                }
                if (operand.type.kind != TypeKind::Pointer) {
                    // Whether a raw pointer dereference is licensed here
                    // (ch01 §1.3: only inside `unsafe {}`) is the move
                    // checker's job (scpp.movecheck), not codegen's --
                    // by the time a program reaches codegen it's already
                    // been accepted, so this is purely an "operand has no
                    // sensible address to load" guard. A reference
                    // operand can't reach here at all (codegen_lvalue's
                    // own Identifier case already auto-dereferences a
                    // reference-typed local, so `*r` where `r` is `T&`
                    // would already have `r` resolved to its referent by
                    // the time this runs).
                    throw CodegenError("dereference ('*') is only supported for a raw pointer or a class with operator*",
                        current_loc_);
                }
                if (is_interface_pointer_type(operand.type)) {
                    throw CodegenError("dereferencing an interface pointer does not yield an assignable storage location",
                        current_loc_);
                }
                // A unique_ptr's/raw pointer's own storage holds the
                // pointer *value* (see to_llvm_type's UniquePtr/Pointer
                // case); dereferencing means loading that value and using
                // it as the new base address, exactly like a reference's
                // own auto-deref above.
                llvm::Value* pointee_ptr =
                    create_load(llvm::PointerType::getUnqual(*context_), operand.ptr, operand.alignment, "deref");
                return LValue{pointee_ptr, *operand.type.pointee, alignment_for_type(*operand.type.pointee)};
            }

            default:
                throw CodegenError("expression is not assignable",
                    current_loc_);
        }
    }

    // `print_int`/`print_bool`/`print_char` are temporary builtins that
    // shell out to libc's `printf` so programs can produce visible output
    // before the language grows a real string type (tracked for M2+). All
    // three return the usual `printf` result (an i32) so they can be used
    // like any other call.
    llvm::Value* codegen_builtin_print(const Expr& expr) {
        if (expr.args.size() != 1) {
            throw CodegenError(expr.name + " expects exactly 1 argument",
                current_loc_);
        }
        llvm::Function* printf_fn = get_or_declare_printf();
        llvm::Value* arg = codegen_expr(*expr.args[0]);

        llvm::Value* format;
        llvm::Value* printf_arg;
        if (expr.name == "print_int") {
            format = builder_->CreateGlobalString("%d\n", "fmt_int");
            printf_arg = arg;
        } else if (expr.name == "print_char") {
            format = builder_->CreateGlobalString("%c\n", "fmt_char");
            // C's variadic calling convention always promotes a `char`
            // argument to `int` (the same "default argument promotion"
            // real C/C++ applies to any variadic call) -- printf's `%c`
            // reads a full `int`-sized argument regardless of the
            // narrower declared parameter type, so the raw i8 value must
            // be sign-extended before being passed through `...` here.
            printf_arg = builder_->CreateSExt(arg, llvm::Type::getInt32Ty(*context_), "charpromo");
        } else {
            format = builder_->CreateGlobalString("%s\n", "fmt_bool");
            llvm::Value* true_str = builder_->CreateGlobalString("true", "str_true");
            llvm::Value* false_str = builder_->CreateGlobalString("false", "str_false");
            // `arg` is the i8 bool representation (see to_llvm_type);
            // CreateSelect needs a 1-bit condition.
            printf_arg = builder_->CreateSelect(bool_to_i1(arg), true_str, false_str, "booltmp");
        }
        return builder_->CreateCall(printf_fn, {format, printf_arg});
    }

    llvm::Function* get_or_declare_printf() {
        if (llvm::Function* existing = module_->getFunction("printf")) {
            return existing;
        }
        llvm::PointerType* char_ptr_type = llvm::PointerType::getUnqual(*context_);
        llvm::FunctionType* printf_type =
            llvm::FunctionType::get(llvm::Type::getInt32Ty(*context_), {char_ptr_type}, /*isVarArg=*/true);
        return llvm::Function::Create(printf_type, llvm::Function::ExternalLinkage, "printf", *module_);
    }

    llvm::Value* codegen_binary(const Expr& expr) {
        if (expr.binary_op == BinaryOp::Assign) {
            LValue lv = codegen_lvalue(*expr.lhs);
            // spec §6.5: `y = x;` -- copy assignment (movecheck has
            // already verified `x` is the exact same class type and
            // that the class is copy-assignable) -- checked *before*
            // the generic value-evaluation path below, since this needs
            // to dispatch to a real function call (the user-declared
            // operator=, so its own side effects -- e.g. incrementing a
            // reference count -- actually run) or a recursive
            // memberwise copy-assign, neither of which is "evaluate the
            // RHS as one flat value, then store it" the way every other
            // assignment kind (including move assignment, which reuses
            // that same generic path below) is.
            if (lv.type.kind == TypeKind::Named && structs_.contains(lv.type.name) &&
                expr.rhs->kind == ExprKind::Identifier) {
                auto src_it = locals_.find(expr.rhs->name);
                if (src_it != locals_.end() && types_equal(src_it->second.type, lv.type)) {
                    if (const Function* user_assign = find_user_declared_copy_assign_ast(lv.type.name)) {
                        llvm::Function* op = module_->getFunction(overload_names_.at(user_assign));
                        builder_->CreateCall(op, {lv.ptr, src_it->second.alloca});
                    } else {
                        codegen_memberwise_copy_assign(lv.ptr, src_it->second.alloca, lv.type.name);
                    }
                    if (expr.lhs->kind == ExprKind::Identifier) {
                        // See the move-assignment path's identical
                        // comment below for why this reset is needed
                        // (covers reassigning a previously-moved-out
                        // variable via a copy this time).
                        auto target_it = locals_.find(expr.lhs->name);
                        if (target_it != locals_.end() && target_it->second.moved_flag != nullptr) {
                            builder_->CreateStore(llvm::ConstantInt::getFalse(*context_),
                                                   target_it->second.moved_flag);
                        }
                    }
                    return lv.ptr;
                }
            }
            llvm::Value* value = codegen_value_for_target(*expr.rhs, lv.type);
            // Refresh to `expr`'s own position -- see the VarDecl case's
            // identical comment in codegen_stmt.
            refresh_debug_location(expr.loc);
            check_store_type(value, to_llvm_type(lv.type), "'" + expr.lhs->name + "'");
            if (lv.type.kind == TypeKind::Named && structs_.contains(lv.type.name)) {
                // spec §6.4(3)/(5): `y = std::move(x);` -- the compiler-
                // synthesized move assignment operator (movecheck has
                // already verified `expr.rhs` is exactly this shape --
                // ordinary class reassignment is rejected before this
                // point is ever reached, see check_moves). Tear down
                // whatever `y` already owns before the `CreateStore`
                // below overwrites it wholesale with the source's own
                // bytes -- `value` above already came from `codegen_expr`
                // on the Move expression, which already nulled (and, for
                // a local class with a destructor, marked moved-out) the
                // source's slot, exactly like move-construction's own
                // identical reasoning (codegen_stmt's VarDecl case).
                llvm::AllocaInst* target_moved_flag = nullptr;
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto target_it = locals_.find(expr.lhs->name);
                    if (target_it != locals_.end()) target_moved_flag = target_it->second.moved_flag;
                }
                codegen_destroy_old_class_state_for_move_assign(lv.ptr, lv.type.name, target_moved_flag);
            }
            create_store(value, lv.ptr, lv.alignment);
            if (lv.type.kind == TypeKind::Named && expr.lhs->kind == ExprKind::Identifier) {
                // spec §6.2(4)/§6.4: an assignment always leaves its own
                // target in the initialized state, holding the newly
                // assigned value -- including the (real, discovered-and-
                // fixed) self-move-assignment case `a = std::move(a);`,
                // where evaluating the RHS above transiently sets `a`'s
                // *own* moved_flag true as a side effect of `a` being the
                // Move's own source (see codegen_expr's Move case) before
                // this same statement's target (also `a`) is overwritten
                // right back with its own (unaliased-copy-preserved)
                // original value. Without this reset, `a`'s destructor
                // would be wrongly skipped at its own later scope-exit,
                // even though it again fully owns a valid value. Also
                // covers reassigning a *previously* moved-out variable
                // (its moved_flag would otherwise still read true from
                // that earlier move, despite this assignment giving it a
                // brand new value).
                auto target_it = locals_.find(expr.lhs->name);
                if (target_it != locals_.end() && target_it->second.moved_flag != nullptr) {
                    builder_->CreateStore(llvm::ConstantInt::getFalse(*context_), target_it->second.moved_flag);
                }
            }
            return value;
        }

        // `&&`/`||` short-circuit like ordinary C++; everything else is a
        // plain eager binary op on the operand values.
        if (expr.binary_op == BinaryOp::And || expr.binary_op == BinaryOp::Or) {
            return codegen_short_circuit(expr);
        }

        // ch06 §6: an operand that's a bare literal has no fixed type of
        // its own (see codegen_value_for_target) -- infer a "context
        // type" from whichever side is *not* a literal (if either is)
        // before evaluating either operand, so e.g. `int64_t x = c + 1;`
        // generates `1` directly as an i64 constant rather than the
        // default i32 (which would otherwise mismatch `c` and fail
        // LLVM's own module verifier at the arithmetic instruction
        // itself, a much less clear diagnostic than check_store_type's
        // own). Movecheck has already rejected any two-distinct-real-
        // scalar-type mismatch, so this can never itself paper over a
        // genuine type error -- only ever resolves an otherwise-untyped
        // literal.
        bool lhs_is_literal = expr.lhs->kind == ExprKind::IntegerLiteral || expr.lhs->kind == ExprKind::FloatLiteral;
        bool rhs_is_literal = expr.rhs->kind == ExprKind::IntegerLiteral || expr.rhs->kind == ExprKind::FloatLiteral;
        std::optional<Type> lhs_type = infer_type(*expr.lhs);
        std::optional<Type> rhs_type = infer_type(*expr.rhs);
        if ((expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne) && lhs_type.has_value() && rhs_type.has_value() &&
            is_interface_pointer_type(binary_operand_type(*lhs_type)) && is_interface_pointer_type(binary_operand_type(*rhs_type))) {
            llvm::Value* lhs_object = extract_interface_object_ptr(codegen_expr(*expr.lhs));
            llvm::Value* rhs_object = extract_interface_object_ptr(codegen_expr(*expr.rhs));
            return i1_to_bool(expr.binary_op == BinaryOp::Eq ? builder_->CreateICmpEQ(lhs_object, rhs_object, "eqtmp")
                                                             : builder_->CreateICmpNE(lhs_object, rhs_object, "netmp"));
        }
        std::optional<Type> pointer_result_type =
            lhs_type.has_value() && rhs_type.has_value() ? pointer_arithmetic_result_type(expr.binary_op, *lhs_type, *rhs_type)
                                                         : std::nullopt;
        bool arithmetic_op = expr.binary_op == BinaryOp::Add || expr.binary_op == BinaryOp::Sub || expr.binary_op == BinaryOp::Mul ||
                             expr.binary_op == BinaryOp::Div;
        bool pointer_operand_present =
            lhs_type.has_value() && rhs_type.has_value() &&
            (binary_operand_type(*lhs_type).kind == TypeKind::Pointer || binary_operand_type(*rhs_type).kind == TypeKind::Pointer);
        if (arithmetic_op && pointer_operand_present && !pointer_result_type.has_value()) {
            throw CodegenError("pointer arithmetic requires 'pointer +/- integer' or 'pointer - pointer' with matching "
                               "non-void pointer types",
                current_loc_);
        }
        bool needs_strict_scalar_match = expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne ||
                                         expr.binary_op == BinaryOp::Lt || expr.binary_op == BinaryOp::Gt ||
                                         expr.binary_op == BinaryOp::Le || expr.binary_op == BinaryOp::Ge;
        if (needs_strict_scalar_match && lhs_type.has_value() && rhs_type.has_value()) {
            const Type& lhs_operand_type = binary_operand_type(*lhs_type);
            const Type& rhs_operand_type = binary_operand_type(*rhs_type);
            if (!types_equal(lhs_operand_type, rhs_operand_type) && !lhs_is_literal && !rhs_is_literal) {
                throw CodegenError("binary operator requires operands of the same type; scpp has no implicit conversion "
                                   "between distinct scalar types",
                                   current_loc_);
            }
        }
        std::optional<Type> context_type;
        if (!pointer_result_type.has_value() && lhs_is_literal && !rhs_is_literal) {
            context_type = lhs_type.has_value() && rhs_type.has_value() ? binary_operand_type(*rhs_type) : infer_type(*expr.rhs);
        } else if (!pointer_result_type.has_value() && rhs_is_literal && !lhs_is_literal) {
            context_type = lhs_type.has_value() && rhs_type.has_value() ? binary_operand_type(*lhs_type) : infer_type(*expr.lhs);
        }
        if (needs_strict_scalar_match && lhs_type.has_value() && rhs_type.has_value() &&
            !types_equal(binary_operand_type(*lhs_type), binary_operand_type(*rhs_type)) &&
            context_type.has_value()) {
            const Type& literal_target = *context_type;
            bool lhs_matches = !lhs_is_literal || ((expr.lhs->kind == ExprKind::FloatLiteral && is_float_scalar_type_name(literal_target.name)) ||
                                                   (expr.lhs->kind == ExprKind::IntegerLiteral &&
                                                    literal_target.kind == TypeKind::Named &&
                                                    literal_target.name != "bool" && literal_target.name != "char"));
            bool rhs_matches = !rhs_is_literal || ((expr.rhs->kind == ExprKind::FloatLiteral && is_float_scalar_type_name(literal_target.name)) ||
                                                   (expr.rhs->kind == ExprKind::IntegerLiteral &&
                                                    literal_target.kind == TypeKind::Named &&
                                                    literal_target.name != "bool" && literal_target.name != "char"));
            if (!(lhs_matches && rhs_matches)) {
                throw CodegenError("binary operator requires operands of the same type; scpp has no implicit conversion "
                                   "between distinct scalar types",
                                   current_loc_);
            }
        }
        llvm::Value* lhs = context_type.has_value() ? codegen_value_for_target(*expr.lhs, *context_type)
                                                      : codegen_expr(*expr.lhs);
        llvm::Value* rhs = context_type.has_value() ? codegen_value_for_target(*expr.rhs, *context_type)
                                                      : codegen_expr(*expr.rhs);

        // ch06 §6: the operand type (preferring the resolved context
        // type above, when there was a literal to resolve; otherwise the
        // LHS -- movecheck has already rejected any two-distinct-real-
        // scalar-type mismatch, so both operands always share one type
        // by the time this runs) decides signed-vs-unsigned-vs-floating-
        // point codegen for every arithmetic/ordering operator below;
        // `Eq`/`Ne` alone are signedness-independent (an icmp/fcmp
        // equality predicate is the same regardless) but still need
        // fcmp for a float operand.
        std::optional<Type> operand_type = context_type.has_value() ? context_type : lhs_type;
        if (operand_type.has_value()) operand_type = binary_operand_type(*operand_type);
        bool is_float = operand_type.has_value() && is_float_scalar_type_name(operand_type->name);
        bool is_unsigned = operand_type.has_value() && is_unsigned_scalar_type_name(operand_type->name);
        bool is_checked = operand_type.has_value() && is_checked_arithmetic_scalar_type_name(operand_type->name);

        switch (expr.binary_op) {
            case BinaryOp::Add:
            case BinaryOp::Sub:
                if (pointer_result_type.has_value()) {
                    const Type& lhs_operand_type = binary_operand_type(*lhs_type);
                    const Type& rhs_operand_type = binary_operand_type(*rhs_type);
                    if (lhs_operand_type.kind == TypeKind::Pointer) {
                        if (rhs_operand_type.kind == TypeKind::Pointer) {
                            return codegen_pointer_difference(lhs, rhs, lhs_operand_type);
                        }
                        return codegen_pointer_offset(lhs, rhs, lhs_operand_type, expr.binary_op == BinaryOp::Sub);
                    }
                    return codegen_pointer_offset(rhs, lhs, binary_operand_type(*rhs_type), /*negate_offset=*/false);
                }
                [[fallthrough]];
            case BinaryOp::Mul:
                if (is_float) return codegen_float_arith(expr.binary_op, lhs, rhs);
                return codegen_checked_arith(expr.binary_op, lhs, rhs, is_unsigned, is_checked);
            case BinaryOp::Div:
                if (is_float) return builder_->CreateFDiv(lhs, rhs, "fdivtmp");
                return codegen_checked_div(lhs, rhs, is_unsigned, is_checked);
            // Comparisons always produce a genuine i1 from icmp/fcmp, but
            // a scpp `bool` result needs to be widened to the i8 every
            // other bool value uses (see i1_to_bool/to_llvm_type) before
            // it can be stored, passed, or returned like any other value.
            case BinaryOp::Eq:
                return i1_to_bool(is_float ? builder_->CreateFCmpOEQ(lhs, rhs, "eqtmp")
                                            : builder_->CreateICmpEQ(lhs, rhs, "eqtmp"));
            case BinaryOp::Ne:
                return i1_to_bool(is_float ? builder_->CreateFCmpONE(lhs, rhs, "netmp")
                                            : builder_->CreateICmpNE(lhs, rhs, "netmp"));
            case BinaryOp::Lt:
                return i1_to_bool(is_float ? builder_->CreateFCmpOLT(lhs, rhs, "lttmp")
                                   : is_unsigned ? builder_->CreateICmpULT(lhs, rhs, "lttmp")
                                                  : builder_->CreateICmpSLT(lhs, rhs, "lttmp"));
            case BinaryOp::Gt:
                return i1_to_bool(is_float ? builder_->CreateFCmpOGT(lhs, rhs, "gttmp")
                                   : is_unsigned ? builder_->CreateICmpUGT(lhs, rhs, "gttmp")
                                                  : builder_->CreateICmpSGT(lhs, rhs, "gttmp"));
            case BinaryOp::Le:
                return i1_to_bool(is_float ? builder_->CreateFCmpOLE(lhs, rhs, "letmp")
                                   : is_unsigned ? builder_->CreateICmpULE(lhs, rhs, "letmp")
                                                  : builder_->CreateICmpSLE(lhs, rhs, "letmp"));
            case BinaryOp::Ge:
                return i1_to_bool(is_float ? builder_->CreateFCmpOGE(lhs, rhs, "getmp")
                                   : is_unsigned ? builder_->CreateICmpUGE(lhs, rhs, "getmp")
                                                  : builder_->CreateICmpSGE(lhs, rhs, "getmp"));
            default: throw CodegenError("unhandled binary operator",
                current_loc_);
        }
    }

    llvm::Value* codegen_short_circuit(const Expr& expr) {
        llvm::Function* current_function = builder_->GetInsertBlock()->getParent();
        bool is_and = expr.binary_op == BinaryOp::And;

        // `lhs`/`rhs` stay in the i8 bool representation throughout (so
        // the merging PHI below can use either directly, matching how
        // every other bool value is stored/passed/returned) -- only the
        // branch conditions themselves need the narrower bool_to_i1 form.
        llvm::Value* lhs = codegen_contextual_bool_value(*expr.lhs);
        llvm::BasicBlock* rhs_block =
            llvm::BasicBlock::Create(*context_, is_and ? "and.rhs" : "or.rhs", current_function);
        llvm::BasicBlock* merge_block =
            llvm::BasicBlock::Create(*context_, is_and ? "and.end" : "or.end", current_function);
        llvm::BasicBlock* lhs_block = builder_->GetInsertBlock();

        if (is_and) {
            builder_->CreateCondBr(bool_to_i1(lhs), rhs_block, merge_block);
        } else {
            builder_->CreateCondBr(bool_to_i1(lhs), merge_block, rhs_block);
        }

        builder_->SetInsertPoint(rhs_block);
        llvm::Value* rhs = codegen_contextual_bool_value(*expr.rhs);
        llvm::BasicBlock* rhs_end_block = builder_->GetInsertBlock();
        builder_->CreateBr(merge_block);

        builder_->SetInsertPoint(merge_block);
        llvm::PHINode* phi = builder_->CreatePHI(llvm::Type::getInt8Ty(*context_), 2, "logictmp");
        phi->addIncoming(lhs, lhs_block);
        phi->addIncoming(rhs, rhs_end_block);
        return phi;
    }
};

} // namespace scpp
