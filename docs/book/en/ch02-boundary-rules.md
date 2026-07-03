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

---

[← Previous: Safety Context](ch01-safety-context.md) · [Table of Contents](README.md) · [Next: Syntactic Sugar →](ch03-syntactic-sugar.md)
