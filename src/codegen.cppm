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
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::map<std::string, LocalSlot> locals_;
    std::unordered_map<std::string, StructInfo> structs_;

    const StructDef* find_struct_def(const std::string& name) const {
        for (const StructDef& def : program_->structs) {
            if (def.name == name) return &def;
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
                // A unique_ptr is just a possibly-null owning pointer at the
                // ABI/codegen level in v0.1: no destructor/drop codegen
                // exists yet (that's M3's "drop insertion"), so it lowers
                // identically to a raw pointer. The move checker is what
                // enforces the ownership discipline on top of this.
                return llvm::PointerType::getUnqual(*context_);
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

    void declare_function(const Function& fn) {
        std::vector<llvm::Type*> param_types;
        param_types.reserve(fn.params.size());
        for (const Param& param : fn.params) {
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

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", llvm_fn);
        builder_->SetInsertPoint(entry);

        locals_.clear();
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
                for (const auto& s : stmt.statements) {
                    // Once a block has a terminator (return), skip anything
                    // after it: unreachable code shouldn't be lowered.
                    if (builder_->GetInsertBlock()->getTerminator() != nullptr) break;
                    codegen_stmt(*s, current_function);
                }
                return;

            case StmtKind::VarDecl: {
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
                return;
            }

            case StmtKind::Return: {
                if (stmt.expr) {
                    builder_->CreateRet(codegen_expr(*stmt.expr));
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

                builder_->SetInsertPoint(then_block);
                codegen_stmt(*stmt.then_branch, current_function);
                if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
                    builder_->CreateBr(merge_block);
                }

                builder_->SetInsertPoint(else_block);
                if (stmt.else_branch) {
                    codegen_stmt(*stmt.else_branch, current_function);
                }
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

                builder_->SetInsertPoint(body_block);
                codegen_stmt(*stmt.then_branch, current_function);
                if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
                    builder_->CreateBr(cond_block);
                }

                builder_->SetInsertPoint(end_block);
                return;
            }
        }
    }

    llvm::Value* codegen_expr(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::IntegerLiteral:
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), expr.int_value, /*isSigned=*/true);

            case ExprKind::BoolLiteral:
                return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context_), expr.bool_value ? 1 : 0);

            case ExprKind::Identifier:
            case ExprKind::Member:
            case ExprKind::Subscript: {
                LValue lv = codegen_lvalue(expr);
                return builder_->CreateLoad(to_llvm_type(lv.type), lv.ptr, "loadtmp");
            }

            case ExprKind::Unary: {
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
                llvm::Function* callee = module_->getFunction(expr.name);
                if (callee == nullptr) {
                    throw CodegenError("call to unknown function '" + expr.name + "'");
                }
                std::vector<llvm::Value*> args;
                args.reserve(expr.args.size());
                for (const auto& arg : expr.args) {
                    args.push_back(codegen_expr(*arg));
                }
                return builder_->CreateCall(callee, args);
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
        }
        throw CodegenError("unhandled expression kind");
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
                if (base.type.kind != TypeKind::Array) {
                    throw CodegenError("subscript on a non-array type");
                }
                llvm::Value* index = codegen_expr(*expr.rhs);
                llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0);
                llvm::Value* elem_ptr =
                    builder_->CreateGEP(to_llvm_type(base.type), base.ptr, {zero, index}, "elemtmp");
                return LValue{elem_ptr, *base.type.element};
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
