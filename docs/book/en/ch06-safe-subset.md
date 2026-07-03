# 6. The Safe Subset Supported in v0.1

Inside safe regions, **only** the following syntax is supported; everything
else reports `E-UNSUPPORTED-IN-SAFE` (explicitly distinct from "unsafe",
meaning "sound checking not yet implemented"):

**Types**
- Scalar primitives: `bool`, integers, floats, `char`.
- `struct` (rules in [§4.1](ch04-struct-vs-class.md); fields of supported
  types only).
- `std::unique_ptr<T>`, `std::vector<T>`, `std::span<T>`, `std::string_view`,
  `std::string` (minimal subset).

**Expressions / Statements**
- Local variable declaration and initialization.
- `&` / `const &` borrows.
- `std::move`.
- Function calls (callee must be `safe`, otherwise `unsafe {}`).
- Arithmetic / logical / comparison operators.
- `if` / `while` / `for` (incl. range-for) / `return`.
- Member access, subscript (`vector`/`span`, with bounds semantics —
  runtime check policy per [§8](ch08-open-questions.md)).

**Not yet supported (safe-region backlog)**
- Templates / generics, `concept`.
- Full checking for user-defined `class` types (constructors/destructors,
  borrows inside method bodies; see [§4.2](ch04-struct-vs-class.md)).
- Inheritance, virtual functions.
- Exceptions.
- Lifetime checking of lambdas capturing references.
- The full aliasing model for `shared_ptr`.
- Complex cross-function lifetimes (cases requiring explicit annotations).

---

[← Previous: Static Checks in Safe Regions](ch05-static-checks.md) · [Table of Contents](README.md) · [Next: Compilation Pipeline →](ch07-compilation-pipeline.md)
