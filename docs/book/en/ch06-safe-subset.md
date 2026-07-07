# 6. The v0.1 Supported Subset

**Only** the following syntax is supported; everything
else reports `E-UNSUPPORTED` (a construct outside this subset --
distinct from an ordinary type/borrow-check error):

**Types**
- **Scalar primitives** (the numeric family):

  | scpp name | Meaning | Notes |
  |-----------|---------|-------|
  | `bool` | boolean, 1 byte wide | `false` is the bit pattern `0`, `true` is `1`. No implicit conversion to or from any other type -- unlike real C++ (`bool` implicitly promotes to `int`; any scalar contextually converts to `bool` in `if`/`while`), scpp requires an explicit cast in both directions, and `if`/`while` conditions must already be `bool`. |
  | `int8_t` / `int16_t` / `int32_t` / `int64_t` | fixed-width signed integers | reuses real C++ `<cstdint>` names verbatim, all already-standardized. Unlike real C++ (where exact-width types are only conditionally provided), scpp guarantees all of these unconditionally on every target -- LLVM natively supports arbitrary-width integers, so there's no platform on which scpp would need to omit any of them. **No `int128_t`** for now: WG21 P1467 (a 128-bit integer type) hasn't been adopted into C++26 (see [ch00](ch00-design-philosophy.md) §7 for scpp's reference standard), and scpp's builtin vocabulary deliberately sticks to names the standard has actually ratified (see [ch00](ch00-design-philosophy.md) §2/§6) rather than anticipating a still-pending proposal's eventual spelling -- add it back once/if a future standard adopts it. |
  | `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t` | fixed-width unsigned integers | same as above -- no `uint128_t` for the same reason |
  | `int` | alias for `int32_t` | **fixed**, regardless of target platform |
  | `long` | alias for `int64_t` | **deliberately fixed** -- real C++ gives `long` a platform-defined width (64-bit on Linux/macOS's LP64, but only 32-bit on Windows's LLP64, even on a 64-bit machine). This is exactly the kind of cross-platform pitfall scpp exists to design away: scpp keeps the familiar spelling (it looks like C++) but gives it one predictable meaning everywhere (it doesn't silently change size when you switch target platforms). |
  | `unsigned int` | alias for `uint32_t` | same fixed-regardless-of-platform treatment as `int`. Unlike real C++, the bare single-word shorthand `unsigned` (meaning `unsigned int`) is **not** valid scpp -- only the full two-word spelling is accepted, to keep `unsigned`-anything unambiguous and grep-able. |
  | `unsigned long` | alias for `uint64_t` | same fixed-regardless-of-platform treatment as `long` |
  | `float32_t` / `float64_t` | IEEE-754 binary32 / binary64 | reuses real, already-standardized C++23 `<stdfloat>` names verbatim |
  | `float` | alias for `float32_t` | matches how real C++ already behaves everywhere in practice -- no `long`-style platform-divergence risk here |
  | `double` | alias for `float64_t` | same |
  | `size_t` | unsigned, pointer-width | matches real C++/Rust (`usize`) semantics: this one is **meant** to vary with the target triple's pointer width -- that's its actual job, not a pitfall to eliminate the way `long`'s ambiguity was. Contrast directly with `long` above: same *shape* of question (does this type's width depend on the platform?), opposite answer, because the two types exist for different reasons. |
  | `ptrdiff_t` | signed, pointer-width | same target-triple-following semantics as `size_t` |
  | `char` | byte value, 1 byte wide | **not an alias** for `uint8_t` or any other type -- a distinct type, with the same no-implicit-conversion rule as `bool` above: converting `char` to or from any other type requires an explicit cast. Because `char` no longer has to share identity with `uint8_t`/`int8_t`, the implementation-defined signedness that plagues real C++'s plain `char` (signed on typical x86 toolchains, unsigned by default on typical ARM ones) never surfaces here -- there's no implicit arithmetic or comparison for it to affect. |
  | *(no `wchar_t`)* | -- | not provided at all, on purpose: real C++'s `wchar_t` is 2 bytes/UTF-16 on Windows but 4 bytes/UTF-32 on Linux and macOS -- an even worse version of the same platform-pitfall pattern as `long`/`char` above (it varies in both width *and* encoding, not just width or signedness). scpp sidesteps it by simply not providing the type, rather than trying to pin down one arbitrary choice. |

  **No implicit conversions between any two distinct scalar types above,
  ever, full stop.** This was already the stated rule for `bool` and
  `char` individually; it now applies uniformly across the entire numeric
  family (e.g. `int8_t -> int16_t`, `int32_t -> float64_t`,
  `unsigned int -> long`) even when the conversion is widening and
  value-preserving -- every conversion between two distinct scpp scalar
  types requires an explicit cast, no exceptions. scpp deliberately
  follows Rust/Swift/Kotlin here rather than real C++'s implicit
  promotion/usual-arithmetic-conversion rules: real C++'s own promotion
  targets specifically `int`/`unsigned int`/`double`, not "the nearest
  wider type", which makes which overload wins depend on which built-in
  type happens to alias the platform's actual `int` -- and two candidates
  that are both merely "ordinary conversion"-tier (e.g. `int16_t` and
  `int64_t` competing for an `int32_t` argument) are flatly ambiguous in
  real C++, with no narrower-wins tie-break at all (see
  [§8](ch08-open-questions.md) Q11). Sidestepping implicit conversions
  entirely keeps every conversion visible at its call site and, as a
  direct side effect, makes function-overload resolution
  ([§5.10](ch05-static-checks.md)) reduce to plain exact-type matching.

- `struct` (rules in [§4.1](ch04-struct-vs-class.md); fields of supported
  types only).
- `class` (see
  [§4.2](ch04-struct-vs-class.md)/[§5.9](ch05-static-checks.md)): member
  variables and member functions may each be `public` or `private`, in
  any combination, exactly like real C++; no inheritance/`protected` in
  v0.1.
  A trivial-typed member may instead be declared `mutable` (see
  [§4.2](ch04-struct-vs-class.md)/
  [§5.9](ch05-static-checks.md)): readable/writable through a `const`
  `this`, but never referenceable, scpp's phase-1 (`Cell`-equivalent)
  answer to interior mutability ([§8](ch08-open-questions.md) Q4).
- `std::unique_ptr<T>`, `std::span<T>`/`std::span<const T>`
  (constructible from a fixed-size array only, see
  [§3](ch03-syntactic-sugar.md)). `std::vector<T>`
  is deferred (only fixed-size arrays `T[N]` are in scope for v0.1).
- `std::expected<T, E>` (see [§5.6](ch05-static-checks.md)): scpp's only vehicle for recoverable
  errors; a compiler builtin type, same treatment as `unique_ptr`/`span`,
  not a real libstdc++/libc++ template instantiation.
- **Generic `struct`/`class` types** (`template<typename T> class X { ... }`,
  real C++ syntax verbatim, including multiple type parameters and
  parameter packs -- see [§5.14](ch05-static-checks.md)): scpp's
  compile-time-polymorphism mechanism ([§5.11](ch05-static-checks.md))
  extended from functions to type definitions. A type parameter may be
  left bare (only move/store/pass-through/return guaranteed) or
  constrained, per method, by a `requires` clause; a generic `struct`'s
  own parameter(s) must instead be concept-constrained to guarantee
  triviality, since a `struct`'s field-triviality rule ([§4.1](ch04-struct-vs-class.md))
  is a whole-type property. A variadic generic type's storage is built
  via recursive inheritance (real C++ has no syntax to expand a pack
  directly into a member list); non-type template parameters are
  supported for scalar types only.
- **`[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]`** (see
  [§5.15](ch05-static-checks.md)): attributes, applied to a `struct`/
  `class`'s own declaration to manually assert the property (overriding
  the structural derivation), or to a generic function's parameter to
  constrain it -- computed by default the same way a real C++
  compiler-intrinsic type trait (e.g. `std::is_trivially_copyable_v<T>`)
  is, not evaluated as ordinary user-written code. Lets library code
  (e.g. a thread-spawning function) require, via an ordinary parameter
  attribute, that whatever it's handed is safe to move to, or share
  with, another thread -- mirroring Rust's `Send`/`Sync` and its
  `unsafe impl` escape hatch.

**Expressions / Statements**
- Local variable declaration and initialization.
- `&` / `const &` borrows; `std::span`/`std::span<const T>` views.
- `&expr` address-of, yielding `const T*` or `T*` depending on whether
  `expr`'s place is only reachable read-only or mutably (see
  [§5.7](ch05-static-checks.md)): always legal (no `[[scpp::unsafe]] { }`
  needed to create one -- only dereferencing a raw pointer is gated, see
  below), the concrete way ordinary code produces a pointer value
  for an `extern "C"` out-parameter. `const T*`/`T*` are genuinely
  distinct types (a one-way implicit `T* -> const T*` conversion only,
  no `const_cast` equivalent yet); writing through a `const T*` is an
  ordinary type error, unconditionally, even inside `[[scpp::unsafe]] { }`.
- `std::move`.
- Function calls, including the "calling an `extern "C"` function requires
  `[[scpp::unsafe]] {}`" rule from [§2](ch02-boundary-rules.md). Functions (free or methods) may be
  **overloaded** by parameter list, never by return type (see
  [§5.10](ch05-static-checks.md)): resolved by exact type match only, since no scpp
  scalar type implicitly converts to another (see the numeric family
  note above) -- ambiguity from pure type mismatch cannot arise as a
  result.
- **Generic functions constrained by a `concept`** (`void f(Shape auto&
  x)`, real C++20 syntax verbatim -- see
  [§5.11](ch05-static-checks.md)): scpp's compile-time-polymorphism mechanism in place of
  inheritance/virtual functions (still out of scope for v0.1, see below).
  Monomorphized per concrete type (zero-cost, no vtable); a constrained
  function's body is checked once, at its own definition, against only
  what the concept's `requires`-expression guarantees -- not deferred to
  instantiation the way real C++ templates otherwise work. A
  concept-constrained parameter may also be a pack (`Concept auto&...
  args`), usable only through a fold expression (real C++17 syntax
  verbatim) -- covers variadic call patterns (e.g. a `std::format`-style
  function) without needing to check a recursively-split pack, which is
  not supported in a function body.
- **Lambda expressions** (`[capture-list](params) { body }`, real C++
  syntax verbatim -- see [§5.12](ch05-static-checks.md)): desugars to an
  anonymous, compiler-synthesized class exactly as in real C++, so no
  new checking machinery beyond the `struct`/`class` rules
  ([§4](ch04-struct-vs-class.md)) is needed. By-value captures are
  ordinary owned members; by-reference captures are reference-typed
  members, making the closure value itself lifetime-tracked like
  `std::span`. `this`/`*this` must always be captured explicitly -- a
  bare `[=]`/`[&]` implicitly capturing `this` is a compile error (real
  C++20 only deprecates this, P0806R2).
- `consteval` functions (see [§4.2](ch04-struct-vs-class.md)): scpp's only compile-time-function
  mechanism, reused verbatim from real C++20 -- every call is
  mandatorily evaluated at compile time, a compile error if any argument
  isn't itself a constant expression. scpp has **no `constexpr`-qualified
  functions**: real C++'s `constexpr` function can be evaluated at
  compile time in a constant-expression context, or silently fall back
  to an ordinary runtime call otherwise -- *which one happens depends on
  the calling context*, not on anything visible at the function's own
  declaration, exactly the kind of context-dependent ambiguity scpp
  tries to avoid elsewhere too (see [ch00](ch00-design-philosophy.md)
  §8). Every scpp function is unambiguously one or the other: `consteval`,
  or an ordinary, undecorated function that's always a runtime call,
  never evaluated at compile time even if every argument happens to be
  constant. `constexpr` **variables** are unaffected -- `constexpr int
  x = 5;` is already unambiguous in real C++ (always a compile-time
  constant, no calling-context dependence), so there's nothing to fix
  there. If a genuine need for one function to serve both a
  compile-time and a runtime caller ever arises, revisit adding
  `constexpr` functions back rather than solving it another way.
- Arithmetic / logical / comparison operators. `+`/`-`/`*` are
  overflow-checked by default (`abort()` on overflow, both signed
  and unsigned; see [§5.8](ch05-static-checks.md)); unchecked but
  guaranteed-wrapping (never UB) inside `[[scpp::unsafe]] { }`. Division/modulo by
  zero (or `INT_MIN / -1`) always `abort()`, whether inside `[[scpp::unsafe]] { }`
  or not.
- `if` / `while` / `return`. (`for`/range-for are deferred beyond v0.1
  -- iteration is expressed with `while` for now.)
- Member access, subscript (fixed-size arrays, `span` -- `span` carries a
  runtime bounds check by default, skipped inside `[[scpp::unsafe]] { }`,
  see [§8](ch08-open-questions.md)).
- `[[scpp::lifetime(name)]]` attribute on reference parameters/declarators
  for multi-group cross-function lifetimes (see [§5.3](ch05-static-checks.md)).
- `[[scpp::unsafe]] { }` blocks (see [§1.3](ch01-safety-context.md)): ordinary
  compound-statements carrying the `[[scpp::unsafe]]` attribute, a lexically-scoped escape hatch that locally
  permits raw pointer dereference and calling an `extern "C"` function (the
  operations from [§5.5](ch05-static-checks.md)'s prohibited list that
  are in scope for v0.1; the rest of that list is deferred beyond v0.1,
  see below), while every other check in
  [§5](ch05-static-checks.md) keeps running unconditionally.
- The function-level `[[scpp::unsafe]]` marker (see
  [§1.2](ch01-safety-context.md)): the same attribute, applied instead to
  a function's own declaration, making the entire body an unsafe context
  and making calls to that function themselves one of
  [§5.5](ch05-static-checks.md)'s gated operations -- scpp's equivalent
  of Rust's `unsafe fn`, for a function whose soundness depends on a
  precondition only its caller can guarantee.
- `extern "C"` function declarations/definitions (see
  [§2.1](ch02-boundary-rules.md)), restricted to
  C-ABI-compatible signature types, including `void` as a return type
  and a pointer's pointee; array parameters
  (`T[N]`) decay to `T*`, matching ordinary C++.
- **No exceptions** (`throw`/`try`/`catch`) -- deliberately excluded from
  scpp entirely, not a backlog item: recoverable errors are
  `std::expected<T, E>` values, propagated with ordinary `if`/`else` (see
  [§5.6](ch05-static-checks.md)); its return value is **mandatorily
  checked** -- silently discarding one is a compile error, not a lint, as
  if every such function were implicitly declared `[[nodiscard]]`.
  Unrecoverable failures (contract violations, bounds checks,
  precondition violations in a constructor/destructor) `abort()` instead
  (see [§5.6](ch05-static-checks.md)/[§8](ch08-open-questions.md)).

**Out of scope for v0.1**
- General/arbitrary template specialization (beyond the one fixed
  empty-pack/head-and-tail pattern a variadic generic type may use, see
  [§5.14](ch05-static-checks.md)), template-template parameters, default
  template arguments, class-typed non-type template parameters,
  associated types, and recursive pack-splitting inside a function body
  -- all explicitly out of scope for
  [§5.11](ch05-static-checks.md)/[§5.14](ch05-static-checks.md)'s generic
  design too.
- Inheritance and virtual functions for `class` types (and therefore
  `protected`) -- see [§4.2](ch04-struct-vs-class.md).
- The full aliasing model for `shared_ptr`.
- `for`/range-for, `std::vector`, `std::string`/`std::string_view`,
  `reinterpret_cast`, `union`, raw `new`/`delete`, and global variables.

---

[← Previous: Static Checks](ch05-static-checks.md) · [Table of Contents](README.md) · [Next: Compilation Pipeline →](ch07-compilation-pipeline.md)
