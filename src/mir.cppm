module;

#include <string>
#include <unordered_map>
#include <vector>

export module scpp.mir;

import scpp.ast;

export namespace scpp {

// A place is a storage location a MIR statement can read from or write to.
// For this iteration, places are whole local variables only (no field/
// index projections): struct and array locals are always fully
// zero-initialized at declaration (see codegen's zero-init handling), so
// sub-object initialization tracking isn't needed for soundness yet.
struct Place {
    std::string local;
};

enum class MirStatementKind {
    // `local` was just declared with no initializer. Whether it starts
    // Initialized (struct/array/unique_ptr -- all zero-initialized by
    // codegen) or Uninitialized (scalars) depends on its type; the
    // dataflow analysis decides that, not this builder.
    Declare,
    // Evaluates `expr` and assigns the result to `local` (a VarDecl with
    // an initializer, or a plain `x = expr;` assignment).
    Assign,
    // Evaluates `expr` and discards the result (e.g. a bare call
    // statement, or an assignment into a member/subscript place rather
    // than a whole local).
    Eval,
    // Marks that `local`'s owned resource (if any) should be released
    // here. Inserted at each function-exit point for unique_ptr locals.
    // No-op in codegen until heap-allocated owning types exist (tracked
    // as a follow-up to M2's unique_ptr); the analysis and placement
    // infrastructure is what this milestone establishes.
    Drop,
};

struct MirStatement {
    MirStatementKind kind;
    std::string local;          // Declare / Assign (target) / Drop
    const Expr* expr = nullptr; // Assign (rhs) / Eval
    Type type;                  // Declare: the declared type
};

enum class TerminatorKind {
    None, // not yet assigned (builder bug if this survives into a finished Body)
    Goto,
    Branch,
    Return,
    Unreachable, // e.g. after two branches that both already returned
};

struct Terminator {
    TerminatorKind kind = TerminatorKind::None;
    size_t target = 0;                  // Goto
    size_t true_target = 0;             // Branch (condition is true)
    size_t false_target = 0;            // Branch (condition is false)
    const Expr* condition = nullptr;    // Branch
    const Expr* return_value = nullptr; // Return (nullable)
};

struct BasicBlock {
    std::vector<MirStatement> statements;
    Terminator terminator;
};

// The MIR for a single function: a CFG of basic blocks, plus the declared
// type of every tracked local (parameters and every VarDecl encountered,
// in declaration order -- matching codegen's existing flat, non-lexically-
// scoped locals_ map, so a variable declared inside one branch is still
// considered part of the same tracked universe as the rest of the
// function).
struct Body {
    std::vector<BasicBlock> blocks;
    std::unordered_map<std::string, Type> local_types;
    std::vector<std::string> locals_in_order;
};

Body build_mir(const Function& fn);

} // namespace scpp

namespace scpp {
namespace {

class MirBuilder {
public:
    explicit MirBuilder(const Function& fn) : fn_(fn) {}

    Body build() {
        for (const Param& param : fn_.params) {
            declare_local(param.name, param.type);
        }
        current_block_ = new_block();
        lower_stmt(*fn_.body);
        insert_drops_before_returns();
        return std::move(body_);
    }

private:
    const Function& fn_;
    Body body_;
    size_t current_block_ = 0;

    void declare_local(const std::string& name, const Type& type) {
        if (!body_.local_types.contains(name)) {
            body_.locals_in_order.push_back(name);
        }
        body_.local_types[name] = type;
    }

    size_t new_block() {
        body_.blocks.push_back(BasicBlock{});
        return body_.blocks.size() - 1;
    }

    BasicBlock& current() { return body_.blocks[current_block_]; }

    [[nodiscard]] bool current_has_terminator() const {
        return body_.blocks[current_block_].terminator.kind != TerminatorKind::None;
    }

    void lower_stmt(const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Block:
                for (const auto& s : stmt.statements) {
                    // Dead code after a return/unreachable terminator
                    // isn't lowered, matching codegen's own behavior.
                    if (current_has_terminator()) break;
                    lower_stmt(*s);
                }
                return;

            case StmtKind::VarDecl: {
                declare_local(stmt.var_name, stmt.type);
                if (stmt.init) {
                    current().statements.push_back(
                        MirStatement{MirStatementKind::Assign, stmt.var_name, stmt.init.get(), stmt.type});
                } else {
                    current().statements.push_back(
                        MirStatement{MirStatementKind::Declare, stmt.var_name, nullptr, stmt.type});
                }
                return;
            }

            case StmtKind::Return: {
                current().terminator =
                    Terminator{TerminatorKind::Return, 0, 0, 0, nullptr, stmt.expr ? stmt.expr.get() : nullptr};
                return;
            }

            case StmtKind::ExprStmt: {
                const Expr& e = *stmt.expr;
                // A plain `name = expr;` is lowered as a proper Assign so
                // the dataflow analysis sees exactly which local becomes
                // (re)initialized; anything else (calls, member/subscript
                // assignment, ...) is an opaque Eval -- sound because
                // struct/array locals are always Initialized as a whole
                // from the moment they're declared (zero-init), so a
                // write to `p.x` never needs to change `p`'s own tracked
                // state.
                if (e.kind == ExprKind::Binary && e.binary_op == BinaryOp::Assign &&
                    e.lhs->kind == ExprKind::Identifier) {
                    current().statements.push_back(
                        MirStatement{MirStatementKind::Assign, e.lhs->name, e.rhs.get(), Type{}});
                } else {
                    current().statements.push_back(MirStatement{MirStatementKind::Eval, "", &e, Type{}});
                }
                return;
            }

            case StmtKind::If: {
                size_t branch_block = current_block_;
                size_t then_block = new_block();
                size_t else_block = new_block();
                size_t merge_block = new_block();

                body_.blocks[branch_block].terminator =
                    Terminator{TerminatorKind::Branch, 0, then_block, else_block, stmt.condition.get(), nullptr};

                current_block_ = then_block;
                lower_stmt(*stmt.then_branch);
                if (!current_has_terminator()) {
                    current().terminator = Terminator{TerminatorKind::Goto, merge_block, 0, 0, nullptr, nullptr};
                }

                current_block_ = else_block;
                if (stmt.else_branch) lower_stmt(*stmt.else_branch);
                if (!current_has_terminator()) {
                    current().terminator = Terminator{TerminatorKind::Goto, merge_block, 0, 0, nullptr, nullptr};
                }

                current_block_ = merge_block;
                return;
            }

            case StmtKind::While: {
                size_t preheader = current_block_;
                size_t cond_block = new_block();
                size_t body_block = new_block();
                size_t end_block = new_block();

                body_.blocks[preheader].terminator = Terminator{TerminatorKind::Goto, cond_block, 0, 0, nullptr, nullptr};
                body_.blocks[cond_block].terminator =
                    Terminator{TerminatorKind::Branch, 0, body_block, end_block, stmt.condition.get(), nullptr};

                current_block_ = body_block;
                lower_stmt(*stmt.then_branch);
                if (!current_has_terminator()) {
                    current().terminator = Terminator{TerminatorKind::Goto, cond_block, 0, 0, nullptr, nullptr};
                }

                current_block_ = end_block;
                return;
            }
        }
    }

    // Inserts `Drop` statements for every unique_ptr local (in reverse
    // declaration order) right before each `Return` terminator. Given our
    // flat, non-lexically-scoped locals (matching codegen), every
    // unique_ptr local in the function is considered part of the same
    // scope; whether it actually still owns a value by the time codegen
    // runs is exactly what the move/init dataflow analysis determines.
    void insert_drops_before_returns() {
        std::vector<std::string> unique_ptr_locals;
        for (const std::string& name : body_.locals_in_order) {
            if (body_.local_types.at(name).kind == TypeKind::UniquePtr) {
                unique_ptr_locals.push_back(name);
            }
        }
        if (unique_ptr_locals.empty()) return;

        for (BasicBlock& block : body_.blocks) {
            if (block.terminator.kind != TerminatorKind::Return) continue;
            for (auto it = unique_ptr_locals.rbegin(); it != unique_ptr_locals.rend(); ++it) {
                block.statements.push_back(MirStatement{MirStatementKind::Drop, *it, nullptr, Type{}});
            }
        }
    }
};

} // namespace

Body build_mir(const Function& fn) {
    MirBuilder builder(fn);
    return builder.build();
}

} // namespace scpp
