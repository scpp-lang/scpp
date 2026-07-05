# 1. Safety Context

Every function is checked ([§5](ch05-static-checks.md)) **by default,
unconditionally** -- there is no per-function or per-file annotation that
turns this on. The only safety-context construct in the language is the
`unsafe { }` block: a lexically scoped, local, composable escape hatch
that relaxes exactly the fixed, enumerated set of operations in
[§5.5](ch05-static-checks.md), and nothing else. This is a deliberate
reversal of an earlier design (a `safe` keyword that opted individual
functions *into* checking, with unmarked functions checked not at all) --
see [§8](ch08-open-questions.md) Q13 for the full reasoning behind the
reversal.

## 1.1 What `unsafe { }` relaxes, and what stays unconditional

- **Everything in [§5.1-§5.4](ch05-static-checks.md)** -- ownership/move
  state, alias-XOR-mutability, lifetime/dangling checks, and zero-init --
  runs **unconditionally, everywhere, including inside an `unsafe { }`
  block**. `unsafe { }` never disables this analysis; there is no
  construct anywhere in scpp that does. This matches Rust exactly: an
  `unsafe fn`'s body, or the inside of an `unsafe { }` block, is still
  fully borrow-checked in Rust -- `unsafe` only unlocks a handful of
  specific operations the borrow checker itself cannot verify, it never
  suspends borrow checking as a whole.
- **Only the operations [§5.5](ch05-static-checks.md) lists become legal
  inside `unsafe { }`**: raw pointer dereference/arithmetic,
  `reinterpret_cast`/incompatible C-style casts, untagged `union` member
  access, raw `new`/`delete`, mutable global/static access, and calling
  an `extern "C"` function (an `extern "C"` declaration is never itself
  checked by any scpp compiler, see [§2.1](ch02-boundary-rules.md), so
  calling one always needs the same vouching `unsafe { }` provides for
  every other item on that list).
- **`span`'s bounds check ([§8](ch08-open-questions.md) Q1) and
  integer-overflow checking ([§5.8](ch05-static-checks.md)) are also
  relaxed inside `unsafe { }`**, for a different reason than the
  operations above: not because they're otherwise illegal (neither ever
  is -- both are ordinary, always-legal syntax), but because skipping a
  scpp-inserted *runtime* check carries none of the
  "corrupted-bookkeeping-leaks-into-surrounding-code" risk that keeps
  [§5.1-§5.4](ch05-static-checks.md) unconditional (see
  [§5.8](ch05-static-checks.md) for the full reasoning).

## 1.2 No function-level marker: wrap the whole body if needed

There is no way to mark an entire function as exempt from
[§5.1-§5.4](ch05-static-checks.md)'s checks, by design -- not a keyword,
not a file-level pragma, nothing. If a function's implementation
genuinely needs broad, pervasive access to [§5.5](ch05-static-checks.md)'s
operations throughout its body (rather than one or two isolated spots),
the way to express that is to wrap the *entire* function body in a single
`unsafe { }` block:

```cpp
int legacy_style_function(int* p, size_t n) {
    unsafe {
        // the whole body lives here; §5.1-§5.4 still apply throughout
    }
}
```

Ownership/move/alias/lifetime checking still applies to every statement
inside that block, exactly as [§1.1](#11-what-unsafe--relaxes-and-what-stays-unconditional)
describes -- wrapping the whole body changes nothing about that. This
mirrors Rust's own 2024-edition tightening (`unsafe_op_in_unsafe_fn`):
even Rust's `unsafe fn` now requires an explicit inner `unsafe { }`
around the specific operations it performs, and its body was never
exempt from borrow checking in the first place, at any edition.

## 1.3 The `unsafe { }` block (design finalized, not yet implemented)

- **Grammar**: `unsafe` followed by an ordinary brace-delimited statement
  list, usable anywhere a statement is expected inside a function body:
  `unsafe { stmt; stmt; ... }`. v0.1 only supports this block-statement
  form. Circle/Safe C++ also allows an `unsafe` prefix directly on a
  single condition expression, a match-arm body, or a constructor's
  mem-initializer; scpp defers all of those since it has no
  match-expressions or constructors yet -- they can be added as their own
  grammar productions later without changing anything specified here.
- **Scoping**: unlike Circle (where `unsafe { }` deliberately does *not*
  open a new lexical scope), scpp's `unsafe { }` is an **ordinary block**:
  it behaves exactly like a plain `{ }` compound statement -- locals
  declared inside go out of scope at the closing `}`, exactly as in
  ordinary C++. A `{ }` that silently scoped differently from every
  other `{ }` in the language would violate scpp's own "looks like C++,
  no surprises" rule. If a value computed inside `unsafe { }` needs to
  survive past it, declare it in the enclosing scope first and assign
  inside -- ordinary C++ already works this way.
- **What gets unlocked**: exactly, and only, the operations listed in
  [§5.5](ch05-static-checks.md) become legal again inside `unsafe { }`:
  raw pointer dereference/arithmetic, `reinterpret_cast`/
  incompatible C-style casts, untagged `union` member access, raw
  `new`/`delete`, mutable global/static access, and calling an
  `extern "C"` function. Of these, **only two are reachable in the
  current implementation**: raw-pointer `*p` dereference (`T*` already
  exists as a type) and calling an `extern "C"` function (declarations
  and calls already exist). The other four have no syntax at all yet in
  v0.1 (`union`, `reinterpret_cast`, `new`/`delete`, and global variables
  aren't lexed/parsed) -- there's nothing to gate for them until their
  own syntax lands; wire up the identical mechanism for each at that
  point.
- **Also relaxed inside `unsafe { }`, for a different reason**: `span`'s
  bounds check ([§8](ch08-open-questions.md) Q1) and integer-overflow
  checking ([§5.8](ch05-static-checks.md)) are skipped too -- but unlike
  the operations above, neither is "illegal outside unsafe" (both are
  always legal syntax, everywhere). They're scpp-inserted *runtime*
  checks that happen to carry none of the "leakage into surrounding
  code" risk the next bullet explains, which is why they can join
  this list without contradicting it.
- **What stays exactly as strict**: everything in
  [§5.1-§5.4](ch05-static-checks.md) -- ownership/move state,
  alias-XOR-mutability, lifetime/dangling checks, and zero-init -- keeps
  running **unconditionally** through an `unsafe { }` block's statements.
  `unsafe { }` is a narrow, operation-level escape hatch, **not** a
  "stop checking this region" switch, and must not be implemented by
  skipping movecheck for the block's statements -- only by relaxing the
  specific checks listed above. A `std::unique_ptr` can't be double-moved
  and alias-XOR-mutability can't be violated whether or not the code sits
  inside `unsafe { }`; otherwise, code right after the block (still under
  full checking) could observe corrupted state that was never actually
  proven sound. (Raw pointers themselves are still never move/borrow-
  tracked, inside or outside `unsafe { }` -- that's unchanged; it's the
  same "raw pointers aren't a tracked type" rule that already applies
  everywhere.)
- **Nesting**: `unsafe { }` may nest inside another `unsafe { }` --
  harmless, not an error.
- **Implementation shape** (for whoever builds this): reuse the existing
  `StmtKind::Block` AST node (add an `is_unsafe` flag), and mirror the
  existing `ScopeExit` MIR pattern with a matching pair of marker
  statements (e.g. `UnsafeEnter`/`UnsafeExit`) bracketing the block's
  lowered statements. The movecheck walker keeps a simple nesting counter
  (increment on `UnsafeEnter`, decrement on `UnsafeExit`) alongside its
  existing traversal -- this does **not** need to join across CFG branches
  the way `DataflowState` does, since it's a purely lexical/structural
  fact at each program point, never a flow-sensitive one (every branch of
  an `if` already closes out its own `unsafe { }` blocks, if any, before
  reaching the merge point). Gate each of the two currently-reachable
  checks above on `counter == 0`; `span`'s bounds check and
  integer-overflow checking (once implemented) gate on the same counter,
  no separate mechanism needed.

---

[← Previous: Design Philosophy](ch00-design-philosophy.md) · [Table of Contents](README.md) · [Next: Boundary Rules →](ch02-boundary-rules.md)
