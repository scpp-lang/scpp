# 8. Open Questions (to be decided later)

1. **Out-of-bounds subscript**: in safe regions, does `vector[i]` / `span[i]`
   insert a runtime bounds check (like Rust), or require a checked API?
   **Settled and implemented (M6)**: `span[i]` inserts a runtime bounds
   check by default, calling `abort()` on failure (`vector` doesn't exist
   yet, but will follow the same policy) -- this is `safe` code's
   behavior. Inside `unsafe { }` (or an entirely native function), the
   check is skipped -- same treatment, and for the same reason, as
   integer-overflow checking (Q2 below / [§5.8](ch05-static-checks.md)):
   skipping a scpp-inserted *runtime* check carries none of the
   "corrupted bookkeeping leaking into surrounding safe code" risk that
   keeps move/borrow/lifetime checking unconditional (see
   [§1.3](ch01-safety-context.md)).
2. **Integer overflow**: does safe check signed overflow? **Settled**:
   yes -- checked in `safe` code (both signed and unsigned), `abort()`
   on overflow, unconditionally (no debug/release split); unchecked but
   guaranteed-wrapping (never UB) inside `unsafe { }` or an `unsafe`
   function, achieved by never emitting LLVM's `nsw`/`nuw` on scpp's own
   arithmetic codegen. Division/modulo by zero or `INT_MIN / -1` always
   `abort()`, `safe` or `unsafe` alike -- there's no wrapped result for
   the hardware to fall back on. Deliberately diverges from Rust's
   debug-only default (see [§5.8](ch05-static-checks.md) for the full
   reasoning, including why overflow-checking -- unlike
   [§5.1-§5.4](ch05-static-checks.md)'s checks -- can safely join what
   `unsafe { }` relaxes without risking the "leakage into surrounding
   safe code" that [§1.3](ch01-safety-context.md) otherwise guards
   against).
3. **Panic model**: how do OOB / assertion failures terminate? `std::terminate`
   or a custom panic + stack unwinding? **Settled and implemented (M6)**:
   calls libc's `abort()` directly (lower-level than `std::terminate()`,
   doesn't depend on the C++ runtime's terminate-handler machinery, same
   effect -- the process ends immediately, no stack unwinding).
4. **Interior mutability**: introduce a `Cell`/`RefCell` equivalent to
   carry legal mutable aliasing? **Settled, phase 1 only**: reuse real
   C++'s `mutable` keyword, but stricter -- a `mutable` field must be a
   trivial type (matching `struct`'s own field-triviality rule,
   [§4.1](ch04-struct-vs-class.md)), readable/writable through any
   `this` regardless of `const`, but can **never** be referenced or
   have its address taken (see [§4.2](ch04-struct-vs-class.md)/
   [§5.9](ch05-static-checks.md)). This gives scpp the `Cell<T>` half
   (zero runtime cost, since a value nothing can ever reference cannot
   alias) for free, using existing C++ syntax. The `RefCell` half
   (borrowing an actual reference to non-trivial interior state, with a
   runtime borrow counter panicking/aborting on violation) has no
   existing C++ name to reuse and needs real new machinery -- deferred
   to a later round, not part of this decision.
5. **`safe` vs `const`**: how does a `const` member function map to
   borrows in a safe region? **Settled**: `this` is treated as an
   implicit reference parameter -- `const T&` in a `const` method, `T&`
   otherwise -- subject to exactly the same alias-XOR-mutability,
   whole-root-conservative field access, and lifetime-elision rules as
   any other reference (see [§5.9](ch05-static-checks.md)). Related,
   also settled in the same round: a `class`'s member *variables*
   (including class-level constants) can never be `public`, only member
   *functions* can -- external code always goes through a method call,
   never direct field access (see [§4.2](ch04-struct-vs-class.md));
   class-level constants are exposed via a `static consteval` function
   instead of a public data member (see [§6](ch06-safe-subset.md) for
   why scpp has no `constexpr`-qualified functions). Inheritance (and
   therefore `protected`) remains deferred, not part of this round.
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
8. **Recoverable error handling**: exceptions, or value-based errors?
   **Settled**: no exceptions anywhere in scpp (no `throw`/`try`/`catch`).
   Every failure is either a *bug* (aborts -- already settled by Q3 above)
   or a *recoverable, expected condition* (an ordinary
   `std::expected<T, E>` value, mandatorily checked by the compiler -- see
   [§5.6](ch05-static-checks.md)). Constructors/destructors follow the
   same split (see [§4.2](ch04-struct-vs-class.md)): they may abort on a
   precondition violation, but cannot produce a recoverable error --
   fallible construction goes through a `static` factory function
   returning `std::expected<T, E>` instead (the classic C++
   "named constructor idiom"). **Propagating** a `std::expected`'s error
   up to the caller uses plain `if`/`else` in v0.1 -- a Rust-`?`-style
   postfix operator (spelled `??`, since C++ already uses a bare `?` for
   the ternary operator) was considered and **rejected**: unlike every
   other piece of scpp syntax, a brand-new operator token can't be erased
   or ignored by a real C++ compiler, which would have permanently broken
   the property that stripping `safe`/`unsafe` out of a scpp file leaves
   an ordinary file a real C++ compiler still accepts (see
   [ch00](ch00-design-philosophy.md) §2) -- a real compiler hard-errors
   parsing past the second `?` (trigraphs, the only thing that ever gave
   `??` meaning, were removed in C++17). Revisiting this is deferred
   until the C++ standard itself evolves further in this area.
9. **Are `const T*` and `T*` the same type?** An earlier draft of
   [§5.7](ch05-static-checks.md) (the `&expr` design) assumed scpp's
   `const T*`/`T*` were unified into one untracked type -- they are not,
   in either real C++ or scpp; caught and corrected in discussion. Real
   C++ has always treated them as distinct types (one-way implicit
   `T* -> const T*` conversion, `const_cast` required for the reverse),
   and Rust's `*const T`/`*mut T` enforce the identical split at compile
   time -- rejecting a write through a `*const T` even inside `unsafe`.
   **Settled**: scpp tracks the distinction properly (a new
   `is_mutable_pointee` flag, mirroring how `is_mutable_ref` already
   distinguishes `T&`/`const T&`); the one-way implicit conversion is
   real C++'s own existing rule, not a new one; writing through a
   `const T*` is an ordinary, always-enforced type error, not something
   `unsafe { }` relaxes. No `const_cast` equivalent exists in v0.1 (see
   [§5.7](ch05-static-checks.md)).

---

[← Previous: Compilation Pipeline](ch07-compilation-pipeline.md) · [Table of Contents](README.md) · [Next: MVP Milestones →](ch09-milestones.md)
