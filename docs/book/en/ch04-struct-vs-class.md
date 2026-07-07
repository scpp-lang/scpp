# 4. Struct vs Class Semantics (Fixed Memory Layout / ABI)

This is a key departure from C++: in C++, `struct` and `class` are nearly
identical (differing only in default visibility). In scpp they have
completely different semantics.

## 4.1 `struct`: a purely trivial aggregate

- `struct` may only contain members of **trivial** types, defined
  recursively:
  - Scalar types: `bool`, integers, floats, `char`.
  - Raw pointers `T*` (carry no compiler-tracked lifetime; dereferencing
    still requires `[[scpp::unsafe]] {}`, see [§2](ch02-boundary-rules.md)).
  - Function pointers (either the unsafe-qualified or the
    not-unsafe-qualified pointer-to-function type, see
    [§5.16](ch05-static-checks.md#516-function-pointers)) -- likewise
    carry no compiler-tracked lifetime.
  - Other `struct` types that themselves satisfy this rule (recursively).
  - Fixed-size arrays of trivial types.
- The following are **forbidden** as `struct` members and must instead be
  wrapped in a `class`:
  - References `T&` / `const T&`.
  - `std::span<T>`, `std::string_view` (lifetime-checked borrowed views).
  - `std::unique_ptr<T>`, `std::shared_ptr<T>`, `std::vector<T>`,
    `std::string`, or any type participating in ownership/borrow checking.
- A `struct` definition that violates this rule is a compile error (advising
  "use `class` instead"), not a silent downgrade.

**Initialization**: an uninitialized `struct` local/member is guaranteed by
the compiler to be entirely **zero-initialized** (bitwise). Scalar members
become `0` / `false` / `0.0`; raw pointer members become `nullptr`. This
isn't special to `struct` -- it's a general rule shared by **every** type in
scpp; see [§5.4](ch05-static-checks.md) for the full explanation.

**Copy semantics**: `struct` values may be freely and implicitly copied
bitwise, and do not participate in the move/borrow checking described in
[§5](ch05-static-checks.md) — they carry no lifetime or
exclusive-ownership semantics at all. This is not an opt-in `Copy` trait
(contrast Rust's explicit `#[derive(Copy)]`): declaring a type `struct` is
itself the explicit declaration, and the compiler verifies triviality.

## 4.2 `class`: types that own resources / participate in checking

- `class` may contain members of any type, including `unique_ptr`, `vector`,
  `span`, other `class` types, or fields that themselves carry lifetime or
  ownership semantics.
- `class` participates in the ownership/move/borrow/lifetime checks
  described in [§5](ch05-static-checks.md); it is **not** guaranteed to
  be zero-initialized (requires explicit construction).
- **Construction and destruction**: a `class` may declare one or more
  constructors (real C++ overloading rules, [§5.10](ch05-static-checks.md),
  apply directly -- disambiguated by parameter list, exactly like any
  other overloaded name) and, optionally, a destructor, using real C++
  syntax verbatim. `std::unique_ptr` is just one ordinary library `class`
  that happens to use these rules; any user-defined `class` may likewise
  own a resource and define its own cleanup logic.
- **Move construction and move assignment are never user-written --
  the compiler always provides them**: a program that declares its own
  `ClassName(ClassName&&)` or `operator=(ClassName&&)` is rejected, for
  *every* `class` type, unconditionally. Instead, every `class` gets a
  compiler-synthesized move constructor and move assignment operator
  that recursively move each member in declaration order (scalars/raw
  pointers/`struct` members bitwise, `class`-typed members via their own
  compiler-provided move) -- real C++'s own *implicitly-defined* move
  constructor/assignment, verbatim, just never user-overridable. This is
  a deliberate, permanent restriction, not a temporary one, for the same
  reason real C++ move constructors are a well-known footgun category:
  a hand-written one that forgets to reset the source (e.g. null out a
  raw pointer field after taking it) leaves two live objects owning the
  same resource, both of which will eventually try to free it. Verified
  against real Rust (`rustc`) first: Rust has no "move constructor"
  concept at all -- a move is unconditionally a bitwise copy plus
  compile-time invalidation of the old binding, full stop, with no trait
  or hook to customize it. Custom logic that needs to run when a value
  changes hands lives in `Clone` instead (arbitrary logic, but always an
  explicit `.clone()` call, never implicitly triggered by an ordinary
  move/assignment) -- and a type implementing `Copy` cannot also
  implement `Drop` (verified: `rustc` rejects it, error E0184), closing
  off the exact "silently bitwise-duplicate a resource-owning type"
  hazard real C++'s own implicit special member function rules still
  allow today (declaring only a destructor still merely *deprecates*,
  rather than deletes, the implicitly-declared copy constructor --
  [depr.impldec]). Move is always exactly one thing, structural and
  compiler-owned, never a place for author-supplied logic to go wrong --
  see [§8](ch08-open-questions.md) Q14 for this decision's full record.
  - The moved-from object is placed in the *moved-out* state
    ([§5.1](ch05-static-checks.md)), exactly like moving any other value
    -- its destructor, if it has one, is then never invoked for it
    ([§5.1](ch05-static-checks.md)), a compile-time-decided omission, not
    a runtime null-check the way hand-written C++ typically achieves the
    same safety.
  - Because moving is always this same compiler-provided, purely
    structural operation, self-move-assignment (`x = std::move(x)`)
    needs no defensive `this != &other` check the way real C++ does:
    evaluating `std::move(x)` places `x` in the moved-out state before
    the assignment's own "destroy the old value" step runs, so that step
    is always a no-op for this case, by the same rule as any other
    moved-out object.
  - A `class` with a non-static data member of reference type (`T&`/
    `const T&`) has no move assignment operator at all -- exactly real
    C++'s own rule ([class.copy.assign]) for the same reason (a
    reference cannot be re-seated by assignment) -- only a move
    constructor (references can still be bound once, at construction).
- **Copy construction and copy assignment, unlike move, may be
  user-written** -- real C++ syntax verbatim
  (`ClassName(const ClassName&)`/`operator=(const ClassName&)`), with
  whatever logic the author wants. A `class` gets a compiler-provided
  copy constructor only if it declares **none** of a copy constructor,
  a copy assignment operator, or a destructor itself; symmetrically, it
  gets a compiler-provided copy assignment operator only if it declares
  **none** of a copy assignment operator, a copy constructor, or a
  destructor itself. Declaring any *one* of the three is therefore
  enough to suppress the *other* special member function's automatic
  generation -- there is no "mixed" state where one is user-written and
  the other silently compiler-provided; a `class` that declares only a
  copy constructor (with no destructor) must also declare its own copy
  assignment operator if it wants one at all, and vice versa. See
  [§8](ch08-open-questions.md) Q15 for this decision's full record.
  - This is deliberately **stricter** than real C++'s own rule: real
    C++ only *deprecates* -- but still implicitly generates -- the
    copy constructor when the class has a user-declared destructor or
    copy assignment operator (and symmetrically deprecates, but still
    generates, the copy assignment operator when the class has a
    user-declared destructor or copy constructor), rather than omitting
    the function outright ([depr.impldec]). The C++ standard itself
    flags this as a stopgap, not a final answer: "It is possible that
    future versions of C++ will specify that these implicit definitions
    are deleted" ([depr.impldec]/1). scpp simply adopts that
    already-anticipated future now, turning what real C++ treats as a
    deprecation warning at the point of use into an outright absence of
    the function -- a compile error at the point of use, not a warning.
  - This is scpp's version of Rust's `Clone`, reusing real C++'s own
    copy-constructor/assignment spelling and its ordinary implicit
    triggering (`Foo b = a;`, `b = a;`) instead of an explicit
    `.clone()` call -- deliberately, unlike move, where scpp mirrors
    Rust's *no custom logic at all* rule exactly (see above). The
    reason copy can safely allow what move cannot: Rust's own ban on
    combining `Copy` with `Drop` exists to stop a resource-owning type
    from being *silently, implicitly* bitwise-duplicated -- and scpp
    already achieves that without needing to ban custom logic outright,
    simply by never auto-generating a copy for a `class` with a
    destructor in the first place. A resource-owning `class`'s author
    must explicitly opt back in with their own reviewed copy
    constructor/assignment (typically incrementing a reference count or
    duplicating owned data, e.g. a hand-rolled `shared_ptr`-like type) --
    there is no scenario where copying silently duplicates a resource
    the author never actively wrote code to duplicate correctly.
  - Because user-declared logic is now possible, self-copy-assignment
    (`x = x`) safety is **only** guaranteed for the compiler-provided
    case (which, per the rule above, never coexists with a
    user-declared destructor, copy constructor, or copy assignment
    operator, so there is nothing to double-release regardless of
    aliasing) -- a user-written copy assignment operator must
    defensively check `this != &other` itself if it needs to, exactly
    like real C++, since scpp is not restricting what it may do.
  - Same reference-member carve-out as move: a `class` with a
    non-static data member of reference type may have a compiler-provided
    copy constructor, but never a compiler-provided copy assignment
    operator (a user-written one is still permitted, subject to whatever
    that reference member allows the author to actually do in its body).
- **Fallible construction and destruction**: a constructor or destructor has no
  channel to hand back a `std::expected<T, E>` -- scpp has no exceptions
  to throw through instead (see [§5.6](ch05-static-checks.md)/
  [§8](ch08-open-questions.md)), and neither special member function
  returns an ordinary value. This isn't a new rule to enforce; it falls
  out for free from scpp having no exceptions anywhere.
  - A constructor/destructor may still validate preconditions, but a
    violation can only be handled by aborting (a *bug*, in the
    [§8](ch08-open-questions.md) Q3 sense) -- never by producing a
    recoverable error.
  - A type whose construction can genuinely fail for recoverable reasons
    (file not found, invalid input, ...) must not expose that failure
    through a constructor at all. Model it instead as an ordinary
    `static` member function returning `std::expected<T, E>`, which
    constructs the object (via an always-succeeds constructor) only once
    success is guaranteed. Recommended, though only ordinary C++ access
    control is needed to enforce it: make that plain constructor
    `private`, so the factory function is the only way to obtain an
    instance -- the classic C++ "named constructor idiom" (Marshall
    Cline's C++ FAQ), requiring zero new scpp syntax.
- **Access control**: real, unrestricted C++ access control -- a member
  variable or member function may be `public` or `private` in any
  combination, exactly like real C++ (`protected` remains deferred
  alongside inheritance, see below). Direct external access to a public
  member variable (`a.b`) is checked exactly like a `struct` field
  access ([§5.2](ch05-static-checks.md)): the borrow is recorded against
  the whole root object, conservatively -- the same simplification
  `struct` already uses, not a new one invented for this. A method call
  (`a.f()`) continues to be checked via [§5.9](ch05-static-checks.md)'s
  existing this-as-reference-parameter model. The two compose with no
  new mechanism: both ultimately record a borrow against the same root
  object, so a live borrow of `a.b` correctly conflicts with a call to a
  mutating method on `a`, exactly as two conflicting borrows of the same
  root always do.
  - A class-level constant may therefore be an ordinary `public` `static
    constexpr` member, real C++ syntax verbatim -- `constexpr`
    *variables*, unlike `constexpr` *functions*, are unambiguous and
    already fully supported (see [§5.11](ch05-static-checks.md)) -- or,
    if hiding its value from the public interface is wanted, a `private`
    constant exposed via a `static consteval` function instead, at the
    same zero runtime cost.
- **No inheritance in v0.1** (deferred, not a permanent exclusion -- see
  [§8](ch08-open-questions.md)): `protected` is consequently not a
  recognized access specifier either, since it only has meaning
  relative to derived classes. Revisit both together if/when
  inheritance is designed. Compile-time polymorphism (calling into
  differently-shaped types through a shared, checked interface) doesn't
  need to wait for this, though -- see
  [§5.11](ch05-static-checks.md)'s `concept`/`requires`-based generic
  functions.
- **Interior mutability, phase 1** (answers
  [§8](ch08-open-questions.md) Q4's `Cell` half, `RefCell` deferred):
  scpp reuses real C++'s `mutable` keyword, but with **stricter**
  semantics than real C++'s (which gives a `mutable` field zero
  checking of any kind). A `mutable` member variable:
  - must be a **trivial** type, by the exact same rule §4.1 already
    defines for `struct` fields (scalars, raw pointers, other trivial
    types, fixed-size arrays of those) -- matching Rust's `Cell<T>`
    requiring `T: Copy`;
  - may be read or written through *any* `this`, `const` or not --
    `const` no longer blocks direct access to this one field;
  - can **never** be referenced or have its address taken (`T&`/
    `const T&` binding, `&expr`) -- attempting either is a compile
    error, unconditionally, whether inside `[[scpp::unsafe]] { }` or not.
  Because no reference to it can ever exist, there is no aliasing
  hazard to check at runtime at all -- reads/writes compile to a plain
  load/store, exactly as cheap as Rust's `Cell::get`/`Cell::set`. This
  covers the common cases (counters, flags, small cached values); the
  `RefCell` case (borrowing an actual reference to non-trivial interior
  state, checked -- and panicking/aborting on violation -- at runtime)
  has no existing C++ name to reuse and needs real new machinery (a
  runtime borrow counter, RAII guard types); deferred to a later round.
  `mutable` is meaningless on a `struct` (which has no `this`/methods/
  const-access-control to begin with) and isn't accepted there.

## 4.3 Memory Layout & ABI (fixed, not left implementation-defined)

scpp pins `struct` memory layout to the target platform's **Clang ABI**: for
a given target triple, the layout scpp produces for a `struct` must be
byte-for-byte identical to what Clang produces for the equivalent C struct.
Concretely:

1. Members are laid out in source declaration order; the compiler **never
   reorders** them.
2. Each member is aligned per its type's alignment requirement under the
   target's Clang ABI; padding is inserted between members as needed.
3. The struct's own alignment is the maximum alignment of any member; its
   total size is rounded up to a multiple of that alignment (so arrays of
   the struct keep every element correctly aligned).
4. The first member is always at offset 0 (the struct's address equals its
   first member's address).

Implementation: the compiler emits a non-packed, named LLVM struct type and
lets the target's `DataLayout` compute the layout — the same `DataLayout`
Clang uses for the same target triple — so Clang/C-compatible layout falls
out automatically, with no separate alignment algorithm to maintain in scpp.

**Explicitly unsupported in v0.1**:
- Bit-fields — layout varies too much across platforms/compiler versions to
  pin down confidently right now.
- Packed layout (the equivalent of `#pragma pack(1)` /
  `__attribute__((packed))`) — deferred to a later version via an explicit
  attribute (e.g. `alignas` / `packed`) mapped to LLVM's packed struct type.

---

[← Previous: Syntactic Sugar](ch03-syntactic-sugar.md) · [Table of Contents](README.md) · [Next: Static Checks →](ch05-static-checks.md)
