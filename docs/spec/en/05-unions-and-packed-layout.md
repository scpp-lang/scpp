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

## 9.3 Alignment specifier and alignment query [dcl.align] {#93-alignment-specifier-and-query}

(1) Except as modified by this subclause, [dcl.align] and [expr.alignof]
apply unchanged to SCPP26.

(2) The alignment-specifier `alignas` is supported with the same syntax as
ISO C++:

  (2.1) `alignas(constant-expression)`, where the constant-expression is an
  integral constant expression;

  (2.2) `alignas(type-id)`, which requests the alignment requirement of
  that type-id, equivalently `alignas(alignof(type-id))`; and

  (2.3) multiple alignment-specifiers on the same declaration, in which
  case the strictest non-zero alignment applies.

(3) An alignment-specifier may appertain only to:

  (3.1) the declaration of a variable;

  (3.2) the declaration of a non-static data member that is not a
  bit-field; or

  (3.3) the declaration of a class whose *class-key* is `struct`, `class`,
  or `union`.

If an alignment-specifier appertains to any other declaration, the program
is ill-formed.

[Note: SCPP26 v1 does not otherwise specify bit-field declarations. This
subclause nevertheless follows ISO C++ in not permitting `alignas` to
appertain to a bit-field. — end note]

(4) `alignas(0)` has no effect. Any other constant-expression operand shall
evaluate to a supported valid alignment value. In SCPP26 v1, that means a
positive power of two accepted by the target ABI.

(5) If an alignment-specifier would request an alignment less strict than
the natural alignment requirement of the declared entity or class type, the
program is ill-formed.

(6) If `alignas` appertains to a class declaration, it changes the
alignment requirement of the class type itself. Layout of complete objects,
subobjects, and arrays of that type shall thereafter respect the stricter
alignment.

(7) If `alignas` appertains to a variable or non-static data member, it
changes the minimum alignment requirement of that declared object or
subobject, but it does not by itself change the alignment requirement of
the underlying type named by the declaration.

[Note: for example, `alignas(16) int a[4];` requires the object `a` to be
16-byte aligned, but it does not change the alignment requirement of the
array type `int[4]` itself. — end note]

(8) The `alignof(type-id)` query is supported with the same meaning as ISO
C++: it yields the alignment requirement of the named type as an integral
constant expression of type `std::size_t`.

(9) The `alignof` query in SCPP26 is the ISO C++ `type-id` form. A GNU-style
`alignof(expression)` extension is not part of SCPP26.

(10) `alignof(type-id)` may appear wherever an integral constant expression
is required, including within the operand of `alignas`.

(11) If `[[scpp::packed]]` appertains to a struct or union declaration:

  (11.1) no alignment-specifier shall appertain to that same declaration
  or to any of its non-static data members; and

  (11.2) no non-static data member of that struct or union shall have a
  class type, or array thereof, whose alignment requirement is made
  stricter by an alignment-specifier appertaining to that class type.

A program violating this rule is ill-formed.

[Note: `[[scpp::packed]]` exists to require an exact packed external byte
layout with alignment 1 and no inserted padding. Combining it with `alignas`
would otherwise create contradictory layout requirements. In particular, a
packed object cannot promise correct placement for a member subobject whose
class type itself requires over-aligned placement because of `alignas`. —
end note]

(12) Examples:

Accepted:

```cpp
struct alignas(32) block {
    std::uint64_t words[4];
};

alignas(block) unsigned char scratch[sizeof(block)];

struct header {
    std::uint16_t len;
    alignas(8) std::uint32_t checksum;
};
```

Rejected:

```cpp
alignas(3) int x;                    // not a power-of-two alignment
struct alignas(1) big { long long x; }; // weaker than the type's natural alignment
struct [[scpp::packed]] alignas(8) p { int x; }; // packed and alignas conflict
struct alignas(8) block8 { int x; };
struct [[scpp::packed]] bad { char tag; block8 payload; }; // packed cannot contain an over-aligned class-type member
```

---

[← Previous: Thread-Safety Properties](04-thread-safety-properties.md) · [Table of Contents](README.md) · [Next: Constant evaluation →](06-constant-evaluation.md)
