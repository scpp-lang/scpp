# 2. Boundary Rules (Interaction with `unsafe { }` and `extern "C"`)

This is critical for soundness and must be strict.

| Situation | Rule |
|-----------|------|
| Calling an ordinary (checked-by-default) function | Freely allowed; participates in checking normally, from anywhere -- including from inside an `unsafe { }` block. |
| Calling an `extern "C"` function | **Must be wrapped in `unsafe { }`**, otherwise a compile error. No scpp compiler ever sees the C implementation to check it, so the caller vouches for it instead. |
| Raw pointer dereference | Must be inside `unsafe { }`. |

- Data contracts at the boundary: references/pointers exposed to an
  `extern "C"` function, or obtained via a raw pointer inside
  `unsafe { }`, carry lifetime obligations that are **not enforced**
  beyond that point (the `unsafe { }` block's author is on their own).
  Conversely, a reference or pointer value coming *from* an `extern "C"`
  call, or produced by dereferencing a raw pointer, is **assumed valid**
  by the code that vouched for it via `unsafe { }` (that block's
  obligation).
- The compiler should be able to mark whether a specific `extern "C"`
  declaration has been "manually reviewed as safe to call" -- v0.1 does
  not formalize this and relies on `unsafe { }` vouching at each call
  site instead.
- Mechanism: see [§1.3](ch01-safety-context.md) for the concrete rules of
  `unsafe { }`. In short, the
  checker rejects a `Call` whose callee is an `extern "C"` declaration
  unless the call site is lexically inside an `unsafe { }` block -- the
  same currently-inside-`unsafe` bit also gates raw pointer dereference,
  and will gate the rest of [§5.5](ch05-static-checks.md)'s list once
  their syntax exists.

## 2.1 `extern "C"` declarations

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
    unchecked** -- there is no way to mark it otherwise (no keyword
    exists for that). Calling it therefore requires `unsafe { }`, via
    the exact same mechanism as any other call to unchecked code (no new
    rule needed -- this is the main point of this section: `extern "C"`
    is just a new *source* of unchecked-by-construction function
    signatures, riding entirely on machinery
    [§1.3](ch01-safety-context.md) already defines).
  - **With a body** (`extern "C" int add(int a, int b) { return a + b; }`):
    defines an ordinary scpp function (checked by default, like every
    other function) that additionally gets C linkage, so external C (or
    other-language) code can call *it*. The body is checked exactly like
    any other function; `extern "C"` only constrains the *signature's*
    types (below) and requests C linkage, it says nothing about whether
    the body itself is trusted. This mirrors Rust's own
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
  defined C representation. An `extern "C"` function that needs to work
  with owning/borrowed scpp types internally takes/returns the
  C-compatible raw form at the boundary and converts on entry/exit
  inside its own (checked) body.

---

[← Previous: Safety Context](ch01-safety-context.md) · [Table of Contents](README.md) · [Next: Syntactic Sugar →](ch03-syntactic-sugar.md)
