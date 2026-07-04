# 9. MVP Milestones (implementation order, end-to-end first)

- **M0**: Freeze this spec + choose the implementation language and LLVM
  bindings.
- **M1**: Minimal end-to-end. Subset: scalars + locals + `if`/`while` +
  functions -> AST -> LLVM IR -> executable with correct return value. **No safe
  checks yet**; get the front/back ends connected first.
- **M2**: Type system + `struct` + `unique_ptr` + move semantics (`std::move` as
  a hint); implement **move-out checking** (the simplest sound check).
- **M3**: Build MIR + initialization checking + drop insertion.
- **M4**: Borrow & alias-XOR-mutability checking (intraprocedural).
  **(done)**
- **M5**: NLL-style lifetime inference + dangling-reference checking
  (intraprocedural) + elision rules. **(Done: liveness-driven borrow
  release, the elision rule + dangling check for reference-returning
  functions, borrowing `a.b`/`arr[i]`, references into what a
  `std::unique_ptr` owns (`*p`/`p->x`), and binding/forwarding a
  reference-returning call's result to a new reference or reference
  argument -- see [§5.2](ch05-static-checks.md)/
  [§5.3](ch05-static-checks.md). Raw pointer (`T*`) dereference itself
  landed with `unsafe { }` in M6 below; `&expr` address-of (the
  remaining way to *produce* a `T*` from an arbitrary place) is still
  design-only -- see [§5.7](ch05-static-checks.md).)**
- **M6**: `vector`/`span`/`string_view` support + bounds-check policy +
  diagnostic quality. **(Slice 1 done: `std::span<T>`/
  `std::span<const T>` -- construction from a fixed-size array, a
  `.size` field, runtime bounds checks + `abort()` panics; see
  [§3](ch03-syntactic-sugar.md)/[§6](ch06-safe-subset.md)/
  [§8](ch08-open-questions.md). Implemented as a compiler builtin type
  (like `unique_ptr`), deliberately meant as a concrete prototype for a
  future "generics + generic lifetimes" mechanism. Also done, driven by
  a concrete "call C libraries" goal rather than the original vector/
  string plan: `unsafe { }` blocks ([§1.3](ch01-safety-context.md)),
  `extern "C"` function declarations/definitions including array-
  parameter decay ([§2.1](ch02-boundary-rules.md)), and the `bool`/`char`
  scalar types ([§6](ch06-safe-subset.md)) -- `bool` moved from `i1` to a
  real 1-byte representation in the process. `std::vector`,
  `std::string`/`string_view` (now unblocked by `char`), `for`/range-for,
  `&expr` address-of (design finalized in [§5.7](ch05-static-checks.md)
  -- the one remaining hard blocker for realistic `extern "C"`/POSIX
  interop), and `std::expected<T, E>` (design finalized in
  [§5.6](ch05-static-checks.md)) are all left for later.)**
- **M7+**: Generics/templates, traits/concepts, the `[[scpp::lifetime(name)]]`
  multi-group cross-function lifetime mechanism (design finalized in
  [§5.3](ch05-static-checks.md); not yet implemented), modules & libraries
  (design finalized in [ch11](ch11-modules-and-libraries.md); not yet
  implemented), standard-library expansion, incremental compilation.

---

[← Previous: Open Questions](ch08-open-questions.md) · [Table of Contents](README.md) · [Next: Reference Implementations →](ch10-reference-implementations.md)
