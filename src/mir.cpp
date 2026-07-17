#include "mir.h"

namespace scpp {
namespace {

class MirBuilder {
public:
    explicit MirBuilder(const Function& fn) : fn_(fn) {}

    Body build() {
        body_.function_owning_module = fn_.owning_module;
        body_.function_source_path = fn_.loc.source_path_text();
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
    // One frame per lexically-enclosing block/if-branch/while-body,
    // holding the names declared directly within it -- mirrors codegen's
    // scope_stack_. Parameters are declared before any frame is pushed
    // (see build()), so they're never captured here: they live for the
    // whole function, same as in codegen.
    std::vector<std::vector<std::string>> scope_stack_;
    struct LoopFrame {
        size_t cond_block;
        size_t end_block;
        size_t scope_depth;
    };
    std::vector<LoopFrame> loop_stack_;

    void declare_local(const std::string& name, const Type& type) {
        if (!body_.local_types.contains(name)) {
            body_.locals_in_order.push_back(name);
        }
        body_.local_types[name] = type;
        if (!scope_stack_.empty()) {
            scope_stack_.back().push_back(name);
        }
    }

    void push_scope() { scope_stack_.emplace_back(); }

    // Emits a ScopeExit statement (in reverse declaration order, matching
    // codegen's drop order) for every name declared directly in the scope
    // being popped -- unless the current block already ended in a
    // terminator (e.g. a `return` already exited this scope; no further
    // statements can be appended to that block anyway), matching
    // codegen's identical pop_scope() guard.
    void pop_scope() {
        std::vector<std::string> names = std::move(scope_stack_.back());
        scope_stack_.pop_back();
        if (current_has_terminator()) return;
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            current().statements.push_back(MirStatement{MirStatementKind::ScopeExit, *it, nullptr, Type{},
                                                        SourceLocation{}});
        }
    }

    void emit_scope_exits_to_depth(size_t target_depth) {
        for (size_t depth = scope_stack_.size(); depth > target_depth; depth--) {
            const std::vector<std::string>& names = scope_stack_[depth - 1];
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                current().statements.push_back(MirStatement{MirStatementKind::ScopeExit, *it, nullptr, Type{},
                                                            SourceLocation{}});
            }
        }
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
                push_scope();
                if (stmt.is_unsafe) {
                    current().statements.push_back(
                        MirStatement{MirStatementKind::UnsafeEnter, "", nullptr, Type{}, stmt.loc});
                }
                for (const auto& s : stmt.statements) {
                    // Dead code after a return/unreachable terminator
                    // isn't lowered, matching codegen's own behavior.
                    if (current_has_terminator()) break;
                    lower_stmt(*s);
                }
                // Guarded exactly like pop_scope()'s own ScopeExit
                // emission below: a `return` inside the unsafe block may
                // have already left `current()` terminated, in which case
                // appending anything more to it would be dead code after
                // its terminator.
                if (stmt.is_unsafe && !current_has_terminator()) {
                    current().statements.push_back(
                        MirStatement{MirStatementKind::UnsafeExit, "", nullptr, Type{}, stmt.loc});
                }
                pop_scope();
                return;

            case StmtKind::VarDecl: {
                declare_local(stmt.var_name, stmt.type);
                if (stmt.is_const || stmt.is_constexpr) body_.const_locals.insert(stmt.var_name);
                if (stmt.init && stmt.init->kind == ExprKind::Lambda) {
                    for (const LambdaCapture& capture : stmt.init->lambda_captures) {
                        if (capture.by_reference) {
                            body_.borrow_holding_closure_locals.insert(stmt.var_name);
                            break;
                        }
                    }
                }
                if (stmt.type.kind == TypeKind::Reference || stmt.type.kind == TypeKind::Span) {
                    // `expr` is null when the source omitted an
                    // initializer (`int& r;` / `std::span<int> s;`,
                    // illegal since both must be bound at declaration) --
                    // left for movecheck to reject with a clear
                    // diagnostic rather than validated here, keeping this
                    // builder a straightforward, non-throwing translation.
                    current().statements.push_back(MirStatement{
                        MirStatementKind::BindReference, stmt.var_name, stmt.init.get(), stmt.type, stmt.loc});
                } else if (stmt.init) {
                    current().statements.push_back(
                        MirStatement{MirStatementKind::Assign, stmt.var_name, stmt.init.get(), stmt.type, stmt.loc});
                } else if (stmt.has_ctor_args) {
                    // ch04 §4.2 / spec §6.1: `ClassName name{args};` --
                    // see
                    // MirStatement::ctor_args' own comment for why this
                    // needs to carry the argument list (rather than
                    // falling into the plain, argument-blind Declare case
                    // just below).
                    MirStatement mir_stmt{MirStatementKind::Declare, stmt.var_name, nullptr, stmt.type, stmt.loc};
                    mir_stmt.ctor_args = &stmt.ctor_args;
                    current().statements.push_back(std::move(mir_stmt));
                } else {
                    current().statements.push_back(
                        MirStatement{MirStatementKind::Declare, stmt.var_name, nullptr, stmt.type, stmt.loc});
                }
                return;
            }

            case StmtKind::Return: {
                current().terminator = Terminator{TerminatorKind::Return, 0, 0, 0, nullptr,
                                                   stmt.expr ? stmt.expr.get() : nullptr, stmt.loc};
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
                        MirStatement{MirStatementKind::Assign, e.lhs->name, e.rhs.get(), Type{}, stmt.loc});
                } else {
                    current().statements.push_back(MirStatement{MirStatementKind::Eval, "", &e, Type{}, stmt.loc});
                }
                return;
            }

            case StmtKind::If: {
                size_t branch_block = current_block_;
                size_t then_block = new_block();
                size_t else_block = new_block();
                size_t merge_block = new_block();

                body_.blocks[branch_block].terminator = Terminator{
                    TerminatorKind::Branch, 0, then_block, else_block, stmt.condition.get(), nullptr, stmt.loc};

                current_block_ = then_block;
                push_scope();
                lower_stmt(*stmt.then_branch);
                pop_scope();
                if (!current_has_terminator()) {
                    current().terminator =
                        Terminator{TerminatorKind::Goto, merge_block, 0, 0, nullptr, nullptr, stmt.loc};
                }

                current_block_ = else_block;
                push_scope();
                if (stmt.else_branch) lower_stmt(*stmt.else_branch);
                pop_scope();
                if (!current_has_terminator()) {
                    current().terminator =
                        Terminator{TerminatorKind::Goto, merge_block, 0, 0, nullptr, nullptr, stmt.loc};
                }

                current_block_ = merge_block;
                return;
            }

            case StmtKind::While: {
                size_t preheader = current_block_;
                size_t cond_block = new_block();
                size_t body_block = new_block();
                size_t end_block = new_block();

                body_.blocks[preheader].terminator =
                    Terminator{TerminatorKind::Goto, cond_block, 0, 0, nullptr, nullptr, stmt.loc};
                body_.blocks[cond_block].terminator = Terminator{
                    TerminatorKind::Branch, 0, body_block, end_block, stmt.condition.get(), nullptr, stmt.loc};

                current_block_ = body_block;
                push_scope();
                loop_stack_.push_back(LoopFrame{cond_block, end_block, scope_stack_.size()});
                lower_stmt(*stmt.then_branch);
                pop_scope();
                loop_stack_.pop_back();
                if (!current_has_terminator()) {
                    current().terminator = Terminator{TerminatorKind::Goto, cond_block, 0, 0, nullptr, nullptr, stmt.loc};
                }

                current_block_ = end_block;
                return;
            }

            case StmtKind::Break: {
                if (loop_stack_.empty()) return;
                emit_scope_exits_to_depth(loop_stack_.back().scope_depth);
                current().terminator =
                    Terminator{TerminatorKind::Goto, loop_stack_.back().end_block, 0, 0, nullptr, nullptr, stmt.loc};
                return;
            }

            case StmtKind::Continue: {
                if (loop_stack_.empty()) return;
                emit_scope_exits_to_depth(loop_stack_.back().scope_depth);
                current().terminator =
                    Terminator{TerminatorKind::Goto, loop_stack_.back().cond_block, 0, 0, nullptr, nullptr, stmt.loc};
                return;
            }
        }
    }

    // Inserts `Drop` statements for every unique_ptr local declared
    // anywhere in the function (in reverse declaration order) right
    // before each `Return` terminator. This is deliberately coarser than
    // ScopeExit: codegen doesn't consume MIR yet (it does its own
    // per-scope free()s directly off the AST, see push_scope/pop_scope in
    // codegen.cppm), so these Drop markers are inert placeholders for
    // whenever codegen switches to consuming MIR -- at which point this
    // will need to only drop locals still in scope at the return, not
    // every one ever declared. Harmless for now: Drop has no dataflow
    // effect (see apply_statement in movecheck.cppm), so a marker for an
    // already-out-of-scope local is simply never acted on by anything.
    void insert_drops_before_returns() {
        std::vector<std::string> unique_ptr_locals;
        for (const std::string& name : body_.locals_in_order) {
            const Type& type = body_.local_types.at(name);
            if (type.kind == TypeKind::Named &&
                (type.name == "std::unique_ptr" || type.name.rfind("std::unique_ptr.", 0) == 0)) {
                unique_ptr_locals.push_back(name);
            }
        }
        if (unique_ptr_locals.empty()) return;

        for (BasicBlock& block : body_.blocks) {
            if (block.terminator.kind != TerminatorKind::Return) continue;
            for (auto it = unique_ptr_locals.rbegin(); it != unique_ptr_locals.rend(); ++it) {
                block.statements.push_back(MirStatement{MirStatementKind::Drop, *it, nullptr, Type{},
                                                        SourceLocation{}});
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
