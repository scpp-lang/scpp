module;

export module scpp.mir;

import std;
import scpp.ast;

export namespace scpp {

// A local variable's identity within a single function body: an index
// into Body::local_decls. This is the key the whole move checker uses to
// talk about a local -- deliberately *not* its source name, because a
// name is not unique within a function (two sibling scopes may each
// declare `r`, and an inner scope may shadow an outer one). Keying by
// name conflated all of those into one entity, which let a borrow of one
// go untracked because a namesake declared elsewhere had a different
// type.
//
// A distinct enum type rather than a bare std::size_t alias so it cannot
// be silently confused with the other size_t-shaped index in this file,
// a basic-block number.
enum class LocalId : std::size_t {};

[[nodiscard]] inline std::size_t local_index(LocalId id) { return static_cast<std::size_t>(id); }

// Whether this expression is a name reference that could denote a local:
// a plain identifier, or a bare `f(...)` whose callee is written as a
// name (which may be a callable local -- see monomorphize's bare-call
// redirect). Anything else names a field, a qualified global, or nothing
// at all.
//
// A leading `::` (Expr::explicit_global_qualification) forces lookup from
// the global namespace, so `::x` never names a local however many locals
// are spelled `x`.
[[nodiscard]] inline bool names_a_local_use(const Expr& expr) {
    if (expr.explicit_global_qualification) return false;
    if (expr.kind == ExprKind::Identifier) return true;
    return expr.kind == ExprKind::Call && expr.lhs == nullptr;
}

// Whether name resolution bound this expression to a local, and which
// one. See Expr::resolved_local (ast.cppm) for the `+ 1` encoding these
// hide; nothing outside these two helpers should do that arithmetic.
[[nodiscard]] inline bool has_resolved_local(const Expr& expr) {
    // Guarding on the shape matters as much as the zero check:
    // monomorphize rewrites some Identifier nodes *in place* into Member
    // accesses (`x` -> `this.x` for a captured or implicit-member name),
    // reusing the same node. Such a node keeps whatever resolution it had
    // as an Identifier, so consulting it without this guard would report a
    // member access as a use of a local.
    return names_a_local_use(expr) && expr.resolved_local != 0;
}

[[nodiscard]] inline LocalId resolved_local_of(const Expr& expr) {
    return static_cast<LocalId>(expr.resolved_local - 1);
}

[[nodiscard]] inline bool has_declared_local(const Stmt& stmt) { return stmt.declared_local != 0; }

[[nodiscard]] inline LocalId declared_local_of(const Stmt& stmt) {
    return static_cast<LocalId>(stmt.declared_local - 1);
}

[[nodiscard]] inline bool has_param_local(const Param& param) { return param.resolved_local != 0; }

[[nodiscard]] inline LocalId param_local(const Param& param) {
    return static_cast<LocalId>(param.resolved_local - 1);
}

[[nodiscard]] inline bool has_resolved_local(const LambdaCapture& capture) { return capture.resolved_local != 0; }

[[nodiscard]] inline LocalId resolved_local_of(const LambdaCapture& capture) {
    return static_cast<LocalId>(capture.resolved_local - 1);
}

// Everything the checker needs to know about one declaration. One of
// these per *declaration*, not per name.
// Marks a *synthesized* Identifier node as naming `id`. Movecheck builds
// a handful of such nodes on the fly (a lambda capture rendered as an
// identifier, an implicit `this` receiver) purely to reuse the ordinary
// place-resolution paths. They were never part of the tree
// resolve_locals walked, so nothing else can have bound them, and an
// unbound node would silently read as "not a local" -- which skips
// checks rather than failing loudly.
inline void set_resolved_local(Expr& expr, LocalId id) { expr.resolved_local = local_index(id) + 1; }

struct LocalDecl {
    Type type;
    // The name this local was written with in the source. Diagnostics
    // must print this -- a LocalId is an internal index and must never
    // reach a user-visible message.
    std::string source_name;
    // Where it was declared, so a diagnostic can distinguish two
    // same-named locals ("the `r` declared at line 12").
    SourceLocation decl_loc;
    // Block-scope `static`: participates in ordinary name lookup and
    // borrow tracking like any other local, but its storage duration is
    // program-long rather than stack-scoped.
    bool is_static_lifetime = false;
    // Declared `const`/`constexpr` (Stmt::is_const, ch05/ch06) -- an
    // immutable local, not a parameter: those don't support `const` yet
    // (see parse_param_type). Consulted by movecheck's Assign case to
    // reject reassignment after the single initializing Assign/Declare a
    // const local's own VarDecl lowers to.
    bool is_const = false;
    // ch05 §5.12: initialized with a lambda that has at least one
    // by-reference capture. Such a closure keeps those borrows alive
    // until its own last use / ScopeExit, so reference-liveness treats
    // reads of the closure local itself as reference-like uses.
    bool is_borrow_holding_closure = false;
};

// A place is a storage location a MIR statement can read from or write to.
// For this iteration, places are whole local variables only (no field/
// index projections): struct and array locals are always fully
// zero-initialized at declaration (see codegen's zero-init handling), so
// sub-object initialization tracking isn't needed for soundness yet.
struct Place {
    LocalId local{};
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
    // Declare / Assign (target) / Drop / ScopeExit / BindReference (the
    // reference). Unset (left default) for Eval/UnsafeEnter/UnsafeExit,
    // and for an Assign whose target expression did not resolve to a
    // local; `has_local` says which.
    LocalId local{};
    bool has_local = false;
    // Assign only: the target place as it was written. Set even when the
    // target is *not* a local (a global variable), which is exactly the
    // case `local`/`has_local` cannot describe -- a global has no
    // declaration in this body to be keyed by, but the checks that apply
    // to assigning one still need its name.
    const Expr* target = nullptr;
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

struct SwitchTarget {
    std::size_t block = 0;
};

enum class TerminatorKind {
    None, // not yet assigned (builder bug if this survives into a finished Body)
    Goto,
    Branch,
    Switch,
    Return,
    Unreachable, // e.g. after two branches that both already returned
};

struct Terminator {
    TerminatorKind kind = TerminatorKind::None;
    std::size_t target = 0;                  // Goto
    std::size_t true_target = 0;             // Branch (condition is true)
    std::size_t false_target = 0;            // Branch (condition is false)
    const Expr* condition = nullptr;         // Branch / Switch
    std::vector<SwitchTarget> switch_targets; // Switch
    const Expr* return_value = nullptr; // Return (nullable)
    SourceLocation loc;                 // the originating Stmt's position, see MirStatement::loc
};

struct BasicBlock {
    std::vector<MirStatement> statements;
    Terminator terminator;
};

// The MIR for a single function: a CFG of basic blocks, plus one entry
// per local *declaration* (parameters first, then every VarDecl, in
// declaration order).
//
// `local_decls` is flat and spans the whole function, and that is
// correct: it is keyed by declaration identity (LocalId), which is
// unique, not by source name, which is not. A name stays "known" for
// type-lookup purposes even after its scope has ended -- so the checker
// can still describe a bad read with a type-aware message -- and now
// reports *its own* type rather than that of a later namesake.
//
// Which declaration a given use refers to is decided once, by
// resolve_locals, and recorded on the AST node (Expr::resolved_local);
// this file never resolves a name at lookup time. That split is
// deliberate: lexical scope is a property of the syntax, and by the time
// the checker walks this CFG there is no well-defined "current scope" to
// consult -- the move checker is a worklist fixed-point that revisits
// blocks in convergence order, not source order.
//
// *Liveness* is still what's scoped: each local's tracked dataflow state
// is reset by a `ScopeExit` statement at the end of its enclosing
// block/if-branch/while-body, mirroring codegen's own scope_stack_ (see
// push_scope/pop_scope in codegen.cppm). Because those statements now
// name a LocalId rather than a name, an inner shadow going out of scope
// no longer resets the outer local it shadowed.
struct Body {
    std::vector<BasicBlock> blocks;
    StmtPtr owned_body;
    // Indexed by LocalId.
    std::vector<LocalDecl> local_decls;
    // The owning program this MIR came from, so later movecheck passes can
    // answer whole-program questions (e.g. whether a Named type is really a
    // class, and whether that class is copy-constructible) while walking just
    // this function body.
    const Program* program = nullptr;
    // Copied from the source Function so later passes can enforce
    // compile-time-dependency visibility without storing a raw pointer into
    // Program::functions (which may reallocate while new clones are appended).
    std::string function_owning_module;
    std::string function_visibility_module;
    std::string function_member_owner_class;
    // Mirrors Function::access_context_class -- see its own doc comment
    // (ast.cppm) for why this is kept separate from
    // function_member_owner_class.
    std::string function_access_context_class;
    std::string function_source_path;
    std::vector<std::string> function_namespace_path;

    [[nodiscard]] const LocalDecl& decl(LocalId id) const { return local_decls[local_index(id)]; }
    [[nodiscard]] LocalDecl& decl(LocalId id) { return local_decls[local_index(id)]; }
    [[nodiscard]] const Type& type_of(LocalId id) const { return local_decls[local_index(id)].type; }
    // The name to print for this local in a diagnostic.
    [[nodiscard]] const std::string& name_of(LocalId id) const { return local_decls[local_index(id)].source_name; }
    [[nodiscard]] bool is_valid_local(LocalId id) const { return local_index(id) < local_decls.size(); }

    // The local a use refers to, or nullopt when it isn't a use of a
    // local at all (a global, a function name, an enum constant, or an
    // expression that isn't an Identifier).
    [[nodiscard]] std::optional<LocalId> local_of(const Expr& expr) const {
        if (!has_resolved_local(expr)) return std::nullopt;
        LocalId id = resolved_local_of(expr);
        if (!is_valid_local(id)) return std::nullopt;
        return id;
    }

    [[nodiscard]] std::optional<LocalId> local_of(const LambdaCapture& capture) const {
        if (!has_resolved_local(capture)) return std::nullopt;
        LocalId id = resolved_local_of(capture);
        if (!is_valid_local(id)) return std::nullopt;
        return id;
    }

    // The declared type of the local this expression names, or nullptr
    // when it names something that isn't a local at all (a global, a
    // function, an enum constant, a field access). The overwhelmingly
    // common shape at every consumer, and the one that used to be
    // `local_types.find(expr.name)` -- which found *a* declaration with
    // that spelling rather than *this* expression's declaration.
    [[nodiscard]] const Type* type_if_local(const Expr& expr) const {
        std::optional<LocalId> id = local_of(expr);
        return id.has_value() ? &type_of(*id) : nullptr;
    }

    // The implicit `this` parameter of a member function, if any. Kept as
    // a lookup of its own so no caller has to search by name: `this` is
    // always the first parameter and can never be shadowed or redeclared.
    [[nodiscard]] std::optional<LocalId> this_local() const {
        if (local_decls.empty() || local_decls[0].source_name != "this") return std::nullopt;
        return static_cast<LocalId>(0);
    }
};

// Assigns every local declaration in `fn` its identity, and binds every
// identifier use in `fn`'s body to the declaration it refers to under
// ordinary lexical scoping. Idempotent and deterministic: ids are handed
// out in declaration order (parameters first), so re-running it -- or
// running it on a deep clone -- reproduces exactly the same numbering,
// which is what lets a Body built from one copy of a body describe uses
// found in another.
void resolve_locals(Function& fn);

// The same walk, for a body that has no Function of its own yet: binds
// `body`'s uses against `params` and returns the declaration table they
// index into. `resolve_locals` is this plus the Function unwrapping.
[[nodiscard]] std::vector<LocalDecl> resolve_locals_in(std::vector<Param>& params, Stmt& body);

// Runs resolve_locals over every function in `program`. Every later pass
// assumes a use already knows its declaration, so this must run before
// any of them and again after any pass that synthesizes new functions
// (monomorphization does both). Re-running is free of consequence:
// resolution is idempotent and depends only on the body's own syntax.
void resolve_program_locals(Program& program);

Body build_mir(const Function& fn);

} // namespace scpp

namespace scpp {
namespace {

// The single authority on which declaration a name refers to. Binds every name-use in a function body to the declaration it actually
// refers to, following ordinary lexical scope, and builds the matching
// table of declarations. Both jobs are done by this one walk on purpose:
// the id a use is bound to *is* the index of the LocalDecl created for
// its declaration, so a second, separately-maintained walk could drift
// out of step with this one and silently mis-index the table.
//
// The walk covers the whole body -- including code the CFG lowering
// skips as unreachable -- so that an id can never index past the end of
// local_decls whatever the control flow looks like.
class LocalResolver {
public:
    // `params` is non-const because a parameter is a declaration and so
    // records its own id, exactly as a VarDecl does (Param::resolved_local).
    //
    // `body` may be null: a defaulted or declaration-only function has no
    // body but still has parameters, and codegen still gives those
    // storage, so they are numbered here like any other declaration
    // rather than by a second rule kept in step by hand.
    LocalResolver(std::vector<Param>& params, Stmt* body) : params_(params), body_(body) {}

    // `member_initializers` is a constructor's own `: base{...},
    // field{...}` list. Those expressions are outside the body but are
    // evaluated in the constructor's scope, so they see its parameters
    // -- and, like anything else that reads a local, they only know
    // which one they read once they are resolved here.
    void run(std::vector<MemberInitializer>* member_initializers = nullptr) {
        // Parameters live for the whole function, so they are declared
        // before any scope frame exists and are never popped -- matching
        // both MirBuilder and codegen.
        for (Param& param : params_) {
            LocalDecl decl;
            decl.type = param.type;
            decl.source_name = param.name;
            param.resolved_local = declare(param.name, std::move(decl)) + 1;
        }
        if (member_initializers != nullptr) {
            for (MemberInitializer& member_initializer : *member_initializers) {
                resolve_initializer(member_initializer.initializer);
            }
        }
        if (body_ != nullptr) resolve_stmt(*body_);
    }

    [[nodiscard]] std::vector<LocalDecl> take_decls() { return std::move(decls_); }

private:
    // One declaration of a given spelling, and whether its lexical scope
    // is still open. Entries are never removed: a name that has gone out
    // of scope is still *known*, which is what lets the checker answer a
    // use of it with "out of scope here" (and a type-aware message)
    // rather than the far vaguer "undeclared".
    struct Binding {
        std::size_t id = 0;
        bool in_scope = true;
    };

    std::vector<Param>& params_;
    Stmt* body_;
    std::vector<LocalDecl> decls_;
    std::unordered_map<std::string, std::vector<Binding>> bindings_;
    std::vector<std::vector<std::string>> scope_stack_;

    std::size_t declare(const std::string& name, LocalDecl decl) {
        std::size_t id = decls_.size();
        decls_.push_back(std::move(decl));
        bindings_[name].push_back(Binding{id, true});
        if (!scope_stack_.empty()) scope_stack_.back().push_back(name);
        return id;
    }

    void push_scope() { scope_stack_.emplace_back(); }

    void pop_scope() {
        for (const std::string& name : scope_stack_.back()) {
            auto it = bindings_.find(name);
            if (it == bindings_.end()) continue;
            for (auto binding = it->second.rbegin(); binding != it->second.rend(); ++binding) {
                if (!binding->in_scope) continue;
                binding->in_scope = false;
                break;
            }
        }
        scope_stack_.pop_back();
    }

    // Returns the encoded `id + 1` of the declaration this name refers
    // to, or 0 when it names no local at all (a global, a function, an
    // enum constant, ...).
    //
    // An in-scope declaration always wins, innermost first -- that is
    // ordinary lexical scope, and it is what makes a shadowed outer
    // local unreachable while the shadow is live. Only when none is in
    // scope does the most recent already-closed declaration answer, so
    // that a use after the end of a block still knows *which* variable
    // was meant.
    [[nodiscard]] std::size_t lookup(const std::string& name) const {
        auto it = bindings_.find(name);
        if (it == bindings_.end() || it->second.empty()) return 0;
        for (auto binding = it->second.rbegin(); binding != it->second.rend(); ++binding) {
            if (binding->in_scope) return binding->id + 1;
        }
        return it->second.back().id + 1;
    }

    void resolve_expr(Expr& expr) {
        // Always written, never only-on-match: a node that no longer
        // names a local (because monomorphize rewrote an Identifier into
        // a Member in place) must lose its old resolution rather than
        // keep a stale one.
        expr.resolved_local = names_a_local_use(expr) ? lookup(expr.name) : 0;
        if (expr.lhs != nullptr) resolve_expr(*expr.lhs);
        if (expr.rhs != nullptr) resolve_expr(*expr.rhs);
        if (expr.third != nullptr) resolve_expr(*expr.third);
        for (const ExprPtr& arg : expr.args) resolve_expr(*arg);
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init != nullptr) resolve_expr(*capture.init);
            // A capture list is written *outside* the closure, so a plain
            // `[x]`/`[&x]` names an enclosing local; an init-capture
            // introduces a new name inside the closure instead and so
            // binds to nothing out here.
            capture.resolved_local = capture.init == nullptr ? lookup(capture.name) : 0;
        }
        if (expr.lambda_body != nullptr) resolve_lambda_body(expr);
    }

    // A closure body is lexically nested inside the function that writes
    // it, and movecheck analyses it as part of that function's body (see
    // interfaces.cppm's walk, which descends straight through a Lambda),
    // so its uses have to resolve here too -- leaving them unbound would
    // silently demote reads of a captured local to "not a local", which
    // is precisely the direction that skips checks.
    //
    // Its parameters and its own declarations are therefore declared in a
    // nested scope: without that, a lambda parameter named like an
    // enclosing local would bind uses in the closure to the *enclosing*
    // declaration -- the same mis-binding this whole model exists to
    // eliminate, just one nesting level down.
    //
    // Captured names are deliberately *not* redeclared inside that scope.
    // A capture is not a fresh declaration at this stage: lambdas.cppm
    // later rewrites captured identifiers into member accesses on the
    // closure object, at which point they stop naming locals at all.
    void resolve_lambda_body(Expr& expr) {
        push_scope();
        for (Param& param : expr.lambda_params) {
            LocalDecl decl;
            decl.type = param.type;
            decl.source_name = param.name;
            param.resolved_local = declare(param.name, std::move(decl)) + 1;
        }
        resolve_stmt(*expr.lambda_body);
        pop_scope();
    }

    void resolve_initializer(Initializer& initializer) {
        if (initializer.expr != nullptr) resolve_expr(*initializer.expr);
        for (const ExprPtr& arg : initializer.brace_args) resolve_expr(*arg);
    }

    void resolve_stmt(Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Block:
                push_scope();
                for (const StmtPtr& child : stmt.statements) resolve_stmt(*child);
                pop_scope();
                return;

            case StmtKind::VarDecl: {
                // The initializer is resolved *after* the declaration, so
                // `int x = x;` binds to the new `x` exactly as in C++
                // (and a shadowing declaration shadows from its own
                // initializer onward).
                LocalDecl decl;
                decl.type = stmt.type;
                decl.source_name = stmt.var_name;
                decl.decl_loc = stmt.loc;
                decl.is_static_lifetime = stmt.is_static_local;
                decl.is_const = stmt.is_const || stmt.is_constexpr;
                if (stmt.init != nullptr && stmt.init->kind == ExprKind::Lambda) {
                    for (const LambdaCapture& capture : stmt.init->lambda_captures) {
                        if (capture.by_reference) {
                            decl.is_borrow_holding_closure = true;
                            break;
                        }
                    }
                }
                stmt.declared_local = declare(stmt.var_name, std::move(decl)) + 1;
                if (stmt.init != nullptr) resolve_expr(*stmt.init);
                for (const ExprPtr& arg : stmt.ctor_args) resolve_expr(*arg);
                return;
            }

            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr != nullptr) resolve_expr(*stmt.expr);
                return;

            case StmtKind::If:
                if (stmt.condition != nullptr) resolve_expr(*stmt.condition);
                push_scope();
                if (stmt.then_branch != nullptr) resolve_stmt(*stmt.then_branch);
                pop_scope();
                push_scope();
                if (stmt.else_branch != nullptr) resolve_stmt(*stmt.else_branch);
                pop_scope();
                return;

            case StmtKind::While:
                if (stmt.condition != nullptr) resolve_expr(*stmt.condition);
                push_scope();
                if (stmt.then_branch != nullptr) resolve_stmt(*stmt.then_branch);
                pop_scope();
                return;

            case StmtKind::Switch:
                if (stmt.condition != nullptr) resolve_expr(*stmt.condition);
                // One scope per case, braced or not -- matching both
                // MirBuilder's lowering and codegen's push_scope, which
                // is what lets sibling cases reuse a name.
                for (SwitchCase& switch_case : stmt.switch_cases) {
                    if (switch_case.value != nullptr) resolve_expr(*switch_case.value);
                    push_scope();
                    for (const StmtPtr& child : switch_case.statements) resolve_stmt(*child);
                    pop_scope();
                }
                return;

            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Fallthrough:
                return;
        }
    }
};

class MirBuilder {
public:
    explicit MirBuilder(const Function& fn) : fn_(fn), owned_params_(fn.params) {
        body_.owned_body = deep_clone_stmt(*fn.body);
    }

    Body build() {
        body_.function_owning_module = fn_.owning_module;
        body_.function_visibility_module = fn_.visibility_module.empty() ? fn_.owning_module : fn_.visibility_module;
        body_.function_member_owner_class = fn_.member_owner_class;
        body_.function_access_context_class = fn_.access_context_class;
        body_.function_source_path = fn_.loc.source_path_text();
        body_.function_namespace_path = fn_.namespace_path;
        // Resolving this Body's own copy is what makes build_mir correct
        // on its own, whether or not the caller has already resolved the
        // Function it came from. Both runs assign the same ids (the
        // numbering depends only on declaration order), so a use found in
        // any other copy of this body still names the same entry here.
        LocalResolver resolver{owned_params_, body_.owned_body.get()};
        resolver.run();
        body_.local_decls = resolver.take_decls();
        current_block_ = new_block();
        lower_stmt(*body_.owned_body);
        insert_drops_before_returns();
        return std::move(body_);
    }

private:
    const Function& fn_;
    // The resolver records each declaration's id on the declaration
    // itself, and a parameter is a declaration; `fn_` is const here (the
    // Body is built from a clone), so it gets its own copy to write into,
    // exactly as the body does. The ids match the ones the Function's own
    // resolution assigns, since the numbering depends only on declaration
    // order.
    std::vector<Param> owned_params_;
    Body body_;
    std::size_t current_block_ = 0;
    // One frame per lexically-enclosing block/if-branch/while-body,
    // holding the locals declared directly within it -- mirrors codegen's
    // scope_stack_. Parameters are declared before any frame is pushed
    // (see build()), so they're never captured here: they live for the
    // whole function, same as in codegen.
    std::vector<std::vector<LocalId>> scope_stack_;
    struct ControlFlowFrame {
        std::optional<std::size_t> continue_block;
        std::size_t end_block;
        std::size_t scope_depth;
    };
    std::vector<ControlFlowFrame> control_flow_stack_;

    // Records that `id` was declared in the innermost open scope, so that
    // scope's exit resets exactly this local's liveness -- and, crucially,
    // not that of some other declaration that merely shares its name.
    void note_declared_in_scope(LocalId id) {
        if (!scope_stack_.empty()) {
            scope_stack_.back().push_back(id);
        }
    }

    void push_scope() { scope_stack_.emplace_back(); }

    // Emits a ScopeExit statement (in reverse declaration order, matching
    // codegen's drop order) for every local declared directly in the scope
    // being popped -- unless the current block already ended in a
    // terminator (e.g. a `return` already exited this scope; no further
    // statements can be appended to that block anyway), matching
    // codegen's identical pop_scope() guard.
    void pop_scope() {
        std::vector<LocalId> locals = std::move(scope_stack_.back());
        scope_stack_.pop_back();
        if (current_has_terminator()) return;
        for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
            current().statements.push_back(local_stmt(MirStatementKind::ScopeExit, *it, nullptr, Type{}, SourceLocation{}));
        }
    }

    void emit_scope_exits_to_depth(std::size_t target_depth) {
        for (std::size_t depth = scope_stack_.size(); depth > target_depth; depth--) {
            const std::vector<LocalId>& locals = scope_stack_[depth - 1];
            for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
                current().statements.push_back(local_stmt(MirStatementKind::ScopeExit, *it, nullptr, Type{}, SourceLocation{}));
            }
        }
    }

    [[nodiscard]] static MirStatement local_stmt(MirStatementKind kind, LocalId id, const Expr* expr, const Type& type,
                                                 const SourceLocation& loc) {
        MirStatement stmt;
        stmt.kind = kind;
        stmt.local = id;
        stmt.has_local = true;
        stmt.expr = expr;
        stmt.type = type;
        stmt.loc = loc;
        return stmt;
    }

    // An assignment whose target is written as a bare name. `has_local`
    // tells the checker apart the two cases it must handle differently:
    // a local (keyed by its declaration) and a global (which has none).
    [[nodiscard]] MirStatement assign_stmt(const Expr& target, const Expr* rhs, const SourceLocation& loc) const {
        MirStatement stmt;
        stmt.kind = MirStatementKind::Assign;
        if (std::optional<LocalId> id = body_.local_of(target); id.has_value()) {
            stmt.local = *id;
            stmt.has_local = true;
        }
        stmt.target = &target;
        stmt.expr = rhs;
        stmt.loc = loc;
        return stmt;
    }

    [[nodiscard]] static MirStatement plain_stmt(MirStatementKind kind, const Expr* expr, const SourceLocation& loc) {
        MirStatement stmt;
        stmt.kind = kind;
        stmt.expr = expr;
        stmt.loc = loc;
        return stmt;
    }

    std::size_t new_block() {
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
                        plain_stmt(MirStatementKind::UnsafeEnter, nullptr, stmt.loc));
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
                        plain_stmt(MirStatementKind::UnsafeExit, nullptr, stmt.loc));
                }
                pop_scope();
                return;

            case StmtKind::VarDecl: {
                // Identity comes from resolution, not from the name: two
                // sibling `r`s are two different locals here.
                if (!has_declared_local(stmt)) return;
                LocalId id = declared_local_of(stmt);
                note_declared_in_scope(id);
                if (stmt.type.kind == TypeKind::Reference || stmt.type.kind == TypeKind::Span) {
                    // `expr` is null when the source omitted an
                    // initializer (`int& r;` / `std::span<int> s;`,
                    // illegal since both must be bound at declaration) --
                    // left for movecheck to reject with a clear
                    // diagnostic rather than validated here, keeping this
                    // builder a straightforward, non-throwing translation.
                    current().statements.push_back(
                        local_stmt(MirStatementKind::BindReference, id, stmt.init.get(), stmt.type, stmt.loc));
                } else if (stmt.init) {
                    current().statements.push_back(
                        local_stmt(MirStatementKind::Assign, id, stmt.init.get(), stmt.type, stmt.loc));
                } else if (stmt.has_ctor_args) {
                    // ch04 §4.2 / spec §6.1: `ClassName name{args};` --
                    // see
                    // MirStatement::ctor_args' own comment for why this
                    // needs to carry the argument list (rather than
                    // falling into the plain, argument-blind Declare case
                    // just below).
                    MirStatement mir_stmt = local_stmt(MirStatementKind::Declare, id, nullptr, stmt.type, stmt.loc);
                    mir_stmt.ctor_args = &stmt.ctor_args;
                    current().statements.push_back(std::move(mir_stmt));
                } else {
                    current().statements.push_back(
                        local_stmt(MirStatementKind::Declare, id, nullptr, stmt.type, stmt.loc));
                }
                return;
            }

            case StmtKind::Return: {
                Terminator term;
                term.kind = TerminatorKind::Return;
                term.return_value = stmt.expr ? stmt.expr.get() : nullptr;
                term.loc = stmt.loc;
                current().terminator = std::move(term);
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
                    current().statements.push_back(assign_stmt(*e.lhs, e.rhs.get(), stmt.loc));
                } else {
                    current().statements.push_back(plain_stmt(MirStatementKind::Eval, &e, stmt.loc));
                }
                return;
            }

            case StmtKind::If: {
                std::size_t branch_block = current_block_;
                std::size_t then_block = new_block();
                std::size_t else_block = new_block();
                std::size_t merge_block = new_block();

                Terminator term;
                term.kind = TerminatorKind::Branch;
                term.true_target = then_block;
                term.false_target = else_block;
                term.condition = stmt.condition.get();
                term.loc = stmt.loc;
                body_.blocks[branch_block].terminator = std::move(term);

                current_block_ = then_block;
                push_scope();
                lower_stmt(*stmt.then_branch);
                pop_scope();
                if (!current_has_terminator()) {
                    Terminator term;
                    term.kind = TerminatorKind::Goto;
                    term.target = merge_block;
                    term.loc = stmt.loc;
                    current().terminator = std::move(term);
                }

                current_block_ = else_block;
                push_scope();
                if (stmt.else_branch) lower_stmt(*stmt.else_branch);
                pop_scope();
                if (!current_has_terminator()) {
                    Terminator term;
                    term.kind = TerminatorKind::Goto;
                    term.target = merge_block;
                    term.loc = stmt.loc;
                    current().terminator = std::move(term);
                }

                current_block_ = merge_block;
                return;
            }

            case StmtKind::While: {
                std::size_t preheader = current_block_;
                std::size_t cond_block = new_block();
                std::size_t body_block = new_block();
                std::size_t end_block = new_block();

                Terminator to_cond;
                to_cond.kind = TerminatorKind::Goto;
                to_cond.target = cond_block;
                to_cond.loc = stmt.loc;
                body_.blocks[preheader].terminator = std::move(to_cond);
                Terminator branch;
                branch.kind = TerminatorKind::Branch;
                branch.true_target = body_block;
                branch.false_target = end_block;
                branch.condition = stmt.condition.get();
                branch.loc = stmt.loc;
                body_.blocks[cond_block].terminator = std::move(branch);

                current_block_ = body_block;
                push_scope();
                control_flow_stack_.push_back(ControlFlowFrame{cond_block, end_block, scope_stack_.size()});
                lower_stmt(*stmt.then_branch);
                pop_scope();
                control_flow_stack_.pop_back();
                if (!current_has_terminator()) {
                    Terminator back_edge;
                    back_edge.kind = TerminatorKind::Goto;
                    back_edge.target = cond_block;
                    back_edge.loc = stmt.loc;
                    current().terminator = std::move(back_edge);
                }

                current_block_ = end_block;
                return;
            }

            case StmtKind::Switch: {
                std::size_t dispatch_block = current_block_;
                std::size_t end_block = new_block();
                std::vector<std::size_t> case_blocks;
                case_blocks.reserve(stmt.switch_cases.size());
                for ([[maybe_unused]] const SwitchCase& switch_case : stmt.switch_cases) {
                    case_blocks.push_back(new_block());
                }

                Terminator dispatch;
                dispatch.kind = TerminatorKind::Switch;
                dispatch.condition = stmt.condition.get();
                dispatch.loc = stmt.loc;
                for (std::size_t block : case_blocks) dispatch.switch_targets.push_back(SwitchTarget{block});
                bool has_default = false;
                for (const SwitchCase& switch_case : stmt.switch_cases) {
                    if (!switch_case.value) {
                        has_default = true;
                        break;
                    }
                }
                if (!has_default) dispatch.switch_targets.push_back(SwitchTarget{end_block});
                body_.blocks[dispatch_block].terminator = std::move(dispatch);

                for (std::size_t i = 0; i < stmt.switch_cases.size(); i++) {
                    current_block_ = case_blocks[i];
                    push_scope();
                    control_flow_stack_.push_back(ControlFlowFrame{std::nullopt, end_block, scope_stack_.size()});
                    const SwitchCase& switch_case = stmt.switch_cases[i];
                    for (const StmtPtr& child : switch_case.statements) {
                        if (current_has_terminator()) break;
                        lower_stmt(*child);
                    }
                    control_flow_stack_.pop_back();
                    bool falls_into_next_case =
                        switch_case.statements.empty() ||
                        (!switch_case.statements.empty() && switch_case.statements.back()->kind == StmtKind::Fallthrough);
                    pop_scope();
                    if (!current_has_terminator()) {
                        Terminator term;
                        term.kind = TerminatorKind::Goto;
                        term.target = falls_into_next_case && i + 1 < case_blocks.size() ? case_blocks[i + 1] : end_block;
                        term.loc = stmt.loc;
                        current().terminator = std::move(term);
                    }
                }

                current_block_ = end_block;
                return;
            }

            case StmtKind::Break: {
                if (control_flow_stack_.empty()) return;
                emit_scope_exits_to_depth(control_flow_stack_.back().scope_depth);
                Terminator term;
                term.kind = TerminatorKind::Goto;
                term.target = control_flow_stack_.back().end_block;
                term.loc = stmt.loc;
                current().terminator = std::move(term);
                return;
            }

            case StmtKind::Continue: {
                for (auto it = control_flow_stack_.rbegin(); it != control_flow_stack_.rend(); ++it) {
                    if (!it->continue_block.has_value()) continue;
                    emit_scope_exits_to_depth(it->scope_depth);
                    Terminator term;
                    term.kind = TerminatorKind::Goto;
                    term.target = *it->continue_block;
                    term.loc = stmt.loc;
                    current().terminator = std::move(term);
                    return;
                }
                return;
            }

            case StmtKind::Fallthrough: {
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
        std::vector<LocalId> unique_ptr_locals;
        for (std::size_t i = 0; i < body_.local_decls.size(); i++) {
            const Type& type = body_.local_decls[i].type;
            if (type.kind == TypeKind::Named &&
                (type.name == "std::unique_ptr" || type.name.rfind("std::unique_ptr.", 0) == 0)) {
                unique_ptr_locals.push_back(static_cast<LocalId>(i));
            }
        }
        if (unique_ptr_locals.empty()) return;

        for (BasicBlock& block : body_.blocks) {
            if (block.terminator.kind != TerminatorKind::Return) continue;
            for (auto it = unique_ptr_locals.rbegin(); it != unique_ptr_locals.rend(); ++it) {
                block.statements.push_back(local_stmt(MirStatementKind::Drop, *it, nullptr, Type{}, SourceLocation{}));
            }
        }
    }
};

} // namespace

[[nodiscard]] std::vector<LocalDecl> resolve_locals_in(std::vector<Param>& params, Stmt& body) {
    LocalResolver resolver{params, &body};
    resolver.run();
    return resolver.take_decls();
}

void resolve_locals(Function& fn) {
    LocalResolver resolver{fn.params, fn.body.get()};
    resolver.run(&fn.member_initializers);
}

void resolve_program_locals(Program& program) {
    for (Function& fn : program.functions) resolve_locals(fn);
}

Body build_mir(const Function& fn) {
    MirBuilder builder(fn);
    return builder.build();
}

} // namespace scpp
