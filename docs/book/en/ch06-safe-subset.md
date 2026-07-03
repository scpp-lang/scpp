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
- Function calls (callee must be `safe`, otherwise `unsafe {}`).
- Arithmetic / logical / comparison operators.
- `if` / `while` / `return`. (`for`/range-for are **not implemented yet**
  -- iteration has to be hand-written with `while` for now; the lexer
  keeps a `for` keyword reserved, but there's no corresponding
  statement form in the parser/AST yet.)
- Member access, subscript (fixed-size arrays, `span` -- `span` carries a
  runtime bounds check, see [§8](ch08-open-questions.md)).

**Not yet supported (safe-region backlog)**
- Templates / generics, `concept`.
- Full checking for user-defined `class` types (constructors/destructors,
  borrows inside method bodies; see [§4.2](ch04-struct-vs-class.md)).
- Inheritance, virtual functions.
- Exceptions.
- Lifetime checking of lambdas capturing references.
- The full aliasing model for `shared_ptr`.
- Complex cross-function lifetimes (cases requiring explicit annotations).
- `for`/range-for, `char`/`float`/`double`, `std::vector`,
  `std::string`/`std::string_view`, `unsafe { }` blocks (and, with them,
  raw pointer dereference).

---

[← Previous: Static Checks in Safe Regions](ch05-static-checks.md) · [Table of Contents](README.md) · [Next: Compilation Pipeline →](ch07-compilation-pipeline.md)
