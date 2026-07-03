# 1. Safety Context

The compiler maintains a **safety context** for every function body, lambda,
block, and type:

- **`unsafe` (default)**: ordinary C++ semantics. No ownership / borrow / alias
  checking.
- **`safe`**: all static safety checks enabled (see [§5](ch05-static-checks.md)).

## 1.1 How the context is determined

- An unannotated function/block -> `unsafe` (default).
- A `safe`-annotated function/block -> `safe`.
- A nested block inherits its parent's context unless explicitly overridden.
- An `unsafe { }` block opens an escape hatch inside a `safe` context, restoring
  `unsafe` semantics.
- (Not yet supported) A `safe { }` block enabling checks inside an `unsafe`
  context — deferred to a later version.

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

---

[← Previous: Design Philosophy](ch00-design-philosophy.md) · [Table of Contents](README.md) · [Next: Boundary Rules →](ch02-boundary-rules.md)
