# 5. Static Checks (the soundness core)

By default, the compiler guarantees (for the supported subset)
the following properties, unconditionally, everywhere:

## 5.1 Ownership & Move
- Every value has a unique owner.
- After `std::move(x)`, `x` enters the *moved-out* state; reading a
  moved-out value -> error.
- Reassignment returns a variable to the *initialized* state.
- At end of scope, values still *initialized* are dropped; moved-out values
  are not dropped.
- For a `class`-typed value specifically, "moved" always means the same
  compiler-provided, memberwise-recursive operation, never user-written
  logic -- see [§4.2](ch04-struct-vs-class.md) for the full rule and
  [§8](ch08-open-questions.md) Q14 for why.

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
  rather than only at the end of its lexical scope -- determined via a
  backward liveness analysis over each reference local. This is more
  precise than releasing only at lexical scope end, and accepts more
  legal programs (e.g. a place can be borrowed again immediately after
  its previous borrow's last use, even before the enclosing block ends).
- **No `'a`-style lifetime syntax.** Instead of naming lifetimes as their own
  syntactic category (Rust's `'a`, Circle's `/a`), scpp groups reference
  *parameters* using one opt-out attribute, `[[scpp::lifetime(name)]]`,
  applied to ordinary C++ parameter/declarator syntax.
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
    const std::string& longest(const std::string& x, const std::string& y) {
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
    const int& get_x(const int& x [[scpp::lifetime(a)]],
                     const int& y [[scpp::lifetime(b)]]) [[scpp::lifetime(a)]] {
        return x; // returning y here is rejected: y is group b, the
                  // declared return group is a
    }
    ```
  - **Mutable licensing per group**: a group with no `T&` (mutable) member
    can never back a `T&` return, only `const T&` -- the same rule as
    today's single-parameter case, now applied per group.
  - If there is a `this`/`self` and the function returns a reference
    with no other disambiguation, the output borrows from `this`'s
    group -- see
    [§5.9](#59-methods-and-this)
    for `this`'s full treatment as an implicit reference parameter.
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
  still requires `[[scpp::unsafe]]`.
- Calling a function that returns a reference: the result can be
  consumed as an ordinary value (auto-dereferenced,
  `int y = get_ref(x);`), bound to a new named reference
  (`int& r = get_ref(x);`), or passed onward as a reference argument to
  another function (`g(get_ref(x));`) -- the result is resolved
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

## 5.5 Prohibited (unless in `[[scpp::unsafe]]`)
- Raw pointer dereference, pointer arithmetic.
- `reinterpret_cast`, C-style casts to incompatible types.
- (Untagged) `union`.
- Raw `new` / `delete`.
- Access to mutable global/static variables.
- Calling an `extern "C"` function.
- Calling a function whose own declaration is itself marked
  `[[scpp::unsafe]]` (see [§1.2](ch01-safety-context.md)) -- a
  function-level marker distinct from wrapping a *nested* block, used
  when a function's soundness depends on a precondition only its caller
  can guarantee (mirrors Rust's `unsafe fn`).
- Calling through a value of pointer-to-function type whose own type is
  `[[scpp::unsafe]]`-qualified (see [§5.16](#516-function-pointers)) --
  the same call-site obligation as the previous item, extended to an
  *indirect* call, since the callee behind such a pointer is unknown at
  compile time and may be an `[[scpp::unsafe]]`-marked function or an
  `extern "C"` declaration with no body.

Note what's deliberately *not* on this list: taking a raw pointer's
address in the first place (`&expr`, [§5.7](#57-address-of-x-and-raw-pointers))
is always legal, same as in Rust -- it's
*dereferencing* one that's gated here, not creating one. Also note that
`[[scpp::unsafe]]` relaxes *dereferencing* a raw pointer, not the ordinary,
unconditional type-checking rule that a `const T*` can never be written
through ([§5.7](#57-address-of-x-and-raw-pointers))
-- that check isn't on this list either, because it isn't something
`[[scpp::unsafe]]` ever relaxes. Two more things *do* join this list's
effect (relaxed inside `[[scpp::unsafe]]`), but for a different reason
than everything above: not because they're otherwise illegal (neither
ever is), but because skipping them carries none of the "corrupted
bookkeeping could leak into surrounding code" risk that keeps
[§5.1-§5.4](#51-ownership--move)'s checks unconditional -- `span`'s
bounds check ([§8](ch08-open-questions.md) Q1) and integer-overflow
checking ([§5.8](#58-integer-overflow))
are both scpp-inserted *runtime* checks, not otherwise-illegal
operations, and both are off inside `[[scpp::unsafe]]`, on everywhere
else.

See [§1.3](ch01-safety-context.md) for `[[scpp::unsafe]]`'s exact rules:
it relaxes precisely this list and nothing else -- every other check in
this chapter (§5.1-§5.4) keeps running unconditionally inside any
`[[scpp::unsafe]]` context, whether established by a nested block or by
the function-level marker.

## 5.6 Recoverable Errors: `std::expected<T, E>`

scpp has **no exceptions** -- no `throw`/`try`/`catch` anywhere in the
language (see [§8](ch08-open-questions.md) for the full
rationale). Every failure is exactly one of two kinds, extending the same
split already settled for panics ([§8](ch08-open-questions.md) Q3):

- **A bug / contract violation** (out-of-bounds access, a failed
  precondition, ...): unrecoverable by definition -- handled by aborting,
  already the case for `span`'s bounds check, and for constructors/
  destructors (see [§4.2](ch04-struct-vs-class.md)).
- **A recoverable, expected condition** (file not found, malformed input,
  ...): represented as an ordinary value of type `std::expected<T, E>`,
  returned like any other value, never thrown.

`std::expected<T, E>` is a **compiler builtin type**, not a real
instantiation of the libstdc++/libc++ template and not dependent on
generics/templates landing first. Unlike real C++23's `std::expected`,
its accessors never throw -- there is no exception mechanism in scpp for
them to throw *through*: misusing one (e.g. dereferencing a value-less
`expected`) is a contract violation, checked and handled by aborting the
same way as every other bug in scpp, never a thrown
`std::bad_expected_access<E>`.

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
every other piece of scpp syntax -- all of it spelled as an attribute in
the `scpp` namespace -- a brand-new operator token has no silently-ignored
fallback: a real C++ compiler already accepts an attribute it doesn't
recognize unmodified, but it cannot parse past a brand-new operator token
at all, which would have broken the property that a well-formed scpp file
is already accepted by a real C++ compiler (see
[ch00](ch00-design-philosophy.md) §2). v0.1 therefore requires spelling
propagation out with ordinary `if`/`else`, exactly the way C has for
decades:

```cpp
std::expected<int, ParseError> parse_and_double(const char* s) {
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

## 5.7 Address-of (`&x`) and Raw Pointers

- **Motivation.** So far in this spec, a `T*` value can only ever be *received* (an
  `extern "C"` parameter, or copied from another already-existing `T*`)
  or *derived by decay* (a fixed-size array `T[N]` decays to `T*`,
  [§3](ch03-syntactic-sugar.md)). There is still no way to take the
  address of a plain scalar/struct local, a `.field`, or a `[index]`
  element -- exactly what most real C APIs need for "out" parameters
  (`accept(fd, &addr, &addrlen)`, `getsockopt(fd, ..., &value, &len)`,
  `stat(path, &statbuf)`): a pointer to *your own* storage, not something
  already handed to you as a pointer. This section closes that gap.
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
  in *every* context, including inside `[[scpp::unsafe]]`** -- it isn't on
  [§5.5](#55-prohibited-unless-in-scppunsafe)'s list
  because `[[scpp::unsafe]]` only ever relaxes *that* list, and this isn't a
  member of it: it's the same kind of ordinary type mismatch as assigning
  a `std::string` to an `int`, which `[[scpp::unsafe]]` obviously doesn't
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
  always legal -- no `[[scpp::unsafe]]` needed to
  *write* it -- matching how it's raw-pointer *dereference*, never
  creation, that [§5.5](#55-prohibited-unless-in-scppunsafe)
  actually lists as requiring `[[scpp::unsafe]]` ([§1.3](ch01-safety-context.md)).
  The resulting `T*` may be stored, passed around, returned, or simply
  allowed to dangle once the place it was taken from goes away -- exactly
  as in Rust, and deliberately so: the soundness boundary is entirely at
  the later `*p` dereference (already `[[scpp::unsafe]]`-gated), not at `&expr`
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
  accepted `extern "C"` signature types, so `&expr` is how ordinary
  (checked-by-default) code produces a value to satisfy a C out-parameter:
  ```cpp
  extern "C" int getsockopt(int fd, int level, int optname, void* val, int* len);
  int query(int fd) {
      int value = 0;
      int len = 4;
      [[scpp::unsafe]] {
          getsockopt(fd, 1, 2, &value, &len);
      }
      return value;
  }
  ```
  Note that `&value`/`&len` themselves need no `[[scpp::unsafe]]` -- only
  the *call* to `getsockopt` (an `extern "C"` declaration) does, per
  [§1.3](ch01-safety-context.md)'s existing rule (unrelated to `&`).
- **Deliberately not included**, to keep this a minimal, single-purpose
  addition:
  - Pointer arithmetic (`&x + 1`) -- already
    [§5.5](#55-prohibited-unless-in-scppunsafe)'s territory
    (`[[scpp::unsafe]]`-gated), unaffected by this addition.
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
    as `const`, there is no way to call it in v0.1 -- deferred (see
    [§6](ch06-safe-subset.md)).

## 5.8 Integer Overflow

Real C++ leaves signed integer overflow **undefined behavior** -- even
after C++20 mandated two's-complement *representation* for signed
integers, overflow *behavior* remained a separate, still-unresolved
question (see [ch00](ch00-design-philosophy.md) §8). scpp eliminates
this UB entirely, both by default and inside `[[scpp::unsafe]]`, reusing
the existing checked-by-default/`[[scpp::unsafe]]` axis rather than
introducing a new debug/release build-mode one:

- **By default (everywhere outside `[[scpp::unsafe]]`)**:
  `+`, `-`, and `*` are checked -- for **both signed and unsigned**
  operands (unlike real C++, where unsigned wraparound is always
  "intentional" by definition; scpp treats an unsigned wraparound as
  just as likely to be a bug as a signed one, matching Rust's judgment
  here). On overflow, the program `abort()`s, via the same panic
  mechanism as `span`'s bounds check ([§8](ch08-open-questions.md)
  Q1/Q3) -- unconditionally, not gated by a debug/release compilation
  mode the way Rust's is. Deliberately diverging from Rust here: Rust's
  checks are debug-only by default, providing zero protection in the
  release binaries that actually ship and face real attackers/real data
  -- scpp instead reuses its existing checked-by-default/`[[scpp::unsafe]]`
  axis, which already has no debug/release split anywhere else in the
  spec.
- **In `[[scpp::unsafe]]`**: the check is skipped, but the underlying
  operation is still **not UB** -- it's a guaranteed two's-complement
  wraparound, identical in spirit to how unsigned arithmetic already
  behaves in real C++. Concretely: scpp's codegen never emits LLVM's
  `nsw`/`nuw` ("no signed/unsigned wrap") flags on its `add`/`sub`/`mul`
  instructions -- those flags are exactly what *grants* the optimizer
  license to assume overflow never happens (turning it into a poison
  value/UB if it does); plain LLVM `add`/`sub`/`mul`, without them, are
  already well-defined, wrapping operations with no UB at the IR level.
  Real C++ compiled through Clang can't get this same guarantee without
  a blunt, whole-translation-unit `-fwrapv` flag (Clang has no choice
  but to emit `nsw` otherwise, because the C++ standard mandates UB);
  scpp, generating its own IR directly, simply never opts in to begin
  with.
- **Why this joins what `[[scpp::unsafe]]` relaxes, without reopening
  [§1.3](ch01-safety-context.md)'s "narrow escape hatch, not a
  stop-checking-this-region switch" rule**: unlike
  [§5.1-§5.4](#51-ownership--move)'s checks (move state,
  borrow/aliasing, lifetimes, zero-init), which must keep running
  unconditionally inside `[[scpp::unsafe]]` because skipping them would
  let *inconsistent compiler bookkeeping* leak into the surrounding
  code once the block ends, overflow-checking carries no such risk: an
  unchecked wraparound just produces an ordinary (if numerically wrong)
  value in an ordinary variable -- it cannot corrupt move/borrow/
  lifetime tracking, which is entirely independent of what value a
  variable holds. Any actual memory-safety consequence of that wrong
  value (e.g. using it as an out-of-bounds index) is still caught
  independently by whatever check governs *that* operation (`span`'s
  bounds check doesn't care why an index is wrong).
- **Manual overflow detection becomes reliable inside `[[scpp::unsafe]]`**:
  the classic `if (x + 1 < x)` idiom is unreliable for signed `x` in
  real C++ -- compilers may (and GCC/Clang do) assume signed overflow
  never happens and optimize the check away as unreachable. Since scpp
  never emits `nsw`, there is no such license to exploit, so this idiom
  works exactly as its arithmetic reads: `x = INT_MAX` wraps `x + 1` to
  `INT_MIN`, and `INT_MIN < INT_MAX` is (correctly) true. (This idiom is
  moot by default: the automatic check already aborts at `x + 1`
  itself, before a comparison could ever observe the wrapped value.)
- **Division/modulo are a separate case, not covered by "wraps"**:
  `INT_MIN / -1` (the one case where signed division itself overflows)
  and division/modulo by zero don't have a wrapped result to fall back
  on -- the hardware itself traps (x86 `#DE`). Both `abort()`
  unconditionally, in *every* context, whether inside `[[scpp::unsafe]]`
  or not -- there is no unchecked variant for these two.

## 5.9 Methods and `this`

Answers [§8](ch08-open-questions.md) Q5 ("how does a `const` member
function map to borrows"). scpp reuses real C++ method syntax exactly
(trailing `const` qualifier, no Rust-style `&self`/`&mut self` parameter
spelling) -- only the *borrow-checking treatment* of the implicit `this`
is new:

- **`this` is treated as an implicit reference parameter**, exactly
  like any other reference parameter already covered by
  [§5.2](#52-borrow--aliasing)/[§5.3](#53-lifetime): a `const`-qualified
  method's `this` is treated as `const T&` (a shared borrow of the
  receiver); a non-`const` method's `this` is treated as `T&` (a
  mutable/exclusive borrow) -- for checking purposes only; `this`'s
  actual spelling/type at expression level (`this->x`, `(*this).x`) is
  unchanged.
- **Calling a method borrows the receiver**, exactly like passing a
  reference argument to an ordinary function: `obj.f()` where `f` is
  non-`const` requires a mutable borrow of `obj` for the call (rejected
  if `obj` is already borrowed some other way); a `const` method
  requires only a shared borrow (coexists with other shared borrows of
  `obj`, rejected only against an active mutable one). The borrow is
  released per the same liveness-driven rule as any other reference
  ([§5.3](#53-lifetime)), not just at the end of the full statement.
- **Field access inside a method body** (`this->field`, or bare `field`
  if scpp's member-access sugar allows omitting `this->`) resolves back
  to `this` as its root, exactly like `a.field` resolves to root `a`
  today -- including the existing whole-root-conservative treatment
  ([§5.2](#52-borrow--aliasing)): `this->field1` and `this->field2` are
  recorded against the same root and conflict, exactly as `a.field1`/
  `a.field2` already do for an ordinary struct-typed local `a`. No new
  rule needed here -- this falls directly out of treating `this` as an
  ordinary reference.
- **`const` propagates to field access**, matching real C++: inside a
  `const` method, `this->field = x` is rejected (writing through a
  `const T&`-treated `this`), and calling a non-`const` method on a
  class-typed field through `this` is rejected the same way -- both are
  just the ordinary "can't mutate through a shared borrow" rule
  ([§5.2](#52-borrow--aliasing)) applied to `this`, not a new check.
- **`mutable` fields are the one deliberate exception to the rule
  above** (see [§4.2](ch04-struct-vs-class.md) for the full design):
  reading or writing a `mutable` field through `this` is allowed
  regardless of whether `this` is `const`-treated -- but taking a
  reference or address of a `mutable` field is rejected unconditionally
  (not just inside `const` methods), since that's what keeps this
  exception sound without needing any runtime check: a value that can
  never be referenced can never alias, checked or not.
- **The `this`-elision rule in [§5.3](#53-lifetime) is now active**:
  previously specified but dormant ("v0.1 has no class method/`this`
  concept yet, so this rule never actually applies") -- a method that
  returns a reference, with no other lifetime-group disambiguation in
  play, now genuinely defaults to borrowing from `this`'s group. This
  mirrors Rust's own third lifetime-elision rule (an elided output
  lifetime defaults to `&self`/`&mut self`'s lifetime when one is
  present).
- **Calling any method requires `this`'s pointee to be initialized**
  (not moved-out) -- the same definite-initialization precondition as
  any other dereference ([§5.4](#54-initialization)), nothing
  method-specific about it.
- **Moving out of a field through `this`** (e.g. a `unique_ptr` field)
  is restricted by the same rule an ordinary reference already
  enforces: you cannot move out of `*this` or one of its fields while
  only holding a borrow of it (a non-`const` method only ever has `T&`,
  never owns the receiver outright). Rust hits the identical wall and
  works around it with `std::mem::take`/`Option::take()` ("replace with
  a valid placeholder while moving the old value out") -- scpp doesn't
  yet have an equivalent idiom designed; flagged here as a concrete
  open follow-up, not solved by this section.

## 5.10 Function Overloading

Answers [§8](ch08-open-questions.md) Q11. scpp allows multiple functions
(free functions or methods) to share a name, distinguished by parameter
list only -- **never** by return type (a rule [§11](ch11-modules-and-libraries.md)'s
mangling scheme has reserved room for since it was first written). Real
C++'s own overload resolution ranks implicit-conversion sequences (Exact
Match > Promotion > Conversion > ...), which turns out to be considerably
more surprising than it looks when actually exercised: promotion targets
are specifically `int`/`unsigned int`/`double`, not "the nearest wider
type", so which overload wins can depend on which built-in type happens to
alias the platform's actual `int` -- and two candidates that are both
merely "ordinary conversion"-tier (e.g. `int16_t` and `int64_t` competing
for an `int32_t` argument) are flatly ambiguous in real C++, with no
narrower-wins tie-break at all (verified against real compiler behavior
while designing this section, not assumed).

- **Resolution rule: exact type match only.** [§6](ch06-safe-subset.md)
  establishes that no scpp scalar type implicitly converts to or from any
  other (extending `bool`/`char`'s original rule to the whole numeric
  family) -- so overload resolution needs no conversion-ranking algorithm
  at all: a candidate is viable only if every parameter's type is
  identical to the corresponding argument's type. Because two overloads
  can never share an identical parameter-type list (that's an ordinary
  redefinition error, not two overloads), **exact-type matching can never
  itself produce an ambiguous result** -- the only two outcomes are
  "exactly one candidate matches" or "zero candidates match" (a compile
  error requiring an explicit cast at the call site, same as any other
  type mismatch). This is a deliberate departure from real C++, matching
  Rust/Swift/Kotlin instead (see [§8](ch08-open-questions.md) Q11).
- **By-value vs. by-reference is a separate, orthogonal axis.**
  `f(T)`/`f(T&)`/`f(const T&)` are three distinct, legal overloads --
  useful in scpp specifically, since they mean take-ownership/
  mutable-borrow/shared-borrow respectively. No new disambiguation logic
  is needed here: [§5.1](#51-ownership--move) already requires an
  explicit `std::move(x)` to move out of a named place, so a bare lvalue
  argument is only ever viable against a `T&`/`const T&` parameter, never
  a `T` one; conversely `std::move(x)` (or an ordinary prvalue/temporary)
  is only viable against a `T` parameter. When a mutable (non-`const`)
  lvalue makes both `T&` and `const T&` simultaneously viable, `T&` wins
  -- reused directly from real C++'s own tie-break rule (this is what
  makes `T& get(); const T& get() const;` work as two legitimate
  overloads, resolving the gap flagged in
  [§5.9](#59-methods-and-this)).
- **Scoping matches ordinary C++ name lookup.** A name declared in an
  inner scope hides the *entire* outer overload set (no merging across
  scope boundaries) -- the same rule that already applies to any other
  declaration, nothing new for overloading specifically. A
  `using foo::bar;` declaration ([§11](ch11-modules-and-libraries.md))
  imports *every* overload of `bar` visible at `foo::bar`, not just one.
- **No interaction with ADL.** The candidate set for an unqualified call
  is exactly the one lexical-scope-and-`using`-declaration lookup already
  assembles ([§11](ch11-modules-and-libraries.md)) -- consistent with, and
  made simpler by, scpp having no argument-dependent lookup at all.
- **Ambiguity is still a hard compile error in general** -- exact-type
  matching happens to make it unreachable for v0.1's scalar-type-only
  overload sets, but the rule is stated as a general principle (not
  merely "doesn't happen to arise yet") for whenever a future feature
  (templates/generics, chiefly) reintroduces the possibility.
- **Explicitly out of scope for this round**: overload resolution
  involving templates/generic functions (deferred alongside templates),
  and default parameter values (a separate, undesigned feature). Taking
  the address of an overloaded name as a function pointer, once deferred
  until function pointers themselves were designed, is now resolved by
  [§5.16](#516-function-pointers): the target pointer-to-function type
  (from the declaration being initialized, a parameter being passed, or
  an explicit cast) selects the one overload whose parameter-type-list
  and return type match it exactly -- the same target-type-driven rule
  real C++ already uses for `&overloaded_name`, reused verbatim, and
  still deterministic since exact-type matching (above) guarantees at
  most one candidate can ever match a given target type.

## 5.11 Generic Functions and Concepts

Answers [§8](ch08-open-questions.md) Q12. scpp's answer to compile-time
polymorphism, reusing real C++20 `concept`/`requires` syntax and the
abbreviated function-template form (`Concept auto` parameters) verbatim --
deliberately instead of inheritance/virtual functions (which stay
deferred, see [§4.2](ch04-struct-vs-class.md)). Every call is
monomorphized (a separate copy of the code generated per concrete type,
exactly like real C++ templates/Rust generics), so this is zero-cost:
no vtable, no runtime dispatch at all.

```cpp
template<typename T>
concept Shape = requires(const T& t) {
    { t.area() } -> std::same_as<double>;
};

void print_area(const Shape auto& s) {
    // s.area() is legal -- the concept guarantees it; nothing else about s is
}
```

- **A `concept` is a compile-time predicate over one type, spelled and
  checked exactly like real C++20** -- no changes to the grammar or
  semantics of `concept`/`requires` themselves.
- **Satisfaction is structural, exactly like real C++ (not nominal, unlike
  Rust's `impl Trait for Type`).** A type satisfies a concept purely by
  having matching members -- no explicit "this type implements this
  concept" declaration exists or is required. This was a deliberate,
  considered choice: Rust's nominal model (explicit `impl` blocks) avoids
  a real, known category of bug (a type "accidentally" satisfying a
  concept it was never intended to support, since two unrelated concepts
  happening to both require a same-named, same-shaped method would
  silently both match) -- but real C++ has no `impl`-block-equivalent
  syntax to reuse for it, and inventing one would be exactly the kind of
  new, non-erasable grammar [ch00](ch00-design-philosophy.md) §2/§6
  rules against. **Revisit only if a future C++ standard itself adds a
  nominal opt-in mechanism** -- the same resolution already applied to
  the rejected `??` operator (see [§8](ch08-open-questions.md) Q8).
- **A concept-constrained function's body is fully checked once, at its
  own definition, treating the constrained parameter's type abstractly**
  -- only operations the concept's `requires`-expression actually
  guarantees are legal inside the body; anything else is a compile error
  at the generic function's own definition site, regardless of what any
  particular instantiation would allow. This is a deliberate departure
  from real C++ templates (even concept-constrained ones), which still
  defer most body type-checking to instantiation time ("two-phase
  lookup") -- the reason a real C++ template's error messages are
  notoriously tied to whatever concrete type triggered instantiation,
  rather than the generic definition itself. scpp's choice matches Rust's
  trait-bound model instead, and is required to keep this chapter's
  checking properly intraprocedural (see
  [§11.6](ch11-modules-and-libraries.md)) -- a generic function has no
  single concrete signature to check a body against otherwise.
- **A concept is optional, not mandatory, on a constrained parameter.**
  Writing bare `auto` (no concept name in front of it) is legal -- it
  means the parameter's type is treated as fully opaque: the body may
  move it, store it, pass it to another function accepting a compatible
  (bare-or-narrower) parameter, and return it, but calling any method on
  it or applying any operator to it is a compile error, exactly as if it
  were constrained by a concept whose `requires`-expression guarantees
  nothing. Naming a concept (`Concept auto`) unlocks whatever its
  `requires`-expression additionally guarantees, checked the same
  once-at-definition way. The same rule governs a generic type's own
  type parameter ([§5.14](#514-generic-types)).
- **Parameter packs are supported in both generic-function spellings, but
  not with identical body-level power.** An abbreviated-form pack
  (`Concept auto&... args`) is supported, but only usable through a fold
  expression -- real C++17 syntax verbatim (`(pack op ...)`,
  `(... op pack)`, or with an initial value on either side). A fold
  expression's meaning is defined for a pack of any length, including
  zero, so checking one reduces to checking the folded operator once
  against what the concept guarantees per element -- no different in kind
  from checking any other concept-guaranteed operation, and still
  entirely at the generic function's own definition:
  ```cpp
  template<typename T>
  concept Formattable = requires(const T& t, std::ostream& os) { os << t; };

  void log(const Formattable auto&... args) {
      (std::cout << ... << args) << "\n";
  }
  ```
  A **full-header-form** pack
  (`template<typename... Args> void f(Args... args)`) is likewise
  supported, and may additionally be forwarded as a pack expansion in
  another call or construction argument list (`g(args...)`,
  `new T(args...)`) because the template header names the pack directly.
  **What is still not supported is recursively splitting a pack inside a
  function body** (peeling off a first element and recursing on the rest,
  the classic C++ variadic-template idiom) -- each recursive step would
  need its own separately-checked signature, breaking the
  once-at-definition property. Recursive pack-splitting is, however, how
  a generic *type*'s own storage is built ([§5.14](#514-generic-types))
  -- there, it's the type definition itself doing the recursion, with
  each step still checked once, not a function body's control flow.
- **Compound requirements (`{ expr } -> Constraint;`) must constrain the
  result to an exact type, spelled `std::same_as<T>`** -- never
  `std::convertible_to<T>`, and never a bare type name (`-> T` is not
  legal C++ grammar at all: a `type-constraint` there must name a
  concept, not a type; verified against a real compiler while designing
  this). `std::convertible_to` would be meaningless in scpp anyway, since
  [§6](ch06-safe-subset.md) already establishes no scpp scalar type
  implicitly converts to another -- "convertible to" and "same as"
  collapse to the same thing. The exact type named this way is what the
  generic function body may treat the expression's result as.
- **Simple requirements (`{ expr };`, no `->` clause) constrain nothing
  about the result's type** -- consistent with the point above, the
  generic body may only use such an expression as a discarded
  expression-statement (called for its side effect); binding its result
  to anything, or using it in any type-dependent way, is a compile error,
  since there is genuinely no type to reason about under
  once-at-definition checking.
- **Type-requirements (`typename T::Foo;`) and nested requirements
  (arbitrary boolean constant-expressions) are not supported in v0.1** --
  scpp has no associated-type/nested-type-alias mechanism yet for the
  former, and the latter is a much more open-ended feature (arbitrary
  compile-time predicates over types) than this round is scoped to
  design.
- **Generic functions may be spelled with either the abbreviated C++20
  form** (`void f(Concept auto& x)`) **or the full header form**
  (`template<Concept T> void f(T& x)`) -- real C++ treats the two as
  fully equivalent, and so does scpp; neither is preferred or
  restricted. The full header form also allows multiple type parameters
  (`template<typename T, typename U> void f(T& a, U& b)`, matching
  generic types, [§5.14](#514-generic-types)) and a "return-type-only"
  generic function -- a type parameter with no corresponding
  function-parameter position at all, requiring the caller to supply it
  explicitly at the call site (e.g. `template<typename T> T make();
  make<Circle>();`) -- something the abbreviated form cannot express,
  since it always ties a constrained parameter to a function parameter's
  own declared position. This is exactly what
  [§5.14](#514-generic-types)'s base-class-deduction accessor pattern
  (`get<I>`) needs: an explicit, non-deduced non-type argument at the
  call site, and a parameter type -- naming a class-template
  specialization -- that no `Concept auto` placeholder could spell.
- **No default method bodies in a concept** -- unlike a Rust trait,
  which can supply a default implementation a type may inherit or
  override, a real C++ `concept` is purely a structural predicate; it
  cannot carry a method body at all. This isn't a scpp-specific
  restriction, it's what `concept` already means in real C++.
- **Mangling needs no new mechanism.** A monomorphized instantiation's
  parameter types are, by the time codegen runs, ordinary concrete types
  (e.g. `print_area(const Shape auto&)` instantiated for `Circle` is
  exactly `print_area(const Circle&)`) --
  [§11.9](ch11-modules-and-libraries.md)'s existing parameter-type
  encoding already gives every distinct instantiation a distinct mangled
  symbol, for the same reason it already disambiguates ordinary
  overloads.
- **Explicitly out of scope for this round**: recursive pack-splitting
  inside a function body (as opposed to fold expressions, above),
  explicit specialization of a function template, template-template
  parameters, default template arguments, associated types, and
  **general** dynamic dispatch / object-safe interface erasure
  (scpp's virtual-function/`dyn`-equivalent, deferred alongside
  inheritance). Generic `struct`/`class` types, once out of scope
  entirely, are now designed -- see [§5.14](#514-generic-types). A
  narrower, callable-specific owning erasure layer
  (`std::function`/`std::move_only_function`) is designed separately in
  [§5.18](#518-type-erased-call-wrappers-stdfunction-and-stdmove_only_function).

## 5.12 Closures (Lambda Expressions)

Reuses real C++ lambda syntax verbatim -- `[capture-list](params) { body }`,
including `mutable`, trailing return types, and generic (C++14, `auto`
parameter) lambdas. A lambda expression's type is, exactly as in real
C++, an anonymous, compiler-synthesized class: one member per captured
name, plus an `operator()` implementing the body. This is not a new
concept -- it is the same desugaring real C++ already performs -- so
every existing `struct`/`class` rule ([§4](ch04-struct-vs-class.md))
applies to a closure's members directly, with no new machinery:

- **A by-value capture** (`[x]`, or an init-capture `[x = expr]`) is an
  ordinary owned member, copied or moved in exactly like initializing a
  `class` member from an argument ([§4.2](ch04-struct-vs-class.md)),
  subject to the same move rules as anywhere else
  ([§5.1](#51-ownership--move)). Init-capture is how a move-only type
  crosses into a closure, exactly like passing one to a constructor:
  `[p = std::move(p)]` for a `std::unique_ptr<T> p`.
- **A by-reference capture** (`[&x]`) is a reference-typed member --
  forbidden in a `struct` but allowed in a `class`
  ([§4.1](ch04-struct-vs-class.md)/[§4.2](ch04-struct-vs-class.md)) -- so
  the closure value itself becomes a lifetime-tracked value, exactly like
  a `class` holding a `T&`/`const T&` field, or `std::span`. It
  participates in the same alias-XOR-mutability and dangling checks as
  any other reference-holding value
  ([§5.1](#51-ownership--move)-[§5.3](#53-lifetime)): the borrowed local
  cannot be moved, reassigned, or allowed to go out of scope while the
  closure is still alive. Capturing more than one name by reference
  (`[&a, &b]`) ties the closure's own lifetime to all of them jointly --
  the same conservative default-grouping treatment
  [§5.3](#53-lifetime) already gives a function with several ungrouped
  reference parameters.
- **`[=]`/`[&]` (whole-scope implicit captures), and mixed forms like
  `[&, x]`/`[=, &y]`, are accepted as-is** -- scpp does not require every
  capture to be individually named. Real C++ does not treat blanket
  captures as something to avoid in general, so scpp adds no restriction
  real C++ itself does not call for.
- **Exception: `this`/`*this` must always be captured explicitly.** A
  bare `[=]` or `[&]` implicitly capturing `this` (because the lambda is
  written inside a method and reads a member) is a **compile error** --
  write `[this]`, `[*this]`, `[=, this]`, or `[&, this]` instead. Real
  C++20 only *deprecates* this (P0806R2), because `[=]`'s implicit `this`
  capture is genuinely misleading: it looks like the whole receiver is
  copied, but it actually captures a raw pointer to it -- a real,
  documented source of use-after-free bugs if the closure outlives the
  object. scpp makes this a hard error rather than a deprecation warning,
  matching how this spec already treats every other recognized C++
  footgun (e.g. [§6](ch06-safe-subset.md)'s bare `unsigned` ban). This
  rule is dormant in practice until class-method-body checking is itself
  designed ([§4.2](ch04-struct-vs-class.md); [§5.9](#59-methods-and-this)
  covers only what is checked so far) -- specified now so it is already
  the rule the day method bodies gain full checking, rather than a gap
  discovered later.

**Calling a closure** (`c(args)`) is an ordinary call to its
(compiler-synthesized) `operator()`, checked exactly like any other
method call ([§5.9](#59-methods-and-this)) -- nothing closure-specific
about it.

**Passing a closure to another function** has two distinct shapes. The
zero-cost path uses a concept-constrained generic parameter
([§5.11](#511-generic-functions-and-concepts)) and stays monomorphized
per concrete closure type:

```cpp
template<typename T>
concept IntConsumer = requires(T f, int x) { f(x); };

void for_each_doubled(std::span<int> s, IntConsumer auto&& f) {
    for (int i = 0; i < s.size; ++i) f(s[i] * 2);
}
```

This remains the default for algorithm-style code. The owning,
type-erased path is [§5.18](#518-type-erased-call-wrappers-stdfunction-and-stdmove_only_function)'s
`std::function<Sig>` / `std::move_only_function<Sig>` family instead:
use those when what matters is storing, returning, or heterogeneously
owning "some callable of this signature", not preserving the concrete
closure type at compile time.

**Nothing new is needed to stop a reference-capturing closure from
escaping** -- e.g. into a global array, via some other function it's
passed to. [§5.3](#53-lifetime)'s intraprocedural model, and
[ch11 §11.9](ch11-modules-and-libraries.md#119-soundness-cross-module-signatures-are-all-the-checker-needs)'s
restatement of it, already establish that a caller only ever needs a
callee's *signature*, never its body: if some function's own body stores
its closure parameter into `'static`-duration storage, that function's
own signature must already promise its parameter lives that long,
checked once at that function's own definition -- otherwise that
function itself would never have compiled. A closure tied to a
short-lived local simply does not satisfy such a signature, so passing it
to that function is rejected at the call site, without the caller's own
checker ever inspecting the callee's body.

## 5.13 Lifetime-Generic Parameters

Lets library code accept a closure, verify -- through an ordinary
`concept` -- that the closure's own parameter is safe to call with a
reference of *any* lifetime, and then actually call it with a value the
library's own function body creates internally, whose lifetime the
closure's author could never have named in advance. This is a minimal,
targeted extension of [§5.3](#53-lifetime)'s existing named-group
mechanism and [§5.11](#511-generic-functions-and-concepts)'s
concept-constrained generics -- one reserved group name, plus one new
piece of concept-checking semantics, no new grammar. It is what makes a
Rust-`thread::scope`-shaped API (a callback whose parameter's lifetime is
chosen by the callee, not the caller) expressible as ordinary library
code, rather than needing to be a compiler-hardcoded special case like
`std::thread`/`std::span` -- designing that library API itself is
separate follow-up work, not part of this language-level definition.

- **`[[scpp::lifetime(generic)]]` is a reserved group name.** Tagging a
  reference parameter this way assigns it a fresh, compiler-synthesized
  group that no other group anywhere in the program can ever be unified
  with, because `generic` does not name a group any user-written code can
  otherwise spell or reuse. This needs no new body-checking machinery --
  it is exactly [§5.3](#53-lifetime)'s existing "groups are mutually
  independent unless explicitly unified" rule, applied to a group nobody
  else can ever name. A consequence already implied by that existing
  rule, not a separately-stated one: within the function or closure's own
  body, a `generic`-tagged parameter (or anything derived from it) may be
  used for ordinary, synchronous operations -- read it, call a method on
  it, pass it on to another function accepting a compatible bound -- but
  it can never be written into any of the function's own by-reference
  captures, any other named group, the function's return value, or
  global/static storage: there is no group it could legally be unified
  with to permit any of that.
- **A `requires`-expression's own probe parameter may carry the same
  tag.** Tagging a compound requirement's probe parameter
  `[[scpp::lifetime(generic)]]` changes what that requirement checks:
  ```cpp
  template<typename T>
  concept AcceptsToken = requires(T a, Token& tok [[scpp::lifetime(generic)]]) {
      { a(tok) } -> std::same_as<void>;
  };
  ```
  is satisfied only if `T`'s own matching call-operator/function
  parameter is *also* declared `[[scpp::lifetime(generic)]]` at its own
  definition -- not merely "callable with some `Token&`". This is new
  concept-checking semantics attached to an already-legal C++20 grammar
  position (a `requires`-expression's parameters may already carry
  attributes, and a real compiler already accepts and ignores ones it
  doesn't recognize, see [ch00](ch00-design-philosophy.md) §2) -- real
  C++20 concepts have no notion of inspecting a parameter's lifetime-group
  at all, so this is scpp-specific semantics, not new syntax.
- **Call-site exemption.** Once a value's type is known -- through such a
  concept, or because the concrete callable's own declaration is directly
  visible -- to have a `generic`-tagged parameter, calling it with *any*
  concrete argument is unconditionally permitted, regardless of what
  group that argument belongs to, including a reference whose lifetime is
  invented fresh inside the *caller's own* body (a local variable the
  callee could never have named in advance):
  ```cpp
  void with_fresh_token(AcceptsToken auto&& f) {
      Token tok;   // invented here, inside this function's own body
      f(tok);      // permitted: f's own parameter is declared lifetime-generic
  }
  ```
  Ordinarily, passing a reference argument requires it to satisfy
  whatever group the parameter belongs to (see [§5.3](#53-lifetime));
  this is the one exemption to that rule, justified precisely because the
  previous two rules already guarantee the callee's own body cannot do
  anything requiring the argument to outlive the single call.

This reaches the same soundness pattern real Rust's `std::thread::scope`
relies on via a higher-ranked trait bound
(`for<'scope> FnOnce(&'scope Scope<'scope, 'env>) -> T`), without adding
generics over lifetimes as their own first-class category:
[§5.3](#53-lifetime) already declined `'a`-style lifetime syntax in
favor of the simpler, non-generic named-group mechanism, and these three
rules extend that same mechanism rather than reversing that choice --
covering the one pattern (a callback whose parameter's lifetime is chosen
by the callee, not the caller) the plain grouping mechanism could not
otherwise reach.

## 5.14 Generic Types

Extends [§5.11](#511-generic-functions-and-concepts)'s answer to
compile-time polymorphism from functions to `struct`/`class`
definitions themselves -- real C++ template syntax verbatim
(`template<typename T> class X { ... };`), including multiple type
parameters and parameter packs, with the same concept-optional
principle §5.11 established, applied per-member instead of
monolithically:

- **A generic type's own type parameter(s) may be left bare** -- exactly
  like a bare generic-function parameter (§5.11), this means "no
  operations guaranteed beyond the universal baseline every scpp type
  already has" (move, store as a field, pass to a compatible parameter,
  return -- see [§5.1](#51-ownership--move)). This is enough to declare
  fields of the bare type and write ordinary constructors/accessors
  around them; it is not enough to call any method on the type or apply
  any operator to it.
- **Each method (including a constructor) may add its own `requires
  Concept<T>` clause**, real C++20 syntax verbatim, unlocking whatever
  that concept additionally guarantees *for that one method's own body,
  checked once at that method's own definition* -- exactly the same
  once-at-definition principle as a generic function, just decomposed
  per member instead of applied to one shared, class-wide constraint:
  ```cpp
  template<typename T>
  class Vec {
      T item;
  public:
      Vec(T x) : item(std::move(x)) {}

      void push(T x) { item = std::move(x); }         // needs nothing from T

      bool less_than(const T& other) const requires std::totally_ordered<T> {
          return item < other;                          // only this method needs comparison
      }
  };
  ```
  A caller using `Vec<SomeType>` may call `push` regardless of what
  `SomeType` supports; calling `less_than` is only legal if `SomeType`
  actually satisfies `std::totally_ordered` -- checked, and rejected
  with a precise diagnostic, at the call site, not deferred instantiation
  gibberish.
- **A generic `struct`'s type parameter(s) cannot be left bare** --
  unlike `class`, a `struct`'s fields must *all* be trivial
  ([§4.1](ch04-struct-vs-class.md)), and triviality is a whole-type
  layout/ABI property, not something individual methods could each
  separately guarantee (a `struct` has no methods to decompose it
  across in the first place). A generic `struct`'s type parameter(s)
  must therefore be constrained, at the `struct` itself, by a concept
  that guarantees triviality.
- **Parameter packs (`typename... Ts`) are supported for building a
  type whose own member layout varies by arity** (e.g. a `Tuple`-like
  type), but only via **recursive inheritance** -- real C++ has no
  syntax to expand a pack directly into a member list (`Ts... elems;`
  is proposed, as [P1858](https://wg21.link/P1858), but not adopted
  into any C++ standard as of C++23; verified it is rejected by a
  current compiler, and scpp's erasure principle
  ([ch00](ch00-design-philosophy.md) §2) requires every construct to be
  accepted by a real, unmodified C++ compiler). Exactly two, fixed
  patterns are supported for a variadic generic type -- not
  arbitrary/general specialization:
  ```cpp
  template<typename... Ts> class Tuple;

  template<> class Tuple<> {};   // base case: the empty pack

  template<typename Head, typename... Tail>
  class Tuple<Head, Tail...> : private Tuple<Tail...> {   // recursive case
      Head head;
  };
  ```
  The base case is the *empty* pack (`Tuple<>`), not one remaining
  element -- a parameter pack matching zero arguments is itself
  ordinary, standard C++ ([temp.variadic], since C++11), not a special
  case scpp invented; real `std::tuple` implementations (libstdc++
  verified directly) instead stop recursing at exactly one remaining
  element, purely as their own implementation-specific micro-optimization
  (saving one level of an otherwise-empty base class) -- the C++
  standard mandates no particular internal layout for `std::tuple` at
  all (only that `get` be constant-time, see below), so scpp is free to
  pick the simpler, uniform empty-pack base case instead.
- **Non-type template parameters are supported, restricted to scalar
  types** ([§6](ch06-safe-subset.md)'s scalar family: `bool`, the fixed-width
  integers, `char`, `float32_t`/`float64_t`, `size_t`, `ptrdiff_t`, and
  their aliases) -- excluding `struct`/`class`, which sidesteps real
  C++20's "structural type" machinery (memberwise-comparison-based
  template-argument matching) entirely, since nothing so far needs a
  class-typed non-type parameter. Two non-type arguments are the same
  template argument if and only if their **bit patterns are identical**
  -- this one rule is uniformly correct for every scalar: it collapses
  to ordinary value equality for integers (no scalar integer
  representation is redundant, doubly so since C++20 mandates
  two's-complement), and it is, verified against a real compiler, *the*
  actual rule real C++ already applies to floating-point non-type
  arguments (`0.0` and `-0.0` compare equal but are **not** the same
  template argument, since their bit patterns differ; two independently
  computed `NaN`s with identical bit patterns **are** the same template
  argument, even though `NaN == NaN` is false) -- so this is one uniform
  rule, not two, and it is not a new one.
- **Indexed access into a recursively-defined variadic type reuses
  deduction from a base class** -- real, standard C++ behavior
  ([temp.deduct.call]), not a scpp-specific mechanism: if a function
  template's parameter type names a class-template specialization, and
  the actual argument's type has a unique base class (direct or
  indirect) matching that specialization pattern, the compiler deduces
  the function's own template parameters from that base class's actual
  arguments. Threading a scalar, non-type index through the recursive
  definition above (mirroring how real `std::tuple` is actually
  implemented internally -- verified directly against libstdc++'s
  `<tuple>`) lets an accessor deduce, in one step, exactly which level
  of the inheritance chain holds the requested element -- giving
  constant-time indexed access (the one guarantee the C++ standard
  actually places on `std::get`) without the accessor's own body ever
  recursing:
  ```cpp
  template<size_t Idx, typename... Ts> class TupleImpl;

  template<size_t Idx> class TupleImpl<Idx> {};   // empty pack: recursion's natural end

  template<size_t Idx, typename Head, typename... Tail>
  class TupleImpl<Idx, Head, Tail...> : public TupleImpl<Idx + 1, Tail...> {
  public:
      Head value;
  };

  // Full header form (see §5.11) -- needed here since Head/Tail are
  // deduced from a base class, and I is supplied explicitly by the
  // caller (get<2>(t)), neither of which the abbreviated auto form
  // could express.
  template<size_t I, typename Head, typename... Tail>
  Head& get(TupleImpl<I, Head, Tail...>& t) { return t.value; }
  ```
- **Explicitly out of scope for this round**: general/arbitrary
  specialization (beyond the one fixed empty-pack/head-and-tail pattern
  above), template-template parameters, default template arguments,
  class-typed non-type template parameters, and associated types.

## 5.15 Thread-Safety Structural Properties: `[[scpp::thread_movable]]`, `[[scpp::thread_shareable]]`, and `[[scpp::thread_movable_if(a, b)]]`

Lets library code (a thread-spawning function, for instance) require,
via an ordinary parameter attribute, that whatever it's handed is
actually safe to use across a genuine concurrency boundary. The two
properties involved are still the same -- thread-movable and
thread-shareable -- but scpp exposes them in three surface forms:

- the parameter/type-declaration attributes `[[scpp::thread_movable]]`
  and `[[scpp::thread_shareable]]`;
- the conditional type-declaration attribute
  `[[scpp::thread_movable_if(a, b)]]`; and
- the builtin predicates `scpp::is_thread_movable(T)` and
  `scpp::is_thread_shareable(T)`.

Applied to a generic function's parameter, `[[scpp::thread_movable]]`
or `[[scpp::thread_shareable]]` constrains that parameter's (possibly
template-deduced) type to satisfy the corresponding property, exactly
like stacking `[[scpp::lifetime(name)]]` on a parameter
([§5.3](#53-lifetime-groups)). Applied instead to a `struct`/`class`'s
own declaration, the attributes become the library author's explicit
statement of that type's thread-safety contract, overriding what the
structural derivation below would otherwise conclude on its own --
mirroring Rust's explicit `unsafe impl Send`/`unsafe impl Sync`.

The builtin predicates are compiler intrinsics, not ordinary function
calls: they take a **type name** in parentheses, exactly the way a real
C++ builtin trait is written (`__is_trivially_copyable(T)`-style), and
may appear anywhere a boolean constant-expression is allowed. For any
type `T`, `scpp::is_thread_movable(T)` means: "the thread-movable value
of `T` after applying any override on `T` itself, otherwise `T`'s
structural result below"; `scpp::is_thread_shareable(T)` is defined the
same way for the thread-shareable property.

- **`[[scpp::thread_movable]]`** answers: can a value of this type be
  handed, by value, to a newly spawned thread, such that the spawning
  thread retains no further access to it? (Mirrors Rust's `Send`.)
- **`[[scpp::thread_shareable]]`** answers: is it safe for two or more
  threads to simultaneously hold a `const T&` to the same object?
  (Mirrors Rust's `Sync`.)
- **Structural derivation** (the default, absent a manual override on
  the type itself), computed recursively:
  - Every scalar type: both properties hold.
  - A type containing a reference member (`T&`/`const T&`/`T&&` -- whether an
    ordinary `class` field or a by-reference closure capture, see
    [§5.12](#512-closures-lambda-expressions)) is never thread-movable
    -- moving such a value to another thread does not transfer the
    referent's ownership, so doing so would not, unlike moving an owned
    value, make the referent inaccessible to the original thread.
  - `struct`/`class` (with no reference member): thread-movable and
    thread-shareable each hold if and only if every member itself has
    that property -- **except** a `mutable` member
    ([§4.2](ch04-struct-vs-class.md)), which makes the whole type
    thread-movable but **never** thread-shareable (mirrors Rust's
    `Cell<T>`: safe to hand exclusively to one thread at a time, unsafe
    for two threads to simultaneously read through what looks like a
    shared reference, since `mutable` is precisely the sanctioned way
    to write through one).
  - Raw pointer `T*`: neither, by default -- matching how a raw pointer
    already requires vouching for anything the checker cannot verify on
    its own ([§5.5](#55-prohibited-unless-in-scppunsafe)); vouch by
    wrapping it in a `struct`/`class` marked `[[scpp::thread_movable]]`/
    `[[scpp::thread_shareable]]`, below.
  - A closure ([§5.12](#512-closures-lambda-expressions)): thread-movable
    if and only if it has no by-reference capture at all (see above) and
    every by-value-captured member's type is itself thread-movable.
    Thread-shareable if and only if it has no by-*mutable*-reference
    capture (concurrent calls through a shared mutable reference could
    race), every by-value-captured member's type is thread-shareable,
    and every by-*const*-reference-captured member's referent type is
    thread-shareable.
- **Manual override on a type declaration**: a `struct`/`class` may be
  declared `[[scpp::thread_movable]]` and/or `[[scpp::thread_shareable]]`
  directly, on its own definition:
  ```cpp
  struct [[scpp::thread_movable]] RawBufferHandle {
      int* data;
      int len;
      // the author has verified this type's own invariants make it
      // sound to hand to another thread, even though the structural
      // rule above would say "no" for any type containing a raw pointer
  };
  ```
  This overrides the structural derivation above entirely for that type
  -- exactly like Rust's `unsafe impl Send for RawBufferHandle {}` --
  and is the only way to make a type whose structure the compiler cannot
  verify on its own (most commonly, one containing a raw pointer)
  participate in either property at all.
- **Conditional override on a type declaration**: a (typically generic)
  `class` may instead declare
  `[[scpp::thread_movable_if(a, b)]]`, where `a` and `b` are boolean
  constant-expressions evaluated separately for each instantiation. The
  first argument becomes the type's own thread-movable value; the second
  becomes its own thread-shareable value. Together they replace, for that
  instantiation of that type, whatever the structural derivation above
  would otherwise have concluded from the type's fields alone. The usual
  way to write such conditions is in terms of the builtin predicates on
  the type's parameters:
  ```cpp
  template<typename T>
  class [[scpp::thread_movable_if(
      scpp::is_thread_movable(T),
      scpp::is_thread_shareable(T)
  )]] MyOwningBox {
      T* ptr;
  };
  ```
  This is the general mechanism for expressing "my internal representation
  contains pieces (for example raw pointers) whose bare structure is not
  enough for the compiler to infer the real concurrency invariant, but I,
  the type author, can state that invariant explicitly."
- **`std::unique_ptr<T>` is one worked example of that general mechanism.**
  Its implementation may explicitly say:
  ```cpp
  template<typename T>
  class [[scpp::thread_movable_if(
      scpp::is_thread_movable(T),
      scpp::is_thread_shareable(T)
  )]] unique_ptr {
      // ...
  };
  ```
  This is **not** a compiler-hardcoded exception for the library type
  name `std::unique_ptr`. It is ordinary library code using the same
  attribute any user-defined owning box can use. The point of the
  override is that `unique_ptr`'s raw-pointer field, viewed by structure
  alone, is indistinguishable from an arbitrary aliased pointer; the
  library author is explicitly vouching for the stronger invariant that
  this pointer is the unique owner, so handing the whole `unique_ptr` to
  another thread transfers that exclusive access as well. This mirrors
  Rust's explicit conditional `Send`/`Sync` impls for uniquely owned heap
  pointers.
- **`std::shared_ptr<T>` is another worked example, but with a conjunctive
  rule rather than independent forwarding.** Its declaration may say:
  ```cpp
  template<typename T>
  class [[scpp::thread_movable_if(
      scpp::is_thread_movable(T) && scpp::is_thread_shareable(T),
      scpp::is_thread_movable(T) && scpp::is_thread_shareable(T)
  )]] shared_ptr {
      // ...
  };
  ```
  Again, this is an ordinary library declaration using the general
  override mechanism, not a magic compiler rule for one specific type
  name. The conjunction matters: moving one `shared_ptr` handle to
  another thread does **not** revoke access through any other surviving
  handle, so the pointee must already be safe both to move across a
  thread boundary and to be shared across one.
- **Constraining a parameter**: a thread-spawning function attaches
  either attribute directly to its closure parameter, exactly like
  attaching `[[scpp::lifetime(name)]]`:
  ```cpp
  template<typename T>
  void spawn(T&& f [[scpp::thread_movable]]) {
      // ...
  }
  ```
  The compiler checks, at each call to `spawn`, whether the deduced
  type of `f`'s argument is thread-movable (by the structural rule above,
  or by its own manual override) -- ill-formed if not, precisely where
  any other parameter-attribute violation would already be rejected.
- **A by-reference capture's safety, where relevant, rests on what's
  already there.** A *mutable* reference capture (`[&x]` binding a
  non-`const` `x`) is already an exclusive borrow
  ([§5.2](#52-borrow--aliasing)): alias-XOR-mutability alone already
  guarantees no other thread can be concurrently touching `x`, which is
  why thread-movable/thread-shareable do not need to, and do not,
  further constrain it. A *shared* (`const`) reference capture, by
  contrast, can coexist with other shared borrows of the same root,
  including ones held by other threads (for example, two different
  scoped threads each capturing `const T&` from the same enclosing
  scope) -- this is exactly the case thread-shareable exists to answer
  for.

## 5.16 Function Pointers

Real C/C++ function pointer syntax, reused verbatim, plus one addition: a
pointer-to-function type is either *unsafe-qualified* or not, tracking, as
part of the type itself, whether calling through it needs an unsafe
context -- exactly parallel to Rust's own `fn` vs `unsafe fn` pointer
types (formalized in
[the formal spec's §5.2](../../spec/en/01-unsafe.md#52-function-pointer-types-dclptrscppunsafe)).

- **Grammar and spelling: identical to real C++.** `RetType (*p)(ParamTypes...)`
  declares `p` as a pointer to function; an ordinary function name, or
  `&function-name`, decays to a value of that type
  ([expr.unary.op]/[conv.func]), exactly as in real C++. Nothing about
  this needs new syntax.
- **Why a plain function pointer type isn't enough.** [§1.2](ch01-safety-context.md)
  already lets a function's own declaration opt into call-site gating
  (`[[scpp::unsafe]] RetType f(...)`) -- calling `f` then requires an
  unsafe context. If taking `f`'s address produced an ordinary, ungated
  pointer-to-function value, storing it in a plain-looking variable and
  calling through that variable would silently bypass the gate -- the
  exact hazard [§5.5](#55-prohibited-unless-in-scppunsafe)'s enumerated
  list exists to close everywhere else. So the pointer's type itself must
  carry the same obligation.
- **Spelling the marked type: `[[scpp::unsafe]]` right after the `*`.**
  ```cpp
  int (* [[scpp::unsafe]] up)(int, int);  // unsafe-qualified
  int (*                  sp)(int, int);  // not unsafe-qualified (default)
  ```
  This is the one placement real C++ grammar already gives an attribute
  on a pointer declarator (`T* [[attr]] p;`), so, like
  `[[scpp::unsafe]]`'s other two placements ([§1.3](ch01-safety-context.md)),
  it introduces no new grammar. Two other candidate positions were
  considered and rejected: immediately before the `*` (where a vendor
  calling-convention keyword like MSVC's `__stdcall` goes) is not a
  position real `[[...]]` attribute grammar accepts at all (verified:
  clang rejects it, "an attribute list cannot appear here"); after the
  parameter list (attaching to the *function type*, per
  [§1.3](ch01-safety-context.md)'s own note about that position) also
  parses, but attaching the marker to the *pointer* itself, right where
  it is written, was chosen as the more direct spelling.
- **Taking a function's address picks the type automatically.** The
  address of an ordinary function -- one whose own declaration is not
  `[[scpp::unsafe]]`-marked -- is a value of the *not*-unsafe-qualified
  pointer type. The address of a function whose own declaration *is*
  `[[scpp::unsafe]]`-marked, or of an `extern "C"` declaration with no
  body ([§2.1](ch02-boundary-rules.md), already gated the same way,
  [§5.5](#55-prohibited-unless-in-scppunsafe)), is a value of the
  unsafe-qualified pointer type. There is nothing to annotate at the
  point the address is taken -- the flavor follows from the function
  being pointed to, not from how the pointer variable happens to be
  spelled.
  ```cpp
  [[scpp::unsafe]] int get_unchecked(int* base, int index) { return base[index]; }
  int add(int a, int b) { return a + b; }

  int (* [[scpp::unsafe]] up)(int*, int) = get_unchecked;  // OK
  int (*                  sp)(int, int)  = add;            // OK
  int (*                  bad)(int*, int) = get_unchecked; // ill-formed:
                                     // get_unchecked's address is
                                     // unsafe-qualified, bad isn't
  ```
- **One-directional implicit conversion: not-unsafe-qualified →
  unsafe-qualified only.** A not-unsafe-qualified pointer-to-function
  value can always be stored in an unsafe-qualified pointer variable
  (harmless -- it only ever *widens* the caller's obligation, never
  removes a real one); the reverse is rejected. This mirrors real
  C++17's own rule for `noexcept` function pointers (a pointer to a
  `noexcept` function converts to a plain one, never the reverse,
  [conv.fctptr]) -- the identical shape of rule, applied to a different
  promise.
  ```cpp
  int (* [[scpp::unsafe]] up2)(int, int) = add;   // OK: widening
  int (*                  sp2)(int*, int) = get_unchecked;  // ill-formed
  ```
- **Calling through the pointer.** Calling through a not-unsafe-qualified
  pointer is an ordinary, ungated operation -- exactly like calling a
  named ordinary function, since by construction only an ordinary
  function's address can ever have populated it. Calling through an
  unsafe-qualified pointer joins
  [§5.5](#55-prohibited-unless-in-scppunsafe)'s list: it requires an
  unsafe context, just like calling an `[[scpp::unsafe]]`-marked function
  by name.
  ```cpp
  int r1 = up(base, 0);                   // ill-formed: outside unsafe context
  int r2;
  [[scpp::unsafe]] { r2 = up(base, 0); }  // OK
  int r3 = sp(1, 2);                      // OK: sp is never unsafe-qualified
  ```
  This is a genuinely new gated operation, not a restatement of an
  existing one: [§5.5](#55-prohibited-unless-in-scppunsafe)'s existing
  "calling a function marked `[[scpp::unsafe]]`" entry only covers a call
  that names the function directly -- a call through a pointer's
  *postfix-expression* denotes the pointer value, not the function
  itself, so it needed its own rule.
- **Calling is not dereferencing.** `fp(args)` (or the equivalent
  `(*fp)(args)`, also legal, exactly as in real C++) never triggers
  [§5.5](#55-prohibited-unless-in-scppunsafe)'s raw-pointer-dereference
  gate: that gate is about reading data *through* a pointer
  ([§5.7](#57-address-of-x-and-raw-pointers)), and invoking code at an
  address is a distinct operation with its own rule, stated above. A
  not-unsafe-qualified function pointer therefore never needs
  `[[scpp::unsafe]]` to call, no matter how it's written.
- **Trivial, like any other raw pointer.** A function pointer (either
  flavor) carries no compiler-tracked lifetime and never participates in
  move/borrow checking
  ([§5.1](#51-ownership--move)-[§5.2](#52-borrow--aliasing)) -- the same
  treatment `T*` already gets ([§4.1](ch04-struct-vs-class.md)). It is
  freely copyable and a legal `struct` member.
- **Resolves [§5.10](#510-function-overloading)'s deferred item**:
  `&overloaded_name` now has a rule -- see there.
- **Explicitly out of scope for this round**: pointer-to-member-function
  (`RetType (Class::*)(ParamTypes...)`), which additionally interacts
  with [§5.9](#59-methods-and-this)'s `this`-as-borrowed-parameter model
  in ways not yet designed; and a function that returns a function
  pointer, where real C++'s own declarator grammar makes the leading and
  trailing attribute positions ambiguous with each other for the *outer*
  function. Neither has an existing use case forcing the issue yet.

## 5.17 Dereference Operators on Classes

scpp lets a `class` declare `operator*()` as an ordinary non-static
member function, in the two useful shapes:

```cpp
class Box {
public:
    T& operator*();
    const T& operator*() const;
};
```

The important point is what scpp **does not** do here: it introduces no
new borrow rule, no new ownership exception, and no separate `operator->`
protocol. The compiler simply treats `*x` as sugar for an ordinary method
call, and `x->y` as the same old `(*x).y` sugar it already used for
pointer-like types.

- **`*x` is an ordinary method call.** If `x` has class type and that
  class declares a matching `operator*()`, the expression `*x` is checked
  exactly as if the user had written `x.operator*()`. The non-`const`
  overload is selected for a mutable receiver, the `const` overload for a
  `const` receiver, by the same receiver-borrowing rules as any other
  method call ([§5.9](#59-methods-and-this)).
- **The return value is just an ordinary returned reference.** Because the
  supported shape returns `T&` or `const T&`, everything that already
  applies to a reference returned from a method applies here too:
  borrowing `*x` records a borrow against `x`'s root object, and keeping
  that reference alive blocks moving or reassigning `x` for exactly the
  same reason keeping a method-returned reference alive does
  ([§5.3](#53-lifetime)/[§5.9](#59-methods-and-this)). For example,
  `int& r = *b; Box c(std::move(b));` is rejected not by a special
  "`operator*` rule", but by the existing "cannot move while borrowed"
  rule.
- **`x->y` is still only sugar for `(*x).y`.** So once `*x` works for a
  user-defined `class`, `x->field` and `x->method()` work automatically
  too -- no second design is needed. A `std::unique_ptr<T>` still works
  the same way; a user-defined `class` merely joins that same surface.
- **There is no separately overloadable `operator->`.** scpp does not
  provide real C++'s separate `operator->` protocol. The language gets the
  useful `x->y` spelling already from the single `(*x).y` rewrite above,
  so adding a second operator name would only create a second path to the
  same borrow-checked effect.
- **Current scope is intentionally narrow.** This is a `class` feature,
  not a `struct` one; the new operator name is `operator*` only. Other
  operator names such as `operator+` remain out of scope here, and
  `operator=` remains governed separately by the ordinary copy-assignment
  rules from [§4.2](ch04-struct-vs-class.md).

## 5.18 Type-Erased Call Wrappers: `std::function` and `std::move_only_function`

scpp supports two owning, type-erased callable wrappers, following real
C++'s split:

- **`std::function<Sig>`** stores a callable target of signature `Sig`,
  but only when that target is copy-constructible.
- **`std::move_only_function<Sig>`** stores a callable target of
  signature `Sig` even when the target is move-only.

The key design point is what these wrappers are **not**: they are not a
deep compiler builtin in the old `unique_ptr`/`span`/`expected` sense.
Once generic `class` templates can express the needed surface --
multiple template parameters, partial specialization on a function-type
template argument, generic constructors, and named parameter packs in
methods -- both wrappers are ordinary library code. The only
compiler-intrinsic help they need is the ability to decompose a
function-type template argument such as `int(int, int)` into "return type
`int` plus parameter-type pack `(int, int)`", the same role real C++
already gives partial-specialization pattern matching on `R(Args...)`.

```cpp
template<typename Sig>
class function;

template<typename R, typename... Args>
class function<R(Args...)> {
    // ...
};

template<typename Sig>
class move_only_function;
```

**These wrappers never have a null / empty state.** This is a deliberate
departure from real C++'s `std::function`/`std::move_only_function`,
which are default-constructible, testable with `operator bool`, and may
be empty. In scpp, once a `function<Sig>` or `move_only_function<Sig>`
object exists, it always owns a valid callable target. If a program
needs optionality, it spells that in the type:

```cpp
std::optional<std::function<void(int)>> maybe_callback;
std::optional<std::move_only_function<void()>> maybe_job;
```

That keeps "there might be no callable here" explicit, instead of
hiding it as a sentinel state inside a nominally owning wrapper. Moving a
wrapper therefore follows the ordinary [§4.2](ch04-struct-vs-class.md)
class rules: the target is moved to the destination wrapper, and the
source wrapper itself becomes an ordinary moved-out object, not a
special still-usable-but-empty callable box.

**`std::move_only_function` supports cv/ref-qualified signatures from
the start.** This includes signatures such as:

```cpp
std::move_only_function<void() const> f1;
std::move_only_function<void() &>     f2;
std::move_only_function<void() &&>    f3;
```

The qualifier belongs to the stored callable's call operator contract,
exactly as it does in real C++23: a wrapper instantiated for `void() &`
promises an lvalue-callable target; one instantiated for `void() &&`
promises an rvalue-callable target.

**A release/extract operation is acceptable, but it should be
consuming.** scpp does not want a post-extraction "empty callable"
sentinel any more than it wants a post-move one. So the acceptable shape
is the moral equivalent of `unique_ptr::release()`, but expressed in the
same moved-out-state model the rest of scpp already uses: extract the
underlying callable by value without destroying it, and treat the wrapper
object itself as consumed / moved-out thereafter.

**Use the erased wrappers only when erasure is actually the point.** If a
function merely wants to call whatever closure or function object it was
given, the generic path from [§5.11](#511-generic-functions-and-concepts)
and [§5.12](#512-closures-lambda-expressions) stays preferable: no heap
box, no erased dispatch, and full visibility of the concrete type to the
compiler. `std::function` and `std::move_only_function` exist for the
cases generic monomorphization cannot express cleanly: storing several
different callable types behind one owning field, returning "some
callable of this signature" without exposing its concrete type, or
passing a callback across an ABI / architectural boundary where the
static type must be hidden.

---

[← Previous: Struct vs Class Semantics](ch04-struct-vs-class.md) · [Table of Contents](README.md) · [Next: The v0.1 Supported Subset →](ch06-safe-subset.md)
