module;

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

export module scpp.codegen;

import scpp.ast;

export namespace scpp {

struct CodegenError : std::runtime_error {
    explicit CodegenError(const std::string& message) : std::runtime_error(message) {}
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
        program_ = &program;
        for (const StructDef& def : program.structs) {
            declare_struct(def);
        }
        for (const Function& fn : program.functions) {
            declare_function(fn);
        }
        for (const Function& fn : program.functions) {
            define_function(fn);
        }
        std::string error;
        llvm::raw_string_ostream error_stream(error);
        if (llvm::verifyModule(*module_, &error_stream)) {
            throw CodegenError("module verification failed: " + error);
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

    struct LocalSlot {
        llvm::AllocaInst* alloca;
        Type type;
    };

    const Program* program_ = nullptr;
    // The AST-level Function currently being lowered by define_function,
    // consulted by StmtKind::Return to tell whether *this* function's own
    // return type is a reference (see codegen_stmt's Return case).
    const Function* current_function_def_ = nullptr;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::map<std::string, LocalSlot> locals_;
    std::unordered_map<std::string, StructInfo> structs_;
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
                                    "use class instead");
            case TypeKind::Reference:
                throw CodegenError("a reference cannot be a struct field in this version");
            case TypeKind::Span:
                throw CodegenError("a std::span cannot be a struct field in this version (it is a "
                                    "lifetime-checked borrowed view; use class instead)");
            case TypeKind::Array:
                validate_trivial(*type.element, in_progress);
                return;
            case TypeKind::Named: {
                if (type.name == "int" || type.name == "bool") return;
                const StructDef* def = find_struct_def(type.name);
                if (def == nullptr) {
                    throw CodegenError("unknown type '" + type.name + "'");
                }
                if (std::find(in_progress.begin(), in_progress.end(), type.name) != in_progress.end()) {
                    throw CodegenError("struct '" + type.name + "' cannot contain itself by value "
                                                                 "(did you mean a pointer '" +
                                        type.name + "*'?)");
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
                                    "of trivial types are allowed in a struct; see spec ch04)");
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
                if (type.name == "bool") return llvm::Type::getInt1Ty(*context_);
                {
                    auto it = structs_.find(type.name);
                    if (it != structs_.end()) return it->second.llvm_type;
                }
                throw CodegenError("unsupported type '" + type.name + "'");
        }
        throw CodegenError("unhandled type kind");
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
            throw CodegenError("a reference to std::unique_ptr is not yet supported in this version");
        }
        if (pointee.kind == TypeKind::Reference) {
            throw CodegenError("a reference to a reference is not supported");
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
            if (param.type.kind != TypeKind::Reference) continue;
            if (found != nullptr) {
                throw CodegenError("function '" + fn.name +
                                    "' returns a reference but has more than one reference parameter; scpp "
                                    "v0.1 can only infer a returned reference's lifetime when there is exactly "
                                    "one (spec ch05.3)");
            }
            found = &param;
        }
        if (found == nullptr) {
            throw CodegenError("function '" + fn.name +
                                "' returns a reference but has no reference parameter to infer its lifetime "
                                "from (spec ch05.3)");
        }
        if (fn.return_type.is_mutable_ref && !found->type.is_mutable_ref) {
            throw CodegenError("function '" + fn.name +
                                "' returns a mutable reference ('T&') but its sole reference parameter '" +
                                found->name + "' is a shared reference ('const T&')");
        }
    }

    void declare_function(const Function& fn) {
        if (fn.return_type.kind == TypeKind::Reference) {
            validate_reference_return_elision(fn);
            validate_reference_pointee(*fn.return_type.pointee);
        }
        std::vector<llvm::Type*> param_types;
        param_types.reserve(fn.params.size());
        for (const Param& param : fn.params) {
            if (param.type.kind == TypeKind::Reference) {
                validate_reference_pointee(*param.type.pointee);
            }
            param_types.push_back(to_llvm_type(param.type));
        }
        llvm::FunctionType* fn_type =
            llvm::FunctionType::get(to_llvm_type(fn.return_type), param_types, /*isVarArg=*/false);
        llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, fn.name, *module_);
    }

    void define_function(const Function& fn) {
        llvm::Function* llvm_fn = module_->getFunction(fn.name);
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition");
        }

        current_function_def_ = &fn;
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
            throw CodegenError("function '" + fn.name + "' does not return on all paths");
        }
    }

    void codegen_stmt(const Stmt& stmt, llvm::Function* current_function) {
        switch (stmt.kind) {
            case StmtKind::Block:
                push_scope();
                for (const auto& s : stmt.statements) {
                    // Once a block has a terminator (return), skip anything
                    // after it: unreachable code shouldn't be lowered.
                    if (builder_->GetInsertBlock()->getTerminator() != nullptr) break;
                    codegen_stmt(*s, current_function);
                }
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
                                            "' must be initialized (bound to a variable) at declaration");
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
                                            "' must be initialized (bound to an array) at declaration");
                    }
                    LValue source = codegen_lvalue(*stmt.init);
                    if (source.type.kind != TypeKind::Array) {
                        throw CodegenError("std::span<T> can currently only be constructed from a "
                                            "fixed-size array in this version");
                    }
                    if (to_llvm_type(*source.type.element) != to_llvm_type(*stmt.type.pointee)) {
                        throw CodegenError("cannot construct span '" + stmt.var_name +
                                            "': array element type does not match the span's element type");
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

                llvm::Type* llvm_type = to_llvm_type(stmt.type);
                llvm::AllocaInst* slot = builder_->CreateAlloca(llvm_type, nullptr, stmt.var_name);
                if (stmt.init) {
                    builder_->CreateStore(codegen_expr(*stmt.init), slot);
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
                llvm::Value* cond = codegen_expr(*stmt.condition);
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
                llvm::Value* cond = codegen_expr(*stmt.condition);
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
    // value. Returns the raw LLVM result: if the callee returns a
    // reference, that result is still just the *address* at this point
    // (see to_llvm_type's Reference case) -- it's up to the caller to
    // decide whether to dereference it (codegen_expr, for a value
    // context) or hand the address on as-is (codegen_lvalue, for a
    // reference-returning call used itself as a further borrow source --
    // see resolve_borrow_source_root in movecheck.cppm).
    llvm::Value* codegen_call(const Expr& expr) {
        llvm::Function* callee = module_->getFunction(expr.name);
        if (callee == nullptr) {
            throw CodegenError("call to unknown function '" + expr.name + "'");
        }
        const Function* callee_def = find_function_def(expr.name);
        std::vector<llvm::Value*> args;
        args.reserve(expr.args.size());
        for (size_t i = 0; i < expr.args.size(); i++) {
            bool param_is_reference = callee_def != nullptr && i < callee_def->params.size() &&
                                       callee_def->params[i].type.kind == TypeKind::Reference;
            if (param_is_reference) {
                // Bind the reference parameter to the argument's address
                // rather than passing its value, exactly like a local
                // reference's own VarDecl.
                args.push_back(codegen_lvalue(*expr.args[i]).ptr);
            } else {
                args.push_back(codegen_expr(*expr.args[i]));
            }
        }
        return builder_->CreateCall(callee, args);
    }

    llvm::Value* codegen_expr(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::IntegerLiteral:
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), expr.int_value, /*isSigned=*/true);

            case ExprKind::BoolLiteral:
                return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context_), expr.bool_value ? 1 : 0);

            case ExprKind::Identifier:
            case ExprKind::Subscript: {
                LValue lv = codegen_lvalue(expr);
                return builder_->CreateLoad(to_llvm_type(lv.type), lv.ptr, "loadtmp");
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
                return builder_->CreateLoad(to_llvm_type(lv.type), lv.ptr, "loadtmp");
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
                llvm::Value* operand = codegen_expr(*expr.lhs);
                if (expr.unary_op == UnaryOp::Neg) return builder_->CreateNeg(operand, "negtmp");
                return builder_->CreateNot(operand, "nottmp");
            }

            case ExprKind::Binary:
                return codegen_binary(expr);

            case ExprKind::Call: {
                if (expr.name == "print_int" || expr.name == "print_bool") {
                    return codegen_builtin_print(expr);
                }
                llvm::Value* result = codegen_call(expr);
                const Function* callee_def = find_function_def(expr.name);
                if (callee_def != nullptr && callee_def->return_type.kind == TypeKind::Reference) {
                    // The callee returns a reference -- an address,
                    // lowered identically to a pointer (see
                    // to_llvm_type) -- so using the call's result as a
                    // *value* here means auto-dereferencing it, exactly
                    // like a reference local's own read (see
                    // codegen_lvalue's Identifier case).
                    return builder_->CreateLoad(to_llvm_type(*callee_def->return_type.pointee), result,
                                                 "derefcalltmp");
                }
                return result;
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
        }
        throw CodegenError("unhandled expression kind");
    }

    // `std::make_unique<T>(...)` is a compiler builtin (like std::move),
    // not a real generic function call -- scpp has no `new` expression at
    // all; make_unique is the only sanctioned way to heap-allocate. v0.1
    // supports exactly two forms: zero arguments (zero-initializes T, like
    // a bare `T x;`) or one argument when T is a scalar (int/bool),
    // initializing it to that value. Everything else (multiple arguments,
    // or one argument for a struct/array/pointer T) needs real constructor
    // support that doesn't exist yet.
    llvm::Value* codegen_make_unique(const Expr& expr) {
        llvm::Type* element_type = to_llvm_type(expr.type);
        bool element_is_scalar =
            expr.type.kind == TypeKind::Named && (expr.type.name == "int" || expr.type.name == "bool");

        llvm::Value* initial_value;
        if (expr.args.empty()) {
            initial_value = llvm::Constant::getNullValue(element_type);
        } else if (expr.args.size() == 1 && element_is_scalar) {
            initial_value = codegen_expr(*expr.args[0]);
        } else {
            throw CodegenError(
                "std::make_unique<T>(...) currently only supports zero arguments (zero-initializes "
                "T) or exactly one argument when T is a scalar (int/bool)");
        }

        llvm::Function* malloc_fn = get_or_declare_malloc();
        uint64_t size_in_bytes = module_->getDataLayout().getTypeAllocSize(element_type);
        llvm::Value* size_arg = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), size_in_bytes);
        llvm::Value* heap_ptr = builder_->CreateCall(malloc_fn, {size_arg}, "newptr");
        builder_->CreateStore(initial_value, heap_ptr);
        return heap_ptr;
    }

    // Releases every *currently in-scope* unique_ptr local's owned
    // resource. Called right before each `return` (see StmtKind::Return).
    // `free(NULL)` is a well-defined no-op in C, so unconditionally
    // freeing whatever value is *currently* in each slot is always
    // correct, regardless of whether that local still owns a value, was
    // moved-out (its slot was nulled by Move's codegen), or was never
    // assigned past its own zero-init. Locals whose block scope already
    // closed before reaching this return were already dropped by
    // pop_scope() and removed from `locals_`, so they aren't
    // double-freed here.
    void free_unique_ptr_locals() {
        llvm::Function* free_fn = get_or_declare_free();
        for (const auto& [name, slot] : locals_) {
            if (slot.type.kind != TypeKind::UniquePtr) continue;
            llvm::Value* current = builder_->CreateLoad(to_llvm_type(slot.type), slot.alloca, "droptmp");
            builder_->CreateCall(free_fn, {current});
        }
    }

    void push_scope() { scope_stack_.emplace_back(); }

    // Drops every unique_ptr declared directly in the scope being popped
    // (in reverse declaration order, matching C++/Rust destruction
    // order), then removes all of that scope's names from `locals_` so
    // they're correctly treated as out-of-scope afterward (e.g. a
    // variable declared only inside an `if` branch can no longer be
    // referenced once that branch ends). If the current block already
    // has a terminator (e.g. the scope ended in `return`, which already
    // freed everything via free_unique_ptr_locals), no drop instructions
    // are emitted here -- there both to avoid inserting unreachable code
    // after a terminator and to avoid a double free.
    void pop_scope() {
        std::vector<std::string> names = std::move(scope_stack_.back());
        scope_stack_.pop_back();

        bool already_terminated = builder_->GetInsertBlock()->getTerminator() != nullptr;
        if (!already_terminated) {
            llvm::Function* free_fn = get_or_declare_free();
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                auto slot_it = locals_.find(*it);
                if (slot_it == locals_.end() || slot_it->second.type.kind != TypeKind::UniquePtr) continue;
                llvm::Value* current =
                    builder_->CreateLoad(to_llvm_type(slot_it->second.type), slot_it->second.alloca, "scopedroptmp");
                builder_->CreateCall(free_fn, {current});
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
    void emit_span_bounds_check(llvm::Value* index, llvm::Value* size) {
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

    // Computes the storage location (pointer + scpp Type) of an lvalue
    // expression, i.e. anything that can appear on the left of `=` or be
    // read via a plain load: a variable, or a chain of `.field`/`[index]`
    // off of one. Member-of-call-result (e.g. `f().x` where f returns a
    // struct by value) is intentionally not supported yet since it has no
    // backing storage to take a pointer to; that is deferred to whenever
    // by-value struct temporaries need addressable storage.
    LValue codegen_lvalue(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::Identifier: {
                auto it = locals_.find(expr.name);
                if (it == locals_.end()) {
                    throw CodegenError("use of undeclared variable '" + expr.name + "'");
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
                    throw CodegenError("member access '." + expr.name + "' on a non-struct type");
                }
                const StructInfo& info = structs_.at(base.type.name);
                auto field_it = std::find(info.field_names.begin(), info.field_names.end(), expr.name);
                if (field_it == info.field_names.end()) {
                    throw CodegenError("struct '" + base.type.name + "' has no field '" + expr.name + "'");
                }
                size_t field_index = static_cast<size_t>(field_it - info.field_names.begin());
                llvm::Value* field_ptr =
                    builder_->CreateStructGEP(info.llvm_type, base.ptr, field_index, expr.name);
                return LValue{field_ptr, info.field_types[field_index]};
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
                    // Runtime bounds check (spec ch08: safe regions insert
                    // bounds checks by default) -- unlike a fixed-size
                    // array's subscript below, a span's length is only
                    // known at runtime, so there's no way to reject an
                    // out-of-bounds constant index at compile time.
                    emit_span_bounds_check(index, size);
                    llvm::Value* elem_ptr =
                        builder_->CreateGEP(to_llvm_type(*base.type.pointee), data, {index}, "elemtmp");
                    return LValue{elem_ptr, *base.type.pointee};
                }
                if (base.type.kind != TypeKind::Array) {
                    throw CodegenError("subscript on a non-array type");
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
                // codegen_expr's own Call case.
                const Function* callee_def = find_function_def(expr.name);
                if (callee_def == nullptr || callee_def->return_type.kind != TypeKind::Reference) {
                    throw CodegenError("expression is not assignable");
                }
                return LValue{codegen_call(expr), *callee_def->return_type.pointee};
            }

            case ExprKind::Unary: {
                // Only `*p` (Deref) is addressable; Neg/Not produce a
                // plain value with no backing storage.
                if (expr.unary_op != UnaryOp::Deref) {
                    throw CodegenError("expression is not assignable");
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
                        "dereference ('*') is only supported for std::unique_ptr or a raw pointer");
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
                throw CodegenError("expression is not assignable");
        }
    }

    // `print_int`/`print_bool` are temporary builtins that shell out to
    // libc's `printf` so programs can produce visible output before the
    // language grows a real string type (tracked for M2+). Both return the
    // usual `printf` result (an i32) so they can be used like any other call.
    llvm::Value* codegen_builtin_print(const Expr& expr) {
        if (expr.args.size() != 1) {
            throw CodegenError(expr.name + " expects exactly 1 argument");
        }
        llvm::Function* printf_fn = get_or_declare_printf();
        llvm::Value* arg = codegen_expr(*expr.args[0]);

        llvm::Value* format;
        llvm::Value* printf_arg;
        if (expr.name == "print_int") {
            format = builder_->CreateGlobalString("%d\n", "fmt_int");
            printf_arg = arg;
        } else {
            format = builder_->CreateGlobalString("%s\n", "fmt_bool");
            llvm::Value* true_str = builder_->CreateGlobalString("true", "str_true");
            llvm::Value* false_str = builder_->CreateGlobalString("false", "str_false");
            printf_arg = builder_->CreateSelect(arg, true_str, false_str, "booltmp");
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
            case BinaryOp::Add: return builder_->CreateAdd(lhs, rhs, "addtmp");
            case BinaryOp::Sub: return builder_->CreateSub(lhs, rhs, "subtmp");
            case BinaryOp::Mul: return builder_->CreateMul(lhs, rhs, "multmp");
            case BinaryOp::Div: return builder_->CreateSDiv(lhs, rhs, "divtmp");
            case BinaryOp::Eq: return builder_->CreateICmpEQ(lhs, rhs, "eqtmp");
            case BinaryOp::Ne: return builder_->CreateICmpNE(lhs, rhs, "netmp");
            case BinaryOp::Lt: return builder_->CreateICmpSLT(lhs, rhs, "lttmp");
            case BinaryOp::Gt: return builder_->CreateICmpSGT(lhs, rhs, "gttmp");
            case BinaryOp::Le: return builder_->CreateICmpSLE(lhs, rhs, "letmp");
            case BinaryOp::Ge: return builder_->CreateICmpSGE(lhs, rhs, "getmp");
            default: throw CodegenError("unhandled binary operator");
        }
    }

    llvm::Value* codegen_short_circuit(const Expr& expr) {
        llvm::Function* current_function = builder_->GetInsertBlock()->getParent();
        bool is_and = expr.binary_op == BinaryOp::And;

        llvm::Value* lhs = codegen_expr(*expr.lhs);
        llvm::BasicBlock* rhs_block =
            llvm::BasicBlock::Create(*context_, is_and ? "and.rhs" : "or.rhs", current_function);
        llvm::BasicBlock* merge_block =
            llvm::BasicBlock::Create(*context_, is_and ? "and.end" : "or.end", current_function);
        llvm::BasicBlock* lhs_block = builder_->GetInsertBlock();

        if (is_and) {
            builder_->CreateCondBr(lhs, rhs_block, merge_block);
        } else {
            builder_->CreateCondBr(lhs, merge_block, rhs_block);
        }

        builder_->SetInsertPoint(rhs_block);
        llvm::Value* rhs = codegen_expr(*expr.rhs);
        llvm::BasicBlock* rhs_end_block = builder_->GetInsertBlock();
        builder_->CreateBr(merge_block);

        builder_->SetInsertPoint(merge_block);
        llvm::PHINode* phi = builder_->CreatePHI(llvm::Type::getInt1Ty(*context_), 2, "logictmp");
        phi->addIncoming(lhs, lhs_block);
        phi->addIncoming(rhs, rhs_end_block);
        return phi;
    }
};

} // namespace scpp
