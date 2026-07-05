# 4. Struct vs Class Semantics (Fixed Memory Layout / ABI)

This is a key departure from C++: in C++, `struct` and `class` are nearly
identical (differing only in default visibility). In scpp they have
completely different semantics.

## 4.1 `struct`: a purely trivial aggregate

- `struct` may only contain members of **trivial** types, defined
  recursively:
  - Scalar types: `bool`, integers, floats, `char`.
  - Raw pointers `T*` (carry no compiler-tracked lifetime; dereferencing
    still requires `unsafe {}`, see [§2](ch02-boundary-rules.md)).
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
- Full checking rules for user-defined `class` types (constructors/
  destructors, borrows across method bodies, etc.) are out of scope for
  v0.1 — see the backlog in [§8](ch08-open-questions.md). v0.1 first
  checks only the standard-library-provided owning type `unique_ptr`
  (milestone M2).
- **Fallible construction and destruction** (design finalized, ahead of
  the rest of `class` support): a constructor or destructor has no
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
- **Access control** (design finalized, ahead of the rest of `class`
  support): a member **variable** -- including a class-level constant --
  can never be `public`; only member **functions** can be. Writing
  `public:` above a member variable is a compile error. External code
  can therefore only ever reach a class's data through a method call,
  never through direct field access -- this is also what keeps the
  method-borrow-checking design in
  [§5.9](ch05-static-checks.md) tractable: the borrow checker only has
  to reason about *method calls* crossing a class's boundary, never
  about arbitrary external field-level aliasing the way it already does
  for `struct`.
  - A class-level constant is exposed via a `static consteval` function
    instead of a public data member (see [§6](ch06-safe-subset.md) for
    scpp's `consteval`-only rule for compile-time functions) -- this has
    the exact same (zero) runtime cost a public constant would have had.
- **No inheritance in v0.1** (deferred, not a permanent exclusion -- see
  [§8](ch08-open-questions.md)): `protected` is consequently not a
  recognized access specifier either, since it only has meaning
  relative to derived classes. Revisit both together if/when
  inheritance is designed. Compile-time polymorphism (calling into
  differently-shaped types through a shared, checked interface) doesn't
  need to wait for this, though -- see
  [§5.11](ch05-static-checks.md)'s `concept`/`requires`-based generic
  functions.
- **Interior mutability, phase 1** (design finalized; answers
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
    error, unconditionally, `safe` or `unsafe` alike.
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

[← Previous: Syntactic Sugar](ch03-syntactic-sugar.md) · [Table of Contents](README.md) · [Next: Static Checks in Safe Regions →](ch05-static-checks.md)
