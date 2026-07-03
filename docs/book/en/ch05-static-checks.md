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
- A borrow's source can be a plain variable name, or a `.field`
  projection (`a.b`) or subscript (`arr[i]`) off one. v0.1 treats both
  **whole-root conservatively**: borrowing `a.b` is recorded against the
  root `a`, and so is borrowing `a.c` -- the two are considered
  conflicting even though the fields never actually overlap in memory.
  This matches how Rust itself treats a dynamically-indexed array/slice
  element (`arr[i]`/`arr[j]` conflict there too, absent an explicit split
  API like `split_at_mut`); v0.1 just applies that same conservative rule
  to struct fields as well, rather than Rust's field-sensitive precision
  for structs. Workarounds: pass each field as its own **separate call
  argument** (each such borrow begins and ends within its own call, so
  sequential calls never overlap), or keep the two named references' own
  live ranges (see the liveness analysis in §5.3) from overlapping.

## 5.3 Lifetime
- A borrow must not outlive the borrowed value (**no dangling
  references**).
- v0.1 performs **intraprocedural borrow checking only**, based on
  NLL-style dataflow analysis (liveness-driven region inference): a
  reference local's borrow is released right after its **last use**,
  rather than only at the end of its lexical scope -- implemented via a
  backward liveness analysis over each reference local. This is more
  precise than releasing only at lexical scope end, and accepts more
  legal programs (e.g. a place can be borrowed again immediately after
  its previous borrow's last use, even before the enclosing block ends).
- **No lifetime syntax is exposed.** Cross-function signatures use the
  following **elision defaults** (a simplified version of Rust's lifetime
  elision):
  - If a function has exactly one reference input parameter, all reference
    outputs borrow from its lifetime.
  - If there is a `this` and the function returns a reference, the output
    borrows from `this`'s lifetime. (v0.1 has no `class` method/`this`
    concept yet, so this rule never actually applies -- only the one
    above does.)
  - Any other case that cannot be inferred -> error, advising "this
    signature is not yet supported; refactor or return by value / smart
    pointer". (No explicit annotation syntax is introduced in v0.1.)
- **Dangling check**: for every `return` statement whose declared return
  type is a reference, the compiler resolves the returned expression
  (a plain variable, `a.b`, `arr[i]`, `*p`/`p->x` where `p` is a
  `std::unique_ptr<T>`, or a call to another reference-returning
  function, expanded recursively) back to its root place, and requires
  that root to be **exactly** the one reference parameter selected by
  the elision rule above -- otherwise it's rejected. This is v0.1's
  concrete answer to "does this function's returned reference dangle".
- A reference can point into what a `std::unique_ptr` owns
  (`int& r = *p;` / `int& r = p->field;` -- see ch03's `*`/`->` sugar):
  the borrow is recorded against `p` itself, so moving (`std::move(p)`)
  or reassigning `p` while that borrow is alive is rejected (it would
  otherwise dangle / use-after-free). Dereferencing a raw pointer `T*`
  still requires `unsafe { }`, which v0.1 hasn't implemented yet, so
  that one is left for a later version.
- Calling a function that returns a reference: the result can be
  consumed as an ordinary value (auto-dereferenced,
  `int y = get_ref(x);`), bound to a new named reference
  (`int& r = get_ref(x);`), or passed onward as a reference argument to
  another function (`g(get_ref(x));`) -- movecheck resolves the result
  back through the call chain to its real root place, subject to the
  exact same alias-XOR-mutability checks as a plain variable borrow.

## 5.4 Initialization
- scpp has **no concept of an "uninitialized variable"**: any local or
  member that has no explicit initializer is always guaranteed by the
  compiler to be **zero-initialized** (bitwise) -- scalars become `0` /
  `false` / `0.0`, raw pointers become `nullptr`, and aggregate types like
  `struct`, arrays, and `std::unique_ptr` are zeroed field-by-field /
  element-by-element (see [§4.1](ch04-struct-vs-class.md) for `struct`'s
  specific rules). This applies uniformly to every type, not just a
  special case for some of them.
- Reading an uninitialized variable is therefore **structurally
  impossible**: a variable is well-defined from the moment it's declared,
  with no flow-sensitive dataflow analysis needed to prove "every path
  initializes it before use".
- This differs from both Rust (requires explicit initialization; reading
  an uninitialized value is a compile error) and ordinary C++ (no
  initialization by default; reading is undefined behavior): scpp always
  provides a well-defined default value, leaving "is this default value
  the one you wanted" up to the programmer.

## 5.5 Prohibited in Safe Regions (unless in `unsafe { }`)
- Raw pointer dereference, pointer arithmetic.
- `reinterpret_cast`, C-style casts to incompatible types.
- (Untagged) `union`.
- Raw `new` / `delete`.
- Access to mutable global/static variables.
- Calling a function not annotated `safe`.

---

[← Previous: Struct vs Class Semantics](ch04-struct-vs-class.md) · [Table of Contents](README.md) · [Next: The Safe Subset Supported in v0.1 →](ch06-safe-subset.md)
