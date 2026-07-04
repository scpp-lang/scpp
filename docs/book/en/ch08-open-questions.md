# 8. Open Questions (to be decided later)

1. **Out-of-bounds subscript**: in safe regions, does `vector[i]` / `span[i]`
   insert a runtime bounds check (like Rust), or require a checked API?
   **Settled and implemented (M6)**: `span[i]` inserts a runtime bounds
   check by default, calling `abort()` on failure (`vector` doesn't exist
   yet, but will follow the same policy).
2. **Integer overflow**: does safe check signed overflow? Leaning: panic in
   debug, wrapping/UB in release? TBD.
3. **Panic model**: how do OOB / assertion failures terminate? `std::terminate`
   or a custom panic + stack unwinding? **Settled and implemented (M6)**:
   calls libc's `abort()` directly (lower-level than `std::terminate()`,
   doesn't depend on the C++ runtime's terminate-handler machinery, same
   effect -- the process ends immediately, no stack unwinding).
4. **Interior mutability**: introduce a `Cell`/`RefCell` equivalent to carry
   legal mutable aliasing?
5. **`safe` vs `const`**: how does a `const` member function map to borrows in a
   safe region?
6. **ABI / interop with existing C++ libraries**: how to engineer safe code
   calling third-party headers (all unsafe) — treat them all as `unsafe`?
   **Settled**: `extern "C"` ([§2.1](ch02-boundary-rules.md), design
   finalized) is scpp's *only* interop mechanism with the outside world;
   scpp-to-scpp code sharing across files is [ch11](ch11-modules-and-libraries.md)
   (design finalized). Interop with *existing, unmodified C++ libraries*
   specifically (arbitrary classes, templates, overloads, exceptions,
   RTTI) is explicitly **not pursued** -- considered and rejected the
   idea of transpiling checked scpp to real C++ text and compiling it
   with Clang (which would have made this easy, at the cost of a full
   rework of the already-working direct-to-LLVM-IR codegen path); direct
   LLVM IR generation also has a strictly higher optimization ceiling for
   safe-region code than that alternative would (scoped `alias.scope`/
   `noalias` LLVM metadata, derived from the borrow checker's own
   NLL-precision aliasing proofs, has no C++ source-level equivalent --
   not even `__restrict` reaches it, since `__restrict` only ever maps to
   the coarser, whole-parameter `noalias` attribute).
7. **Language/compiler name, file extension.**

---

[← Previous: Compilation Pipeline](ch07-compilation-pipeline.md) · [Table of Contents](README.md) · [Next: MVP Milestones →](ch09-milestones.md)
