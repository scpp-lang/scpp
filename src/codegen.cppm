module;

#include <algorithm>
#include <cstdint>
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
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

export module scpp.codegen;

import scpp.ast;

export namespace scpp {

struct CodegenError : std::runtime_error {
    explicit CodegenError(const std::string& message, SourceLocation loc = {})
        : std::runtime_error(message), loc(loc) {}
    SourceLocation loc;
};

// Lowers the M1/M2 AST subset (scalars + locals + control flow + functions +
// trivial structs, no borrow/move checks yet) directly to LLVM IR.
class Codegen {
public:
    explicit Codegen(const std::string& module_name)
        : context_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>(module_name, *context_)),
          builder_(std::make_unique<llvm::IRBuilder<>>(*context_)) {}

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
            if (!def.template_params.empty()) {
                generic_type_template_names.insert(def.name);
                continue;
            }
            if (def.is_synthetic_check_only) continue;
            declare_class(def);
        }
        build_overload_names();
        auto is_never_compiled = [&](const Function& fn) {
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
            if (!fn.params.empty() && fn.params[0].name == "this" &&
                fn.params[0].type.kind == TypeKind::Reference &&
                witness_class_names.contains(fn.params[0].type.pointee->name)) {
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
            return !fn.params.empty() && fn.params[0].name == "this" && fn.params[0].type.pointee != nullptr &&
                   generic_type_template_names.contains(fn.params[0].type.pointee->name);
        };
        for (const Function& fn : program.functions) {
            if (is_never_compiled(fn)) continue;
            declare_function(fn);
        }
        for (const Function& fn : program.functions) {
            // A bodyless `extern "C"` declaration (ch02 §2.1) already got
            // its LLVM `declare` from declare_function above; there's no
            // body to lower.
            if (fn.body != nullptr && !is_never_compiled(fn)) define_function(fn);
        }
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
    };

    // A storage location: an LLVM pointer plus the scpp-level Type stored
    // there. Needed (rather than just an llvm::Value*) so Member/Subscript
    // chains can resolve field indices and element types as they walk down
    // (e.g. `p.inner.x` needs to know `p.inner`'s struct type to find `x`).
    struct LValue {
        llvm::Value* ptr;
        Type type;
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

    struct LocalSlot {
        llvm::AllocaInst* alloca;
        Type type;
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
    std::map<std::string, LocalSlot> locals_;
    std::unordered_map<std::string, StructInfo> structs_;
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

    const StructDef* find_struct_def(const std::string& name) const {
        for (const StructDef& def : program_->structs) {
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
    std::optional<Type> infer_type(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return Type{.kind = TypeKind::Named, .name = "int"};
            case ExprKind::BoolLiteral: return Type{.kind = TypeKind::Named, .name = "bool"};
            case ExprKind::CharLiteral: return Type{.kind = TypeKind::Named, .name = "char"};
            case ExprKind::StringLiteral: {
                Type result;
                result.kind = TypeKind::Pointer;
                result.pointee = std::make_shared<Type>(Type{.kind = TypeKind::Named, .name = "char"});
                result.is_mutable_pointee = false;
                return result;
            }

            case ExprKind::Identifier: {
                auto it = locals_.find(expr.name);
                return it == locals_.end() ? std::nullopt : std::optional<Type>(it->second.type);
            }

            case ExprKind::Move: {
                if (expr.lhs->kind != ExprKind::Identifier) return std::nullopt;
                auto it = locals_.find(expr.lhs->name);
                return it == locals_.end() ? std::nullopt : std::optional<Type>(it->second.type);
            }

            case ExprKind::MakeUnique: {
                Type result;
                result.kind = TypeKind::UniquePtr;
                result.pointee = std::make_shared<Type>(expr.type);
                return result;
            }

            case ExprKind::Lambda: {
                // ch05 §5.12: once resolved (movecheck's closure-
                // resolution pass), `expr.name` holds the synthesized
                // closure class's own name -- its type is exactly that
                // class, by value (matching MakeUnique's identical shape
                // just above: a fresh, concretely-typed value).
                if (expr.name.empty()) return std::nullopt;
                return Type{.kind = TypeKind::Named, .name = expr.name};
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
                auto field_it = std::find(info.field_names.begin(), info.field_names.end(), expr.name);
                if (field_it == info.field_names.end()) return std::nullopt;
                const Type& field_type = info.field_types[static_cast<size_t>(field_it - info.field_names.begin())];
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
                if (base->kind == TypeKind::Array) return *base->element;
                if (base->kind == TypeKind::Span) return *base->pointee;
                return std::nullopt;
            }

            case ExprKind::Unary:
                switch (expr.unary_op) {
                    case UnaryOp::Not: return Type{.kind = TypeKind::Named, .name = "bool"};
                    case UnaryOp::Neg: return infer_type(*expr.lhs);
                    case UnaryOp::AddressOf: {
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
                        if (!operand ||
                            (operand->kind != TypeKind::UniquePtr && operand->kind != TypeKind::Pointer)) {
                            return std::nullopt;
                        }
                        return *operand->pointee;
                    }
                }
                return std::nullopt;

            case ExprKind::Binary:
                switch (expr.binary_op) {
                    case BinaryOp::Add:
                    case BinaryOp::Sub:
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
                        return Type{.kind = TypeKind::Named, .name = "bool"};
                }
                return std::nullopt;

            case ExprKind::Call: {
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
                const Function* callee = resolve_overload_by_type(callee_name, expr.args, param_offset, receiver_is_mutable);
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
            case ExprKind::MakeUnique:
            case ExprKind::IntegerLiteral:
            case ExprKind::BoolLiteral:
            case ExprKind::CharLiteral:
            case ExprKind::StringLiteral:
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
        return arg_type.has_value() && types_equal(*arg_type, expected_type);
    }

    // Whether `arg` is a legitimate argument for a candidate overload's
    // parameter declared as `param_type` -- mirrors movecheck's own
    // argument_matches_parameter (ch05 §5.10) exactly (same by-value/
    // by-reference/std::unique_ptr distinctions), just phrased over
    // codegen's own infer_type/types_equal instead of movecheck's.
    bool argument_matches_parameter(const Expr& arg, const Type& param_type) {
        if (param_type.kind == TypeKind::UniquePtr) {
            bool produces_rvalue = arg.kind == ExprKind::Move || arg.kind == ExprKind::MakeUnique;
            if (!produces_rvalue) return false;
            std::optional<Type> arg_type = infer_type(arg);
            return arg_type.has_value() && arg_type->kind == TypeKind::UniquePtr &&
                   types_equal(*arg_type->pointee, *param_type.pointee);
        }
        if (param_type.kind == TypeKind::Reference && param_type.is_rvalue_ref) {
            // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- mirror image of
            // the ordinary-reference case just below.
            return produces_rvalue_of_type(arg, *param_type.pointee);
        }
        if (param_type.kind == TypeKind::Reference) {
            if (arg.kind == ExprKind::Move || arg.kind == ExprKind::MakeUnique ||
                arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::BoolLiteral ||
                arg.kind == ExprKind::CharLiteral || arg.kind == ExprKind::StringLiteral) {
                return false;
            }
            std::optional<Type> arg_type = infer_type(arg);
            return arg_type.has_value() && types_equal(*arg_type, *param_type.pointee);
        }
        std::optional<Type> arg_type = infer_type(arg);
        return arg_type.has_value() && types_equal(*arg_type, param_type);
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
                return it->second.type.kind == TypeKind::Reference && !it->second.type.is_mutable_ref;
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
    // usable for an ordinary call (expr.args) and a `ClassName name(args);`
    // constructor-call VarDecl (stmt.ctor_args), which has no Expr of its
    // own to hand over. `receiver_is_mutable` is the method-call
    // receiver's own mutability (ch05 §5.9's implicit `this`
    // parameter) -- meaningless when `param_offset` is 0 (an ordinary
    // free-function/constructor call, no receiver at all), and always
    // `true` for a constructor call (there's no *existing* object yet
    // for read-only-reachability to apply to).
    const Function* resolve_overload_by_type(const std::string& callee_name, const std::vector<ExprPtr>& args,
                                              size_t param_offset, bool receiver_is_mutable = true) {
        std::vector<const Function*> candidates;
        for (const Function& fn : program_->functions) {
            if (fn.name == callee_name) candidates.push_back(&fn);
        }
        if (candidates.empty()) return nullptr;
        if (candidates.size() == 1) return candidates[0];

        std::vector<const Function*> matches;
        for (const Function* fn : candidates) {
            if (fn->params.size() != args.size() + param_offset) continue;
            // The receiver (`this`): viable only if the candidate's own
            // `this` mutability doesn't demand more than the receiver
            // place can actually provide.
            if (param_offset == 1 && fn->params[0].type.is_mutable_ref && !receiver_is_mutable) continue;
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
        auto is_read_only_arg = [&](const Expr& arg) {
            std::optional<Type> t = infer_type(arg);
            return t.has_value() && t->kind == TypeKind::Reference && !t->is_mutable_ref;
        };
        auto mutable_ref_score = [&](const Function* fn) {
            int score = 0;
            if (param_offset == 1 && fn->params[0].type.is_mutable_ref && receiver_is_mutable) score++;
            for (size_t i = 0; i < args.size(); i++) {
                const Type& param_type = fn->params[i + param_offset].type;
                if (param_type.kind == TypeKind::Reference && param_type.is_mutable_ref &&
                    !is_read_only_arg(*args[i])) {
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
    // types, and structs whose fields are themselves all trivial.
    // `in_progress` detects a struct containing itself *by value*, which
    // must be rejected (as in C, this would be an infinitely-sized type);
    // self-reference via pointer is fine since pointers don't recurse here.
    void validate_trivial(const Type& type, std::vector<std::string>& in_progress) {
        switch (type.kind) {
            case TypeKind::Pointer:
                return;
            case TypeKind::UniquePtr:
                throw CodegenError("std::unique_ptr carries ownership and cannot be a struct field; "
                                    "use class instead",
                    current_loc_);
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
                if (type.name == "int" || type.name == "bool" || type.name == "char") return;
                if (type.name == "void") {
                    throw CodegenError("'void' cannot be a struct field (only a return type or a "
                                        "pointer's pointee -- 'void*' -- may be 'void')",
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
        std::vector<std::string> in_progress;
        for (const StructField& field : def.fields) {
            try {
                validate_trivial(field.type, in_progress);
            } catch (const CodegenError& e) {
                throw CodegenError("struct '" + def.name + "' field '" + field.name + "': " + e.what() +
                                    " (only scalars, pointers, trivial structs, and fixed-size arrays "
                                    "of trivial types are allowed in a struct; see spec ch04)",
                    current_loc_);
            }
        }

        StructInfo info;
        std::vector<llvm::Type*> llvm_field_types;
        llvm_field_types.reserve(def.fields.size());
        for (const StructField& field : def.fields) {
            info.field_names.push_back(field.name);
            info.field_types.push_back(field.type);
            llvm_field_types.push_back(to_llvm_type(field.type));
        }
        info.llvm_type = llvm::StructType::create(*context_, llvm_field_types, "struct." + def.name);
        structs_[def.name] = std::move(info);
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
    void declare_class(const ClassDef& def) {
        StructInfo info;
        std::vector<llvm::Type*> llvm_field_types;
        llvm_field_types.reserve(def.fields.size());
        for (const ClassField& field : def.fields) {
            info.field_names.push_back(field.name);
            info.field_types.push_back(field.type);
            llvm_field_types.push_back(to_llvm_type(field.type));
        }
        info.llvm_type = llvm::StructType::create(*context_, llvm_field_types, "class." + def.name);
        structs_[def.name] = std::move(info);
    }

    llvm::Type* to_llvm_type(const Type& type) {
        switch (type.kind) {
            case TypeKind::Pointer:
            case TypeKind::UniquePtr:
            case TypeKind::Reference:
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
                throw CodegenError("unsupported type '" + type.name + "'",
                    current_loc_);
        }
        throw CodegenError("unhandled type kind",
            current_loc_);
    }

    // A reference's referent may not itself be a std::unique_ptr in this
    // version: that would require the borrow checker to also reason about
    // moving/dropping the owner out from under a live borrow, which is
    // deferred (see spec ch05.2/M4 scope notes near BorrowState below).
    // Nor may it be another reference: reference-to-reference aliasing
    // analysis is likewise out of scope for v0.1's intraprocedural,
    // first-order borrow checking.
    void validate_reference_pointee(const Type& pointee) {
        if (pointee.kind == TypeKind::UniquePtr) {
            throw CodegenError("a reference to std::unique_ptr is not yet supported in this version",
                current_loc_);
        }
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
        switch (type.kind) {
            case TypeKind::Named:
            case TypeKind::Pointer:
            case TypeKind::Array:
                return;
            case TypeKind::UniquePtr:
                throw CodegenError("function '" + fn_name + "' (extern \"C\"): " + context_description +
                                    " cannot be std::unique_ptr -- it has no defined C representation "
                                    "(spec ch02 §2.1)",
                    current_loc_);
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
            case TypeKind::Named: return a.name == b.name;
            case TypeKind::Pointer: return a.is_mutable_pointee == b.is_mutable_pointee && types_equal(*a.pointee, *b.pointee);
            case TypeKind::UniquePtr: return types_equal(*a.pointee, *b.pointee);
            case TypeKind::Reference:
                return a.is_mutable_ref == b.is_mutable_ref && a.is_rvalue_ref == b.is_rvalue_ref &&
                       types_equal(*a.pointee, *b.pointee);
            case TypeKind::Span:
                return a.is_mutable_ref == b.is_mutable_ref && types_equal(*a.pointee, *b.pointee);
            case TypeKind::Array: return a.array_size == b.array_size && types_equal(*a.element, *b.element);
        }
        return false;
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
            case TypeKind::Named: return type.name;
            case TypeKind::Pointer: return mangle_type(*type.pointee) + (type.is_mutable_pointee ? "_ptr" : "_cptr");
            case TypeKind::UniquePtr: return mangle_type(*type.pointee) + "_uptr";
            case TypeKind::Reference:
                return mangle_type(*type.pointee) +
                       (type.is_rvalue_ref ? "_rref" : (type.is_mutable_ref ? "_ref" : "_cref"));
            case TypeKind::Span: return mangle_type(*type.pointee) + (type.is_mutable_ref ? "_span" : "_cspan");
            case TypeKind::Array: return mangle_type(*type.element) + "_arr" + std::to_string(type.array_size);
        }
        return "?";
    }

    // The full, human-readable-spelled-out type text ch11 §11.9's real
    // mangling scheme requires (e.g. "int", "const int&",
    // "std::unique_ptr<int>") -- mirrors cli.cppm's own type_to_string
    // (a separate module, so duplicated rather than shared; both are
    // small, stable, one-purpose functions).
    [[nodiscard]] static std::string verbatim_type_spelling(const Type& type) {
        switch (type.kind) {
            case TypeKind::Named: return type.name;
            case TypeKind::Pointer:
                return (type.is_mutable_pointee ? std::string() : std::string("const ")) +
                       verbatim_type_spelling(*type.pointee) + "*";
            case TypeKind::UniquePtr: return "std::unique_ptr<" + verbatim_type_spelling(*type.pointee) + ">";
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
            bool recovered_from_elsewhere = !fns[0]->owning_module.empty();
            bool exported_from_this_module = program_->module_name.empty() ? false : fns[0]->is_exported;
            if (recovered_from_elsewhere || exported_from_this_module) {
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
        for (const Param& param : fn.params) {
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
            param_types.push_back(to_llvm_type(param.type));
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
        if (fn.body != nullptr && fn.owning_module.empty() && !program_->module_name.empty() && !fn.is_exported) {
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
        // Mirrors movecheck's entry_state.unsafe_depth (ch01 §1.3): every
        // function is checked by default and starts outside any unsafe
        // context -- unsafe_depth_ only ever increases via an explicit
        // `unsafe { }` block within this same function's own body (the
        // old "native function = implicitly unsafe everywhere" concept
        // is fully retired).
        unsafe_depth_ = 0;
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", llvm_fn);
        builder_->SetInsertPoint(entry);

        locals_.clear();
        scope_stack_.clear();
        size_t index = 0;
        for (auto& arg : llvm_fn->args()) {
            const Param& param = fn.params[index++];
            arg.setName(param.name);
            llvm::AllocaInst* slot = builder_->CreateAlloca(arg.getType(), nullptr, param.name);
            builder_->CreateStore(&arg, slot);
            locals_[param.name] = LocalSlot{slot, param.type};
        }

        codegen_stmt(*fn.body, llvm_fn);

        // Every well-formed M1 function must return on all paths; if the
        // generated block has no terminator (e.g. missing trailing return),
        // that is a user error we surface instead of emitting invalid IR.
        if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
            throw CodegenError("function '" + fn.name + "' does not return on all paths",
                current_loc_);
        }
    }

    void codegen_stmt(const Stmt& stmt, llvm::Function* current_function) {
        // Refreshed on every call (including each recursive call for a
        // nested statement) so a CodegenError thrown while handling
        // `stmt` points at `stmt` itself -- see current_loc_ and
        // codegen_expr's identical opening comment.
        current_loc_ = stmt.loc;
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
                    validate_reference_pointee(*stmt.type.pointee);
                    // Store the *address* of the referent (not its value)
                    // -- codegen_lvalue on the initializer gives exactly
                    // that, and also enforces it resolves to a real,
                    // addressable place (a plain variable, or a further
                    // member/subscript chain off one).
                    llvm::Value* referent_addr = codegen_lvalue(*stmt.init).ptr;
                    llvm::AllocaInst* slot =
                        builder_->CreateAlloca(llvm::PointerType::getUnqual(*context_), nullptr, stmt.var_name);
                    builder_->CreateStore(referent_addr, slot);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
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
                    LValue source = codegen_lvalue(*stmt.init);
                    if (source.type.kind != TypeKind::Array) {
                        throw CodegenError("std::span<T> can currently only be constructed from a "
                                            "fixed-size array in this version",
                            current_loc_);
                    }
                    if (to_llvm_type(*source.type.element) != to_llvm_type(*stmt.type.pointee)) {
                        throw CodegenError("cannot construct span '" + stmt.var_name +
                                            "': array element type does not match the span's element type",
                            current_loc_);
                    }
                    llvm::Type* span_type = to_llvm_type(stmt.type);
                    llvm::Value* size_value =
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), source.type.array_size);
                    llvm::Value* span_value = llvm::UndefValue::get(span_type);
                    span_value = builder_->CreateInsertValue(span_value, source.ptr, {0});
                    span_value = builder_->CreateInsertValue(span_value, size_value, {1});
                    llvm::AllocaInst* slot = builder_->CreateAlloca(span_type, nullptr, stmt.var_name);
                    builder_->CreateStore(span_value, slot);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
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
                // initializer rather than `ClassName name(args);`'s own
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
                    llvm::AllocaInst* closure_ptr = codegen_construct_lambda(*stmt.init);
                    locals_[stmt.var_name] = LocalSlot{closure_ptr, stmt.type};
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    return;
                }

                llvm::Type* llvm_type = to_llvm_type(stmt.type);
                llvm::AllocaInst* slot = builder_->CreateAlloca(llvm_type, nullptr, stmt.var_name);
                if (stmt.has_ctor_args) {
                    // `ClassName name(args);` (ch04 §4.2): direct-
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
                    if (stmt.type.kind != TypeKind::Named || !structs_.contains(stmt.type.name)) {
                        throw CodegenError("'" + stmt.var_name +
                                            "(...)' constructor-call syntax is only supported for a class type",
                            current_loc_);
                    }
                    builder_->CreateStore(llvm::Constant::getNullValue(llvm_type), slot);
                    locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
                    if (!scope_stack_.empty()) {
                        scope_stack_.back().push_back(stmt.var_name);
                    }
                    std::string ctor_name = stmt.type.name + "_new";
                    // ch05 §5.10: a class may declare multiple
                    // constructors (all synthesized as "ClassName_new"),
                    // resolved by exact argument-type match exactly like
                    // any other overloaded name.
                    const Function* ctor_def = resolve_overload_by_type(ctor_name, stmt.ctor_args, /*param_offset=*/1);
                    if (ctor_def == nullptr) {
                        throw CodegenError("class '" + stmt.type.name + "' has no constructor matching this call",
                            current_loc_);
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
                    llvm::Value* init_value = codegen_expr(*stmt.init);
                    // Refresh to `stmt`'s own position: codegen_expr just
                    // recursed through `stmt.init` (possibly a compound
                    // expression like `a + b`), leaving current_loc_ at
                    // whichever sub-expression it last visited rather
                    // than the statement check_store_type is actually
                    // about.
                    current_loc_ = stmt.loc;
                    check_store_type(init_value, llvm_type, "variable '" + stmt.var_name + "'");
                    builder_->CreateStore(init_value, slot);
                } else {
                    // scpp has no concept of an uninitialized variable: a
                    // local declared without an initializer is always
                    // zero-initialized (0 / false / null / all-zero
                    // fields), for every type -- scalars and raw pointers
                    // included, not just struct/array/unique_ptr.
                    builder_->CreateStore(llvm::Constant::getNullValue(llvm_type), slot);
                }
                locals_[stmt.var_name] = LocalSlot{slot, stmt.type};
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
                    value = current_function_def_ != nullptr && current_function_def_->return_type.kind == TypeKind::Reference
                                ? codegen_lvalue(*stmt.expr).ptr
                                : codegen_expr(*stmt.expr);
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
                codegen_expr(*stmt.expr);
                return;

            case StmtKind::If: {
                // `stmt.condition` is a `bool` expression, stored/passed
                // as i8 (see to_llvm_type) -- CreateCondBr needs a 1-bit
                // condition, so narrow it right here (see bool_to_i1).
                llvm::Value* cond = bool_to_i1(codegen_expr(*stmt.condition));
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
                llvm::Value* cond = bool_to_i1(codegen_expr(*stmt.condition));
                builder_->CreateCondBr(cond, body_block, end_block);

                // The body's scope is popped (and its unique_ptr locals
                // dropped) at the end of *every* iteration, right before
                // jumping back to re-check the condition -- so a
                // unique_ptr re-declared each iteration doesn't leak the
                // previous iteration's allocation.
                builder_->SetInsertPoint(body_block);
                push_scope();
                codegen_stmt(*stmt.then_branch, current_function);
                pop_scope();
                if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
                    builder_->CreateBr(cond_block);
                }

                builder_->SetInsertPoint(end_block);
                return;
            }
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
        const Function* callee_def = resolve_overload_by_type(callee_name, expr.args, param_offset, receiver_is_mutable);
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
        if (this_arg != nullptr) args.insert(args.begin(), this_arg);
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
    std::vector<llvm::Value*> codegen_call_args(const std::vector<ExprPtr>& args, const Function* callee_def,
                                                  size_t param_offset) {
        std::vector<llvm::Value*> result;
        result.reserve(args.size());
        for (size_t i = 0; i < args.size(); i++) {
            bool param_is_reference = callee_def != nullptr && i + param_offset < callee_def->params.size() &&
                                       callee_def->params[i + param_offset].type.kind == TypeKind::Reference;
            bool param_is_rvalue_reference =
                param_is_reference && callee_def->params[i + param_offset].type.is_rvalue_ref;
            if (param_is_rvalue_reference) {
                // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- the move
                // checker has already verified this argument produces a
                // genuine rvalue (produces_rvalue_of_type), which may not
                // itself be an addressable place (a literal, a fresh
                // std::make_unique<T>(...)/call result, ...).
                if (args[i]->kind == ExprKind::Lambda) {
                    // ch05 §5.12: a lambda literal's own codegen (see
                    // codegen_expr's Lambda case) already allocates a
                    // fresh temporary and returns *its* address directly
                    // (a class value is always represented/passed by
                    // address in this codebase, never as a bare
                    // aggregate SSA value) -- using that address
                    // directly as the argument avoids double-wrapping it
                    // in yet another temporary, which would pass "a
                    // pointer to a pointer to the closure" instead of "a
                    // pointer to the closure".
                    result.push_back(codegen_expr(*args[i]));
                    continue;
                }
                // Otherwise: evaluate it as an ordinary *value*
                // (codegen_expr, not codegen_lvalue -- this also reuses
                // std::move's own codegen unchanged, including its
                // "null out the source slot" side effect when the moved
                // value is itself a std::unique_ptr), then materialize a
                // fresh stack temporary to hold it and pass that
                // temporary's address -- exactly what real C++ does when
                // binding a reference to a prvalue.
                llvm::Value* value = codegen_expr(*args[i]);
                llvm::AllocaInst* temp = builder_->CreateAlloca(value->getType(), nullptr, "rvaluetmp");
                builder_->CreateStore(value, temp);
                result.push_back(temp);
            } else if (param_is_reference) {
                // Bind the reference parameter to the argument's address
                // rather than passing its value, exactly like a local
                // reference's own VarDecl.
                result.push_back(codegen_lvalue(*args[i]).ptr);
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
        return builder_->CreateLoad(to_llvm_type(lv.type), lv.ptr, "loadtmp");
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
        current_loc_ = expr.loc;
        switch (expr.kind) {
            case ExprKind::IntegerLiteral:
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), expr.int_value, /*isSigned=*/true);

            case ExprKind::BoolLiteral:
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

            case ExprKind::Identifier:
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
                    // Same lvalue-then-load pattern as Identifier/Member/
                    // Subscript above: codegen_lvalue resolves *what*
                    // `*p` addresses (see its own Unary case), this just
                    // reads the value stored there.
                    LValue lv = codegen_lvalue(expr);
                    return builder_->CreateLoad(to_llvm_type(lv.type), lv.ptr, "loadtmp");
                }
                if (expr.unary_op == UnaryOp::AddressOf) {
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
                llvm::Value* operand = codegen_expr(*expr.lhs);
                if (expr.unary_op == UnaryOp::Neg) return builder_->CreateNeg(operand, "negtmp");
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
                if (expr.name == "print_int" || expr.name == "print_bool" || expr.name == "print_char") {
                    return codegen_builtin_print(expr);
                }
                CallResult result = codegen_call(expr);
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
                // plain, currently-Initialized unique_ptr variable. At the
                // IR level a move is: read the old value, then null out the
                // source slot -- so even code that (incorrectly) bypassed
                // the move checker would observe a null pointer rather than
                // an aliased/duplicated one.
                LValue lv = codegen_lvalue(*expr.lhs);
                llvm::Type* llvm_type = to_llvm_type(lv.type);
                llvm::Value* old_value = builder_->CreateLoad(llvm_type, lv.ptr, "movetmp");
                builder_->CreateStore(llvm::Constant::getNullValue(llvm_type), lv.ptr);
                return old_value;
            }

            case ExprKind::MakeUnique:
                return codegen_make_unique(expr);

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
    llvm::AllocaInst* codegen_construct_lambda(const Expr& expr) {
        const StructInfo& info = structs_.at(expr.name);
        llvm::AllocaInst* closure = builder_->CreateAlloca(info.llvm_type, nullptr, "lambdatmp");
        for (size_t i = 0; i < expr.lambda_captures.size(); i++) {
            const LambdaCapture& capture = expr.lambda_captures[i];
            llvm::Value* field_ptr = builder_->CreateStructGEP(info.llvm_type, closure, i, capture.name);
            if (capture.by_reference) {
                Expr ident;
                ident.kind = ExprKind::Identifier;
                ident.loc = expr.loc;
                ident.name = capture.name;
                llvm::Value* address = codegen_lvalue(ident).ptr;
                builder_->CreateStore(address, field_ptr);
                continue;
            }
            llvm::Value* value;
            if (capture.init) {
                value = codegen_expr(*capture.init);
            } else {
                Expr ident;
                ident.kind = ExprKind::Identifier;
                ident.loc = expr.loc;
                ident.name = capture.name;
                value = codegen_expr(ident);
            }
            check_store_type(value, to_llvm_type(info.field_types[i]), "capture '" + capture.name + "'");
            builder_->CreateStore(value, field_ptr);
        }
        return closure;
    }

    // `std::make_unique<T>(...)` is a compiler builtin (like std::move),
    // not a real generic function call -- scpp has no `new` expression at
    // all; make_unique is the only sanctioned way to heap-allocate. v0.1
    // supports exactly two forms: zero arguments (zero-initializes T, like
    // a bare `T x;`) or one argument when T is a scalar (int/bool/char),
    // initializing it to that value. Everything else (multiple arguments,
    // or one argument for a struct/array/pointer T) needs real constructor
    // support that doesn't exist yet.
    llvm::Value* codegen_make_unique(const Expr& expr) {
        llvm::Type* element_type = to_llvm_type(expr.type);
        bool element_is_scalar = expr.type.kind == TypeKind::Named &&
                                  (expr.type.name == "int" || expr.type.name == "bool" || expr.type.name == "char");

        llvm::Value* initial_value;
        if (expr.args.empty()) {
            initial_value = llvm::Constant::getNullValue(element_type);
        } else if (expr.args.size() == 1 && element_is_scalar) {
            initial_value = codegen_expr(*expr.args[0]);
            // Refresh to `expr`'s own position -- see the VarDecl case's
            // identical comment above.
            current_loc_ = expr.loc;
            check_store_type(initial_value, element_type,
                              "std::make_unique<" + expr.type.name + ">(...)'s argument");
        } else {
            throw CodegenError(
                "std::make_unique<T>(...) currently only supports zero arguments (zero-initializes "
                "T) or exactly one argument when T is a scalar (int/bool/char)",
                current_loc_);
        }

        llvm::Function* malloc_fn = get_or_declare_malloc();
        uint64_t size_in_bytes = module_->getDataLayout().getTypeAllocSize(element_type);
        llvm::Value* size_arg = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), size_in_bytes);
        llvm::Value* heap_ptr = builder_->CreateCall(malloc_fn, {size_arg}, "newptr");
        builder_->CreateStore(initial_value, heap_ptr);
        return heap_ptr;
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
        return module_->getFunction(class_name + "_delete");
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
        llvm::Function* free_fn = get_or_declare_free();
        for (const auto& [name, slot] : locals_) {
            if (slot.type.kind == TypeKind::UniquePtr) {
                llvm::Value* current = builder_->CreateLoad(to_llvm_type(slot.type), slot.alloca, "droptmp");
                builder_->CreateCall(free_fn, {current});
            } else if (slot.type.kind == TypeKind::Named) {
                if (llvm::Function* dtor = find_destructor(slot.type.name)) {
                    builder_->CreateCall(dtor, {slot.alloca});
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
            llvm::Function* free_fn = get_or_declare_free();
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                auto slot_it = locals_.find(*it);
                if (slot_it == locals_.end()) continue;
                if (slot_it->second.type.kind == TypeKind::UniquePtr) {
                    llvm::Value* current = builder_->CreateLoad(to_llvm_type(slot_it->second.type),
                                                                 slot_it->second.alloca, "scopedroptmp");
                    builder_->CreateCall(free_fn, {current});
                } else if (slot_it->second.type.kind == TypeKind::Named) {
                    if (llvm::Function* dtor = find_destructor(slot_it->second.type.name)) {
                        builder_->CreateCall(dtor, {slot_it->second.alloca});
                    }
                }
            }
        }
        for (const std::string& name : names) {
            locals_.erase(name);
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

    // `+`/`-`/`*` (ch05 §5.8): overflow-checked by default (aborting,
    // via the same panic mechanism as emit_span_bounds_check, on overflow
    // -- both signed and unsigned per the spec, though only scpp's signed
    // `int` exists as an arithmetic type so far), or a plain, guaranteed-
    // wrapping (never UB) operation inside `unsafe { }`
    // (unsafe_depth_ > 0) -- achieved simply by using the plain
    // CreateAdd/CreateSub/CreateMul instructions, which (unlike real
    // Clang's) never get an `nsw`/`nuw` flag anywhere in this codebase, so
    // they're already well-defined, wrapping LLVM IR on their own. Only
    // i32 (scpp's `int`) is checked: `char`/`bool` (i8) arithmetic isn't a
    // defined part of the numeric-family spec yet (ch06), so any other
    // width falls back to the same plain instruction regardless of
    // context, unchanged from before this check existed.
    llvm::Value* codegen_checked_arith(BinaryOp op, llvm::Value* lhs, llvm::Value* rhs) {
        const char* name = op == BinaryOp::Add ? "addtmp" : op == BinaryOp::Sub ? "subtmp" : "multmp";
        if (unsafe_depth_ > 0 || !lhs->getType()->isIntegerTy(32)) {
            switch (op) {
                case BinaryOp::Add: return builder_->CreateAdd(lhs, rhs, name);
                case BinaryOp::Sub: return builder_->CreateSub(lhs, rhs, name);
                case BinaryOp::Mul: return builder_->CreateMul(lhs, rhs, name);
                default: throw CodegenError("unhandled checked-arithmetic operator",
                    current_loc_);
            }
        }

        llvm::Intrinsic::ID intrinsic_id = op == BinaryOp::Add   ? llvm::Intrinsic::sadd_with_overflow
                                            : op == BinaryOp::Sub ? llvm::Intrinsic::ssub_with_overflow
                                                                   : llvm::Intrinsic::smul_with_overflow;
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

    // `/` (ch05 §5.8): `b == 0` or (the one case signed division itself
    // overflows) `a == INT_MIN && b == -1` always abort() -- unconditionally,
    // in *every* context, whether inside `unsafe { }` or not, unlike
    // +/-/* above: both trap at the hardware level (x86 #DE) with no
    // wrapped result for the hardware to fall back on, so there is no
    // "unsafe and still defined" variant to fall back to here. Only i32
    // is checked, for the same reason codegen_checked_arith only checks
    // i32.
    llvm::Value* codegen_checked_div(llvm::Value* lhs, llvm::Value* rhs) {
        if (!lhs->getType()->isIntegerTy(32)) {
            return builder_->CreateSDiv(lhs, rhs, "divtmp");
        }

        llvm::Type* i32 = llvm::Type::getInt32Ty(*context_);
        llvm::Value* zero = llvm::ConstantInt::get(i32, 0, /*isSigned=*/true);
        llvm::Value* int_min =
            llvm::ConstantInt::get(i32, static_cast<uint64_t>(std::numeric_limits<int32_t>::min()), /*isSigned=*/true);
        llvm::Value* neg_one = llvm::ConstantInt::get(i32, static_cast<uint64_t>(-1), /*isSigned=*/true);

        llvm::Value* divides_by_zero = builder_->CreateICmpEQ(rhs, zero, "divzero");
        llvm::Value* overflows = builder_->CreateAnd(builder_->CreateICmpEQ(lhs, int_min, "isintmin"),
                                                       builder_->CreateICmpEQ(rhs, neg_one, "isnegone"), "divoverflow");
        llvm::Value* traps = builder_->CreateOr(divides_by_zero, overflows, "divtraps");

        llvm::Function* current_function = builder_->GetInsertBlock()->getParent();
        llvm::BasicBlock* fail_block = llvm::BasicBlock::Create(*context_, "div.fail", current_function);
        llvm::BasicBlock* ok_block = llvm::BasicBlock::Create(*context_, "div.ok", current_function);
        builder_->CreateCondBr(traps, fail_block, ok_block);

        builder_->SetInsertPoint(fail_block);
        builder_->CreateCall(get_or_declare_abort(), {});
        builder_->CreateUnreachable();

        builder_->SetInsertPoint(ok_block);
        return builder_->CreateSDiv(lhs, rhs, "divtmp");
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
        current_loc_ = expr.loc;
        switch (expr.kind) {
            case ExprKind::Identifier: {
                auto it = locals_.find(expr.name);
                if (it == locals_.end()) {
                    throw CodegenError("use of undeclared variable '" + expr.name + "'",
                        current_loc_);
                }
                if (it->second.type.kind == TypeKind::Reference) {
                    // A reference-typed local's own alloca just holds the
                    // address it's bound to (see the VarDecl case below,
                    // and how a Reference parameter arrives already as
                    // that address): auto-dereference once so every
                    // caller (reads, writes-through, and Member/Subscript
                    // base resolution) transparently operates on the
                    // referent, exactly like a real C++ reference.
                    llvm::Value* referent_ptr = builder_->CreateLoad(
                        llvm::PointerType::getUnqual(*context_), it->second.alloca, "deref");
                    return LValue{referent_ptr, *it->second.type.pointee};
                }
                return LValue{it->second.alloca, it->second.type};
            }

            case ExprKind::Member: {
                LValue base = codegen_lvalue(*expr.lhs);
                if (base.type.kind != TypeKind::Named || !structs_.contains(base.type.name)) {
                    throw CodegenError("member access '." + expr.name + "' on a non-struct type",
                        current_loc_);
                }
                const StructInfo& info = structs_.at(base.type.name);
                auto field_it = std::find(info.field_names.begin(), info.field_names.end(), expr.name);
                if (field_it == info.field_names.end()) {
                    throw CodegenError("struct '" + base.type.name + "' has no field '" + expr.name + "'",
                        current_loc_);
                }
                size_t field_index = static_cast<size_t>(field_it - info.field_names.begin());
                llvm::Value* field_ptr =
                    builder_->CreateStructGEP(info.llvm_type, base.ptr, field_index, expr.name);
                const Type& field_type = info.field_types[field_index];
                if (field_type.kind == TypeKind::Reference) {
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
                        builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), field_ptr, "fieldderef");
                    return LValue{referent_ptr, *field_type.pointee};
                }
                return LValue{field_ptr, field_type};
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
                    return LValue{elem_ptr, *base.type.pointee};
                }
                if (base.type.kind != TypeKind::Array) {
                    throw CodegenError("subscript on a non-array type",
                        current_loc_);
                }
                llvm::Value* index = codegen_expr(*expr.rhs);
                llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                llvm::Value* elem_ptr =
                    builder_->CreateGEP(to_llvm_type(base.type), base.ptr, {zero, index}, "elemtmp");
                return LValue{elem_ptr, *base.type.element};
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
                return LValue{result.value, *result.callee_def->return_type.pointee};
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
                return LValue{ptr, Type{.kind = TypeKind::Named, .name = expr.name}};
            }

            case ExprKind::Unary: {
                // Only `*p` (Deref) is addressable; Neg/Not produce a
                // plain value with no backing storage.
                if (expr.unary_op != UnaryOp::Deref) {
                    throw CodegenError("expression is not assignable",
                        current_loc_);
                }
                LValue operand = codegen_lvalue(*expr.lhs);
                if (operand.type.kind != TypeKind::UniquePtr && operand.type.kind != TypeKind::Pointer) {
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
                    throw CodegenError(
                        "dereference ('*') is only supported for std::unique_ptr or a raw pointer",
                        current_loc_);
                }
                // A unique_ptr's/raw pointer's own storage holds the
                // pointer *value* (see to_llvm_type's UniquePtr/Pointer
                // case); dereferencing means loading that value and using
                // it as the new base address, exactly like a reference's
                // own auto-deref above.
                llvm::Value* pointee_ptr =
                    builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), operand.ptr, "deref");
                return LValue{pointee_ptr, *operand.type.pointee};
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
            llvm::Value* value = codegen_expr(*expr.rhs);
            // Refresh to `expr`'s own position -- see the VarDecl case's
            // identical comment in codegen_stmt.
            current_loc_ = expr.loc;
            check_store_type(value, to_llvm_type(lv.type), "'" + expr.lhs->name + "'");
            if (lv.type.kind == TypeKind::UniquePtr) {
                // Move-assignment semantics: release whatever this
                // unique_ptr currently owns *before* overwriting it with
                // the new value, so reassigning one that already owns a
                // real allocation doesn't leak it. free(NULL) is a safe
                // no-op, so this is correct whether or not there was a
                // real prior allocation (freshly declared, already moved
                // out, etc).
                llvm::Value* old_value = builder_->CreateLoad(to_llvm_type(lv.type), lv.ptr, "oldtmp");
                builder_->CreateCall(get_or_declare_free(), {old_value});
            }
            builder_->CreateStore(value, lv.ptr);
            return value;
        }

        // `&&`/`||` short-circuit like ordinary C++; everything else is a
        // plain eager binary op on the operand values.
        if (expr.binary_op == BinaryOp::And || expr.binary_op == BinaryOp::Or) {
            return codegen_short_circuit(expr);
        }

        llvm::Value* lhs = codegen_expr(*expr.lhs);
        llvm::Value* rhs = codegen_expr(*expr.rhs);

        switch (expr.binary_op) {
            case BinaryOp::Add:
            case BinaryOp::Sub:
            case BinaryOp::Mul:
                return codegen_checked_arith(expr.binary_op, lhs, rhs);
            case BinaryOp::Div: return codegen_checked_div(lhs, rhs);
            // Comparisons always produce a genuine i1 from icmp, but a
            // scpp `bool` result needs to be widened to the i8 every
            // other bool value uses (see i1_to_bool/to_llvm_type) before
            // it can be stored, passed, or returned like any other value.
            case BinaryOp::Eq: return i1_to_bool(builder_->CreateICmpEQ(lhs, rhs, "eqtmp"));
            case BinaryOp::Ne: return i1_to_bool(builder_->CreateICmpNE(lhs, rhs, "netmp"));
            case BinaryOp::Lt: return i1_to_bool(builder_->CreateICmpSLT(lhs, rhs, "lttmp"));
            case BinaryOp::Gt: return i1_to_bool(builder_->CreateICmpSGT(lhs, rhs, "gttmp"));
            case BinaryOp::Le: return i1_to_bool(builder_->CreateICmpSLE(lhs, rhs, "letmp"));
            case BinaryOp::Ge: return i1_to_bool(builder_->CreateICmpSGE(lhs, rhs, "getmp"));
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
        llvm::Value* lhs = codegen_expr(*expr.lhs);
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
        llvm::Value* rhs = codegen_expr(*expr.rhs);
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
