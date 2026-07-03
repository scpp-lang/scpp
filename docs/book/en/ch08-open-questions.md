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
7. **Language/compiler name, file extension.**

---

[← Previous: Compilation Pipeline](ch07-compilation-pipeline.md) · [Table of Contents](README.md) · [Next: MVP Milestones →](ch09-milestones.md)
