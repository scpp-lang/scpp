# 6. The Safe Subset Supported in v0.1

Inside safe regions, **only** the following syntax is supported; everything
else reports `E-UNSUPPORTED-IN-SAFE` (explicitly distinct from "unsafe",
meaning "sound checking not yet implemented"):

**Types**
- **Scalar primitives** (design finalized for the numeric family; `bool`
  implemented, the rest **not yet implemented** except `char`, currently
  being implemented):

  | scpp name | Meaning | Notes |
  |-----------|---------|-------|
  | `bool` | boolean, 1 byte wide | implemented. `false` is the bit pattern `0`, `true` is `1`. No implicit conversion to or from any other type -- unlike real C++ (`bool` implicitly promotes to `int`; any scalar contextually converts to `bool` in `if`/`while`), scpp requires an explicit cast in both directions, and `if`/`while` conditions must already be `bool`. |
  | `int8_t` / `int16_t` / `int32_t` / `int64_t` / `int128_t` | fixed-width signed integers | reuses real C++ `<cstdint>` names; `int128_t` anticipates the pending WG21 P1467 proposal (not yet standard C++, but exactly what that proposal would call it). Unlike real C++ (where exact-width types are only conditionally provided), scpp guarantees all of these unconditionally on every target -- LLVM natively supports arbitrary-width integers, so there's no platform on which scpp would need to omit `int128_t`. |
  | `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t` / `uint128_t` | fixed-width unsigned integers | same as above |
  | `int` | alias for `int32_t` | **fixed**, regardless of target platform |
  | `long` | alias for `int64_t` | **deliberately fixed** -- real C++ gives `long` a platform-defined width (64-bit on Linux/macOS's LP64, but only 32-bit on Windows's LLP64, even on a 64-bit machine). This is exactly the kind of cross-platform pitfall scpp exists to design away: scpp keeps the familiar spelling (it looks like C++) but gives it one predictable meaning everywhere (it doesn't silently change size when you switch target platforms). |
  | `unsigned int` | alias for `uint32_t` | same fixed-regardless-of-platform treatment as `int`. Unlike real C++, the bare single-word shorthand `unsigned` (meaning `unsigned int`) is **not** valid scpp -- only the full two-word spelling is accepted, to keep `unsigned`-anything unambiguous and grep-able. |
  | `unsigned long` | alias for `uint64_t` | same fixed-regardless-of-platform treatment as `long` |
  | `float32_t` / `float64_t` | IEEE-754 binary32 / binary64 | reuses real, already-standardized C++23 `<stdfloat>` names verbatim |
  | `float` | alias for `float32_t` | matches how real C++ already behaves everywhere in practice -- no `long`-style platform-divergence risk here |
  | `double` | alias for `float64_t` | same |
  | `size_t` | unsigned, pointer-width | matches real C++/Rust (`usize`) semantics: this one is **meant** to vary with the target triple's pointer width -- that's its actual job, not a pitfall to eliminate the way `long`'s ambiguity was. Contrast directly with `long` above: same *shape* of question (does this type's width depend on the platform?), opposite answer, because the two types exist for different reasons. |
  | `ptrdiff_t` | signed, pointer-width | same target-triple-following semantics as `size_t` |
  | `char` | byte value, 1 byte wide | **not an alias** for `uint8_t` or any other type -- a distinct type, with the same no-implicit-conversion rule as `bool` above: converting `char` to or from any other type requires an explicit cast. Because `char` no longer has to share identity with `uint8_t`/`int8_t`, the implementation-defined signedness that plagues real C++'s plain `char` (signed on typical x86 toolchains, unsigned by default on typical ARM ones) never surfaces here -- there's no implicit arithmetic or comparison for it to affect. This also dissolves the previously-flagged conflict with concurrent implementation work that spells `char` as signed `i8`: since `char` is no longer required to be the same type as `uint8_t`, that internal representation choice is no longer in tension with the spec. |
  | *(no `wchar_t`)* | -- | not provided at all, on purpose: real C++'s `wchar_t` is 2 bytes/UTF-16 on Windows but 4 bytes/UTF-32 on Linux and macOS -- an even worse version of the same platform-pitfall pattern as `long`/`char` above (it varies in both width *and* encoding, not just width or signedness). scpp sidesteps it by simply not providing the type, rather than trying to pin down one arbitrary choice. |

- `struct` (rules in [§4.1](ch04-struct-vs-class.md); fields of supported
  types only).
- `std::unique_ptr<T>` (implemented), `std::span<T>`/`std::span<const T>`
  (implemented, M6 slice 1 -- but currently only constructible from a
  fixed-size array, see [§3](ch03-syntactic-sugar.md)). `std::vector<T>`
  is planned but **not implemented yet** (only fixed-size arrays `T[N]`
  exist today).

**Expressions / Statements**
- Local variable declaration and initialization.
- `&` / `const &` borrows; `std::span`/`std::span<const T>` views.
- `std::move`.
- Function calls. (The "callee must be `safe`, otherwise `unsafe {}`"
  rule from [§2](ch02-boundary-rules.md) is **not yet enforced** -- calling
  a non-`safe` function from a `safe` one currently isn't rejected at all;
  this ships together with `unsafe { }` itself, see below.)
- Arithmetic / logical / comparison operators.
- `if` / `while` / `return`. (`for`/range-for are **not implemented yet**
  -- iteration has to be hand-written with `while` for now; the lexer
  keeps a `for` keyword reserved, but there's no corresponding
  statement form in the parser/AST yet.)
- Member access, subscript (fixed-size arrays, `span` -- `span` carries a
  runtime bounds check, see [§8](ch08-open-questions.md)).
- `[[scpp::lifetime(name)]]` attribute on reference parameters/declarators
  for multi-group cross-function lifetimes (see [§5.3](ch05-static-checks.md)
  -- **design finalized, not yet implemented**).
- `unsafe { }` blocks (see [§1.3](ch01-safety-context.md) -- **design
  finalized, not yet implemented**): a lexically-scoped escape hatch
  inside a `safe` function that locally permits raw pointer dereference
  and calling a non-`safe` function (the only two of
  [§5.5](ch05-static-checks.md)'s prohibited operations reachable in v0.1
  today), while every other check in [§5](ch05-static-checks.md) keeps
  running unconditionally.
- `extern "C"` function declarations/definitions (see
  [§2.1](ch02-boundary-rules.md) -- **design finalized, not yet
  implemented**), restricted to C-ABI-compatible signature types.
  Needs `extern`, minimal string-literal lexing, and `void` as a type
  first (none exist yet -- see below).

**Not yet supported (safe-region backlog)**
- Templates / generics, `concept`.
- Full checking for user-defined `class` types (constructors/destructors,
  borrows inside method bodies; see [§4.2](ch04-struct-vs-class.md)).
- Inheritance, virtual functions.
- Exceptions.
- Lifetime checking of lambdas capturing references.
- The full aliasing model for `shared_ptr`.
- Implementation of the `[[scpp::lifetime(name)]]` multi-group mechanism
  spec'd in [§5.3](ch05-static-checks.md) (design only so far; every
  cross-function case still falls back to the single-reference-parameter/
  `this` elision or the new default-group rule until this lands).
- Implementation of `unsafe { }` blocks spec'd in
  [§1.3](ch01-safety-context.md) (design only so far).
- Implementation of `extern "C"` spec'd in [§2.1](ch02-boundary-rules.md)
  (design only so far), and its three prerequisites: an `extern` keyword,
  minimal string-literal lexing (just the token `"C"`, not general string
  literals), and `void` as a valid type name (no void-returning functions
  or `void*` exist yet either, independent of `extern "C"`).
- Implementation of the numeric scalar family spec'd above (design
  finalized; `char` is currently being implemented, the rest are not
  started): `int8_t`/.../`int128_t`, `uint8_t`/.../`uint128_t`, the
  `int`/`long`/`unsigned int`/`unsigned long` fixed-width aliases,
  `float32_t`/`float64_t` (and the `float`/`double` aliases), `size_t`,
  `ptrdiff_t`.
- `for`/range-for, `std::vector`, `std::string`/`std::string_view` (need
  `char` first, in progress). `reinterpret_cast`, `union`, raw
  `new`/`delete`, and global variables have no syntax at all yet, so
  `unsafe { }`'s permission for them is moot until each lands.

---

[← Previous: Static Checks in Safe Regions](ch05-static-checks.md) · [Table of Contents](README.md) · [Next: Compilation Pipeline →](ch07-compilation-pipeline.md)
