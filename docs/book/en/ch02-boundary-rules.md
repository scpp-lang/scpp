# 2. Boundary Rules (Safe <-> Unsafe interaction)

This is critical for soundness and must be strict.

| Call direction | Rule |
|----------------|------|
| `unsafe` calls `safe` | **Freely allowed**. A safe function is safe for any caller. |
| `safe` calls `safe` | Freely allowed; participates in checking normally. |
| `safe` calls `unsafe` | **Must be wrapped in `unsafe { }`**, otherwise a compile error. The programmer vouches for it. |
| Raw pointer deref in `safe` | Must be inside `unsafe { }`. |

- Data contracts at the boundary: references/pointers a safe function exposes to
  the unsafe world carry lifetime obligations that are **not enforced** on the
  unsafe side (the unsafe side is on its own). Conversely, a reference passed
  from unsafe into safe is **assumed valid for the duration of the call**
  (caller's obligation).
- The compiler should be able to mark whether an `unsafe` function has been
  "manually reviewed as safe to call" — v0.1 does not formalize this and relies
  on `unsafe { }` vouching.
- Mechanism: see [§1.3](ch01-safety-context.md) for the concrete rules of
  `unsafe { }` (design finalized, not yet implemented). In short, the
  checker rejects a `Call` whose callee's `Function::is_safe` is false
  unless the call site is lexically inside an `unsafe { }` block (or the
  caller itself is an `unsafe` function) -- the same
  currently-inside-`unsafe` bit also gates raw pointer dereference, and
  will gate the rest of [§5.5](ch05-static-checks.md)'s list once their
  syntax exists.

## 2.1 `extern "C"` declarations (design finalized, not yet implemented)

This is the concrete boundary to *actual* C, not just to unchecked scpp --
calling libc or any other C library. It reuses real C++ syntax as-is; scpp
adds no new spelling here, only extra restrictions and the safety wiring
below.

- **Grammar**: exactly C++'s existing forms --
  ```cpp
  extern "C" int printf(const char* fmt, ...);   // single declaration
  extern "C" {                                    // block form: sugar for
      void* malloc(size_t size);                  // repeating `extern "C"`
      void free(void* p);                          // on each declaration,
      void abort();                                // same as real C++
  }
  ```
  Only the literal linkage string `"C"` is accepted in v0.1 (not `"C++"`
  or anything else) -- any other string is a compile error naming what's
  actually supported.
- **Declaration vs. definition -- these behave differently**:
  - **No body** (`extern "C" int foo(int x);`): declares a function
    that's *defined elsewhere* and linked in externally. The compiler has
    no visibility into its implementation, so it is **always implicitly
    `unsafe`** -- writing `safe extern "C" int foo(int x);` is a compile
    error ("cannot mark an external declaration `safe`: its
    implementation isn't visible to the compiler"). Calling it from a
    `safe` function therefore requires `unsafe { }`, via the exact same
    mechanism as any other safe-calls-unsafe boundary (no new rule
    needed -- this is the main point of this section: `extern "C"` is
    just a new *source* of `unsafe`-by-construction function signatures,
    riding entirely on machinery [§1.3](ch01-safety-context.md) already
    defines).
  - **With a body** (`extern "C" int add(int a, int b) { return a + b; }`):
    defines an ordinary scpp function that additionally gets C linkage,
    so external C (or other-language) code can call *it*. `safe` and
    `extern "C"` are orthogonal here -- `safe extern "C" int add(...)
    { ... }` is allowed, and the body is checked exactly like any other
    `safe` function; `extern "C"` only constrains the *signature's* types
    (below) and requests C linkage, it says nothing about whether the
    body itself is trusted. This mirrors Rust's own
    `#[no_mangle] pub extern "C" fn foo(...)`, where the signature must be
    FFI-safe but the body is ordinary checked Rust.
- **Signature types are restricted to C-ABI-compatible types**, checked on
  every parameter and the return type, for both the declaration and
  definition forms: scalars; raw pointers `T*` (including `void*` -- `void`
  becomes a valid pointee-only type name for this purpose; `const T*` is
  its own distinct type, see [§5.7](ch05-static-checks.md) -- `printf`'s
  `const char* fmt` above really is read-only now, not a dropped
  qualifier); `struct`
  (already guaranteed Clang-ABI-compatible layout, see
  [§4.3](ch04-struct-vs-class.md)), by value or by pointer; fixed-size
  arrays `T[N]` in parameter position (decay to pointer, as in ordinary
  C++). **Rejected**: `T&`/`const T&`, `std::unique_ptr`, `std::span`,
  `std::string`/`std::string_view`, `std::vector`, `std::shared_ptr`,
  `std::expected` (see [§5.6](ch05-static-checks.md)/
  [§6](ch06-safe-subset.md) -- recoverable errors have no defined C
  representation either), and `[[scpp::lifetime(name)]]` (meaningless
  without a borrow-checked type to attach to) -- none of these have a
  defined C representation. A `safe
  extern "C"` function that needs to work with owning/borrowed scpp types
  internally takes/returns the C-compatible raw form at the boundary and
  converts on entry/exit inside its own (checked) body.
- **Prerequisites this needs that don't exist yet** (none of these are
  specific to `extern "C"` -- they're general gaps it happens to be the
  first feature to need):
  - An `extern` keyword (not lexed yet).
  - Minimal string-literal lexing, just enough to recognize the token
    `"C"` -- v0.1 doesn't need general string literals as an expression
    type yet (that waits for `std::string`/`char`, per
    [§6](ch06-safe-subset.md)); this is a narrow, separate slice of that
    same eventual work.
  - `void` as a valid type name, for `void*` params/locals and
    void-returning functions -- scpp currently has no way to declare a
    function returning `void` at all (`to_llvm_type` has no case for it).
  - Variadic parameters (`...`, for `printf`-family functions) are useful
    but **not required** for a first slice: most common libc entry points
    (`malloc`, `free`, `memcpy`, `strlen`, ...) aren't variadic. Parse and
    store a `has_varargs` flag on the declaration up front, but the actual
    variadic call-site codegen (argument promotion, `isVarArg=true` on the
    `llvm::FunctionType`) can land as a fast-follow.
- **Implementation shape**: this generalizes a pattern already
  hand-written three times in codegen.cppm today --
  `get_or_declare_malloc`/`get_or_declare_free`/`get_or_declare_abort`
  each manually build an LLVM `FunctionType` and `Function::Create` it
  with `ExternalLinkage` and no body for one hardcoded libc function.
  A user-facing `extern "C"` declaration should emit exactly that same
  shape of LLVM `declare` generically, from the parsed signature, instead
  of each new libc dependency needing its own hand-written C++ method.

---

[← Previous: Safety Context](ch01-safety-context.md) · [Table of Contents](README.md) · [Next: Syntactic Sugar →](ch03-syntactic-sugar.md)
