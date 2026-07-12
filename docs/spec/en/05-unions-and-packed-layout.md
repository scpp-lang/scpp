# 9 Union types and packed layout

## 9.1 Union types [class.union.scpp] {#91-union-types}

(1) A class declaration whose *class-key* is `union` declares a union type.

(2) A union type is an aggregate whose non-static data members share a
common storage representation; each such member begins at offset zero.

(3) This document defines no tagged-union construct. Every union is
therefore an untagged union for the purposes of
[§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe).

(4) Access to a non-static data member of a union is a gated operation
under [§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe).

(5) When such an access is permitted by
[§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe), the selected member
expression is otherwise governed by the ordinary rules that apply to an
expression of that member's type, including
[§6](02-ownership-and-move.md) and
[§7](03-dereference-and-member-access.md).

[Note: the unsafe gate is about selecting which representation the bytes
are to be interpreted as. It does not suspend ownership, lifetime, or
other rules on the resulting expression. — end note]

## 9.2 Packed layout attribute [dcl.attr.scpp.packed] {#92-packed-layout-attribute}

(1) The *attribute-token* `packed`, in the *attribute-namespace* `scpp`
([dcl.attr.grammar]), may appear in an *attribute-specifier-seq*
appertaining to the declaration of a struct or union. No
*attribute-argument-clause* shall be present.

(2) If an *attribute-specifier-seq* containing the attribute-token
`packed` appertains to anything other than the declaration of a struct or
union, the program is ill-formed.

(3) If `[[scpp::packed]]` appertains to a struct declaration, the
non-static data members of that struct are laid out in declaration order
with no padding inserted between consecutive members, and the alignment
requirement of the struct is 1.

(4) If `[[scpp::packed]]` appertains to a union declaration, each
non-static data member of that union has offset zero, and the alignment
requirement of the union is 1.

(5) The size of a union to which `[[scpp::packed]]` appertains is the
least size sufficient to contain its largest non-static data member.

(6) This attribute is an ordinary attribute available to any
user-declared struct or union. It is introduced to express a required
external byte layout; no distinguished library type name is involved.

[Note: the following declarations match the shape of common foreign ABI
layouts whose byte representation is specified exactly:

```cpp
union [[scpp::packed]] epoll_data_t {
    void* ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
};

struct [[scpp::packed]] epoll_event {
    uint32_t events{};
    epoll_data_t data{};
};
```

ISO C++ itself has no standard packed-layout attribute; existing C and
C++ toolchains typically expose this facility with extensions such as
`__attribute__((packed))` or `#pragma pack`. — end note]

---

[← Previous: Thread-Safety Properties](04-thread-safety-properties.md) · [Table of Contents](README.md) · [Next: Constant evaluation →](06-constant-evaluation.md)
