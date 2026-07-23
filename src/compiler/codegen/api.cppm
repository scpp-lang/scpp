module;

// This partition only *declares* the Codegen class -- every member uses
// llvm::LLVM-C's own opaque reference types (llvm::LLVMValueRef, llvm::LLVMTypeRef, ...)
// rather than llvm::LLVM's native C++ classes, so only `import llvm;` below
// (module `llvm`; this partition only actually needs the Types.h surface
// from `llvm`'s own `:types` partition, reached transitively through
// `:core`'s own re-export -- see libs/llvm/) is needed here, not any of
// llvm::LLVM's heavier C++ API headers (those are only needed by the .cppm
// partitions that actually call llvm::LLVM-C functions -- see e.g.
// orchestration.cppm). A rigorous, function-by-function empirical audit
// (see libs/README.md) found official llvm::LLVM-C fully covers every llvm::LLVM
// operation this project's codegen needs, so there is no custom wrapper
// of any kind anywhere in this module.

export module scpp.compiler.codegen:api;

import std;
import llvm;
import scpp.ast;
import scpp.constexpr_engine;
import :errors;

export namespace scpp {

class Codegen {
public:
    explicit Codegen(const std::string& module_name, std::string source_path = {}, bool emit_debug_info = false)
        ;

    // context_/module_/builder_/dibuilder_ are now plain llvm::LLVM-C handles
    // (llvm::LLVMContextRef etc.), not std::unique_ptr, so Codegen must dispose
    // them itself (~Codegen, defined in orchestration.cppm alongside the
    // constructor). A shallow member-wise copy of these handles would let
    // two Codegen instances both dispose the same underlying llvm::LLVM context/
    // module/builder -- a double-free -- so copying is disallowed. Every
    // real usage (driver.cppm, codegen_test.cpp, driver_test.cpp) only
    // ever constructs one plain named local and never moves or copies it,
    // so disallowing moves too (rather than writing hand-rolled,
    // never-actually-exercised move semantics) is the simplest correct
    // choice.
    Codegen(const Codegen&) = delete;
    Codegen& operator=(const Codegen&) = delete;
    Codegen(Codegen&&) = delete;
    Codegen& operator=(Codegen&&) = delete;
    ~Codegen();

    // Sets the target triple and data layout on the module. Must be called
    // (if at all) before generate(), since generate() may need
    // target-accurate type sizes (e.g. for std::make_unique's malloc
    // call). Optional: without it, the module keeps llvm::LLVM's default,
    // non-target-specific data layout, which is fine for callers (like
    // codegen_test) that only inspect the generated IR text and don't need
    // bit-perfect target sizing. Takes the triple and data layout as plain
    // strings (rather than llvm::Triple/llvm::DataLayout) so this API
    // boundary needs no llvm::LLVM C++ headers on either side -- a caller still
    // using llvm::LLVM's C++ TargetMachine API (as driver.cppm currently does)
    // can get both via triple.str() and data_layout.getStringRepresentation().
    void set_target(const std::string& triple, const std::string& data_layout);

    llvm::LLVMModuleRef generate(const Program& program);

    // Renders the generated module as llvm::LLVM IR text. Exposed so callers (and
    // tests) don't need to include llvm::LLVM headers directly.
    std::string module_ir() const;

private:
    struct StructInfo {
        llvm::LLVMTypeRef llvm_type = nullptr;
        std::vector<std::string> field_names;
        std::vector<Type> field_types;
        std::vector<unsigned> field_alignments;
        std::vector<std::size_t> field_physical_indices;
        bool is_union = false;
        bool is_packed = false;
        bool has_ordinary_vtable = false;
        unsigned abi_align = 1;

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
        [[nodiscard]] std::optional<std::size_t> find_field_index(const std::string& name) const {
            for (std::size_t i = field_names.size(); i > 0; i--) {
                if (field_names[i - 1] == name) return i - 1;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::size_t physical_field_index(std::size_t logical_index) const {
            return field_physical_indices[logical_index];
        }
    };

    // A storage location: an llvm::LLVM pointer plus the scpp-level Type stored
    // there. Needed (rather than just an llvm::LLVMValueRef) so Member/Subscript
    // chains can resolve field indices and element types as they walk down
    // (e.g. `p.inner.x` needs to know `p.inner`'s struct type to find `x`).
    struct LValue {
        llvm::LLVMValueRef ptr;
        Type type;
        std::optional<unsigned> alignment;
    };

    // codegen_call's result: the raw llvm::LLVM call value, plus the resolved
    // callee's own AST-level Function (its return type is what codegen_
    // expr/codegen_lvalue's own Call cases need next, e.g. to decide
    // whether to auto-dereference a reference-returning result -- see
    // codegen_call's own comment for why a method call's receiver can
    // only ever be resolved once, so both must come from a single call).
    struct CallResult {
        llvm::LLVMValueRef value;
        const Function* callee_def; // nullptr only if truly unknown (defensive; codegen_call already
                                     // required a matching llvm::LLVM function to exist)
    };

    [[nodiscard]] static bool is_scalar_type_name(const std::string& name);
    [[nodiscard]] static const EnumDef* find_enum_def(const Program* program, const std::string& name);
    [[nodiscard]] static const EnumVariant* find_enum_variant(const Program* program, const std::string& name,
                                                   const EnumDef** owning_enum = nullptr);

    [[nodiscard]] static bool is_enum_cast_store_builtin_name(const std::string& name);

    struct LocalSlot {
        llvm::LLVMValueRef alloca;
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
        llvm::LLVMValueRef moved_flag = nullptr;
    };

    struct GlobalSlot {
        llvm::LLVMValueRef global = nullptr;
        Type type;
        bool is_const = false;
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
    llvm::LLVMContextRef context_;
    llvm::LLVMModuleRef module_;
    llvm::LLVMBuilderRef builder_;
    // Unlike context_/module_/builder_ (unconditionally set by every
    // constructor call), dibuilder_ is only assigned a real value by
    // initialize_debug_info() when emit_debug_info_ is true -- so it
    // needs an explicit null default here (a raw llvm::LLVMDIBuilderRef has no
    // implicit-nullptr default the way its former std::unique_ptr did)
    // for the destructor's own null-check (see orchestration.cppm) to be
    // correct when debug info is disabled.
    llvm::LLVMDIBuilderRef dibuilder_ = nullptr;
    llvm::LLVMMetadataRef compile_unit_ = nullptr;
    llvm::LLVMMetadataRef compile_unit_file_ = nullptr;
    llvm::LLVMMetadataRef current_debug_scope_ = nullptr;
    llvm::LLVMMetadataRef current_subprogram_ = nullptr;
    std::string source_path_;
    bool emit_debug_info_ = false;
    std::map<std::string, LocalSlot> locals_;
    std::unordered_map<std::string, GlobalSlot> globals_;
    std::unordered_map<std::string, StructInfo> structs_;
    std::unordered_set<std::string> declaring_aggregates_;
    // ch05 §5.10: each Function's actual llvm::LLVM symbol name -- the plain
    // `fn.name` unchanged for the overwhelmingly common case (exactly one
    // function under that name; critically, this is what keeps `main`/
    // `extern "C"` functions' names exactly as declared, since llvm::LLVM
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
        llvm::LLVMBasicBlockRef cond_block;
        llvm::LLVMBasicBlockRef end_block;
        std::size_t scope_depth;
    };
    std::vector<LoopFrame> loop_stack_;
    std::unordered_map<std::string, llvm::LLVMMetadataRef> debug_type_cache_;
    std::unordered_map<std::string, llvm::LLVMMetadataRef> debug_file_cache_;
    llvm::LLVMTypeRef interface_representation_llvm_type_ = nullptr;
    std::unordered_map<std::string, llvm::LLVMTypeRef> interface_dispatch_table_types_;
    std::unordered_map<std::string, std::vector<const Function*>> interface_dispatch_methods_cache_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> interface_slot_indices_cache_;
    std::unordered_map<std::string, llvm::LLVMValueRef> interface_dispatch_tables_;
    std::unordered_map<std::string, llvm::LLVMValueRef> interface_dispatch_thunks_;
    std::unordered_map<std::string, llvm::LLVMTypeRef> ordinary_vtable_types_;
    std::unordered_map<std::string, std::vector<const Function*>> ordinary_virtual_methods_cache_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> ordinary_slot_indices_cache_;
    std::unordered_map<std::string, llvm::LLVMValueRef> ordinary_vtables_;
    std::unordered_map<std::string, llvm::LLVMValueRef> ordinary_destructor_thunks_;
    std::vector<std::string> current_global_namespace_path_;

    [[nodiscard]] std::string default_debug_source_path() const;

    [[nodiscard]] llvm::LLVMMetadataRef debug_file_for_path(const std::string& path);

    [[nodiscard]] llvm::LLVMMetadataRef debug_file_for_program();

    [[nodiscard]] const std::vector<std::string>& current_lookup_namespace_path() const;

    [[nodiscard]] const GlobalSlot* find_visible_global_slot(const std::string& name,
                                                             bool explicit_global_qualification = false) const;

    [[nodiscard]] std::string mangle_global_symbol_name(const std::string& name) const;

    [[nodiscard]] llvm::LLVMMetadataRef debug_file_for_loc(const SourceLocation& loc);

    void initialize_debug_info();

    void finalize_debug_info();

    [[nodiscard]] llvm::LLVMMetadataRef debug_type_for(const Type& type);

    void refresh_debug_location(SourceLocation loc);

    void maybe_emit_parameter_debug_decl(const Param& param, llvm::LLVMValueRef slot, unsigned index);

    void maybe_emit_local_debug_decl(const std::string& name, const Type& type, llvm::LLVMValueRef slot, SourceLocation loc);

    // Hoists named local-variable storage to the function entry block so
    // llvm::LLVM can describe it with one stable frame-base location even when
    // the declaration itself lives in a nested scope whose initializer
    // emits its own control flow (e.g. checked arithmetic overflow
    // diamonds). Preserves declaration order among existing entry-block
    // allocas for tests/IR readability.
    llvm::LLVMValueRef create_entry_block_alloca(llvm::LLVMTypeRef type, const std::string& name,
                                                std::optional<unsigned> alignment = std::nullopt);

    void attach_debug_subprogram(llvm::LLVMValueRef llvm_fn, const Function& fn);

    const StructDef* find_struct_def(const std::string& name) const;

    const ClassDef* find_class_def(const std::string& name) const;

    [[nodiscard]] bool is_named_record_type(const Type& type) const;

    const Function* find_function_def(const std::string& name) const;

    [[nodiscard]] bool type_names_interface(const std::string& name) const;

    [[nodiscard]] bool is_interface_named_type(const Type& type) const;

    [[nodiscard]] bool is_interface_pointer_type(const Type& type) const;

    [[nodiscard]] bool is_interface_reference_type(const Type& type) const;

    [[nodiscard]] bool is_interface_representation_type(const Type& type) const;

    [[nodiscard]] std::string current_enclosing_class_name() const;

    [[nodiscard]] llvm::LLVMTypeRef interface_representation_type();

    [[nodiscard]] llvm::LLVMValueRef build_interface_value(llvm::LLVMValueRef object_ptr, llvm::LLVMValueRef dispatch_ptr);

    [[nodiscard]] llvm::LLVMValueRef extract_interface_object_ptr(llvm::LLVMValueRef interface_value);

    [[nodiscard]] llvm::LLVMValueRef extract_interface_dispatch_ptr(llvm::LLVMValueRef interface_value);

    [[nodiscard]] bool has_accessible_base_conversion(const std::string& source_name, const std::string& target_name,
                                                      std::string_view current_class) const;

    [[nodiscard]] bool types_compatible_with_base_conversion(const Type& source_type, const Type& target_type,
                                                             std::string_view current_class) const;

    ExprPtr clone_expr(const Expr& expr) const;

    [[nodiscard]] const Function* resolve_converting_constructor_by_type(const std::string& class_name, const Expr& arg);

    void store_constexpr_value_into(llvm::LLVMValueRef dest_ptr, const Type& dest_type, const ConstexprValue& value);

    llvm::LLVMValueRef codegen_consteval_class_value(const Expr& expr, const std::string& class_name);

    llvm::LLVMValueRef codegen_constructed_class_value(const std::string& class_name, const std::vector<ExprPtr>& args,
                                                 const Function* ctor_def, const Expr* original_expr = nullptr);

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
    [[nodiscard]] bool is_for_range_size_builtin(const Expr& expr) const;

    std::optional<Type> infer_type(const Expr& expr);

    // Whether `arg` produces a genuine rvalue of exactly `expected_type`
    // -- mirrors movecheck's own produces_rvalue_of_type (ch03/ch05
    // §5.11), used only for the `T&&`/`Concept auto&&` branch of
    // argument_matches_parameter just below (a monomorphized `Concept
    // auto&&` call site is, by the time codegen sees it, an ordinary
    // `T&&` parameter of a concrete type -- see the concept-
    // monomorphization pass -- so this needs no concept-specific logic
    // of its own).
    bool produces_rvalue_of_type(const Expr& arg, const Type& expected_type);

    bool const_reference_binds_materialized_temporary(const Expr& arg, const Type& param_type);

    [[nodiscard]] TargetLayoutInfo current_target_layout_info() const;

    [[nodiscard]] static std::size_t align_up(std::size_t value, std::size_t alignment);

    [[nodiscard]] std::size_t alignment_bytes_for_type(const Type& type) const;

    llvm::LLVMValueRef codegen_sizeof_value(const Expr& expr);

    llvm::LLVMValueRef codegen_alignof_value(const Expr& expr);

    [[nodiscard]] bool is_lvalue_copy_source_shape(const Expr& expr);

    [[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type);

    [[nodiscard]] bool is_implicit_move_return_source(const Expr& expr, const Type& target_type);

    // Whether `arg` is a legitimate argument for a candidate overload's
    // parameter declared as `param_type` -- mirrors movecheck's own
    // argument_matches_parameter (ch05 §5.10) exactly, just phrased over
    // codegen's own infer_type/types_equal instead of movecheck's.
    const Function* find_single_argument_converting_constructor(const std::string& class_name, const Expr& arg);

    bool argument_type_matches_parameter(const Type& arg_type, const Type& candidate_param_type);

    bool argument_matches_parameter(const Expr& arg, const Type& param_type);

    bool constructor_parameter_accepts_argument_directly(const Expr& arg, const Type& param_type);

    // Whether `expr` (a method-call receiver or reference-parameter
    // argument) is only reachable *read-only* -- mirrors movecheck's own
    // is_read_only_reachable (same overall shape/scope), needed here
    // purely to resolve which overload a call targets when a const/
    // non-const method pair (or an ordinary T&/const T& overload pair)
    // makes the receiver's/argument's own mutability the only
    // distinguishing factor (ch05 §5.10).
    bool is_read_only_place(const Expr& expr);

    bool receiver_matches_method_qualifier(const Expr& receiver_expr, const Function& fn);

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
                                              std::size_t param_offset, bool receiver_is_mutable = true,
                                              const Expr* receiver_expr = nullptr);

    const Function* resolve_constructor_overload_exact(const std::string& class_name, const std::vector<ExprPtr>& args);

    // Recursively verifies a type is trivial per the language spec (ch04):
    // scalars, raw pointers (any pointee), fixed-size arrays of trivial
    // types, and structs/unions whose fields are themselves all trivial.
    // `in_progress` detects a struct/union containing itself *by value*, which
    // must be rejected (as in C, this would be an infinitely-sized type);
    // self-reference via pointer is fine since pointers don't recurse here.
    void validate_trivial(const Type& type, std::vector<std::string>& in_progress);

    void declare_struct(const StructDef& def);

    // Registers a `class`'s layout the same way declare_struct does for a
    // `struct` (a named llvm::LLVM struct type, keyed into the same `structs_`
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
    void declare_class(const ClassDef& def);

    llvm::LLVMTypeRef to_llvm_type(const Type& type);

    [[nodiscard]] std::optional<unsigned> alignment_for_type(const Type& type) const;

    llvm::LLVMValueRef create_load(llvm::LLVMTypeRef type, llvm::LLVMValueRef ptr, std::optional<unsigned> alignment,
                                const std::string& name = "");

    llvm::LLVMValueRef create_store(llvm::LLVMValueRef value, llvm::LLVMValueRef ptr, std::optional<unsigned> alignment);

    // llvm::LLVMBuildCall2 needs the callee's function type explicitly, unlike
    // IRBuilder::CreateCall(Function*, ...), which infers it from the
    // callee. This overload is for the common case of calling a known
    // global function by value, whose own type (retrievable via
    // llvm::LLVMGlobalGetValueType) is its function type.
    llvm::LLVMValueRef build_call(llvm::LLVMValueRef callee, std::vector<llvm::LLVMValueRef> args, const std::string& name = "");

    // Overload for calling through an already-known function type, e.g. an
    // indirect call through a function-pointer value (vtable/interface
    // dispatch) that carries no function type of its own to query.
    llvm::LLVMValueRef build_call(llvm::LLVMTypeRef fn_type, llvm::LLVMValueRef callee, std::vector<llvm::LLVMValueRef> args,
                            const std::string& name = "");

    void zero_initialize_storage(llvm::LLVMValueRef ptr, const Type& type, std::optional<unsigned> alignment = std::nullopt);

    // A reference's referent may not itself be another reference:
    // reference-to-reference aliasing analysis is still out of scope for
    // v0.1's intraprocedural, first-order borrow checking.
    void validate_reference_pointee(const Type& pointee);

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
    void validate_reference_return_elision(const Function& fn);

    [[nodiscard]] static bool is_bare_void(const Type& type);

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
                                    const std::string& context_description);

    // Structural (deep) equality between two Types -- see movecheck's own
    // types_equal (this file has no access to it: separate module,
    // separate data model) for why a naive `=default` comparison of
    // Type's shared_ptr pointee/element wouldn't work. Used only for
    // function-overload resolution (ch05 §5.10)'s exact-type-match rule.
    // Reference additionally requires is_rvalue_ref to match: `T&`/
    // `const T&` and `T&&` (ch03) are distinct parameter types, never
    // interchangeable -- meaningless for Span.
    [[nodiscard]] static bool types_equal(const Type& a, const Type& b);

    [[nodiscard]] static const Type& binary_operand_type(const Type& type);

    [[nodiscard]] static bool is_pointer_arithmetic_offset_type(const Type& type);

    [[nodiscard]] bool pointer_supports_arithmetic(const Type& type) const;

    [[nodiscard]] std::optional<Type> pointer_arithmetic_result_type(BinaryOp op, const Type& lhs, const Type& rhs) const;

    // A short, llvm::LLVM-identifier-safe (alphanumeric/underscore only, no
    // spaces) encoding of `type`, used only to build a *disambiguating*
    // symbol-name suffix for a *local* (non-cross-module) overloaded
    // function (see build_overload_names) -- unlike ch11 §11.9's real
    // mangling scheme (used for symbols crossing a module boundary, see
    // mangle_exported_symbol below), this only has to be unique *within
    // this one compiled file*, so a compact tag scheme is simpler and
    // just as correct for the one job this specific helper needs it for.
    [[nodiscard]] static std::string mangle_type(const Type& type);

    [[nodiscard]] static std::string method_lookup_name(const Function& fn);

    [[nodiscard]] static std::string interface_method_slot_key(const Function& fn);

    [[nodiscard]] const std::vector<const Function*>& interface_dispatch_methods(const std::string& interface_name);

    [[nodiscard]] llvm::LLVMTypeRef interface_dispatch_table_type(const std::string& interface_name);

    [[nodiscard]] std::optional<std::size_t> interface_method_slot_index(const std::string& interface_name,
                                                                    const Function& method);

    [[nodiscard]] const Function* find_direct_method_by_slot(const std::string& class_name, const std::string& slot_key) const;

    [[nodiscard]] const Function* resolve_interface_slot_provider(const std::string& class_name, const std::string& slot_key) const;

    [[nodiscard]] llvm::LLVMTypeRef interface_dispatch_function_type(const Function& method);

    [[nodiscard]] bool interface_destructor_uses_raw_this(const Function& fn) const;

    [[nodiscard]] bool class_has_ordinary_vtable(const std::string& class_name) const;

    [[nodiscard]] llvm::LLVMTypeRef llvm_param_type_for_function(const Function& fn, const Param& param, std::size_t index);

    [[nodiscard]] llvm::LLVMValueRef get_or_create_interface_dispatch_thunk(const std::string& concrete_class_name,
                                                                         const Function& target);

    [[nodiscard]] llvm::LLVMValueRef get_or_create_interface_destructor_thunk(const std::string& concrete_class_name,
                                                                            const Function& interface_destructor);

    [[nodiscard]] const std::vector<const Function*>& ordinary_virtual_methods(const std::string& class_name);

    [[nodiscard]] llvm::LLVMTypeRef ordinary_vtable_type(const std::string& class_name);

    [[nodiscard]] std::optional<std::size_t> ordinary_method_slot_index(const std::string& class_name, const Function& method);

    [[nodiscard]] llvm::LLVMValueRef get_or_create_ordinary_destructor_thunk(const std::string& concrete_class_name);

    [[nodiscard]] llvm::LLVMValueRef get_or_create_ordinary_vtable(const std::string& class_name);

    void initialize_ordinary_vtable_pointer(const std::string& class_name, llvm::LLVMValueRef object_ptr);

    [[nodiscard]] llvm::LLVMValueRef interface_dispatch_entry_for(const std::string& concrete_class_name, const Function& method);

    [[nodiscard]] llvm::LLVMValueRef get_or_create_interface_dispatch_table(const std::string& concrete_class_name,
                                                                               const std::string& interface_name);

    [[nodiscard]] static Type function_pointer_type_from_signature(const Type& return_type,
                                                                   const std::vector<Type>& param_types,
                                                                   bool is_unsafe);

    [[nodiscard]] static bool same_function_pointer_shape_ignoring_unsafe(const Type& a, const Type& b);

    [[nodiscard]] std::optional<Type> resolve_function_designator_type(const Expr& expr,
                                                                       const std::optional<Type>& target_type = std::nullopt);

    [[nodiscard]] llvm::LLVMValueRef codegen_function_pointer_value_for_target(const Expr& expr, const Type& target_type);

    // The full, human-readable-spelled-out type text ch11 §11.9's real
    // mangling scheme requires (e.g. "int", "const int&",
    // "std::unique_ptr<int>") -- mirrors cli.cppm's own type_to_string
    // (a separate module, so duplicated rather than shared; both are
    // small, stable, one-purpose functions).
    [[nodiscard]] static std::string verbatim_type_spelling(const Type& type);

    // Splits a dotted module name ("org.lotx.cmath") into segments --
    // codegen's own copy of the parser's split_dotted_name (separate
    // module, no shared code; both are tiny, stable helpers).
    [[nodiscard]] static std::vector<std::string> split_dotted(const std::string& dotted);

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
    [[nodiscard]] std::string mangle_exported_symbol(const Function& fn) const;

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
    void build_overload_names();

    void declare_function(const Function& fn);

    void define_function(const Function& fn);

    void define_defaulted_function(const Function& fn);

    // ch05 §5.14: emits a thin, codegen-only wrapper body for a
    // Function::forwards_to stub (an inherited method a derived class
    // doesn't itself override, synthesized by movecheck's
    // synthesize_inherited_method_forwards) -- calls the real target
    // function directly, forwarding every llvm::LLVM argument (including
    // `this`) completely unchanged: the derived class's own flattened,
    // base-first layout (declare_class) already makes its leading bytes
    // byte-identical to the base's own full layout, so no pointer
    // adjustment/bitcast is needed at all (llvm::LLVM's opaque pointers carry
    // no type information to begin with). `fn.body` is always null for
    // one of these (see Function::forwards_to's own comment) -- this is
    // the *only* place that ever runs for it, never codegen_stmt/
    // codegen_expr.
    void define_forwarding_function(const Function& fn);

    void codegen_stmt(const Stmt& stmt, llvm::LLVMValueRef current_function);

    // Builds and emits the actual `call` instruction for `expr` (a Call
    // expression naming a real, non-builtin function -- callers handle
    // `print_int`/`print_bool` themselves before reaching here), binding
    // each reference-typed argument to its address rather than its
    // value. The raw llvm::LLVM result: if the callee returns a reference,
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
    CallResult codegen_call(const Expr& expr);

    // Builds the llvm::LLVM argument list for a call to `callee_def` (nullable
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
    llvm::LLVMValueRef codegen_materialize_rvalue_reference_source(const Expr& expr);

    llvm::LLVMValueRef codegen_materialize_const_reference_source(const Expr& expr, const Type& target_type);

    void codegen_copy_construct_class(llvm::LLVMValueRef dest_ptr, llvm::LLVMValueRef src_ptr, const std::string& class_name);

    [[nodiscard]] bool is_constructor_function(const Function& fn) const;

    [[nodiscard]] std::string unqualified_template_base_name(std::string_view class_name) const;

    [[nodiscard]] bool names_direct_base(const std::string& member_name, const ClassDef& def) const;

    [[nodiscard]] bool names_base(const std::string& member_name, const BaseSpecifier& base) const;

    void collect_virtual_interface_bases_in_construction_order(const ClassDef& def, std::vector<const ClassDef*>& out,
                                                               std::unordered_set<std::string>& seen) const;

    [[nodiscard]] std::vector<const ClassDef*> collect_virtual_interface_bases_in_construction_order(const ClassDef& def) const;

    [[nodiscard]] const MemberInitializer* find_explicit_interface_initializer(const Function& ctor,
                                                                               const ClassDef& interface_def) const;

    void emit_complete_object_interface_initializers(const ClassDef& most_derived_def, const Function* ctor_def,
                                                     llvm::LLVMValueRef object_ptr);

    [[nodiscard]] llvm::LLVMValueRef load_this_object_ptr();

    [[nodiscard]] LValue codegen_raw_member_storage(llvm::LLVMValueRef object_ptr, const std::string& class_name,
                                                    const ClassField& field);

    [[nodiscard]] LValue codegen_raw_member_storage(llvm::LLVMValueRef object_ptr, const std::string& class_name,
                                                    const StructField& field);

    void initialize_reference_storage(const LValue& target, const Expr& expr);

    void initialize_span_storage(const LValue& target, const Expr& expr);

    // Direct-initializing a fresh class object from another prvalue of the
    // exact same type (`T x{f()};`, `new T(f())`, `T(f())`) materializes the
    // source object directly into the destination storage instead of routing
    // back through ordinary constructor overload resolution.
    bool try_initialize_class_storage_from_same_type_source(const LValue& target, const std::vector<ExprPtr>& args);

    void initialize_storage_from_expr(const LValue& target, const Expr& expr);

    void initialize_storage_from_brace_args(const LValue& target, const std::vector<ExprPtr>& args);

    void initialize_storage(const LValue& target, const Initializer& init);

    template <typename FieldT>
    void emit_default_initializers_for_record_fields(llvm::LLVMValueRef object_ptr, const std::string& class_name,
                                                     const std::vector<FieldT>& fields) {
        for (const FieldT& field : fields) {
            if (!field.default_initializer) continue;
            LValue field_storage = codegen_raw_member_storage(object_ptr, class_name, field);
            initialize_storage(field_storage, *field.default_initializer);
        }
    }

    void emit_constructor_member_initializers(const Function& fn);

    [[nodiscard]] bool class_has_any_constructor(const std::string& class_name) const;

    void emit_default_initializers_for_class_storage(llvm::LLVMValueRef object_ptr, const ClassDef& class_def,
                                                    bool initialize_virtual_interface_bases);

    llvm::LLVMValueRef codegen_class_value_for_boundary(const Expr& expr, const Type& target_type,
                                                  bool allow_implicit_converting_ctor = false);

    llvm::LLVMValueRef codegen_interface_value_for_target(const Expr& expr, const Type& target_type);

    llvm::LLVMValueRef codegen_span_value_for_target(const Expr& expr, const Type& target_type);

    llvm::LLVMValueRef codegen_contextual_bool_value(const Expr& expr);

    llvm::LLVMValueRef codegen_contextual_bool_i1(const Expr& expr);

    std::vector<llvm::LLVMValueRef> codegen_call_args(const std::vector<ExprPtr>& args, const Function* callee_def,
                                                  std::size_t param_offset);

    std::vector<llvm::LLVMValueRef> codegen_call_args_for_types(const std::vector<ExprPtr>& args,
                                                          const std::vector<Type>& param_types);

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
    llvm::LLVMValueRef load_value(const LValue& lv);

    // `bool` is stored/passed/returned as a full byte (i8; see
    // to_llvm_type), but llvm::LLVM's branch/select instructions require a
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
    llvm::LLVMValueRef bool_to_i1(llvm::LLVMValueRef v);

    // The inverse of bool_to_i1: widens an i1 (an icmp result, or another
    // logical operation already in the 1-bit domain) back up to the i8
    // representation every ordinary bool value uses -- the choke point
    // every comparison/logical operator goes through before its result
    // is stored, passed, or returned as an actual `bool`.
    llvm::LLVMValueRef i1_to_bool(llvm::LLVMValueRef v);

    [[nodiscard]] bool enum_value_fits_source_type(const Type& source_type, long long enum_value);

    llvm::LLVMValueRef build_integral_enum_match(llvm::LLVMValueRef source, const Type& source_type, long long enum_value);

    llvm::LLVMValueRef enum_variant_constant(llvm::LLVMTypeRef enum_storage_type, const Type& underlying_type, long long enum_value);

    CallResult codegen_enum_cast_store_builtin(const Expr& expr, const Function& callee_def);

    // ch06 §6: a bare numeric literal (Integer/Float) has no fixed type
    // of its own the way a named variable does -- exactly like real
    // C++'s own literal-suffix rules (and, more directly, how Rust
    // treats an unsuffixed integer/float literal as unconstrained until
    // context picks a concrete type): generates the constant directly in
    // `target_type`'s own llvm::LLVM representation when the source shape is a
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
    llvm::LLVMValueRef codegen_value_for_target(const Expr& expr, const Type& target_type);

    // Verifies `value`'s llvm::LLVM type exactly matches `expected` before it's
    // stored into a place declared as `expected` (a VarDecl initializer,
    // a plain assignment's RHS, or std::make_unique<T>(...)'s scalar
    // argument) -- scpp has no implicit conversion between distinct
    // scalar types (bool/char/int are all separate, ch06), and, unlike a
    // mismatched call argument/return value/binary operand (all already
    // rejected by llvm::LLVM's own module verifier), a *store*'s address
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
    void check_store_type(llvm::LLVMValueRef value, llvm::LLVMTypeRef expected, const std::string& what);

    void define_global_initializers(const Program& program);

    llvm::LLVMValueRef codegen_expr(const Expr& expr);

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
    // own address (an `llvm::LLVMValueRef`, like any other class-typed
    // value in this codebase -- see codegen_expr's Lambda case, and
    // codegen_lvalue's own Lambda case for an IIFE's receiver, ch05
    // §5.12's `[](...){...}()`).
    llvm::LLVMValueRef codegen_construct_lambda(const Expr& expr, llvm::LLVMValueRef existing_storage = nullptr);

    llvm::LLVMValueRef codegen_new_expr(const Expr& expr);

    void codegen_delete_expr(const Expr& expr);

    void codegen_destroy_expr(const Expr& expr);

    // Returns `class_name`'s destructor function, if it has one (see
    // parse_class_def's `ClassName_delete` synthesized-name scheme) --
    // nullptr if `class_name` isn't a class, or is one with no destructor
    // defined. A class with no destructor needs no cleanup at scope exit
    // at all (same as a plain struct); this is deliberately *not* an
    // error, unlike a missing constructor for constructor-call syntax
    // (VarDecl's own check) -- a destructor is optional, a constructor
    // call always names one explicitly.
    llvm::LLVMValueRef find_destructor(const std::string& class_name);

    [[nodiscard]] const Function* find_destructor_ast(const std::string& class_name) const;

    void emit_interface_destructor_dispatch_call(const std::string& interface_name, llvm::LLVMValueRef interface_value);

    // spec §6.5: codegen's own counterpart to movecheck's identically-
    // named helpers (has_user_declared_copy_ctor/copy_assign/dtor and
    // is_copy_constructible/is_copy_assignable) -- kept as a small,
    // separately-duplicated copy per module (the established pattern
    // already used for types_equal, rather than a shared
    // cross-module utility), since codegen has direct Program access
    // (`program_`) rather than movecheck's Body-only architecture.
    [[nodiscard]] const Function* find_user_declared_copy_ctor_ast(const std::string& class_name);

    [[nodiscard]] const Function* find_user_declared_copy_assign_ast(const std::string& class_name);

    [[nodiscard]] bool has_user_declared_dtor(const std::string& class_name);

    [[nodiscard]] bool is_copy_constructible(const std::string& class_name);

    [[nodiscard]] bool is_copy_assignable(const std::string& class_name);

    [[nodiscard]] bool is_field_copy_constructible(const Type& type);

    [[nodiscard]] bool is_field_copy_assignable(const Type& type);

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
    void codegen_memberwise_copy_construct(llvm::LLVMValueRef dest_ptr, llvm::LLVMValueRef src_ptr,
                                            const std::string& class_name);

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
    void codegen_memberwise_copy_assign(llvm::LLVMValueRef dest_ptr, llvm::LLVMValueRef src_ptr, const std::string& class_name);

    [[nodiscard]] bool class_has_destructor_in_chain(const std::string& class_name);

    void emit_destructor_chain_calls(const std::string& class_name, llvm::LLVMValueRef object_ptr);

    // spec §6.4: a fresh, zero-initialized (`false`) `i1` slot for
    // tracking whether a class-typed local has been moved-out, so
    // scope-exit cleanup (codegen_call_destructor_chain_unless_moved) can
    // correctly skip invoking its destructor chain if so (spec §6.3/§6.4:
    // "its destructor, if declared, is not invoked for it"). Returns
    // null when neither the class nor any of its bases declares a
    // destructor at all.
    llvm::LLVMValueRef create_moved_flag_if_has_destructor(const std::string& class_name);

    // spec §6.3/§6.4/ch05 §5.14: emits the whole most-derived-to-base
    // destructor chain guarded by `!moved_flag` when present, matching
    // real C++'s reverse-of-construction destruction order for a derived
    // object's base subobjects.
    void codegen_call_destructor_chain_unless_moved(const std::string& class_name, llvm::LLVMValueRef object_ptr,
                                                    llvm::LLVMValueRef moved_flag);

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
    void codegen_destroy_old_class_state_for_move_assign(llvm::LLVMValueRef ptr, const std::string& class_name,
                                                         llvm::LLVMValueRef moved_flag = nullptr);

    // Releases every *currently in-scope* unique_ptr local's owned
    // resource, and runs every currently-in-scope class-typed local's
    // destructor (ch04 §4.2), if it has one -- both this function's own
    // parameters and every block-scoped local across every enclosing
    // scope level up to the function's own top. Called right before each
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
    // pop_scope() and removed from `locals_`/`scope_stack_`, so they
    // aren't double-freed/double-destructed here. Destruction order is:
    // block-scoped locals (deepest scope first, reverse declaration
    // order within each, via emit_scope_cleanup_to_depth(0)), then this
    // function's own parameters in reverse parameter order -- matching
    // real C++'s "reverse of construction order" rule throughout (a
    // parameter is conceptually constructed before any of the body's own
    // locals, so it's destroyed after all of them).
    void free_unique_ptr_locals();

    void push_scope();

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
    void pop_scope();

    // Runs destructor/unique_ptr cleanup for every local declared in the
    // scopes from the current depth down to (but not including)
    // `target_depth`, deepest scope first and -- within each scope --  in
    // reverse declaration order (matching C++/Rust destruction order,
    // same as pop_scope()'s own single-scope case just above). Used both
    // for `break`/`continue` (target_depth = the loop's own scope depth,
    // so only the scopes entered since the loop was entered get cleaned
    // up) and, with target_depth 0, as the first phase of
    // free_unique_ptr_locals() (see above), for `return` (see
    // StmtKind::Return), which must unwind every enclosing scope all the
    // way to the function's own top, however deeply nested the return
    // is. Reads `scope_stack_` only; it does not itself pop any scope,
    // since the caller's own control-flow jump (a branch or a `ret`
    // terminator) makes whatever remains of the current block's codegen
    // unreachable anyway. Note this alone does *not* clean up function
    // parameters (see free_unique_ptr_locals's own comment) -- they are
    // never pushed onto scope_stack_, only onto locals_.
    void emit_scope_cleanup_to_depth(std::size_t target_depth);

    llvm::LLVMValueRef get_or_declare_malloc();

    llvm::LLVMValueRef get_or_declare_free();

    llvm::LLVMValueRef get_or_declare_abort();

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
    void emit_span_bounds_check(llvm::LLVMValueRef index, llvm::LLVMValueRef size);

    // Attempts to read `expr` as a compile-time-constant integer without
    // any general constant-expression evaluation (that broader job
    // belongs to scpp.constexpr_engine, which runs in an earlier pass,
    // well before codegen ever sees this expression) -- just the same
    // narrow, single-token recognition codegen_value_for_target already
    // gives a bare/negated integer literal (`10`, `-1`), since that's the
    // only form codegen itself can cheaply and unambiguously know the
    // value of. Returns std::nullopt for anything else (e.g. a runtime
    // variable, or a named `constexpr` constant -- codegen never folds
    // those), in which case the caller falls back to a runtime check.
    // Used by codegen_lvalue's fixed-size-array ExprKind::Subscript case
    // to reject an out-of-bounds constant index at compile time instead
    // of emitting a runtime check for it, since a fixed array's bound is
    // always statically known (ch05 §9.4).
    [[nodiscard]] std::optional<long long> try_eval_constant_index(const Expr& expr) const;

    // Emits a runtime bounds check for a fixed-size array subscript,
    // exactly like emit_span_bounds_check above but for a bound that's
    // already known at compile time (a fixed array's declared size `N`,
    // ch05 §9.4) rather than loaded from memory at runtime -- reuses that
    // same check (0 <= index < size doesn't care where `size` came from),
    // so it inherits the identical `unsafe_depth_` skip and abort()
    // panic model. Only reached for a non-constant index; a compile-time-
    // constant out-of-bounds index is instead rejected earlier,
    // unconditionally, as a compile-time CodegenError (see
    // try_eval_constant_index's caller in codegen_lvalue).
    void emit_array_bounds_check(llvm::LLVMValueRef index, long long bound);

    // ch06 §6: the numeric family's own signed/unsigned/floating
    // classification, by scpp type name -- llvm::LLVM draws no signed/
    // unsigned distinction at the *type* level (only i8/i16/i32/i64),
    // so every integer arithmetic/comparison/division instruction that
    // cares about signedness (sdiv vs udiv, icmp slt vs icmp ult, ...)
    // needs this to pick the right one. `bool`/`char` are deliberately
    // excluded from both is_unsigned/is_float (neither is ever true for
    // them) *and* is_checked_arithmetic_scalar below (ch06: no
    // arithmetic is defined for them yet, pre-existing, unchanged
    // scope) -- every other named scalar (the numeric family proper) is
    // checked.
    [[nodiscard]] static bool is_float_scalar_type_name(const std::string& name);
    [[nodiscard]] static bool is_integral_scalar_type_name(const std::string& name);
    [[nodiscard]] static bool is_unsigned_scalar_type_name(const std::string& name);
    [[nodiscard]] static bool is_checked_arithmetic_scalar_type_name(const std::string& name);

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
    [[nodiscard]] static bool is_unsigned_for_cast(const std::string& name);

    [[nodiscard]] std::string scalar_name_for_cast(const Type& type) const;

    // ch06 §6: the actual conversion instruction for `static_cast<T>(expr)`/
    // `(T)expr`, given `value` already evaluated as `source_type`'s own
    // llvm::LLVM representation -- dispatches on the (source, target) type
    // pair exactly like a real compiler's own cast-kind selection:
    // int<->int (sext/zext for widening -- signedness from the *source*
    // type, matching real C++'s own conversion rules; trunc for
    // narrowing; a bare bitcast-free no-op when both are the exact same
    // width, e.g. int8_t<->uint8_t<->char<->bool, all i8), float<->float
    // (fpext widening / fptrunc narrowing), and float<->int (sitofp/
    // uitofp, fptosi/fptoui, signedness from whichever side is the
    // integer one).
    llvm::LLVMValueRef codegen_scalar_cast(llvm::LLVMValueRef value, const Type& source_type, const Type& target_type);

    // `+`/`-`/`*` on a floating-point type (ch06 §6): IEEE-754 arithmetic
    // has no UB-on-overflow concept to guard against at all (overflow
    // produces +/-infinity, underflow a signed zero or subnormal, both
    // well-defined by the standard itself) -- so, unlike the integer
    // path below, there is nothing to check regardless of unsafe
    // context; always the plain fadd/fsub/fmul instruction.
    llvm::LLVMValueRef codegen_float_arith(BinaryOp op, llvm::LLVMValueRef lhs, llvm::LLVMValueRef rhs);

    // `+`/`-`/`*` (ch05 §5.8): overflow-checked by default (aborting,
    // via the same panic mechanism as emit_span_bounds_check, on
    // overflow -- both signed and unsigned per the spec), or a plain,
    // guaranteed-wrapping (never UB) operation inside `unsafe { }`
    // (unsafe_depth_ > 0) -- achieved simply by using the plain
    // CreateAdd/CreateSub/CreateMul instructions, which (unlike real
    // Clang's) never get an `nsw`/`nuw` flag anywhere in this codebase, so
    // they're already well-defined, wrapping llvm::LLVM IR on their own.
    // `is_checked` is false for `bool`/`char` (ch06: no arithmetic is
    // defined for either yet, pre-existing, unchanged scope) -- every
    // other integer width, signed or unsigned, is checked regardless of
    // width (generalized from this codebase's original int-only, i32-only
    // scope now that the rest of the numeric family exists).
    llvm::LLVMValueRef codegen_checked_arith(BinaryOp op, llvm::LLVMValueRef lhs, llvm::LLVMValueRef rhs, bool is_unsigned,
                                        bool is_checked);

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
    llvm::LLVMValueRef codegen_checked_div(llvm::LLVMValueRef lhs, llvm::LLVMValueRef rhs, bool is_unsigned, bool is_checked);

    llvm::LLVMValueRef codegen_pointer_offset(llvm::LLVMValueRef base_ptr, llvm::LLVMValueRef offset, const Type& pointer_type, bool negate_offset);

    llvm::LLVMValueRef codegen_pointer_difference(llvm::LLVMValueRef lhs_ptr, llvm::LLVMValueRef rhs_ptr, const Type& pointer_type);

    // Computes the storage location (pointer + scpp Type) of an lvalue
    // expression, i.e. anything that can appear on the left of `=` or be
    // read via a plain load: a variable, or a chain of `.field`/`[index]`
    // off of one. Member-of-call-result (e.g. `f().x` where f returns a
    // struct by value) is intentionally not supported yet since it has no
    // backing storage to take a pointer to; that is deferred to whenever
    // by-value struct temporaries need addressable storage.
    LValue codegen_lvalue(const Expr& expr);

    // `print_int`/`print_bool`/`print_char` are temporary builtins that
    // shell out to libc's `printf` so programs can produce visible output
    // before the language grows a real string type (tracked for M2+). All
    // three return the usual `printf` result (an i32) so they can be used
    // like any other call.
    llvm::LLVMValueRef codegen_builtin_print(const Expr& expr);

    llvm::LLVMValueRef get_or_declare_printf();

    llvm::LLVMValueRef codegen_binary(const Expr& expr);

    llvm::LLVMValueRef codegen_short_circuit(const Expr& expr);
};

} // namespace scpp
