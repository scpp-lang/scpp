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

## 9.4 Array declarators [dcl.array] {#94-array-declarators}

(1) Except as modified by this subclause, [dcl.array] applies unchanged to
SCPP26. This subclause governs every array declarator alike, regardless of
the declaration in which it appears: a local variable, a non-static data
member, a function parameter, or any other array declarator [dcl.array]
itself permits.

(2) The constant-expression giving an array's bound is, syntactically, an
ordinary expression exactly as in ISO C++; this document imposes no
restriction on its form beyond (4)-(7).

[Note: in particular, nothing in this document limits that expression to a
single integer-literal token. Forms accepted in this position by ISO C++ --
and therefore by SCPP26 -- include, without limitation, a `sizeof`
expression ([expr.sizeof]), an `alignof(type-id)` query
([§9.3](#93-alignment-specifier-and-query)), an *id-expression* naming a
`constexpr` variable ([dcl.constexpr]), and an arithmetic or comparison
combination of such subexpressions, nested to any depth. — end note]

(3) Evaluation of an array-bound constant-expression is required constant
evaluation ([§7](06-constant-evaluation.md)); that clause governs which
operands and operations are permitted during that evaluation.

[Note: [§7.2](06-constant-evaluation.md#72-supported-subset-exprconstscppsupport)
guarantees support for, among other operations, `sizeof`/`alignof` queries
and arithmetic and comparison operations on integer operands. — end note]

(4) The array-bound constant-expression shall be a converted constant
expression of type `std::size_t` ([expr.const.const]). Its value specifies
the array bound, that is, the number of elements in the array; that value
shall be greater than zero. A program that violates either requirement is
ill-formed.

(5) If the array-bound constant-expression is not a constant expression,
the program is ill-formed.

[Note: a non-`const`, non-`constexpr` local variable, or any other entity
whose value is not known until runtime, is not usable in a constant
expression ([expr.const]) and therefore cannot be read by an array bound.
This document does not thereby introduce variable-length arrays: unlike the
VLA extension some C++ implementations accept as a non-conforming
extension, an array-bound constant-expression that is not a constant
expression remains ill-formed in SCPP26, exactly as in strict ISO C++. —
end note]

(6) If required constant evaluation of an array-bound constant-expression
would need the size, alignment, or completeness of a class type that is
not yet complete at that point of the program -- including the class type
of the very declaration in which the array member appears -- the program
is ill-formed.

[Note: this is the same rule ISO C++ already applies to `sizeof`
([expr.sizeof]): a class type is incomplete until the closing `}` of its
own definition, so a non-static data member's array bound cannot use
`sizeof` on its own enclosing class. — end note]

(7) If an array-bound constant-expression is value-dependent
([temp.dep.constexpr]) on a template parameter, required constant
evaluation of that array-bound constant-expression is performed at each
point of instantiation ([temp.point]) of the template declaring that
parameter, using the template argument corresponding to that parameter at
that instantiation. The resulting array-bound constant-expression is
subject to (2)-(6), exactly as for any other array-bound
constant-expression.

[Note: for example, in `template<typename T> struct Box { char
storage[sizeof(T)]; };`, the array bound `sizeof(T)` is value-dependent
within the template definition. Instantiating `Box<int>` substitutes `int`
for `T`, so that instantiation's array bound is the ordinary
constant-expression `sizeof(int)`; instantiating `Box<double>` likewise
yields the array bound `sizeof(double)` for that instantiation. This
mirrors ISO C++'s own treatment of a value-dependent expression
([temp.dep.constexpr], [temp.point]): the expression is fixed once, in the
template definition, but is evaluated separately at each point of
instantiation, using that instantiation's template arguments. — end note]

(8) Examples:

Accepted:

```cpp
char literal_bound[8];

char sizeof_bound[sizeof(int) + 4];

constexpr int kBufferSize = 64;
char named_constant_bound[kBufferSize];

struct Header {
    std::uint16_t a;
    std::uint32_t b;
};
char alignof_bound[alignof(Header) * 2];

template<typename T>
struct Box {
    char storage[sizeof(T)]; // value-dependent in the template definition;
                              // resolved to sizeof(int), sizeof(double), etc.
                              // at each point of instantiation (7)
};
```

Rejected:

```cpp
int n = 5;
char runtime_bound[n];      // ill-formed: n is not a constant expression (not a VLA)

struct Self {
    char buf[sizeof(Self)]; // ill-formed: Self is incomplete here
};

char zero_bound[0];         // ill-formed: array bound shall be greater than zero
char negative_bound[-1];    // ill-formed: array bound shall be greater than zero
```

---

[← Previous: Thread-Safety Properties](04-thread-safety-properties.md) · [Table of Contents](README.md) · [Next: Constant evaluation →](06-constant-evaluation.md)
