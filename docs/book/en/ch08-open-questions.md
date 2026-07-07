# 8. Open Questions (to be decided later)

1. **Out-of-bounds subscript**: does `vector[i]` / `span[i]`
   insert a runtime bounds check (like Rust), or require a checked API?
   **Settled**: `span[i]` inserts a runtime bounds
   check by default, calling `abort()` on failure (`vector` is deferred
   beyond v0.1, see [§6](ch06-safe-subset.md), but will follow the same
   policy) -- this is the default,
   checked-by-default behavior. Inside `[[scpp::unsafe]]`, the
   check is skipped -- same treatment, and for the same reason, as
   integer-overflow checking (Q2 below / [§5.8](ch05-static-checks.md)):
   skipping a scpp-inserted *runtime* check carries none of the
   "corrupted bookkeeping leaking into surrounding code" risk that
   keeps move/borrow/lifetime checking unconditional (see
   [§1.1](ch01-safety-context.md)).
2. **Integer overflow**: does scpp check signed overflow? **Settled**:
   yes -- checked by default (both signed and unsigned), `abort()`
   on overflow, unconditionally (no debug/release split); unchecked but
   guaranteed-wrapping (never UB) inside `[[scpp::unsafe]]`,
   achieved by never emitting LLVM's `nsw`/`nuw` on scpp's own
   arithmetic codegen. Division/modulo by zero or `INT_MIN / -1` always
   `abort()`, whether inside `[[scpp::unsafe]]` or not -- there's no wrapped result for
   the hardware to fall back on. Deliberately diverges from Rust's
   debug-only default (see [§5.8](ch05-static-checks.md) for the full
   reasoning, including why overflow-checking -- unlike
   [§5.1-§5.4](ch05-static-checks.md)'s checks -- can safely join what
   `[[scpp::unsafe]]` relaxes without risking the "leakage into surrounding
   code" that [§1.1](ch01-safety-context.md) otherwise guards
   against).
3. **Panic model**: how do OOB / assertion failures terminate? `std::terminate`
   or a custom panic + stack unwinding? **Settled**:
   calls libc's `abort()` directly (lower-level than `std::terminate()`,
   doesn't depend on the C++ runtime's terminate-handler machinery, same
   effect -- the process ends immediately, no stack unwinding).
4. **Interior mutability**: introduce a `Cell`/`RefCell` equivalent to
   carry legal mutable aliasing? **Settled, phase 1 only**: reuse real
   C++'s `mutable` keyword, but stricter -- a `mutable` field must be a
   trivial type (matching `struct`'s own field-triviality rule,
   [§4.1](ch04-struct-vs-class.md)), readable/writable through any
   `this` regardless of `const`, but can **never** be referenced or
   have its address taken (see [§4.2](ch04-struct-vs-class.md)/
   [§5.9](ch05-static-checks.md)). This gives scpp the `Cell<T>` half
   (zero runtime cost, since a value nothing can ever reference cannot
   alias) for free, using existing C++ syntax. The `RefCell` half
   (borrowing an actual reference to non-trivial interior state, with a
   runtime borrow counter panicking/aborting on violation) has no
   existing C++ name to reuse and needs real new machinery -- deferred
   to a later round, not part of this decision.
5. **`const` methods and borrowing**: how does a `const` member function map to
   borrows? **Settled**: `this` is treated as an
   implicit reference parameter -- `const T&` in a `const` method, `T&`
   otherwise -- subject to exactly the same alias-XOR-mutability,
   whole-root-conservative field access, and lifetime-elision rules as
   any other reference (see [§5.9](ch05-static-checks.md)). Related,
   revisited later: whether a `class`'s member *variables* (including
   class-level constants) must always be `private`, only member
   *functions* ever `public` -- **reversed**: real C++ access control is
   fully supported for both, in any combination (see
   [§4.2](ch04-struct-vs-class.md)), since direct external access to a
   public field composes, with no new mechanism, with this Q's own
   this-as-reference-parameter model -- both already record a borrow
   against the same root object. Inheritance (and therefore `protected`)
   remains deferred, not part of this round.
6. **ABI / interop with existing C++ libraries**: how to engineer ordinary
   (checked-by-default) code calling third-party headers (all inherently
   unchecked) — treat them all as requiring `[[scpp::unsafe]]`?
   **Settled**: `extern "C"` ([§2.1](ch02-boundary-rules.md)) is scpp's *only* interop mechanism with the outside world;
   scpp-to-scpp code sharing across files is [ch11](ch11-modules-and-libraries.md). Interop with *existing, unmodified C++ libraries*
   specifically (arbitrary classes, templates, overloads, exceptions,
   RTTI) is explicitly **not pursued** -- considered and rejected the
   idea of transpiling checked scpp to real C++ text and compiling it
   with Clang (which would have made this easy, at the cost of a full
   rework of the already-working direct-to-LLVM-IR codegen path); direct
   LLVM IR generation also has a strictly higher optimization ceiling for
   this code than that alternative would (scoped `alias.scope`/
   `noalias` LLVM metadata, derived from the borrow checker's own
   NLL-precision aliasing proofs, has no C++ source-level equivalent --
   not even `__restrict` reaches it, since `__restrict` only ever maps to
   the coarser, whole-parameter `noalias` attribute).
7. **Language/compiler name, file extension.** File extension **settled**:
   `.scpp` (not `.cpp`) -- source files are now visibly distinct from
   plain C++ at a glance, which matters more now that safety checking is
   the unconditional default rather than an opt-in per function (see
   [ch01](ch01-safety-context.md)): an ordinary, unmodified `.cpp` file
   fed to the scpp compiler would have every function checked, whether
   its author intended that or not, and would very likely fail to
   compile unless it already happens to follow scpp's move/borrow
   discipline. Language/compiler name remains open.
8. **Recoverable error handling**: exceptions, or value-based errors?
   **Settled**: no exceptions anywhere in scpp (no `throw`/`try`/`catch`).
   Every failure is either a *bug* (aborts -- already settled by Q3 above)
   or a *recoverable, expected condition* (an ordinary
   `std::expected<T, E>` value, mandatorily checked by the compiler -- see
   [§5.6](ch05-static-checks.md)). Constructors/destructors follow the
   same split (see [§4.2](ch04-struct-vs-class.md)): they may abort on a
   precondition violation, but cannot produce a recoverable error --
   fallible construction goes through a `static` factory function
   returning `std::expected<T, E>` instead (the classic C++
   "named constructor idiom"). **Propagating** a `std::expected`'s error
   up to the caller uses plain `if`/`else` in v0.1 -- a Rust-`?`-style
   postfix operator (spelled `??`, since C++ already uses a bare `?` for
   the ternary operator) was considered and **rejected**: unlike every
   other piece of scpp syntax -- all of it spelled as an attribute in the
   `scpp` namespace -- a brand-new operator token has no silently-ignored
   fallback: a real C++ compiler already accepts an attribute it doesn't
   recognize unmodified, but a real compiler hard-errors
   parsing past the second `?` (trigraphs, the only thing that ever gave
   `??` meaning, were removed in C++17), which would have permanently
   broken the property that a well-formed scpp file is already accepted
   by a real C++ compiler (see [ch00](ch00-design-philosophy.md) §2).
   Revisiting this is deferred until the C++ standard itself evolves
   further in this area.
9. **Are `const T*` and `T*` the same type?** An earlier draft of
   [§5.7](ch05-static-checks.md) (the `&expr` design) assumed scpp's
   `const T*`/`T*` were unified into one untracked type -- they are not,
   in either real C++ or scpp; caught and corrected in discussion. Real
   C++ has always treated them as distinct types (one-way implicit
   `T* -> const T*` conversion, `const_cast` required for the reverse),
   and Rust's `*const T`/`*mut T` enforce the identical split at compile
   time -- rejecting a write through a `*const T` even inside `unsafe`.
   **Settled**: scpp tracks the distinction properly (a new
   `is_mutable_pointee` flag, mirroring how `is_mutable_ref` already
   distinguishes `T&`/`const T&`); the one-way implicit conversion is
   real C++'s own existing rule, not a new one; writing through a
   `const T*` is an ordinary, always-enforced type error, not something
   `[[scpp::unsafe]]` relaxes. No `const_cast` equivalent exists in v0.1 (see
   [§5.7](ch05-static-checks.md)).
10. **Namespace design**: how much of C++'s namespace feature does scpp
    support, and how does it interact with modules? **Settled**: scpp
    reuses real C++ namespace syntax verbatim (including C++17's one-line
    nested-namespace definition) with three permanent restrictions -- no
    `using namespace` anywhere (only single-name `using foo::bar;` is
    allowed), no anonymous namespaces (redundant with the module
    export-surface mechanism, [§11.3](ch11-modules-and-libraries.md)),
    and no argument-dependent lookup (ADL) at all, ever -- a call always
    resolves from lexical scope and explicit `using` declarations, never
    from an argument's type (see
    [§11.4](ch11-modules-and-libraries.md)). A new rule, with no real C++
    analogue, ties namespaces to modules at the export boundary: an
    `export`-marked declaration is only actually exported if it's
    lexically inside a namespace matching (as a prefix, deeper nesting
    allowed) the current module's own dotted name (see
    [§11.5](ch11-modules-and-libraries.md)) -- deliberately with no
    implicit/default namespace (an earlier draft considered one; rejected
    because it can't survive erasure, see
    [ch00](ch00-design-philosophy.md) §2/§6). This turns real C++'s
    "which header defines this qualified name" guesswork into a
    mechanically guaranteed fact: any fully-qualified name determines
    exactly one module to `import`. Qualified-name resolution across
    several imported modules uses longest-prefix-match against the set of
    actually-imported module names; if two imported modules could both
    resolve the same qualified name, it's a compile error ("ambiguous
    qualified name") rather than a silent longest-match-wins pick, for
    the same reason ADL is rejected: a later, unrelated `import` should
    never silently change what existing code means.
11. **Function overloading**: does scpp allow multiple functions sharing a
    name, and how are candidates resolved? **Settled**: yes, distinguished
    by parameter list only, never return type (see
    [§5.10](ch05-static-checks.md)). Real C++'s own overload resolution
    ranks implicit-conversion sequences (Exact Match > Promotion >
    Conversion > ...) -- verified against real compiler behavior while
    designing this, this turns out to be considerably more surprising
    than it looks: promotion targets specifically `int`/`unsigned
    int`/`double`, not "the nearest wider type" (so `int8_t` competing
    for `int16_t`/`int32_t`/`int64_t` overloads picks whichever aliases
    the platform's actual `int`, not the closest one), and two merely
    "ordinary conversion"-tier candidates (e.g. `int16_t` and `int64_t`
    competing for an `int32_t` argument) are flatly ambiguous, with no
    narrower-wins tie-break at all. Considered and rejected matching
    Java/C#'s alternative (their widening-conversion chain prefers
    whichever candidate needs the least widening, a real, coherent, but
    C++-incompatible rule). **Settled on matching Rust/Swift/Kotlin
    instead**: no implicit conversions between any two distinct scpp
    scalar types at all, extending `bool`/`char`'s existing rule to the
    whole numeric family (see [§6](ch06-safe-subset.md)) -- every
    conversion needs an explicit cast, full stop. This reduces overload
    resolution to plain exact-type matching, which (since two overloads
    can never share an identical parameter-type list) can never itself
    produce an ambiguous result: the only outcomes are "exactly one
    candidate matches" or "zero match" (explicit cast required).
    By-value/by-reference (`f(T)`/`f(T&)`/`f(const T&)`) is a separate,
    orthogonal axis, disambiguated for free by the existing
    explicit-`std::move`-required rule ([§5.1](ch05-static-checks.md));
    `T&` beats `const T&` for a mutable lvalue when both are viable,
    reused from real C++ -- this is what makes const/non-const method
    overloading (`get()`/`get() const`) work, resolving the gap flagged
    in [§5.9](ch05-static-checks.md). Scoping, `using`-declaration
    imports, and the deliberate absence of ADL all reuse existing rules
    unchanged (see [§5.10](ch05-static-checks.md) for the full design).
12. **Compile-time polymorphism without inheritance**: how does scpp let
    differently-shaped types share an interface, given inheritance and
    virtual functions stay deferred? **Settled**: real C++20
    `concept`/`requires`, plus the abbreviated `Concept auto`
    function-parameter form, reused verbatim -- monomorphized per
    concrete type (zero-cost, no vtable), see
    [§5.11](ch05-static-checks.md). Satisfaction is **structural**,
    exactly like real C++ (not nominal like Rust's `impl Trait for
    Type`) -- considered adding an explicit opt-in declaration to avoid
    "accidental" concept satisfaction, but rejected for the same reason
    as the `??` operator (Q8 above): real C++ has no such syntax to
    reuse, and inventing one would break erasure; revisit only if a
    future C++ standard adds one. A constrained function's body is
    checked **once, at its own definition**, against only what the
    concept guarantees -- a deliberate departure from real C++ templates
    (which defer most body-checking to instantiation), needed to keep
    checking intraprocedural the same way the rest of
    [§5](ch05-static-checks.md) already is. A compound requirement's
    return-type constraint must be spelled `std::same_as<T>` (never
    `std::convertible_to<T>`, and never a bare type name -- verified
    `-> T` alone is not legal C++ grammar at all); a requirement with no
    return-type constraint may only be used for its side effect inside
    the generic body, never bound to anything, since there is no type to
    reason about otherwise. Generic *types* (containers like a future
    `Vec<T>`), variadic templates, non-type template parameters,
    specialization, associated types, and dynamic dispatch/type erasure
    all remain out of scope for this round.
13. **Removing the `safe` keyword: is safety-by-default better than
    safety-opt-in?** An earlier design had two keywords, `safe` and
    `unsafe`: an unmarked function (called a "native function" in earlier
    drafts) received *no* checking at all, not even
    [§5.1-§5.4](ch05-static-checks.md)'s move/borrow/lifetime analysis;
    `safe` opted a function into full checking. **Settled: reversed.**
    scpp now has no `safe`/`unsafe` keyword at all -- every function is
    checked ([§5](ch05-static-checks.md)) unconditionally, by default,
    with no annotation; `[[scpp::unsafe]]` never relaxes
    [§5.1-§5.4](ch05-static-checks.md)'s move/borrow/lifetime analysis for
    any function, marked or not -- only [§5.5](ch05-static-checks.md)'s
    fixed operation list, and (for a function marked `[[scpp::unsafe]]`
    itself) which callers may call it (see [ch01](ch01-safety-context.md)).
    The "native function" concept is
    **fully retired**, not merely renamed: there is no longer any way to
    write a function with zero move/borrow/lifetime checking -- if a
    function's body genuinely needs broad access to
    [§5.5](ch05-static-checks.md)'s operations, there are two ways to
    express that, depending on who needs to vouch for it (see
    [§1.2](ch01-safety-context.md)): wrap the entire body in one
    `[[scpp::unsafe]] { }` block if the function can still vouch for
    itself under any input it accepts, so ordinary code should keep being
    able to call it freely; or mark the function's own declaration
    `[[scpp::unsafe]]` if its soundness instead depends on a precondition
    only the caller can guarantee (scpp's equivalent of Rust's `unsafe
    fn`), which additionally requires every caller to vouch for the call
    itself. Either way, [§5.1-§5.4](ch05-static-checks.md) stays fully
    active throughout the body -- exactly mirroring how Rust's
    own `unsafe fn` body is never exempt from borrow checking, at any
    edition. This was accepted specifically because scpp has no existing
    codebase to onboard incrementally yet (unlike a hypothetical
    "gradually adopt scpp in a legacy C++ project" scenario, where
    everything defaulting to fully checked would make an unmodified file
    fail to compile) -- and because closing the remaining gaps in v0.1's
    subset (`std::vector`, `std::string`, templates,
    inheritance, etc., see [§6](ch06-safe-subset.md)) doesn't need
    a long-lived escape hatch, so no permanent "opt out of checking
    entirely" mechanism is needed to cover them. One direct, load-bearing
    consequence: [§8](ch08-open-questions.md) Q7's file extension
    (`.scpp`, not `.cpp`) exists specifically so a plain, unmodified
    `.cpp` file is never accidentally fed to the scpp compiler and
    silently subjected to checking its author never asked for.
14. **Move construction/assignment: user-written, or compiler-only?**
    Real C++ lets a class define its own `T(T&&)`/`operator=(T&&)` with
    arbitrary logic. **Settled: compiler-only, no exceptions.** A program
    that declares either for any `class` type is rejected outright; every
    `class` instead gets a compiler-synthesized move constructor and move
    assignment operator that recursively move each member -- real C++'s
    own *implicitly-defined* versions, verbatim, just never
    user-overridable (see [§4.2](ch04-struct-vs-class.md)). Verified
    against real Rust (rustc) first: Rust has no "move constructor"
    concept at all -- a move is unconditionally a bitwise copy plus
    compile-time invalidation of the old binding, full stop, with no
    trait or hook to customize it. Custom logic that needs to run when a
    value changes hands lives in `Clone` instead (arbitrary logic,
    but always an explicit `.clone()` call, never implicitly triggered by
    an ordinary move/assignment) -- and a type implementing `Copy` cannot
    also implement `Drop` (verified: `rustc` rejects it, error E0184),
    closing off the exact "silently bitwise-duplicate a
    resource-owning type" hazard real C++'s implicit special member
    function rules still allow today (declaring only a destructor still
    merely *deprecates*, rather than deletes, the implicitly-declared
    copy constructor -- [depr.impldec]). Move is likewise always exactly
    one thing, structural and compiler-owned, never a place for
    author-supplied logic to go wrong. A concrete consequence: self-move-
    assignment (`x = std::move(x)`) needs no `this != &other` guard --
    real C++'s classic footgun -- because there is no user-written body
    for aliasing to break; evaluating `std::move(x)` already places `x`
    in the moved-out state before the assignment's own "destroy the old
    value" step runs, making that step a no-op for exactly this case, by
    the same rule as any other moved-out object. Copy construction/
    assignment for `class` types is settled separately -- see Q15.
15. **Copy construction/assignment: same restriction as move, or
    different?** **Settled: different -- user-written copy is allowed.**
    Unlike move, a program *may* declare its own
    `ClassName(const ClassName&)`/`operator=(const ClassName&)`, with
    arbitrary logic. A `class` gets a compiler-provided copy
    constructor only if it declares none of a copy constructor, a copy
    assignment operator, or a destructor itself; symmetrically, it gets
    a compiler-provided copy assignment operator only if it declares
    none of a copy assignment operator, a copy constructor, or a
    destructor itself (see [§4.2](ch04-struct-vs-class.md)). Declaring
    any *one* of the three therefore suppresses the *other* special
    member function's automatic generation -- there is no "mixed" state
    where one is user-written and the other silently compiler-provided;
    a `class` that declares only a copy constructor (with no
    destructor) must also declare its own copy assignment operator if
    it wants one at all, and vice versa. This is deliberately stricter
    than real C++'s own rule: real C++ only *deprecates* -- but still
    implicitly generates -- the counterpart special member function in
    exactly these circumstances, rather than omitting it
    ([depr.impldec]); the standard itself anticipates going further,
    noting "It is possible that future versions of C++ will specify
    that these implicit definitions are deleted" ([depr.impldec]/1).
    scpp adopts that anticipated future immediately, as a compile-time
    absence of the function (an error at the point of use) rather than
    a mere warning. This is scpp's version of Rust's `Clone` -- but
    reusing real C++'s own copy-constructor/assignment spelling and its
    ordinary *implicit* triggering (`Foo b = a;`, `b = a;`), rather than
    requiring an explicit `.clone()` call the way Rust does. The reason
    copy can safely allow what move cannot: Rust's own `Copy`+`Drop` ban
    exists specifically to stop a resource-owning type from being
    *silently, implicitly* bitwise-duplicated -- scpp reaches the
    identical guarantee without banning custom logic outright, simply
    by never auto-generating a copy for any `class` with a destructor in
    the first place; a resource-owning `class`'s author must explicitly
    opt back in with their own reviewed copy constructor/assignment
    (typically incrementing a reference count or duplicating owned data,
    e.g. a hand-rolled `shared_ptr`-like type) for it to be copyable at
    all. A direct consequence of allowing user-written logic:
    self-copy-assignment (`x = x`) safety is guaranteed only for the
    compiler-provided case (which, per the rule above, never coexists
    with a user-declared destructor, copy constructor, or copy
    assignment operator, so there is nothing to double-release
    regardless of aliasing) -- a user-written copy assignment operator
    must defensively check `this != &other` itself if it needs to,
    exactly like real C++, since scpp does not restrict what it may do.
16. **Function pointers: does scpp need them, and how do safe/unsafe
    interact with them?** Real C++'s `RetType (*)(ParamTypes...)` was
    entirely undesigned until now (see [§5.10](ch05-static-checks.md)'s
    former deferred note). **Settled: reused verbatim, plus an
    unsafe-qualified/not-unsafe-qualified type split** (see
    [§5.16](ch05-static-checks.md#516-function-pointers)). The core
    risk: [§1.2](ch01-safety-context.md) already lets a function's own
    declaration require callers to wrap the call in `[[scpp::unsafe]] {}`
    -- if taking that function's address produced an ordinary, ungated
    pointer value, storing it in a plain-looking variable would let any
    caller invoke it without ever entering an unsafe context, defeating
    the function-level marker entirely. Verified against real Rust
    first: Rust's own `fn(...)` vs `unsafe fn(...)` pointer types are
    exactly this split (confirmed with `rustc`: calling an `unsafe fn`
    pointer outside `unsafe {}` is rejected, error E0133; a safe `fn`
    implicitly coerces to the `unsafe fn` pointer type, never the
    reverse). scpp's version spells the marker `[[scpp::unsafe]]`
    immediately after the `*` in a pointer-to-function declarator
    (`int (* [[scpp::unsafe]] p)(int)`) -- the one placement real C++
    attribute grammar already accepts on a pointer declarator
    (`T* [[attr]] p;`), verified against real clang; two alternatives
    were tried and rejected first: immediately before the `*` (where
    MSVC's `__stdcall` goes) is not a position real `[[...]]` attribute
    grammar accepts at all (clang: "an attribute list cannot appear
    here"), and after the parameter list parses but attaches to the
    function type rather than the pointer itself. Taking the address of
    an ordinary function yields the not-unsafe-qualified type; taking
    the address of a function marked `[[scpp::unsafe]]`, or of a
    bodyless `extern "C"` declaration, yields the unsafe-qualified type
    -- automatically, with nothing to annotate at the point the address
    is taken. Conversion is one-directional (not-unsafe-qualified →
    unsafe-qualified only), mirroring real C++17's identically-shaped
    rule for `noexcept` function pointers ([conv.fctptr]: a pointer to a
    `noexcept` function converts to a plain one, never the reverse).
    Calling through an unsafe-qualified pointer joins
    [§5.5](ch05-static-checks.md)'s gated-operation list; calling
    through one that isn't is an ordinary operation, and -- since
    invoking code at an address is a distinct operation from
    dereferencing a pointer to read data -- never needs
    `[[scpp::unsafe]]` regardless. A function pointer of either flavor
    is trivial, exactly like a raw data pointer (see
    [§4.1](ch04-struct-vs-class.md)): no compiler-tracked lifetime, no
    move/borrow tracking, freely copyable, legal as a `struct` member.
    This also resolves [§5.10](ch05-static-checks.md)'s own deferred
    item: `&overloaded_name` now picks the one overload whose type
    exactly matches the target pointer-to-function type, real C++'s own
    target-type-driven rule, reused verbatim. Deferred, with no forcing
    use case yet: pointer-to-member-function, and a function that
    itself returns a function pointer (the outer function's own
    leading/trailing attribute positions would need disambiguating from
    the returned pointer's).
17. **Should scpp support user-defined dereference operators, and if so,
    how much of real C++'s operator surface comes with them?** **Settled:
    yes for `operator*`, no separate `operator->`, and no new borrow rule.**
    Verified against the current compiler first: a `class` may declare
    `operator*()` / `operator*() const`; `*x` then works on that class,
    `x->y` works through the pre-existing `(*x).y` rewrite, while
    `operator->` and arithmetic operator names such as `operator+` are
    currently rejected. The key design choice is that `*x` is **not** a
    new primitive with its own ownership/borrowing semantics: the
    compiler simply desugars it to an ordinary method call
    (`x.operator*()`), so [§5.9](ch05-static-checks.md)'s existing
    `this`-as-reference-parameter machinery, and
    [§5.3](ch05-static-checks.md)'s existing "returned reference borrows
    from the receiver" rules, do all the work already. This was confirmed
    empirically too: keeping `int& r = *b;` alive and then attempting
    `std::move(b)` is rejected by the same old "cannot move while
    borrowed" rule, with no `operator*`-specific exception. `operator->`
    was deliberately left out because scpp already gets the useful `x->y`
    spelling for free from the one rewrite it already had
    (`x->y` → `(*x).y`); adding a second, separately overloadable operator
    name would only create another path to the same borrow-checked effect,
    with no new expressive power needed today. See
    [§5.17](ch05-static-checks.md#517-dereference-operators-on-classes).
18. **Should `class` values be allowed to cross call/return boundaries by
    value, or should scpp keep the old "references only except special
    cases" rule?** **Settled: ordinary by-value class parameters and
    returns are allowed, and they reuse the same copy/move semantics every
    other class construction site already uses.** Verified against the
    current compiler first: a copyable lvalue passed to a by-value
    parameter invokes the copy constructor; a move-only class crosses a
    by-value parameter or return boundary only as a **fresh value** (e.g.
    `std::move(x)` or a call already returning that class by value); and
    once inside the callee the parameter is just an ordinary local object,
    so moving out of it marks that parameter moved-out and suppresses its
    destructor exactly like any other moved-out local. One more detail was
    worth settling explicitly because real C++ gets it badly: overload
    viability is **copyability-aware**. With a noncopyable bare local,
    `choose(Token)` is simply not a viable candidate, so a `choose(Token&)`
    overload can still win. Real C++ itself was checked here too: the same
    shape can be left ambiguous even though the by-value path cannot
    actually be taken, because viability and deleted-copy diagnosis happen
    later than the overload-ranking step. scpp rejects that footgun by
    making "this boundary cannot actually be constructed" stop the
    candidate from participating at all. See
    [§4.2](ch04-struct-vs-class.md) and
    [§6.6](../spec/en/02-ownership-and-move.md#66-by-value-parameters-of-class-type-exprcall)/
    [§6.7](../spec/en/02-ownership-and-move.md#67-by-value-return-of-class-type-stmtreturn).
19. **Should scpp have owning, type-erased callable wrappers like
    `std::function`, and if so should they keep real C++'s nullable/empty
    state?** **Settled: yes to both `std::function<Sig>` and
    `std::move_only_function<Sig>`, no to an internal empty state, and no
    deep compiler builtin.** Real C++ was rechecked here directly:
    constructing `std::function<int(int)>` from a move-only callable
    triggers the standard library's own "target must be copy-constructible"
    failure, while `std::move_only_function` accepts move-only callables
    and supports ref/cv-qualified signatures such as `void() const`,
    `void() &`, and `void() &&`; both real wrappers are also
    default-constructible, `operator bool`-testable, and `std::function`
    throws `std::bad_function_call` when an empty value is invoked. Rust
    was checked too: `Box<dyn Fn(i32) -> i32>` has no analogous default
    empty state at all -- optionality is spelled as `Option<Box<dyn Fn...>>`.
    scpp follows that shape instead: if a program may or may not have a
    callable, it should say so in the type with
    `std::optional<std::function<Sig>>` /
    `std::optional<std::move_only_function<Sig>>`, not hide absence inside
    the wrapper. The wrappers themselves are library types, not magic
    compiler-known names: once generic class templates can express
    multi-parameter specializations and decompose a function-type template
    argument `R(Args...)`, the rest is ordinary library code. The only
    compiler-level help accepted is that one decomposition step, analogous
    to real C++ partial-specialization matching on a function type. A
    `release()`-style extraction operation is acceptable too, but only in
    a consuming form that leaves the wrapper itself moved-out, not
    "alive but empty". See
    [§5.18](ch05-static-checks.md#518-type-erased-call-wrappers-stdfunction-and-stdmove_only_function).

---

[← Previous: Compilation Pipeline](ch07-compilation-pipeline.md) · [Table of Contents](README.md) · [Next: MVP Milestones →](ch09-milestones.md)
