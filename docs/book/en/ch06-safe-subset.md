# 6. The Safe Subset Supported in v0.1

Inside safe regions, **only** the following syntax is supported; everything
else reports `E-UNSUPPORTED-IN-SAFE` (explicitly distinct from "unsafe",
meaning "sound checking not yet implemented"):

**Types**
- Scalar primitives: `bool`, `int`. (`float`/`char` are planned but **not
  implemented yet** -- no corresponding lexer/type support exists;
  `std::string`/`std::string_view` need a `char` type first and are
  likewise unimplemented.)
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
- `for`/range-for, `char`/`float`/`double`, `std::vector`,
  `std::string`/`std::string_view`. `reinterpret_cast`, `union`, raw
  `new`/`delete`, and global variables have no syntax at all yet, so
  `unsafe { }`'s permission for them is moot until each lands.

---

[← Previous: Static Checks in Safe Regions](ch05-static-checks.md) · [Table of Contents](README.md) · [Next: Compilation Pipeline →](ch07-compilation-pipeline.md)
