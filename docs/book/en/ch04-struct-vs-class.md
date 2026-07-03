# 4. Struct vs Class Semantics (Fixed Memory Layout / ABI)

This is a key departure from C++: in C++, `struct` and `class` are nearly
identical (differing only in default visibility). In scpp they have
completely different semantics.

## 4.1 `struct`: a purely trivial aggregate

- `struct` may only contain members of **trivial** types, defined
  recursively:
  - Scalar types: `bool`, integers, floats, `char`.
  - Raw pointers `T*` (carry no compiler-tracked lifetime; dereferencing
    still requires `unsafe {}`, see [§2](ch02-boundary-rules.md)).
  - Other `struct` types that themselves satisfy this rule (recursively).
  - Fixed-size arrays of trivial types.
- The following are **forbidden** as `struct` members and must instead be
  wrapped in a `class`:
  - References `T&` / `const T&`.
  - `std::span<T>`, `std::string_view` (lifetime-checked borrowed views).
  - `std::unique_ptr<T>`, `std::shared_ptr<T>`, `std::vector<T>`,
    `std::string`, or any type participating in ownership/borrow checking.
- A `struct` definition that violates this rule is a compile error (advising
  "use `class` instead"), not a silent downgrade.

**Initialization**: an uninitialized `struct` local/member is guaranteed by
the compiler to be entirely **zero-initialized** (bitwise). Scalar members
become `0` / `false` / `0.0`; raw pointer members become `nullptr`. This is a
stronger guarantee than flow-sensitive initialization checking — a `struct`
can never be read in an uninitialized state.

**Copy semantics**: `struct` values may be freely and implicitly copied
bitwise, and do not participate in the move/borrow checking described in
[§5](ch05-static-checks.md) — they carry no lifetime or
exclusive-ownership semantics at all. This is not an opt-in `Copy` trait
(contrast Rust's explicit `#[derive(Copy)]`): declaring a type `struct` is
itself the explicit declaration, and the compiler verifies triviality.

## 4.2 `class`: types that own resources / participate in checking

- `class` may contain members of any type, including `unique_ptr`, `vector`,
  `span`, other `class` types, or fields that themselves carry lifetime or
  ownership semantics.
- `class` participates in the ownership/move/borrow/lifetime checks
  described in [§5](ch05-static-checks.md); it is **not** guaranteed to
  be zero-initialized (requires explicit construction).
- Full checking rules for user-defined `class` types (constructors/
  destructors, borrows across method bodies, etc.) are out of scope for
  v0.1 — see the backlog in [§8](ch08-open-questions.md). v0.1 first
  checks only the standard-library-provided owning type `unique_ptr`
  (milestone M2).

## 4.3 Memory Layout & ABI (fixed, not left implementation-defined)

scpp pins `struct` memory layout to the target platform's **Clang ABI**: for
a given target triple, the layout scpp produces for a `struct` must be
byte-for-byte identical to what Clang produces for the equivalent C struct.
Concretely:

1. Members are laid out in source declaration order; the compiler **never
   reorders** them.
2. Each member is aligned per its type's alignment requirement under the
   target's Clang ABI; padding is inserted between members as needed.
3. The struct's own alignment is the maximum alignment of any member; its
   total size is rounded up to a multiple of that alignment (so arrays of
   the struct keep every element correctly aligned).
4. The first member is always at offset 0 (the struct's address equals its
   first member's address).

Implementation: the compiler emits a non-packed, named LLVM struct type and
lets the target's `DataLayout` compute the layout — the same `DataLayout`
Clang uses for the same target triple — so Clang/C-compatible layout falls
out automatically, with no separate alignment algorithm to maintain in scpp.

**Explicitly unsupported in v0.1**:
- Bit-fields — layout varies too much across platforms/compiler versions to
  pin down confidently right now.
- Packed layout (the equivalent of `#pragma pack(1)` /
  `__attribute__((packed))`) — deferred to a later version via an explicit
  attribute (e.g. `alignas` / `packed`) mapped to LLVM's packed struct type.

---

[← Previous: Syntactic Sugar](ch03-syntactic-sugar.md) · [Table of Contents](README.md) · [Next: Static Checks in Safe Regions →](ch05-static-checks.md)
