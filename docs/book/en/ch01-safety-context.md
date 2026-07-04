# 1. Safety Context

The compiler maintains a **safety context** for every function body, lambda,
block, and type:

- **`unsafe` (default, a.k.a. a "native function")**: ordinary C++
  semantics. No ownership / borrow / alias checking, and no scpp-inserted
  runtime checks either.
- **`safe`**: all static safety checks enabled (see [§5](ch05-static-checks.md)),
  plus scpp's own runtime checks (`span` bounds, integer overflow -- see
  [§5.5](ch05-static-checks.md)/[§5.8](ch05-static-checks.md)).

## 1.1 How the context is determined

- An unannotated function/block -> `unsafe` (default) -- called a
  **native function** in this book: ordinary C++, with no scpp checking
  of any kind, not even the compile-time checks in
  [§5.1-§5.4](ch05-static-checks.md). This is descriptive terminology,
  not a keyword -- there's no `native` token to write; simply omitting
  `safe` is what puts a function here.
- A `safe`-annotated function/block -> `safe`.
- A nested block inherits its parent's context unless explicitly overridden.
- An `unsafe { }` block, written inside a `safe` function, opens an
  escape hatch that locally relaxes exactly what
  [§1.3](#13-the-unsafe--block-design-finalized-not-yet-implemented)
  lists.
- `safe { }`/`unsafe { }` inside a **native** function are both a
  compile error, permanently by design (not a temporary gap): a native
  function has no active checking to relax or re-enable in the first
  place -- see [§1.3](#13-the-unsafe--block-design-finalized-not-yet-implemented)'s
  "Nesting".

## 1.2 Annotation positions

```cpp
safe int f(...);                 // free function
struct S { safe void g(); };     // member function
safe struct Widget { ... };      // type-level: all members safe by default (v0.2)
auto lam = safe [](int x){...};  // lambda (v0.2)
```

`safe` is a **leading** annotation, before the return type -- not a
trailing specifier like `noexcept`/`override`. This was reconsidered
against putting it in the trailing, `noexcept`-like position (as Sean
Baxter's Circle/Safe C++ does), but `safe` is meant to become the
*common*, aspirational case across a codebase, not a rare exception the
way `override`/`noexcept` are in ordinary C++. A leading keyword is the
most scannable position -- it sits at the same, predictable spot
regardless of how long or wrapped a parameter list is, so a reader can
scan straight down the left edge of a file to see which functions are
safe, the same way `virtual`/`inline`/`explicit`/`constexpr` are
scanned for today. `[[scpp::lifetime(name)]]` ([§5.3](ch05-static-checks.md))
is unaffected by this and stays trailing, since attributes have no
leading position to begin with: `safe int& foo(...) [[scpp::lifetime(a)]] { ... }`.

v0.1 **only requires function-level `safe` and block-level `unsafe { }`**. All
other annotation positions go to the backlog.

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
  unannotated C++. A `{ }` that silently scoped differently from every
  other `{ }` in the language would violate scpp's own "looks like C++,
  no surprises" rule. If a value computed inside `unsafe { }` needs to
  survive past it, declare it in the enclosing scope first and assign
  inside -- ordinary C++ already works this way.
- **What gets unlocked**: exactly, and only, the operations listed in
  [§5.5](ch05-static-checks.md) become legal again inside `unsafe { }`,
  with the same meaning they'd have in an ordinary (unannotated/`unsafe`)
  function: raw pointer dereference/arithmetic, `reinterpret_cast`/
  incompatible C-style casts, untagged `union` member access, raw
  `new`/`delete`, mutable global/static access, and calling a function not
  annotated `safe`. Of these, **only two are reachable in the current
  implementation**: raw-pointer `*p` dereference (`T*` already exists as a
  type) and calling a non-`safe` function (calls and `Function::is_safe`
  already exist -- though the "callee must be `safe`" rejection itself has
  never been implemented yet either, so shipping `unsafe { }` means
  shipping that check too, gated the same way). The other four have no
  syntax at all yet in v0.1 (`union`, `reinterpret_cast`, `new`/`delete`,
  and global variables aren't lexed/parsed) -- there's nothing to gate for
  them until their own syntax lands; wire up the identical mechanism for
  each at that point.
- **Also relaxed inside `unsafe { }`, for a different reason**: `span`'s
  bounds check ([§8](ch08-open-questions.md) Q1) and integer-overflow
  checking ([§5.8](ch05-static-checks.md)) are skipped too -- but unlike
  the operations above, neither is "illegal outside unsafe" (both are
  always legal syntax, everywhere). They're scpp-inserted *runtime*
  checks that happen to carry none of the "leakage into surrounding
  safe code" risk the next bullet explains, which is why they can join
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
  full `safe` checking) could observe corrupted state that was never
  actually proven sound. (Raw pointers themselves are still never
  move/borrow-tracked, inside or outside `unsafe { }` -- that's unchanged;
  it's the same "raw pointers aren't a tracked type" rule that already
  applies everywhere.)
- **Nesting**: `unsafe { }` may nest inside another `unsafe { }` --
  harmless, not an error, as long as both are still lexically inside a
  `safe` function. `unsafe { }` (or `safe { }`) written inside a
  **native** function's body, though, **is a compile error**: a native
  function has no active checking for either block to relax or
  re-enable, so the marker isn't merely redundant there, it's
  meaningless -- and rejecting it also catches a likely leftover from
  moving code between a `safe` function and a native one. (This
  tightens an earlier draft of this section, which treated `unsafe { }`
  inside a native function as a harmless no-op rather than an error.)
  `safe { }` re-enabling checks inside a native function is, similarly,
  a permanent design choice, not a temporary gap (unchanged from
  [§1.1](#11-how-the-context-is-determined), just reworded there for
  the same reason).
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
  no separate mechanism needed. Rejecting `unsafe { }`/`safe { }` inside
  a native function is a separate, much simpler check that doesn't need
  this counter at all -- just look at the enclosing function's own
  `Function::is_safe` at parse/resolve time, independent of any nesting
  state.

---

[← Previous: Design Philosophy](ch00-design-philosophy.md) · [Table of Contents](README.md) · [Next: Boundary Rules →](ch02-boundary-rules.md)
