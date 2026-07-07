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
    // Marks that `local` has just gone out of lexical scope (its
    // enclosing block/if-branch/while-body ended). The dataflow analysis
    // resets its tracked state to Bottom here, so a reference to it after
    // this point on this path is correctly rejected -- mirroring
    // codegen's own scope_stack_-driven behavior (see push_scope/
    // pop_scope in codegen.cppm), which stops tracking a block-scoped
    // local at the exact same point.
    ScopeExit,
    // `local` (declared `T&`/`const T&`, or `std::span<T>`/
    // `std::span<const T>` -- see ch03/ch06/M6) is *bound* to the place
    // named by `expr` (a plain Identifier, a `.field`/`[index]` chain, or
    // a call to a reference-returning function -- see ch05.2/ch05.3).
    // Emitted only by a VarDecl whose type is Reference or Span, never by
    // a later plain assignment: unlike every other type, a reference
    // cannot be rebound after its first binding (real C++ has no syntax
    // for that), so any subsequent `local = expr;` is an ordinary Assign
    // that means "write *through* the reference to its current
    // referent", not "rebind it" -- the dataflow analysis tells these
    // apart by which MIR statement kind produced them, not by inspecting
    // `local`'s type at each Assign. A `std::span<T>` local is real-C++
    // reassignable in principle (it's an ordinary value, unlike a
    // reference), but v0.1 conservatively treats it exactly like a
    // reference here too -- bound once at declaration, never rebound --
    // as an explicit, deliberately-scoped-down first slice; lifting that
    // is a follow-up, not a soundness requirement.
    BindReference,
    // Marks lexical entry into / exit from an `unsafe { }` block (ch01
    // §1.3) -- emitted as a bracketing pair around a Block statement's
    // lowered statements exactly when its `is_unsafe` flag is set (see
    // MirBuilder::lower_stmt's Block case). `local`/`expr` are unused
    // (left default). The move checker keeps a simple nesting counter
    // incremented/decremented by these (see DataflowState::unsafe_depth
    // in movecheck.cppm) to know whether it's currently licensed to
    // relax the specific checks ch05.5 lists -- this is a purely lexical/
    // structural fact at each program point, not itself flow-sensitive
    // (every branch of an `if`/`while` always closes out any `unsafe { }`
    // blocks it opened before reaching a successor), so it doesn't need
    // real join semantics the way move/borrow state does.
    UnsafeEnter,
    UnsafeExit,
};

struct MirStatement {
    MirStatementKind kind;
    std::string local;          // Declare / Assign (target) / Drop / ScopeExit / BindReference (the reference)
    const Expr* expr = nullptr; // Assign (rhs) / Eval / BindReference (the place being borrowed)
    Type type;                  // Declare: the declared type; BindReference: the reference's own type
    // The originating Stmt's position (see SourceLocation, ast.cppm) --
    // used only so movecheck's diagnostics (DataflowState::current_loc)
    // can point at the right source line, never consulted by any actual
    // dataflow check.
    SourceLocation loc;
    // Declare only: non-null exactly when the originating VarDecl used
    // constructor-call syntax (`ClassName name(args);`, ch04 §4.2,
    // Stmt::has_ctor_args) -- a raw, non-owning pointer straight at the
    // original AST's own Stmt::ctor_args vector (which outlives this MIR
    // Body, exactly like `expr` above pointing into the same AST), so
    // the dataflow checker can process each argument's own move/borrow
    // effects (e.g. `Outer y(std::move(x));` marking `x` moved-out) --
    // previously entirely invisible to movecheck, which only ever saw a
    // bare Declare with no way to reach the arguments at all. Placed
    // last (rather than next to `type` above, which might read more
    // naturally) so every existing positional aggregate-init call site
    // that predates this field stays valid unchanged.
    const std::vector<ExprPtr>* ctor_args = nullptr;
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
    SourceLocation loc;                 // the originating Stmt's position, see MirStatement::loc
};

struct BasicBlock {
    std::vector<MirStatement> statements;
    Terminator terminator;
};

// The MIR for a single function: a CFG of basic blocks, plus the declared
// type of every tracked local (parameters and every VarDecl encountered,
// in declaration order). `local_types`/`locals_in_order` span the whole
// function regardless of lexical scope -- a name stays "known" for type-
// lookup purposes (so the checker can still describe a bad read with a
// type-aware message) even after its scope has ended. *Liveness* is what's
// actually scoped: each local's tracked dataflow state is reset by a
// `ScopeExit` statement at the end of its enclosing block/if-branch/
// while-body, mirroring codegen's own scope_stack_ (see push_scope/
// pop_scope in codegen.cppm).
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
    // One frame per lexically-enclosing block/if-branch/while-body,
    // holding the names declared directly within it -- mirrors codegen's
    // scope_stack_. Parameters are declared before any frame is pushed
    // (see build()), so they're never captured here: they live for the
    // whole function, same as in codegen.
    std::vector<std::vector<std::string>> scope_stack_;

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
            current().statements.push_back(MirStatement{MirStatementKind::ScopeExit, *it, nullptr, Type{}});
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
                    // ch04 §4.2: `ClassName name(args);` -- see
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
                lower_stmt(*stmt.then_branch);
                pop_scope();
                if (!current_has_terminator()) {
                    current().terminator = Terminator{TerminatorKind::Goto, cond_block, 0, 0, nullptr, nullptr, stmt.loc};
                }

                current_block_ = end_block;
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
