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
    // immutable declaration. Also carries a `const` *parameter*'s
    // qualifier: [dcl.fct]/5 deletes that from the function's type, so
    // `Param::type` cannot hold it and this is the only place it
    // survives (see Param::is_const). Consulted by place_is_read_only,
    // which is what every write, borrow and address-of check asks.
    bool is_const = false;
    // Declared with the `constexpr` specifier specifically. Implied by
    // `is_const` ([dcl.constexpr]/1 makes such an object const), kept
    // apart from it only so a diagnostic names the keyword the source
    // actually spells: "'w' is declared const at line 2" pointed at a
    // line reading `constexpr char w[6]{...}` sends the reader looking
    // for a `const` that is not there.
    bool is_constexpr = false;
    // ch05 §5.12: initialized with a lambda that has at least one
    // by-reference capture. Such a closure keeps those borrows alive
    // until its own last use / ScopeExit, so reference-liveness treats
    // reads of the closure local itself as reference-like uses.
    bool is_borrow_holding_closure = false;
};

// One step of a place's projection path: `.field`, `[constant]`, or a
// dereference `*`.
//
// A `[i]` with a non-constant index does not name a statically-known
// element, so it is still not a projection step -- see place_of, which
// declines to build a Place for it rather than building an approximate
// one that would silently under-report.
//
// A dereference *is* one. `*p` names the object `p` currently points to,
// and the expression that produced the pointer is what identifies it, in
// exactly the way a field name identifies a member: two occurrences of
// `*p` with no intervening write to `p` name the same object. That two
// *different* pointers may also name it is an aliasing question, and it
// is the same question two `T&` parameters already pose -- spec §6.2
// states no aliasing rule for either, and answers exclusivity for the
// bindings it does govern with the reborrow rules of §6.2(7)-(10) rather
// than with a points-to analysis. Declining to build a place here
// therefore bought no soundness; it only removed `*p` and `p->m` from
// the two-state model entirely.
struct Projection {
    bool is_index = false;         // false: `.field`; true: `[index]`
    bool is_deref = false;         // `*`; `field`/`index` unused
    std::string field;             // `.field`
    std::int64_t index = 0;        // `[index]`

    bool operator==(const Projection&) const = default;
    // Only for deterministic ordering (unordered_map iteration order is
    // not stable, and several diagnostics pick "the lowest" place so the
    // message does not depend on hash order).
    auto operator<=>(const Projection&) const = default;
};

// A place is a storage location a MIR statement can read from or write
// to: a root local (Body::local_decls) plus a projection path naming a
// subobject of it. The empty path is the whole local, which is what the
// move checker used to be able to talk about *at all* -- every map key
// that used to be a bare LocalId is now that local's empty-path Place,
// so this is the same model with the projections filled in, not a
// second one beside it.
//
// spec §6.2(1) puts objects "of automatic, static, thread, or member
// storage duration" in exactly one of the two ownership states, and
// §6.2(3) transitions "an object designated by an id-expression". A
// member named inside its own class is an id-expression, so member
// storage was always in the model; only the key was too coarse to
// record it.
struct Place {
    LocalId local{};
    std::vector<Projection> path;

    bool operator==(const Place&) const = default;

    [[nodiscard]] bool is_whole_local() const { return path.empty(); }

    // Whether `this` is `other` itself or a subobject of it -- the
    // relation moving a place uses to poison what it contains
    // (`o.i` moved out takes `o.i.q` with it).
    [[nodiscard]] bool is_at_or_under(const Place& other) const {
        if (local != other.local) return false;
        if (path.size() < other.path.size()) return false;
        for (std::size_t i = 0; i < other.path.size(); i++) {
            if (!(path[i] == other.path[i])) return false;
        }
        return true;
    }

    [[nodiscard]] bool is_strictly_under(const Place& other) const {
        return path.size() > other.path.size() && is_at_or_under(other);
    }

    // This place with its last projection step removed. Precondition:
    // !is_whole_local().
    [[nodiscard]] Place parent() const {
        Place result = *this;
        result.path.pop_back();
        return result;
    }
};

[[nodiscard]] inline Place whole_local_place(LocalId local) { return Place{local, {}}; }

[[nodiscard]] inline Place projected_field(Place base, std::string field) {
    base.path.push_back(Projection{/*is_index=*/false, /*is_deref=*/false, std::move(field), 0});
    return base;
}

[[nodiscard]] inline Place projected_index(Place base, std::int64_t index) {
    base.path.push_back(Projection{/*is_index=*/true, /*is_deref=*/false, {}, index});
    return base;
}

[[nodiscard]] inline Place projected_deref(Place base) {
    base.path.push_back(Projection{/*is_index=*/false, /*is_deref=*/true, {}, 0});
    return base;
}

// Whether reaching `place` goes through a dereference. The object such a
// place names is the one a *pointer* designated, which is not storage
// this function declares: spec §6.2(1) puts objects "of automatic,
// static, thread, or member storage duration" in the two-state model,
// and which of those -- if any -- a pointee has is a property of
// whoever created it, not of the dereference.
[[nodiscard]] inline bool place_goes_through_deref(const Place& place) {
    for (const Projection& step : place.path) {
        if (step.is_deref) return true;
    }
    return false;
}

// Deliberately a named hasher rather than a std::hash specialization:
// `Place` is exported from a named module, and a specialization of a
// std template for such a type is only found where the specialization
// itself is reachable -- a footgun that silently degrades to the primary
// template being ill-formed at some importer. A hasher passed explicitly
// to every map cannot be missed.
struct PlaceHash {
    [[nodiscard]] std::size_t operator()(const Place& place) const {
        std::size_t h = std::hash<std::size_t>{}(local_index(place.local));
        for (const Projection& step : place.path) {
            std::size_t step_hash = step.is_deref  ? 0x9e3779b9ULL
                                    : step.is_index ? std::hash<std::int64_t>{}(step.index)
                                                    : std::hash<std::string>{}(step.field);
            h ^= step_hash + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }
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
    // Evaluates `expr` and binds it into a member of the object being
    // constructed: one expression of one entry of a constructor's
    // `: base{...}, field{...}` list, emitted into the entry block ahead
    // of the body because that is when it runs.
    //
    // Distinct from Eval because the value is *consumed*, not discarded:
    // Eval reports a discarded [[nodiscard]] result, which `x_{make()}`
    // must not, and `x_{std::move(p)}` is a move-target context exactly
    // as the Assign of a declaration with an initializer is.
    //
    // The member itself is not named here. A field is not a local, so
    // there is no LocalId to record and no per-field state to update --
    // this statement exists to make the *reads and moves* the expression
    // performs visible to the analysis, which is precisely what the list
    // being absent from the MIR hid.
    MemberInit,
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
    // Lexical `[[scpp::unsafe]]` nesting in effect where this block
    // begins, excluding the function-level base (spec §5.1(3): an
    // unsafe context is a property of the enclosing *compound-statement*,
    // i.e. of the syntax).
    //
    // Recorded here because it is a lexical fact and cannot be recovered
    // from the CFG. The move checker's worklist fixed-point used to
    // derive it entirely from the UnsafeEnter/UnsafeExit statements
    // below, flowing it along edges and joining it with `min` -- which
    // works only for straight-line code. A loop's head has a back edge
    // from a block whose `out_state` starts out default-constructed at
    // depth 0, so `min` pinned the head to 0 and, being monotone
    // downward, never recovered: a *stable* wrong fixed point. The
    // effect was that `[[scpp::unsafe]] { while (...) { ... } }` licensed
    // nothing at all inside the loop, for every gated operation.
    int unsafe_depth_on_entry = 0;
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
    // A constructor's own `: base{...}, field{...}` list, deep-copied for
    // the same reason `owned_body` is: the LocalResolver run below writes
    // each identifier's resolved id into the expression it reads, and the
    // Function this Body was built from is const here. Copying an
    // Initializer deep-clones its expressions (see Initializer's copy
    // constructor), so the MemberInit statements below point at nodes
    // this Body owns, not at ones some other copy of the program shares.
    std::vector<MemberInitializer> owned_member_initializers;
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
    // True while this body is a still-uninstantiated generic template
    // (including an abbreviated template -- a `Concept auto`/`auto`
    // parameter). Its parameter types are placeholders, so a type
    // inferred inside it can come from a stand-in signature rather than
    // a real one: a concept's witness class synthesizes a method per
    // requirement and gives it return type `void` to mean "this
    // requirement says nothing about the return type", which
    // infer_expr_type reports as a plain `void`. A rule that merely
    // declines to judge an unrecognised type is unaffected; one that
    // *rejects* on a specific answer must not be founded on that.
    // Every such body is also checked once per instantiation, where the
    // types are real.
    bool function_is_generic_template = false;

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

    // How to spell `place` in a diagnostic. Never prints a LocalId: the
    // root's own source spelling, then the projection steps as written
    // (`s.a.b`, `arr[0]`). A member of the implicit object parameter is
    // spelled the way the source most likely wrote it -- `frames_`, not
    // `this.frames_` -- because that is the name the reader has to find.
    [[nodiscard]] std::string describe_place(const Place& place) const {
        std::string result;
        std::size_t first_step = 0;
        std::optional<LocalId> self = this_local();
        if (self.has_value() && place.local == *self && !place.path.empty() && !place.path[0].is_index &&
            !place.path[0].is_deref) {
            result = place.path[0].field;
            first_step = 1;
        } else {
            result = is_valid_local(place.local) ? name_of(place.local) : std::string("<unknown>");
        }
        // A dereference is spelled by what follows it: `p->m` when a
        // field does, `(*p)[i]` when an element does, plain `*p` when
        // nothing does. Rendering it as a step of its own would print
        // places the user cannot type back.
        bool pending_deref = false;
        for (std::size_t i = first_step; i < place.path.size(); i++) {
            if (place.path[i].is_deref) {
                pending_deref = true;
                continue;
            }
            if (place.path[i].is_index) {
                if (pending_deref) {
                    result = "(*" + result + ")";
                    pending_deref = false;
                }
                result += "[";
                result += std::to_string(place.path[i].index);
                result += "]";
            } else {
                result += pending_deref ? "->" : ".";
                pending_deref = false;
                result += place.path[i].field;
            }
        }
        if (pending_deref) result = "*" + result;
        return result;
    }
};

// Why `expr` does not name a place the move checker can record state
// for. Empty when it does. See place_of.

// The place `expr` names, or nullopt when it names no statically
// identifiable storage at all (a temporary, a call result, a global, a
// non-constant subscript, a raw-pointer dereference). `resolve_root`
// maps a reference/span local to the place it is bound to, so
// `r.a` where `r` is `S& r = s;` is the same place as `s.a`; passing a
// null resolver keeps the projection rooted at the reference local
// itself, which is what a caller that does not track bindings wants.
// How exactly a place has to be identified.
//
//  - Exact: the answer names *this* object and nothing else, which is
//    what recording an ownership-state transition against it requires.
//    `arr[i]` for a non-constant `i` has no exact answer.
//  - Enclosing: the answer names a place that *contains* the object,
//    which is all an aliasing question needs -- `arr[i]` answers `arr`.
//    Never used to write state, only to prove two places disjoint.
enum class PlacePrecision { Exact, Enclosing };

[[nodiscard]] std::optional<Place> place_of(
    const Expr& expr, const Body& body,
    const std::function<std::optional<Place>(LocalId)>& resolve_root = {},
    PlacePrecision precision = PlacePrecision::Exact);

// The same walk for a caller that has no Body -- codegen resolves an
// identifier to a local through its own slot table, not through
// Body::local_of, and must reach the *same* Place for the same
// expression or its teardown would consult a flag the move checker
// never set.
[[nodiscard]] std::optional<Place> place_of(
    const Expr& expr, const std::function<std::optional<LocalId>(const Expr&)>& local_of,
    const std::function<std::optional<Place>(LocalId)>& resolve_root = {},
    PlacePrecision precision = PlacePrecision::Exact);

// Assigns every local declaration in `fn` its identity, and binds every
// identifier use in `fn`'s body to the declaration it refers to under
// ordinary lexical scoping. Idempotent and deterministic: ids are handed
// out in declaration order (parameters first), so re-running it -- or
// running it on a deep clone -- reproduces exactly the same numbering,
// which is what lets a Body built from one copy of a body describe uses
// found in another.
void resolve_locals(Function& fn);

// Runs resolve_locals over every function in `program`. Every later pass
// assumes a use already knows its declaration, so this must run before
// any of them and again after any pass that synthesizes new functions
// (monomorphization does both). Re-running is free of consequence:
// resolution is idempotent and depends only on the body's own syntax.
void resolve_program_locals(Program& program);

Body build_mir(const Function& fn);

// A Body for an expression that has no enclosing function: a namespace-
// scope variable's initializer, a field's default member initializer, or
// a parameter's default argument. None of them can name a local, so
// `local_decls` stays empty and `local_of` correctly answers "not a
// local" for every identifier in them; the module/namespace fields carry
// the visibility context the checks need to decide which declarations
// the expression is allowed to see. Getting those wrong is not a
// cosmetic error: overload resolution runs through them, so a Body with
// the wrong module silently fails to find a callee that is plainly
// visible at the point of use, and a check that depends on the callee's
// type then passes by default.
[[nodiscard]] Body make_initializer_scope_body(const Program& program, const std::string& owning_module,
                                               const std::string& visibility_module,
                                               const std::vector<std::string>& namespace_path,
                                               const std::string& source_path);

// One expression-bearing position that sits outside every function body,
// normalised to the two spellings an initializer can have (`= expr` and
// `{args...}`) so a consumer never has to know which kind of declaration
// it came from.
struct InitializerScope {
    // The declared type the expression initializes, and where to point a
    // diagnostic. `name` is the variable/field/parameter's own name.
    const Type* declared_type = nullptr;
    const Expr* expr = nullptr;
    const std::vector<ExprPtr>* brace_args = nullptr;
    SourceLocation loc;
    std::string name;
    Body body;
    // True only for a namespace-scope variable. A field's type and a
    // parameter's type are declarations too, but each is already checked
    // where it is declared and under its own wording, so a consumer that
    // has a rule about *declaring* a variable applies it only here.
    bool declares_namespace_scope_variable = false;
};

// The single enumeration of every expression position in a program that
// no function body encloses -- equivalently, every position whose
// expressions cannot name a local and so need a synthesized Body: a
// namespace-scope variable's initializer, a class field's default member
// initializer, a struct field's, and a parameter's default argument. A
// constructor's mem-initializer list is deliberately not among them: it
// can name the constructor's parameters, so it is walked with that
// constructor's own Body, alongside its body.
//
// It exists because that list is exactly what has been drifting. Each
// pass that needed it wrote its own copy and each copy was missing
// different entries -- movecheck's conversion checks had all four,
// interface validation had globals and class fields but neither struct
// fields nor parameter defaults, so an interface temporary written in
// either of those two reached codegen, which assumes such an object
// cannot exist and segfaults building it. A pass that needs to see every
// expression a program contains asks here instead, and a position added
// to the language is added to this list once rather than to every pass
// that forgot it last time.
//
// The Body differs per position and genuinely has to: a global's
// initializer resolves names in its own namespace, a field's in the
// record's, a parameter default's in the function's *visibility* module
// rather than its owning one. That is why this hands the caller a Body
// per position rather than pretending one traversal state fits all.
//
// `visit` is called once per position and returns any
// `std::expected<void, E>`; the first failure stops the walk.
template <typename VisitFn>
[[nodiscard]] auto for_each_initializer_scope(const Program& program, VisitFn&& visit)
    -> std::invoke_result_t<VisitFn&, const InitializerScope&> {
    using Result = std::invoke_result_t<VisitFn&, const InitializerScope&>;
    for (const GlobalVar& global : program.globals) {
        if (global.decl == nullptr || global.decl->kind != StmtKind::VarDecl) continue;
        InitializerScope scope;
        scope.declared_type = &global.decl->type;
        scope.expr = global.decl->init.get();
        scope.brace_args = &global.decl->ctor_args;
        scope.loc = global.decl->loc;
        scope.name = global.decl->var_name;
        scope.declares_namespace_scope_variable = true;
        scope.body = make_initializer_scope_body(program, global.owning_module, global.owning_module,
                                                 global.namespace_path, global.decl->loc.source_path_text());
        if (Result r = visit(scope); !r.has_value()) return r;
    }
    for (const ClassDef& def : program.classes) {
        for (const ClassField& field : def.fields) {
            if (!field.default_initializer.has_value()) continue;
            InitializerScope scope;
            scope.declared_type = &field.type;
            scope.expr = field.default_initializer->expr.get();
            scope.brace_args = &field.default_initializer->brace_args;
            scope.loc = field.loc;
            scope.name = field.name;
            scope.body = make_initializer_scope_body(program, def.owning_module, def.owning_module, def.namespace_path,
                                                     scope.loc.source_path_text());
            if (Result r = visit(scope); !r.has_value()) return r;
        }
    }
    for (const StructDef& def : program.structs) {
        for (const StructField& field : def.fields) {
            if (!field.default_initializer.has_value()) continue;
            InitializerScope scope;
            scope.declared_type = &field.type;
            scope.expr = field.default_initializer->expr.get();
            scope.brace_args = &field.default_initializer->brace_args;
            scope.loc = field.loc;
            scope.name = field.name;
            scope.body = make_initializer_scope_body(program, def.owning_module, def.owning_module, def.namespace_path,
                                                     scope.loc.source_path_text());
            if (Result r = visit(scope); !r.has_value()) return r;
        }
    }
    for (const Function& fn : program.functions) {
        for (const Param& param : fn.params) {
            if (param.default_expr == nullptr) continue;
            InitializerScope scope;
            scope.declared_type = &param.type;
            scope.expr = param.default_expr.get();
            scope.loc = param.default_expr->loc;
            scope.name = param.name;
            scope.body = make_initializer_scope_body(program, fn.owning_module, fn.visibility_module, fn.namespace_path,
                                                     fn.loc.source_path_text());
            if (Result r = visit(scope); !r.has_value()) return r;
        }
    }
    return Result{};
}

// [dcl.type.cv]/4 + spec ch05 §5.7 / §6.2(10): "is the object this
// expression designates reachable only read-only?" -- i.e. may a write,
// a mutable `T&`/`std::span<T>` binding, a `T*`, or a non-`const` member
// call be formed through this place?
//
// There were two of these, movecheck's `place_is_read_only` (which
// #492 had already unified out of two *within* movecheck) and
// `Codegen::is_read_only_place`, and they had drifted again in four
// places: only movecheck knew a `constexpr` global is read-only, that
// `*this` is the receiver itself, and that a `const`-qualified or
// `std::span<const T>` *return type* is read-only; only codegen knew
// that a `const`-element array's element is. Worse, codegen's Member
// case *returned* the field's own view-ness instead of falling through
// to the base, so `h.field = 1` through a `const Holder& h` read back
// writable there while movecheck rejected it -- the two passes
// disagreeing about the same place. So there is now exactly one of it,
// living here (neither pass can import the other's partitions), with
// each pass supplying only its own name-resolution.
//
// The rule it implements is that const propagates to *subobjects* and
// stops at *indirections*. `c.field`, `c.arr[i]` and `*this` are parts
// of `c`, so a `const` `c` freezes them. A pointee, a span element, and
// anything reached through a member function's declared return type are
// separate objects reached *through* `c`, so `c`'s own qualification
// says nothing about them: `T* const` is not `const T*`, `const
// std::span<T>&` is not `std::span<const T>`, and a `const`-qualified
// accessor returning `T&` hands back a mutable `T&` -- which is exactly
// how every standard handle type is specified
// ([util.smartptr.shared.obs], [unique.ptr.single.observers],
// [refwrap.access], [span.elem]). Nothing here matches on a type *name*;
// `shared_ptr` is not special, it is merely the commonest spelling of
// "accessor returning a non-const reference".
struct ReadOnlyPlaceQuery {
    // For an Identifier: the named entity's own `const`/`constexpr`-ness
    // and its declared type, or nullopt when this pass cannot resolve the
    // name (left to the other pass's own check).
    std::function<std::optional<std::pair<bool, Type>>(const Expr&)> declared_variable;
    // Any expression's type as this pass infers it.
    std::function<std::optional<Type>(const Expr&)> inferred_type;
    // A Call's *declared return type*. Authoritative on its own: a
    // signature promising `const T&` back hands back a read-only view
    // however mutable the receiver was, and one promising `T&` could only
    // have been called at all with a mutably-reachable argument (the
    // call-argument guard enforces that separately).
    std::function<std::optional<Type>(const Expr&)> call_return_type;
    std::function<const ClassDef*(const std::string&)> class_def;
    std::function<const StructDef*(const std::string&)> struct_def;
};

[[nodiscard]] inline bool type_is_read_only_view(const Type& type) {
    return type.is_const_qualified ||
           ((type.kind == TypeKind::Reference || type.kind == TypeKind::Span) && !type.is_mutable_ref);
}

[[nodiscard]] inline bool place_is_read_only(const Expr& expr, const ReadOnlyPlaceQuery& query) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            if (!query.declared_variable) return false;
            std::optional<std::pair<bool, Type>> declared = query.declared_variable(expr);
            if (!declared.has_value()) return false; // unknown name: left to the other pass's own check
            return declared->first || type_is_read_only_view(declared->second);
        }
        case ExprKind::Member:
        case ExprKind::Subscript: {
            if (expr.lhs == nullptr) return false;
            std::optional<Type> base = query.inferred_type ? query.inferred_type(*expr.lhs) : std::nullopt;
            const Type* effective = base.has_value() ? &*base : nullptr;
            if (effective != nullptr && effective->kind == TypeKind::Reference && effective->pointee != nullptr) {
                effective = effective->pointee.get();
            }
            if (effective != nullptr) {
                // An indirection ends the propagation, and answers on its
                // own: whatever it took to *reach* the pointer/span, what
                // is on the far side of it is qualified by the pointee/
                // element type and by nothing else.
                if (effective->kind == TypeKind::Pointer) return !effective->is_mutable_pointee;
                if (effective->kind == TypeKind::Span) return !effective->is_mutable_ref;
                // From here down every step is a subobject of the base.
                if (effective->is_const_qualified) return true;
                if (expr.kind == ExprKind::Subscript && effective->kind == TypeKind::Array &&
                    effective->element != nullptr && effective->element->is_const_qualified) {
                    return true;
                }
                if (expr.kind == ExprKind::Member && effective->kind == TypeKind::Named) {
                    // A field's own declared type decides first: a `const`
                    // field, or one that is itself a shared borrow (most
                    // notably a lambda capture field preserving an outer
                    // `const T&`), is read-only however mutable the object
                    // holding it is. Otherwise fall through to the base.
                    if (const ClassDef* def = query.class_def ? query.class_def(effective->name) : nullptr;
                        def != nullptr) {
                        for (const ClassField& field : def->fields) {
                            if (field.name != expr.name) continue;
                            if (type_is_read_only_view(field.type)) return true;
                            break;
                        }
                    }
                    if (const StructDef* def = query.struct_def ? query.struct_def(effective->name) : nullptr;
                        def != nullptr) {
                        for (const StructField& field : def->fields) {
                            if (field.name != expr.name) continue;
                            if (type_is_read_only_view(field.type)) return true;
                            break;
                        }
                    }
                }
            }
            return place_is_read_only(*expr.lhs, query);
        }
        case ExprKind::Unary: {
            if (expr.lhs == nullptr) return false;
            if (expr.unary_op == UnaryOp::PreInc || expr.unary_op == UnaryOp::PreDec) {
                return place_is_read_only(*expr.lhs, query);
            }
            if (expr.unary_op != UnaryOp::Deref) return false;
            // ch05 §5.9: `*this` is just an explicit spelling of the
            // receiver object, not an indirection to somewhere else.
            if (expr.lhs->kind == ExprKind::Identifier && expr.lhs->name == "this") {
                return place_is_read_only(*expr.lhs, query);
            }
            std::optional<Type> operand = query.inferred_type ? query.inferred_type(*expr.lhs) : std::nullopt;
            if (!operand.has_value()) return false;
            if (operand->kind == TypeKind::Pointer) return !operand->is_mutable_pointee;
            if (operand->kind == TypeKind::Reference) return !operand->is_mutable_ref;
            return false;
        }
        case ExprKind::Call: {
            std::optional<Type> returned = query.call_return_type ? query.call_return_type(expr) : std::nullopt;
            if (!returned.has_value()) return false;
            if (returned->kind == TypeKind::Reference || returned->kind == TypeKind::Span) {
                return !returned->is_mutable_ref;
            }
            return returned->is_const_qualified;
        }
        default:
            return false;
    }
}



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
            // [dcl.fct]/5: the top-level `const` of `f(const int v)` is
            // not part of the function's type (so it is not on
            // `param.type`), but the parameter *object* is const inside
            // the body -- recorded here on exactly the field a `const`
            // local's declaration uses, so every read-only check sees a
            // const parameter and a const local identically.
            decl.is_const = param.is_const;
            decl.decl_loc = param.loc;
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
            // [dcl.fct]/5: the top-level `const` of `f(const int v)` is
            // not part of the function's type (so it is not on
            // `param.type`), but the parameter *object* is const inside
            // the body -- recorded here on exactly the field a `const`
            // local's declaration uses, so every read-only check sees a
            // const parameter and a const local identically.
            decl.is_const = param.is_const;
            decl.decl_loc = param.loc;
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
                decl.is_constexpr = stmt.is_constexpr;
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
        body_.owned_member_initializers = fn.member_initializers;
    }

    Body build() {
        body_.function_owning_module = fn_.owning_module;
        body_.function_visibility_module = fn_.visibility_module.empty() ? fn_.owning_module : fn_.visibility_module;
        body_.function_member_owner_class = fn_.member_owner_class;
        body_.function_access_context_class = fn_.access_context_class;
        body_.function_source_path = fn_.loc.source_path_text();
        body_.function_namespace_path = fn_.namespace_path;
        body_.function_is_generic_template = fn_.is_generic_template || !fn_.template_params.empty();
        // Resolving this Body's own copy is what makes build_mir correct
        // on its own, whether or not the caller has already resolved the
        // Function it came from. Both runs assign the same ids (the
        // numbering depends only on declaration order), so a use found in
        // any other copy of this body still names the same entry here.
        // The member-initializer list is passed for the same reason
        // resolve_locals passes it: its expressions read the
        // constructor's parameters, and a read only knows which
        // declaration it names once resolved. It declares nothing, so
        // including it cannot shift any id.
        LocalResolver resolver{owned_params_, body_.owned_body.get()};
        resolver.run(&body_.owned_member_initializers);
        body_.local_decls = resolver.take_decls();
        current_block_ = new_block();
        lower_member_initializers();
        lower_stmt(*body_.owned_body);
        close_implicit_function_exit();
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
    // Lexical `[[scpp::unsafe]]` nesting at the point currently being
    // lowered; stamped onto every block created (see
    // BasicBlock::unsafe_depth_on_entry).
    int unsafe_depth_ = 0;
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

    // A constructor's member-initializer list, lowered into the entry
    // block ahead of the body because that is when it runs. Without this
    // the list was invisible to every dataflow check: `Holder(P p) :
    // p_{std::move(p)} { use(p); }` was accepted, and so was
    // `: a_{std::move(p)}, b_{std::move(p)}`.
    //
    // Lowered in *written* order, which is sound only because
    // validate_constructor_member_initialization now requires the written
    // order to be the execution order (interface bases, then the direct
    // base, then fields in declaration order -- what codegen's
    // emit_constructor_member_initializers actually emits, since it
    // selects each field's initializer by name while walking
    // `class_def->fields`). Deriving the order here instead would mean a
    // second implementation of codegen's walk, free to drift from it;
    // requiring the two to be equal makes them equal by construction.
    //
    // Every expression of every entry is lowered, so a base initializer's
    // arguments (`: Base{std::move(p)}`) are seen exactly like a field's.
    void lower_member_initializers() {
        for (const MemberInitializer& init : body_.owned_member_initializers) {
            const SourceLocation& loc = init.loc.is_known() ? init.loc : fn_.loc;
            if (init.initializer.expr != nullptr) {
                current().statements.push_back(
                    plain_stmt(MirStatementKind::MemberInit, init.initializer.expr.get(), loc));
            }
            for (const ExprPtr& arg : init.initializer.brace_args) {
                if (arg == nullptr) continue;
                current().statements.push_back(plain_stmt(MirStatementKind::MemberInit, arg.get(), loc));
            }
        }
    }

    std::size_t new_block() {
        body_.blocks.push_back(BasicBlock{});
        body_.blocks.back().unsafe_depth_on_entry = unsafe_depth_;
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
                    // Tracked lexically alongside the marker statement so
                    // that any block created while lowering the body --
                    // a loop head in particular, which the marker's own
                    // flow can never reach correctly -- records the depth
                    // it actually sits at (see BasicBlock::
                    // unsafe_depth_on_entry).
                    unsafe_depth_++;
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
                if (stmt.is_unsafe) unsafe_depth_--;
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
    // A function that falls off the end of its body returns there just
    // as surely as one that spells `return;`, and every check keyed on
    // TerminatorKind::Return -- §6.3(1)'s "was this put back before the
    // object it belongs to is destroyed", and the unique_ptr drops
    // below -- reaches that exit only if it carries the terminator.
    // Without it the last block ended in TerminatorKind::None, which
    // check_terminator answers with `return {}`: the exit existed in the
    // CFG and was walked, and nothing was asked about it.
    void close_implicit_function_exit() {
        if (current().terminator.kind != TerminatorKind::None) return;
        Terminator term;
        term.kind = TerminatorKind::Return;
        term.return_value = nullptr;
        term.loc = fn_.body != nullptr ? fn_.body->loc : fn_.loc;
        current().terminator = std::move(term);
    }

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

[[nodiscard]] Body make_initializer_scope_body(const Program& program, const std::string& owning_module,
                                               const std::string& visibility_module,
                                               const std::vector<std::string>& namespace_path,
                                               const std::string& source_path) {
    Body body;
    body.program = &program;
    body.function_owning_module = owning_module;
    body.function_visibility_module = visibility_module;
    body.function_namespace_path = namespace_path;
    body.function_source_path = source_path;
    return body;
}

namespace {

// The constant value of a subscript index, when it has one. Only a
// literal is accepted here on purpose: anything that needs evaluating
// (a named constant, a constant-folded expression) is the constant
// evaluator's job, and movecheck runs before it -- accepting a *maybe*
// constant index would produce a place that is right only sometimes,
// which is exactly the under-reporting a bare LocalId key used to do.
[[nodiscard]] std::optional<std::int64_t> constant_subscript_index(const Expr& index) {
    if (index.kind == ExprKind::IntegerLiteral || index.kind == ExprKind::CharLiteral) return index.int_value;
    return std::nullopt;
}

// `*h` on a class type is rewritten to a call to the selected
// `operator*` (`operator_deref`), and one `h->m` step to a call to
// `operator->` (`operator_arrow`) wrapped in the implicit `*` that
// completes it. Both name the object the receiver points to, so both
// are part of the *place*, not opaque call results.
[[nodiscard]] bool is_indirection_operator_call(const Expr& expr) {
    return expr.kind == ExprKind::Call && (expr.name == "operator_deref" || expr.name == "operator_arrow");
}

} // namespace

[[nodiscard]] std::optional<Place> place_of(const Expr& expr, const Body& body,
                                            const std::function<std::optional<Place>(LocalId)>& resolve_root,
                                            PlacePrecision precision) {
    return place_of(
        expr, [&body](const Expr& e) { return body.local_of(e); }, resolve_root, precision);
}

[[nodiscard]] std::optional<Place> place_of(const Expr& expr,
                                            const std::function<std::optional<LocalId>(const Expr&)>& local_of,
                                            const std::function<std::optional<Place>(LocalId)>& resolve_root,
                                            PlacePrecision precision) {
    if (expr.explicit_global_qualification) return std::nullopt;
    switch (expr.kind) {
        case ExprKind::Identifier: {
            std::optional<LocalId> local = local_of(expr);
            if (!local.has_value()) return std::nullopt;
            if (resolve_root) {
                if (std::optional<Place> bound = resolve_root(*local); bound.has_value()) return bound;
            }
            return whole_local_place(*local);
        }
        case ExprKind::Member: {
            if (expr.lhs == nullptr) return std::nullopt;
            std::optional<Place> base = place_of(*expr.lhs, local_of, resolve_root, precision);
            if (!base.has_value()) return std::nullopt;
            return projected_field(std::move(*base), expr.name);
        }
        case ExprKind::Subscript: {
            if (expr.lhs == nullptr || expr.rhs == nullptr) return std::nullopt;
            std::optional<Place> base = place_of(*expr.lhs, local_of, resolve_root, precision);
            if (!base.has_value()) return std::nullopt;
            std::optional<std::int64_t> index = constant_subscript_index(*expr.rhs);
            if (!index.has_value()) {
                // Which element is unknown; the array itself still
                // contains it, and that is enough to prove disjointness
                // from anything outside the array.
                if (precision == PlacePrecision::Enclosing) return base;
                return std::nullopt;
            }
            return projected_index(std::move(*base), *index);
        }
        case ExprKind::Unary: {
            if (expr.unary_op != UnaryOp::Deref || expr.lhs == nullptr) return std::nullopt;
            std::optional<Place> base = place_of(*expr.lhs, local_of, resolve_root, precision);
            if (!base.has_value()) return std::nullopt;
            return projected_deref(std::move(*base));
        }
        case ExprKind::Call: {
            if (!is_indirection_operator_call(expr) || expr.lhs == nullptr) return std::nullopt;
            std::optional<Place> base = place_of(*expr.lhs, local_of, resolve_root, precision);
            if (!base.has_value()) return std::nullopt;
            // `operator->` yields the *pointer*; the implicit `*` that
            // monomorphize wraps it in supplies the Deref step, so one
            // `->` still contributes exactly one.
            if (expr.name == "operator_arrow") return base;
            return projected_deref(std::move(*base));
        }
        default:
            return std::nullopt;
    }
}

} // namespace scpp
