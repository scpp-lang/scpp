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
- **No `'a`-style lifetime syntax.** Instead of naming lifetimes as their own
  syntactic category (Rust's `'a`, Circle's `/a`), scpp groups reference
  *parameters* using one opt-out attribute, `[[scpp::lifetime(name)]]`,
  applied to ordinary C++ parameter/declarator syntax. (Design finalized;
  **not yet implemented** -- tracked under ch09 M7+.)
  - **Default grouping.** Every reference-typed input parameter whose type
    is reference-compatible with the return type (see the mutable-licensing
    rule below) belongs, unless tagged otherwise, to one shared implicit
    group. This is a conservative approximation: the borrow checker treats
    a returned reference as potentially aliasing *any* member of the group,
    so every argument bound to that group at a call site must stay valid
    for as long as the result is used -- even if the function body only
    ever actually returns one of them. This is a strict generalization of
    the old "at most one reference parameter" rule (it accepts a superset
    of what that rule accepted), and it requires zero annotation. For
    example:
    ```cpp
    safe const std::string& longest(const std::string& x, const std::string& y) {
        return x.size() > y.size() ? x : y;
    }
    ```
    is accepted (`x` and `y` join the one default group; a caller must keep
    both alive as long as the result is used) -- the same shape as Rust's
    `fn longest<'a>(x: &'a str, y: &'a str) -> &'a str`.
  - **Opt-out with `[[scpp::lifetime(name)]]`.** Tagging a reference
    parameter with this attribute pulls it *out* of the default group into
    a group named `name` instead. Two parameters tagged with the same
    `name` share a group; parameters in different groups are treated as
    mutually independent -- the checker assumes *no* relationship between
    them (neither outlives the other), matching Rust's `'a`/`'b` being
    unrelated absent a `where 'a: 'b` bound (scpp does not support
    outlives-constraints between groups; if two groups need relating, they
    must be unified into one instead).
  - **Naming the output's group.** If a function returns a reference and
    more than one group exists among its parameters, the group the output
    borrows from must be named explicitly with the same attribute on the
    function declarator: `[[scpp::lifetime(name)]]`. When only one group is
    in play (the common case), it's elided exactly as today. If
    disambiguation is required, every reference-compatible parameter must
    carry an explicit tag (no group may be left implicit), so the
    function-level attribute always names an unambiguous, explicitly
    declared group. Example (Rust: `fn get_x<'a, 'b>(x: &'a T, y: &'b T) -> &'a T`):
    ```cpp
    safe const int& get_x(const int& x [[scpp::lifetime(a)]],
                          const int& y [[scpp::lifetime(b)]]) [[scpp::lifetime(a)]] {
        return x; // returning y here is rejected: y is group b, the
                  // declared return group is a
    }
    ```
  - **Mutable licensing per group**: a group with no `T&` (mutable) member
    can never back a `T&` return, only `const T&` -- the same rule as
    today's single-parameter case, now applied per group.
  - If there is a `this`/`self` and the function returns a reference with
    no other disambiguation, the output borrows from `this`'s group. (v0.1
    has no `class` method/`this` concept yet, so this rule never actually
    applies.)
  - Any other case that still cannot be resolved -> error, advising to add
    a `[[scpp::lifetime(...)]]` attribute or refactor to return by value /
    smart pointer.
- **Dangling check**: for every `return` statement whose declared return
  type is a reference, the compiler resolves the returned expression
  (a plain variable, `a.b`, `arr[i]`, `*p`/`p->x` where `p` is a
  `std::unique_ptr<T>`, or a call to another reference-returning
  function, expanded recursively) back to its root place, and requires
  that root to be a parameter belonging to the group selected for the
  return type above (by default grouping or by explicit attribute) --
  otherwise it's rejected. This is v0.1's concrete answer to "does this
  function's returned reference dangle".
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
  back through the call chain to its real root place(s), subject to the
  exact same alias-XOR-mutability checks as a plain variable borrow. When
  the callee's return type is elided from a multi-member group (the
  default-grouping case above), the call's result is conservatively
  recorded as potentially borrowing from *every* argument bound to that
  group at this call site, not just one -- an invalidating action (move,
  write, drop) on any of them while the result is live is rejected.

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

Note what's deliberately *not* on this list: taking a raw pointer's
address in the first place (`&expr`, [§5.7](#57-address-of-x-and-raw-pointers-design-finalized-not-yet-implemented))
is always legal in a `safe` context, same as in Rust -- it's
*dereferencing* one that's gated here, not creating one. Also note that
`unsafe { }` relaxes *dereferencing* a raw pointer, not the ordinary,
unconditional type-checking rule that a `const T*` can never be written
through ([§5.7](#57-address-of-x-and-raw-pointers-design-finalized-not-yet-implemented))
-- that check isn't on this list either, because it isn't something
`unsafe { }` ever relaxes.

See [§1.3](ch01-safety-context.md) for `unsafe { }`'s exact rules: it
relaxes precisely this list and nothing else -- every other check in this
chapter (§5.1-§5.4) keeps running unconditionally inside an `unsafe { }`
block.

## 5.6 Recoverable Errors: `std::expected<T, E>` (design finalized, not yet implemented)

scpp has **no exceptions** -- no `throw`/`try`/`catch` anywhere in the
language, safe or unsafe (see [§8](ch08-open-questions.md) for the full
rationale). Every failure is exactly one of two kinds, extending the same
split already settled for panics ([§8](ch08-open-questions.md) Q3):

- **A bug / contract violation** (out-of-bounds access, a failed
  precondition, ...): unrecoverable by definition -- handled by aborting,
  already the case for `span`'s bounds check, and for constructors/
  destructors (see [§4.2](ch04-struct-vs-class.md)).
- **A recoverable, expected condition** (file not found, malformed input,
  ...): represented as an ordinary value of type `std::expected<T, E>`,
  returned like any other value, never thrown.

`std::expected<T, E>` is a **compiler builtin type** -- the same
treatment as `std::unique_ptr`/`std::span` (not a real instantiation of
the libstdc++/libc++ template, and not dependent on generics/templates
landing first). Unlike real C++23's `std::expected`, its accessors never
throw -- there is no exception mechanism in scpp for them to throw
*through*: misusing one (e.g. dereferencing a value-less `expected`) is a
contract violation, checked and handled by aborting the same way as every
other bug in scpp, never a thrown `std::bad_expected_access<E>`.

**Mandatory checking**: a `std::expected<T, E>` value produced by a call
cannot be silently discarded -- as if every such function were implicitly
declared `[[nodiscard]]`, except enforced as a hard error rather than
real C++'s warning-only `[[nodiscard]]`. Ignoring one entirely -- e.g.
calling a `std::expected`-returning function as a bare expression
statement and never inspecting the result -- is a **compile error** in
scpp, not a lint.

**Propagation: plain `if`/`else`, deliberately, for now.** A
Rust-`?`-style postfix operator for propagating a `std::expected`'s error
up to the caller was considered and **rejected** -- see
[§8](ch08-open-questions.md) Q8 for the full reasoning. In short: unlike
every other piece of scpp syntax, a brand-new operator token cannot be
erased or ignored by a real C++ compiler, which would have broken the
property that stripping `safe`/`unsafe` out of a scpp file leaves an
ordinary file a real C++ compiler still accepts unmodified (see
[ch00](ch00-design-philosophy.md) §2). v0.1 therefore requires spelling
propagation out with ordinary `if`/`else`, exactly the way C has for
decades:

```cpp
safe std::expected<int, ParseError> parse_and_double(const char* s) {
    std::expected<int, ParseError> r = parse_int(s);
    if (!r.has_value()) {
        return std::unexpected(r.error());
    }
    return *r * 2;
}
```

This is deliberately left as the only way to propagate a `std::expected`
error in v0.1. Whether/how to make this less verbose is revisited once
the C++ standard itself evolves further in this area -- see
[§8](ch08-open-questions.md) Q8.

## 5.7 Address-of (`&x`) and Raw Pointers (design finalized, not yet implemented)

- **Motivation.** Today a `T*` value can only ever be *received* (an
  `extern "C"` parameter, or copied from another already-existing `T*`)
  or *derived by decay* (a fixed-size array `T[N]` decays to `T*`,
  [§3](ch03-syntactic-sugar.md)). There is still no way to take the
  address of a plain scalar/struct local, a `.field`, or a `[index]`
  element -- exactly what most real C APIs need for "out" parameters
  (`accept(fd, &addr, &addrlen)`, `getsockopt(fd, ..., &value, &len)`,
  `stat(path, &statbuf)`): a pointer to *your own* storage, not something
  already handed to you as a pointer. This is the concrete gap this
  section closes.
- **Grammar.** A new prefix unary operator, `&expr`, where `expr` is one
  of the same forms already accepted as a borrow source for `T&`/
  `const T&` ([§5.2](#52-borrow--aliasing)): a plain local/parameter
  name, a `.field` projection, a `[index]` subscript, or `*p`/`p->x`
  where `p` is a `std::unique_ptr<T>` -- recursively, off any of the
  above. `&expr` evaluates to `const T*` if `expr`'s resolved place is
  only reachable read-only (e.g. through a `const T&` parameter/binding
  anywhere along the projection chain), or to `T*` if it's reachable
  mutably -- the same rule real C++'s own `&expr` already follows, and
  the same split as Rust's `&x as *const T` vs `&mut x as *mut T`.
- **`const T*` and `T*` are genuinely distinct types.** (An earlier draft
  of this section assumed they were unified into one untracked type --
  they are not, in either real C++ or scpp; see
  [§8](ch08-open-questions.md) Q9 for how that got caught and corrected.)
  `T*` converts implicitly to `const T*` (widening -- always legal,
  matching real C++'s own rule); `const T*` does **not** convert to
  `T*` -- v0.1 has no `const_cast`/Rust's `.cast_mut()` equivalent, so
  there is currently no way to obtain a `T*` from a `const T*` at all.
  **Writing through a `const T*` is an ordinary compile-time type error,
  in *every* context, including inside `unsafe { }`** -- it isn't on
  [§5.5](#55-prohibited-in-safe-regions-unless-in-unsafe--)'s list
  because `unsafe { }` only ever relaxes *that* list, and this isn't a
  member of it: it's the same kind of ordinary type mismatch as assigning
  a `std::string` to an `int`, which `unsafe { }` obviously doesn't
  relax either. This exactly mirrors Rust, where `*p = x;` on a
  `p: *const T` is rejected even inside an `unsafe` block.
- **Safe to create; only *using* the result is unsafe -- Rust's model,
  not a new one.** In real Rust, `let p = &x as *const T;` is
  unconditionally safe to write (`&x` is a checked borrow; the cast to a
  raw pointer is an ordinary, safe conversion) -- only `unsafe { *p }`
  needs the escape hatch. Rust's own borrow checker does not even reject
  `fn f() -> *const i32 { let x = 5; &x as *const i32 }`: a raw pointer
  carries no lifetime parameter for the checker to relate to `x`'s scope
  at all, so only returning an actual `&i32` reference (not a `*const
  i32`) would be rejected. scpp adopts the identical split: `&expr` is
  legal directly inside a `safe` function -- no `unsafe { }` needed to
  *write* it -- matching how it's raw-pointer *dereference*, never
  creation, that [§5.5](#55-prohibited-in-safe-regions-unless-in-unsafe--)
  actually lists as requiring `unsafe { }` ([§1.3](ch01-safety-context.md)).
  The resulting `T*` may be stored, passed around, returned, or simply
  allowed to dangle once the place it was taken from goes away -- exactly
  as in Rust, and deliberately so: the soundness boundary is entirely at
  the later `*p` dereference (already `unsafe`-gated), not at `&expr`
  itself.
- **What *is* checked at the moment `&expr` is evaluated:** the same
  definite-initialization check as an ordinary read of `expr`
  ([§5.1](#51-ownership--move)), and -- conservatively, since the
  resulting pointer's eventual use (read or write) can't be known at this
  point -- the same exclusivity a new `T&` binding would require: the
  root must have **no existing borrow at all** (shared or mutable) at
  this instant, or `&expr` is rejected the same way taking a second `T&`
  would be. Unlike an actual `T&`/`const T&` binding, though, `&expr`
  does **not** itself register a new borrow going forward: since the
  produced `T*` is never move/borrow-tracked (unchanged --
  [§5.2](#52-borrow--aliasing)), there is nothing to later release, and
  an ordinary `T&`/`const T&` borrow of the same place immediately
  afterward is unaffected. This is a deliberate, snapshot-only check: it
  cannot (and does not try to) prevent a raw pointer taken now from later
  aliasing a *different*, separately-checked borrow of the same place at
  some future program point -- that's the same responsibility boundary
  Rust places on `unsafe` code.
- **Interaction with `extern "C"`** ([§2.1](ch02-boundary-rules.md)): this
  is the primary motivating use case. `T*`/`const T*` are already
  accepted `extern "C"` signature types, so `&expr` is how a `safe`
  function produces a value to satisfy a C out-parameter:
  ```cpp
  extern "C" int getsockopt(int fd, int level, int optname, void* val, int* len);
  safe int query(int fd) {
      int value = 0;
      int len = 4;
      unsafe {
          getsockopt(fd, 1, 2, &value, &len);
      }
      return value;
  }
  ```
  Note that `&value`/`&len` themselves need no `unsafe` -- only the
  *call* to the non-`safe` `getsockopt` does, per [§1.3](ch01-safety-context.md)'s
  existing rule (unrelated to `&`).
- **Deliberately not included**, to keep this a minimal, single-purpose
  addition:
  - Pointer arithmetic (`&x + 1`) -- already
    [§5.5](#55-prohibited-in-safe-regions-unless-in-unsafe--)'s territory
    (`unsafe { }`-gated), unaffected by this addition.
  - Taking the address of an rvalue/temporary, or of a reference's own
    storage -- `expr` must resolve to an existing place, matching the
    borrow-source grammar it reuses.
  - Rust's `&raw const`/`&raw mut` (address-of *without* going through an
    intermediate reference at all, needed there for packed structs and
    uninitialized memory) -- scpp has neither concept yet, so there's no
    case this would need to cover that plain `&expr` doesn't already.
  - Removing const (`const T*` -> `T*`) -- no `const_cast`/Rust's
    `.cast_mut()` equivalent exists in v0.1. If a real C API's signature
    is honestly non-`const` where scpp's borrow-source is only reachable
    as `const`, there is currently no way to call it -- backlog.
- **Implementation shape** (for whoever builds this): a new
  `UnaryOp::AddressOf`, parsed at the same prefix precedence as the
  existing `*`/`-`/`!` (its natural sibling to `*`'s `UnaryOp::Deref`).
  In movecheck, reuse `resolve_borrow_source_root` to resolve/validate
  `expr`'s root -- exactly the structural resolution `T&`/`const T&`
  binding already uses -- but, unlike `apply_reference_binding`, do
  **not** write into the borrow map afterward: just check the root's
  current borrow state is empty (no shared or mutable borrow), rejecting
  otherwise with the same message shape as an existing "already
  borrowed" rejection, and stop there. In codegen, this reduces to
  whatever `codegen_lvalue` already resolves `expr`'s address to (its
  `.ptr`) -- no new address computation logic, just a new expression case
  that returns that pointer directly as the overall expression's value,
  instead of loading through it the way an ordinary read does. Track
  `const T*` vs. `T*` with a new `Type::is_mutable_pointee` flag on
  `TypeKind::Pointer` (mirroring the existing `Type::is_mutable_ref` on
  `TypeKind::Reference`); determine it at `&expr`'s resolution time from
  whatever mechanism already tracks a projection chain's const-reachability
  for today's `T&`-vs-`const T&` binding check (movecheck must already
  answer this question to reject binding a `T&` to a place only reachable
  via `const T&`). Reject an assignment whose LHS resolves through a
  pointer with `is_mutable_pointee == false` as an ordinary type error in
  whatever pass already checks assignment compatibility -- unconditionally,
  not gated by the `unsafe`-nesting counter from [§1.3](ch01-safety-context.md).

---

[← Previous: Struct vs Class Semantics](ch04-struct-vs-class.md) · [Table of Contents](README.md) · [Next: The Safe Subset Supported in v0.1 →](ch06-safe-subset.md)
