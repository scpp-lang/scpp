# Struct vs Class Semantics (Fixed Memory Layout / ABI)

In ordinary C++, `struct` and `class` are almost the same tool with different
visibility defaults. In scpp, they communicate a much bigger design decision:
**is this value plain layout-defined data, or is it part of the checked,
ownership-tracked world?**

A good rule of thumb is:

- use `struct` when you want a fixed-layout value that behaves like data;
- use `class` when you want a value whose lifetime, ownership, or invariants
  matter to the checker.

That split is one of the main ways scpp makes low-level interop explicit
without giving up Rust-style safety for ordinary code.

## 4.1 `struct`: a purely trivial aggregate

A scpp `struct` is the language's *plain data* form. It is for values whose
meaning comes entirely from their bits and field layout, not from ownership or
custom lifetime behavior.

A `struct` may contain only **trivial** fields, recursively:

- scalar values such as `bool`, integers, floats, and `char`;
- raw pointers `T*`;
- function pointers (including the `[[scpp::unsafe]]`-qualified kind from
  [§5.16](ch05-static-checks.md#516-function-pointers));
- other trivial `struct`/`union` types;
- fixed-size arrays of trivial types.

Just as important is what a `struct` may **not** contain: references,
`std::span`, `std::unique_ptr`, `std::shared_ptr`, `std::vector`,
`std::string`, or any other type whose meaning depends on ownership, borrowing,
or lifetime tracking. If you need those semantics, the type must be a `class`
instead.

That restriction buys three very useful properties.

First, a `struct` local with no explicit initializer starts as all-zero bits.
Second, copying a `struct` is always just copying its bytes; there is no hidden
ownership transfer and no borrow state to update. Third, because every field is
trivial, a `struct` is the natural shape for C ABI boundaries.

Here is the simplest consequence: copying a `struct` gives you an independent
copy of the data.

```cpp
extern "C" int puts(const char* s);

struct PacketHeader {
    int size;
    int kind;
};

int main() {
    PacketHeader original;
    original.size = 64;
    original.kind = 1;

    PacketHeader copy = original;
    copy.size = 128;

    [[scpp::unsafe]] {
        if (original.size == 64 && copy.size == 128) {
            puts("struct copies by value");
        }
    }
    return 0;
}
```

Output:

```text
struct copies by value
```

`original` and `copy` do not alias each other. A `struct` assignment is not a
borrow, not a move, and not a user-defined operation -- it is a plain value
copy.

## 4.2 `class`: types that own resources / participate in checking

A scpp `class` is where the safety checker goes to work. A `class` may contain
other `class` values, references, smart pointers, spans, and any other field
whose meaning depends on ownership, borrowing, or destruction.

In practice, choose `class` when at least one of these is true:

- the value owns a resource;
- a constructor/destructor matters;
- the checker should track moves, borrows, and lifetimes through the value;
- the type exposes behavior, not just layout.

This is also where scpp is intentionally stricter than C++.

- A `class` participates in [§5](ch05-static-checks.md)'s ownership and borrow
  checking.
- Move construction and move assignment are **always compiler-provided**;
  user-written move operations are rejected.
- Copy operations may be user-written, but only under the controlled rules
  described in [§5.1](ch05-static-checks.md).
- A `class` local is not “special C++ uninitialized storage”; scpp's general
  zero-initialization rule still applies until a constructor or explicit
  initialization overwrites that state.

The important mental shift is that a `class` is not “just a `struct` with
methods”. In scpp, a `class` is a checked value whose API, move behavior, and
destruction semantics are part of the language's safety story.

```cpp
extern "C" int puts(const char* s);

class Counter {
private:
    int value;
public:
    Counter(int start) {
        this->value = start;
        return;
    }

    int get() const {
        return this->value;
    }

    void increment() {
        this->value = this->value + 1;
        return;
    }
};

int main() {
    Counter c(5);
    c.increment();
    c.increment();

    [[scpp::unsafe]] {
        if (c.get() == 7) {
            puts("class keeps behavior with the data");
        }
    }
    return 0;
}
```

Output:

```text
class keeps behavior with the data
```

This example is tiny, but it shows the intended split clearly:

- the data is private;
- the object is manipulated through checked methods;
- the type has room to grow into a resource-owning abstraction later without
  changing categories.

A few design consequences follow from that split:

- **No inheritance in v0.1.** `class` is about ownership and checked behavior,
  not about a full object-model feature set. If/when inheritance is designed,
  it will be added deliberately rather than inherited accidentally from C++.
- **`mutable` is phase-1 interior mutability.** A `mutable` field in a `class`
  must itself be trivial; it may be read or written through a `const this`,
  but it may never be borrowed or have its address taken. This is scpp's
  “cheap `Cell<T>`-like” story, not a general escape hatch.
- **Pass-by-value uses the same copy/move rules as locals.** There is no
  separate hidden transport model for function calls or returns.

If you remember only one guideline, remember this one: **`struct` says “treat
me like layout”; `class` says “treat me like a checked object.”**

## 4.3 Memory Layout & ABI (fixed, not left implementation-defined)

This chapter's `struct`/`class` split would be incomplete without the ABI rule
that makes `struct` useful at real boundaries.

scpp pins `struct` layout to the target platform's **Clang ABI**. For a given
target triple, a scpp `struct` must have the same field offsets, padding,
alignment, and total size as the equivalent C struct compiled by Clang.

Concretely:

1. fields stay in source declaration order;
2. each field uses the target ABI's alignment requirement;
3. padding is inserted as needed between fields;
4. the struct's own alignment is the maximum of its fields' alignments;
5. total size is rounded up so arrays of the struct stay correctly aligned.

That guarantee is why `struct` is the right tool for packet headers, FFI
records, device descriptors, and similar “other code must agree on these exact
bytes” data.

`class`, by contrast, is not the ABI-facing shape. Its job is to participate in
the checker and carry behavior. Use `class` inside your safety-checked core;
convert to `struct`, raw pointers, or other C-compatible forms at the edges.

For the rare case where the foreign ABI really requires packed layout, scpp
provides `[[scpp::packed]]` on `struct` and `union`
([§5.19](ch05-static-checks.md#519-union-and-scpppacked)). That is the explicit
layout escape hatch. Bit-fields and a general `alignas`-style user layout
control remain out of scope for v0.1.

---

[← Previous: Syntactic Sugar](ch03-syntactic-sugar.md) · [Table of Contents](README.md) · [Next: Static Checks →](ch05-static-checks.md)
