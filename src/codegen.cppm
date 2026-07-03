module;

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
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

// Lowers the M1 AST subset (scalars + locals + control flow + functions,
// no `safe` checks yet) directly to LLVM IR.
class Codegen {
public:
    explicit Codegen(const std::string& module_name)
        : context_(std::make_unique<llvm::LLVMContext>()),
          module_(std::make_unique<llvm::Module>(module_name, *context_)),
          builder_(std::make_unique<llvm::IRBuilder<>>(*context_)) {}

    llvm::Module& generate(const Program& program) {
        // Declare every function signature up front so calls to
        // not-yet-defined functions resolve correctly.
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
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::map<std::string, llvm::AllocaInst*> locals_;

    llvm::Type* to_llvm_type(const std::string& type_name) {
        if (type_name == "int") return llvm::Type::getInt32Ty(*context_);
        if (type_name == "bool") return llvm::Type::getInt1Ty(*context_);
        throw CodegenError("unsupported type '" + type_name + "'");
    }

    void declare_function(const Function& fn) {
        std::vector<llvm::Type*> param_types;
        param_types.reserve(fn.params.size());
        for (const Param& param : fn.params) {
            param_types.push_back(to_llvm_type(param.type_name));
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
            locals_[param.name] = slot;
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
                llvm::Type* type = to_llvm_type(stmt.type_name);
                llvm::AllocaInst* slot = builder_->CreateAlloca(type, nullptr, stmt.var_name);
                if (stmt.init) {
                    builder_->CreateStore(codegen_expr(*stmt.init), slot);
                }
                locals_[stmt.var_name] = slot;
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

            case ExprKind::Identifier: {
                llvm::AllocaInst* slot = lookup(expr.name);
                return builder_->CreateLoad(slot->getAllocatedType(), slot, expr.name);
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
        }
        throw CodegenError("unhandled expression kind");
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
            if (expr.lhs->kind != ExprKind::Identifier) {
                throw CodegenError("left-hand side of assignment must be a variable");
            }
            llvm::Value* value = codegen_expr(*expr.rhs);
            llvm::AllocaInst* slot = lookup(expr.lhs->name);
            builder_->CreateStore(value, slot);
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

    llvm::AllocaInst* lookup(const std::string& name) {
        auto it = locals_.find(name);
        if (it == locals_.end()) {
            throw CodegenError("use of undeclared variable '" + name + "'");
        }
        return it->second;
    }
};

} // namespace scpp
