# MVP Milestones (implementation order, end-to-end first)

- **M0**: Freeze this spec + choose the implementation language and LLVM
  bindings.
- **M1**: Minimal end-to-end. Subset: scalars + locals + `if`/`while` +
  functions -> AST -> LLVM IR -> executable with correct return value. **No
  checks yet**; get the front/back ends connected first.
- **M2**: Type system + `struct` + `unique_ptr` + move semantics (`std::move` as
  a hint); move-out checking (the simplest sound check).
- **M3**: Build MIR + initialization checking + drop insertion.
- **M4**: Borrow & alias-XOR-mutability checking (intraprocedural).
- **M5**: NLL-style lifetime inference + dangling-reference checking
  (intraprocedural) + elision rules -- see
  [§5.2](ch05-static-checks.md)/[§5.3](ch05-static-checks.md).
- **M6**: `vector`/`span`/`string_view` support + bounds-check policy +
  diagnostic quality -- see
  [§3](ch03-syntactic-sugar.md)/[§6](ch06-safe-subset.md)/
  [§8](ch08-open-questions.md).
- **M7+**: Generics/templates, traits/concepts, the `[[scpp::lifetime(name)]]`
  multi-group cross-function lifetime mechanism (see
  [§5.3](ch05-static-checks.md)), modules & libraries
  (see [ch11](ch11-modules-and-libraries.md)), standard-library expansion,
  incremental compilation.

---

[← Previous: Open Questions](ch08-open-questions.md) · [Table of Contents](README.md) · [Next: Reference Implementations →](ch10-reference-implementations.md)
