# 5. Static Checks in Safe Regions (the soundness core)

Within a safe context, the compiler guarantees (for the supported subset)
the following properties:

## 5.1 Ownership & Move
- Every value has a unique owner.
- After `std::move(x)`, `x` enters the *moved-out* state; reading a
  moved-out value -> error.
- Reassignment returns a variable to the *initialized* state.
- At end of scope, values still *initialized* are dropped; moved-out values
  are not dropped.

## 5.2 Borrow & Aliasing
- **Alias XOR mutability**: at any instant an object may have either any
  number of `const T&` (shared borrows), or exactly one `T&` (mutable
  borrow), never both.
- While an active borrow exists, the borrowed object may not be moved or
  destroyed.

## 5.3 Lifetime
- A borrow must not outlive the borrowed value (**no dangling
  references**).
- v0.1 performs **intraprocedural borrow checking only**, based on
  NLL-style dataflow analysis (liveness-driven region inference).
- **No lifetime syntax is exposed.** Cross-function signatures use the
  following **elision defaults** (a simplified version of Rust's lifetime
  elision):
  - If a function has exactly one reference input parameter, all reference
    outputs borrow from its lifetime.
  - If there is a `this` and the function returns a reference, the output
    borrows from `this`'s lifetime.
  - Any other case that cannot be inferred -> error, advising "this
    signature is not yet supported; refactor or return by value / smart
    pointer". (No explicit annotation syntax is introduced in v0.1.)

## 5.4 Initialization
- Reading an uninitialized variable is forbidden.
- All paths must initialize before use (dataflow analysis).
- `struct` already satisfies this trivially via the zero-init guarantee in
  [§4.1](ch04-struct-vs-class.md), with no flow-sensitive analysis needed.

## 5.5 Prohibited in Safe Regions (unless in `unsafe { }`)
- Raw pointer dereference, pointer arithmetic.
- `reinterpret_cast`, C-style casts to incompatible types.
- (Untagged) `union`.
- Raw `new` / `delete`.
- Access to mutable global/static variables.
- Calling a function not annotated `safe`.

---

[← Previous: Struct vs Class Semantics](ch04-struct-vs-class.md) · [Table of Contents](README.md) · [Next: The Safe Subset Supported in v0.1 →](ch06-safe-subset.md)
