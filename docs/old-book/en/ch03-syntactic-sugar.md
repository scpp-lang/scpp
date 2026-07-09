# 3. Borrowing, Moving, and Views Behind Familiar Syntax

Up to this point, scpp has looked like a small, familiar C++-shaped language.
This chapter introduces the first big twist: **many ordinary C++ spellings keep
their surface syntax, but gain stronger static meaning**.

That is how scpp tries to get Rust-like guarantees without asking you to learn a
completely new surface language. You still write `T&`, `const T&`,
`std::move(x)`, `std::unique_ptr<T>`, and `std::span<T>`. The important change
is that the compiler now treats those spellings as part of a checked ownership
model, not as loose conventions.

## 3.1 `T&` and `const T&` are borrows

In ordinary C++, references often feel like “just another name” for an object.
In scpp, they are much more precise:

- `T&` is a **mutable borrow**;
- `const T&` is a **shared borrow**.

That means the compiler tracks who is allowed to read or write through that
reference while it is alive.

```cpp
extern "C" int puts(const char* s);

void add_bonus(int& score) {
    score = score + 1;
    return;
}

int doubled(const int& score) {
    return score + score;
}

int main() {
    int score = 20;
    add_bonus(score);
    int total = doubled(score);

    [[scpp::unsafe]] {
        if (score == 21 && total == 42) {
            puts("references are checked borrows");
        }
    }
    return 0;
}
```

Output:

```text
references are checked borrows
```

The important lesson is not just that this works. It is **why** it works:

- `add_bonus` gets temporary exclusive write access through `int&`;
- `doubled` gets read-only access through `const int&`;
- once each borrow is finished, the caller regains ordinary access to `score`.

Later, Chapter 5 will show the full aliasing rules. For now, keep this mental
model: **a reference in scpp is part of the borrow checker, not a casual alias.**

## 3.2 `std::move` means ownership really moved

In C++, `std::move` is often explained as a cast that enables move operations.
In scpp, that description is too weak. Here, `std::move(x)` means:

> “transfer ownership out of `x`; do not let me read `x` again until I assign a
> fresh value back into it.”

```cpp
import std;
extern "C" int puts(const char* s);

int main() {
    std::unique_ptr<int> first = std::make_unique<int>(7);
    std::unique_ptr<int> second = std::move(first);

    if (*second == 7) {
        [[scpp::unsafe]] {
            puts("moves leave the old name behind");
        }
    }
    return 0;
}
```

Output:

```text
moves leave the old name behind
```

After the move, `second` is the owner. `first` is now in a moved-out state, so a
later read from `first` would be a compile error.

This is one of the main re-semantified pieces of familiar syntax in scpp: the
spelling is still `std::move`, but the compiler treats it as a checked ownership
transition.

## 3.3 `*` and `->` still feel convenient on owning types

scpp does not want safe code to become awkward. When you own something through a
type such as `std::unique_ptr<T>`, dereferencing and member access still look
like ordinary pointer-shaped code.

```cpp
import std;
extern "C" int puts(const char* s);

struct Pair {
    int left;
    int right;
};

int main() {
    std::unique_ptr<Pair> pair = std::make_unique<Pair>();
    pair->left = 10;
    pair->right = 32;

    [[scpp::unsafe]] {
        if (pair->left + pair->right == 42) {
            puts("unique_ptr still feels like a pointer");
        }
    }
    return 0;
}
```

Output:

```text
unique_ptr still feels like a pointer
```

The surface experience is deliberately ergonomic, but the semantics are safer
than raw-pointer code:

- `pair` still has one clear owner;
- borrowing `*pair` is tracked against `pair` itself;
- moving or reassigning `pair` while such a borrow is alive is rejected.

So the syntax stays familiar, while the compiler keeps the ownership story
coherent.

## 3.4 `std::span<T>` is a borrowed view

`std::span<T>` and `std::span<const T>` are how scpp expresses “look at an
existing sequence without owning it.”

In v0.1, the main thing to remember is that a span is a checked view over an
existing fixed-size array.

```cpp
import std;
extern "C" int puts(const char* s);

int sum(std::span<const int> values) {
    int i = 0;
    int total = 0;
    while (i < values.size) {
        total = total + values[i];
        i = i + 1;
    }
    return total;
}

int main() {
    int numbers[4];
    numbers[0] = 5;
    numbers[1] = 10;
    numbers[2] = 12;
    numbers[3] = 15;

    std::span<const int> view = numbers;

    [[scpp::unsafe]] {
        if (sum(view) == 42) {
            puts("span borrows an existing array");
        }
    }
    return 0;
}
```

Output:

```text
span borrows an existing array
```

Three details matter here:

1. `view` does **not** own `numbers`;
2. `view.size` reads the length as a field-like value;
3. indexing goes through bounds-checked access by default.

That makes `std::span` the natural “slice-like” tool for read-only or mutable
views into existing storage.

## 3.5 Raw pointers still exist, but trusting them is explicit

scpp does not remove raw pointers from the language. It makes the trust boundary
visible instead.

Creating a raw pointer with `&value` is allowed in ordinary safe code. The moment
you dereference that pointer, you must step into `[[scpp::unsafe]]` and take
responsibility locally.

```cpp
extern "C" int puts(const char* s);

int main() {
    int value = 41;
    int* p = &value;

    [[scpp::unsafe]] {
        *p = *p + 1;
        if (value == 42) {
            puts("raw pointers need an explicit trust boundary");
        }
    }
    return 0;
}
```

Output:

```text
raw pointers need an explicit trust boundary
```

This is the pattern to remember throughout the book:

- owning values, borrows, and spans work in checked code by default;
- raw-pointer dereference is still possible;
- but the exact point where you trust unchecked memory is spelled out.

## 3.6 A practical reading of “re-semantified syntax”

If you want one short summary of this chapter, use this:

- `T&` means exclusive borrow;
- `const T&` means shared borrow;
- `std::move(x)` means ownership transfer;
- `std::unique_ptr<T>` fits naturally into that ownership model;
- `std::span<T>` is a non-owning view;
- raw pointers are real, but dereferencing them is explicitly unsafe.

That is the core scpp trick. The language keeps a C++-looking surface, while the
compiler quietly interprets some familiar spellings with much stronger meaning.

## Reference appendix preserved during the rewrite

The material below keeps the chapter's original “exact mapping” value while the
front of the book is being rewritten into a guided tutorial path.

These semantic shifts apply **unconditionally, everywhere** -- including inside
`[[scpp::unsafe]] { }` blocks. Only the specific operations listed in
[§5.5](ch05-static-checks.md) are actually gated by `[[scpp::unsafe]] { }`.

| C++ spelling | Semantics |
|--------------|-----------|
| `std::move(x)` | Compiler builtin **move hint**. Marks `x` as *moved-out*. Reading `x` afterwards is an error until it is reassigned. Not an ordinary function call. |
| `T&` | Mutable borrow `&mut T`: exclusive, participates in alias-XOR-mutability and lifetime checking. |
| `const T&` | Shared borrow `&T`: multiple may coexist, but mutually exclusive with any `&mut`. |
| `T&&` (parameter) | Passed by move (ownership transfer). |
| `std::unique_ptr<T>` | Unique ownership, provided as an ordinary library `class` by the `std` module (`import std;`). Move semantics fit naturally because it is just one more `class` following the same ownership rules as any other. |
| `*x` / `x->y` (`x` a `std::unique_ptr<T>` or a `class` with `operator*`) | Safe dereference/member access, yielding an lvalue for the referent. For a user-defined `class`, `*x` is just sugar for an ordinary `x.operator*()` call, and `x->y` is just `(*x).y` -- there is no separate `operator->` in scpp. The owner object still obeys alias XOR mutability: a borrow of `*x` is recorded against `x`, so moving (`std::move(x)`) or reassigning `x` while that borrow is alive is rejected. See [§5.17](ch05-static-checks.md#517-dereference-operators-on-classes). |
| `*p` (`p` a raw pointer `T*`) | Requires `[[scpp::unsafe]] { }` ([§1.3](ch01-safety-context.md)). |
| `&expr` | Address-of, yielding `const T*` (if `expr` is only reachable read-only) or `T*` (if mutable) -- the same rule real C++ already follows for `&expr`. Safe to write in any context; it is dereferencing a raw pointer that is gated, not creating one. `const T*`/`T*` are genuinely distinct types (one-way implicit `T* -> const T*` only). See [§5.7](ch05-static-checks.md). |
| `std::shared_ptr<T>` | Shared ownership (refcounted). Aliasing follows interior-mutability rules (refined in v0.2). |
| `std::span<T>` / `std::span<const T>` | A lifetime-checked, non-owning view (a “fat pointer”: `{data pointer, length}`). v0.1 can only construct one from a fixed-size array, and it cannot be reassigned after construction. `.size` reads the length as a computed field, not a member-function call. Subscript `s[i]` carries a runtime bounds check by default, calling `abort()` on failure; inside `[[scpp::unsafe]] { }`, that check is skipped. `std::string_view` does not exist yet. |
| local variable `T x;` | Owns its value; dropped (destroyed) at end of scope. |
| `new` / `delete` | Forbidden by default; require `[[scpp::unsafe]] { }`. |
| `[[scpp::lifetime(name)]]` | Attribute (not a new keyword) grouping reference parameters/declarators into named cross-function lifetime groups -- scpp's opt-out alternative to Rust's `'a`/Circle's `/a`; see [§5.3](ch05-static-checks.md). |
| `[capture-list](params) { body }` (lambda expression) | Desugars to an anonymous, compiler-synthesized class exactly as in real C++: one member per capture, `operator()` for the body. By-value captures are ordinary owned members; by-reference captures are reference-typed members, making the closure itself a lifetime-tracked value. `this`/`*this` must always be captured explicitly. See [§5.12](ch05-static-checks.md). |
| `extern "C" ...;` / `extern "C" ... { ... }` | Not re-semantified, just restricted: declares/defines a C-linkage function, signature types limited to C-ABI-compatible ones. A bodyless declaration is always implicitly unchecked; calling it requires `[[scpp::unsafe]] { }`. A definition is checked internally like any other function. See [§2.1](ch02-boundary-rules.md). |

---

[← Previous: Basic Building Blocks](ch02-boundary-rules.md) · [Table of Contents](README.md) · [Next: Struct vs Class →](ch04-struct-vs-class.md)
