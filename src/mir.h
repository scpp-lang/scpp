#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"

namespace scpp {

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
    // constructor-call syntax (`ClassName name{args};`, ch04 §4.2 /
    // spec §6.1,
    // Stmt::has_ctor_args) -- a raw, non-owning pointer straight at the
    // original AST's own Stmt::ctor_args vector (which outlives this MIR
    // Body, exactly like `expr` above pointing into the same AST), so
    // the dataflow checker can process each argument's own move/borrow
    // effects (e.g. `Outer y{std::move(x)};` marking `x` moved-out) --
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
    // The owning program this MIR came from, so later movecheck passes can
    // answer whole-program questions (e.g. whether a Named type is really a
    // class, and whether that class is copy-constructible) while walking just
    // this function body.
    const Program* program = nullptr;
    // Every local declared `const` (Stmt::is_const, ch05/ch06 -- an
    // immutable local, not a parameter: those don't support `const` yet,
    // see parse_param_type) -- consulted by movecheck's own
    // MirStatementKind::Assign case to reject any reassignment after the
    // single, initializing Assign/Declare a const local's own VarDecl
    // itself lowers to.
    std::unordered_set<std::string> const_locals;
    // ch05 §5.12: every local whose initializer is a lambda with at least
    // one by-reference capture. Such a closure value itself keeps those
    // borrows alive until its own last use / ScopeExit, so movecheck's
    // reference-liveness pass treats reads of the closure local itself as
    // "reference-like" uses too.
    std::unordered_set<std::string> borrow_holding_closure_locals;
    // Copied from the source Function so later passes can enforce
    // compile-time-dependency visibility without storing a raw pointer into
    // Program::functions (which may reallocate while new clones are appended).
    std::string function_owning_module;
    std::string function_source_path;
};

Body build_mir(const Function& fn);

} // namespace scpp
