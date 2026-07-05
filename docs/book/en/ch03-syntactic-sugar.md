# 3. Syntactic Sugar / Re-semantification of Existing Syntax

The following C++ spellings acquire strong static semantics, in force
**unconditionally, everywhere** -- including inside an `unsafe { }` block
(see [§1.1](ch01-safety-context.md)). Only the specific operations
[§5.5](ch05-static-checks.md) lists (raw pointer dereference,
`new`/`delete`, calling an `extern "C"` function, etc.) are actually
gated by `unsafe { }`; everything else in this table applies regardless.

| C++ spelling | Semantics |
|--------------|------------------------------|
| `std::move(x)` | Compiler builtin **move hint**. Marks `x` as *moved-out*. Reading `x` afterwards is an error until it is reassigned. Not an ordinary function call. |
| `T&` | Mutable borrow `&mut T`: exclusive, participates in alias-XOR-mutability and lifetime checking. |
| `const T&` | Shared borrow `&T`: multiple may coexist, but mutually exclusive with any `&mut`. |
| `T&&` (parameter) | Passed by move (ownership transfer). |
| `std::unique_ptr<T>` | Unique ownership. Move semantics fit naturally. |
| `*p` / `p->x` (`p` a `std::unique_ptr<T>`) | Safe dereference/member access, yielding an lvalue for the pointee. `p` itself still obeys alias XOR mutability: a borrow of `*p` is recorded against `p`, so moving (`std::move(p)`) or reassigning `p` while that borrow is alive is rejected (it would otherwise dangle/use-after-free). |
| `*p` (`p` a raw pointer `T*`) | Requires `unsafe { }` ([§1.3](ch01-safety-context.md), implemented). |
| `&expr` | Address-of, yielding `const T*` (if `expr` is only reachable read-only) or `T*` (if mutable) -- the same rule real C++ already follows for `&expr`. **Safe to write** in any context (no `unsafe { }` needed) -- it's dereferencing a raw pointer that's gated, not creating one, matching Rust's own `&x as *const T`. `const T*`/`T*` are genuinely distinct types (one-way implicit `T* -> const T*` only; writing through a `const T*` is an ordinary type error, unconditionally). See [§5.7](ch05-static-checks.md) (**design finalized, not yet implemented**). |
| `std::shared_ptr<T>` | Shared ownership (refcounted). Aliasing follows interior-mutability rules (refined in v0.2). |
| `std::span<T>` / `std::span<const T>` | A lifetime-checked, non-owning view (a "fat pointer": `{data pointer, length}`). **v0.1 can only construct one from a fixed-size array** (`std::vector` doesn't exist yet), and it cannot be reassigned after construction (conservatively treated like a reference for now: bound once, never rebound). `.size` reads the length -- **note this is not** a real C++ `.size()` method call: scpp has no member-function-call syntax yet, so this is exposed as a read-only computed field instead. Subscript `s[i]` carries a runtime bounds check by default, calling `abort()` on failure (ch08's settled decision: v0.1 inserts bounds checks by default, panics via `abort()`); skipped inside `unsafe { }` instead, same treatment as integer-overflow checking (see [§1.1](ch01-safety-context.md)/[§5.8](ch05-static-checks.md)). `std::string_view` doesn't exist yet (needs a `char` type first). |
| local variable `T x;` | Owns its value; dropped (destroyed) at end of scope. |
| `new` / `delete` | **Forbidden by default**; require `unsafe { }`. (Raw pointer *dereference* is likewise `unsafe { }`-gated, but the *type* `T*` and taking a raw address with `&expr` above are not -- see [§5.5](ch05-static-checks.md).) |
| `[[scpp::lifetime(name)]]` | Attribute (not a new keyword) grouping reference parameters/declarators into named cross-function lifetime groups -- scpp's opt-out alternative to Rust's `'a`/Circle's `/a`; see [§5.3](ch05-static-checks.md). **Design finalized, not yet implemented.** |
| `extern "C" ...;` / `extern "C" ... { ... }` | Not resemantified, just restricted: declares/defines a C-linkage function, signature types limited to C-ABI-compatible ones. A bodyless declaration is always implicitly unchecked (nothing to verify) -- calling it requires `unsafe { }`; a definition is checked internally like any other function. See [§2.1](ch02-boundary-rules.md) (implemented). |

**Key principle**: these semantic shifts are "invisible" to the user — they
still write familiar C++, they just get extra compile-time errors that
block bugs, by default, everywhere.

---

[← Previous: Boundary Rules](ch02-boundary-rules.md) · [Table of Contents](README.md) · [Next: Struct vs Class Semantics →](ch04-struct-vs-class.md)
