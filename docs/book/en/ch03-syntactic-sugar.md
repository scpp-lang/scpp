# 3. Syntactic Sugar / Re-semantification of Existing Syntax

Inside a **safe context**, the following C++ spellings acquire strong static
semantics. In an unsafe context they keep their ordinary C++ meaning.

| C++ spelling | Semantics in a safe context |
|--------------|------------------------------|
| `std::move(x)` | Compiler builtin **move hint**. Marks `x` as *moved-out*. Reading `x` afterwards is an error until it is reassigned. Not an ordinary function call. |
| `T&` | Mutable borrow `&mut T`: exclusive, participates in alias-XOR-mutability and lifetime checking. |
| `const T&` | Shared borrow `&T`: multiple may coexist, but mutually exclusive with any `&mut`. |
| `T&&` (parameter) | Passed by move (ownership transfer). |
| `std::unique_ptr<T>` | Unique ownership. Move semantics fit naturally. |
| `std::shared_ptr<T>` | Shared ownership (refcounted). Allowed in safe, but aliasing follows interior-mutability rules (refined in v0.2). |
| `std::span<T>` / `std::string_view` | Borrowed views with a lifetime; checked for dangling. |
| local variable `T x;` | Owns its value; dropped (destroyed) at end of scope. |
| `new` / `delete` / raw `T*` | **Forbidden by default** in safe regions; require `unsafe { }`. |

**Key principle**: these semantic shifts are "invisible" to the user — they
still write familiar C++, they just get extra compile-time errors in safe
regions that block bugs.

---

[← Previous: Boundary Rules](ch02-boundary-rules.md) · [Table of Contents](README.md) · [Next: Struct vs Class Semantics →](ch04-struct-vs-class.md)
