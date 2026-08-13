# 16 The null pointer type

## 16.1 The null pointer literal and its type [basic.fundamental.scpp.nullptr]

(1) Except as modified by this clause, [lex.nullptr],
[basic.fundamental], and [conv.ptr] apply unchanged to an SCPP26
program.

(2) The keyword `nullptr` is a literal denoting the null pointer value.
Its type is `nullptr_t`, a fundamental type distinct from every other
type and having exactly one value.

(3) `nullptr_t` may be named as `nullptr_t` or as `std::nullptr_t`; the
two spellings denote the same type. An implementation shall accept both
spellings, and shall determine the type of `nullptr`, in a translation
unit that imports no module.

[Note: C++26 leaves the type itself unspellable and provides only the
library alias `std::nullptr_t` for `decltype(nullptr)`. SCPP26 gives the
type a keyword spelling, as it already does for `size_t` and
`ptrdiff_t`. — end note]

(4) `nullptr_t` may be used wherever a type may be used, including as
the declared type of a variable or of a non-static data member, as a
parameter type, and as a return type. It is deduced by a *placeholder
type* ([dcl.spec.auto]) and by function template argument deduction
([§13.1](08-function-template-argument-deduction.md#131-deduction-from-a-function-call-tempdeductcallscpp))
from an argument of type `nullptr_t`.

(5) `nullptr_t` is not a member of the scalar type family. A
`static_cast` from a prvalue of type `nullptr_t` to a scalar type is
ill-formed.

[Note: `static_cast<int>(nullptr)` is ill-formed. — end note]

## 16.2 Conversions [conv.ptr.scpp]

(1) A prvalue of type `nullptr_t` is implicitly converted to a
destination type that is:

  (1.1) a pointer type, including a pointer to `void` and a pointer to a
  const-qualified type;

  (1.2) a function pointer type;

  (1.3) `nullptr_t`; or

  (1.4) a class type declaring a constructor whose parameter is of type
  `nullptr_t`, by ordinary constructor overload resolution.

(2) A conversion from `nullptr_t` to any other type is ill-formed,
whether requested implicitly or by an explicit cast. In particular,
`nullptr_t` converts neither to `bool` nor to any integral type.

[Note: (2) differs from C++26, which converts a `nullptr_t` prvalue to
`bool` under direct-initialization. SCPP26 provides no
pointer-to-`bool` conversion at all -- for a pointer `p`, both `if (p)`
and `static_cast<bool>(p)` are ill-formed -- so admitting the
conversion for the null pointer literal alone would make it more
`bool`-convertible than the pointers it denotes. The only spelling
C++26 accepts for that conversion is parenthesized
direct-initialization, `bool b(nullptr)`, which is not valid SCPP26 in
any case. — end note]

(3) The conversions of (1) apply at every boundary at which a value is
given to a declared type: the initializer of a variable or of a
non-static data member, an argument of a function call, an argument of
a constructor call, and the operand of a `return` statement.

(4) In an equality comparison, an operand of type `nullptr_t` is
compatible with an operand of pointer type, of function pointer type,
or of type `nullptr_t`, and with an operand of no other type.

[Note: a program tests a pointer for null by comparing it:
`p == nullptr`. — end note]

(5) `nullptr` designates no object. It introduces no borrow
([§6.2](02-ownership-and-move.md#62-ownership-move-state-and-reborrows-basiclife))
and is not a lifetime source; a value initialized from it is not
diagnosed as dangling on that account.

## 16.3 Standard library types [conv.ptr.scpp.lib]

(1) `std::unique_ptr<T>` and `std::shared_ptr<T>` each declare a
constructor whose parameter is of type `nullptr_t`. It constructs an
empty smart pointer owning nothing; `std::unique_ptr<T> p = nullptr;`
and `std::unique_ptr<T> p{};` have the same effect.

(2) `std::optional<T>` declares no such constructor.
`std::optional<T> o = nullptr;` is ill-formed.

[Note: C++26's `std::optional` takes `std::nullopt_t`, and the same
initialization is ill-formed there. — end note]

---

[← Previous: Modules and Namespaces](12-modules-and-namespaces.md) · [Table of Contents](README.md)
