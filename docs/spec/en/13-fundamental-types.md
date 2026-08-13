# 16 Fundamental types and standard conversions

## 16.1 Scalar types [basic.fundamental.scpp]

(1) This clause specifies SCPP26's *scalar types* and its *null pointer
type*, and the conversions between them and every other type. For those
types it replaces [basic.fundamental] and [conv]. In particular
[conv.prom], [conv.integral], [conv.fpprom], [conv.double], [conv.fpint],
and [conv.bool] do not apply to an SCPP26 program, and [expr.arith.conv]
is not applied to the operands of any operator; §1(2) does not
reintroduce them.

(2) The scalar types are exactly the types named in Table 1. Each name in
Table 1 denotes a distinct type. No name in Table 1 is an alias for
another, and two types of the same width in Table 1 are not the same
type.

Table 1 -- Scalar types

| Type | Size (bytes) | Description |
|---|---|---|
| `bool` | 1 | Has the two values `false` and `true`, represented as 0 and 1. |
| `char` | 1 | A byte value, distinct from `int8_t`, from `uint8_t`, and from every other type. |
| `int8_t`, `int16_t`, `int32_t`, `int64_t` | 1, 2, 4, 8 | Signed integers of exactly the stated width. |
| `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` | 1, 2, 4, 8 | Unsigned integers of exactly the stated width. |
| `int` | 4 | A signed 32-bit integer. |
| `long` | 8 | A signed 64-bit integer. |
| `unsigned int` | 4 | An unsigned 32-bit integer. |
| `unsigned long` | 8 | An unsigned 64-bit integer. |
| `float32_t`, `float64_t` | 4, 8 | IEEE-754 binary32 and binary64. |
| `float` | 4 | IEEE-754 binary32. |
| `double` | 8 | IEEE-754 binary64. |
| `size_t` | width of a pointer | An unsigned integer of the target's pointer width. |
| `ptrdiff_t` | width of a pointer | A signed integer of the target's pointer width. |

(3) The alignment requirement of a scalar type equals its size. An
implementation shall provide every type in Table 1 on every target.
`size_t` and `ptrdiff_t` are the only scalar types whose size depends on
the target.

[Note: `long` in particular has the same width on every target, unlike
C++26, where it is 64 bits under LP64 and 32 bits under LLP64. — end
note]

(4) The following C++26 fundamental types are not provided, and their
names do not denote types: `short`, `long long`, `signed char`,
`unsigned char`, `long double`, `wchar_t`, `char8_t`, `char16_t`, and
`char32_t`. The names `int128_t` and `uint128_t` do not denote types.

(5) `unsigned` is not a type-specifier by itself. In a type, `unsigned`
shall be followed immediately by `int` or by `long`; otherwise the
program is ill-formed.

## 16.2 Literals of scalar type [lex.literal.scpp]

(1) An *integer-literal* and a *floating-point-literal* have no type of
their own. Such a literal has the scalar type required by the context in
which it appears: the declared type of the entity it initializes or is
assigned to, the type of the parameter it is an argument for, the return
type of the function whose `return` statement it is the operand of, or
the type of the other operand of a binary operator or of the other arm of
a conditional expression.

(2) An integer-literal may have any type in Table 1 other than `bool` and
`char`. A floating-point-literal may have type `float`, `float32_t`,
`double`, or `float64_t`.

(3) Where no context determines its type, an integer-literal has type
`int` and a floating-point-literal has type `double`. A *placeholder
type* ([dcl.spec.auto]) is not such a context.

(4) `true` and `false` have type `bool`. A *character-literal* has type
`char`.

[Note: `char c = 65;` and `bool b = 1;` are ill-formed; `char c = 'A';`,
`bool b = true;`, and `char c = static_cast<char>(65);` are well-formed.
`auto x = 5;` declares an `int` and `auto y = 1.5;` a `double`. — end
note]

## 16.3 Conversions between scalar types [conv.scpp]

(1) There is no implicit conversion between any two distinct scalar
types. Where a value of a scalar type is required, the value shall have
exactly that type. This applies to

  (1.1) the initializer of a variable or of a non-static data member;

  (1.2) the right operand of an assignment;

  (1.3) an argument of a function call or of a constructor call;

  (1.4) the operand of a `return` statement;

  (1.5) both operands of a binary operator; and

  (1.6) both arms of a conditional expression.

A value of a different scalar type in any of these positions renders the
program ill-formed. §16.2 applies first: a literal operand takes the
required type rather than being converted to it.

[Note: the rule holds however small the difference between the two types
and whether or not the conversion would preserve the value. `int8_t` to
`int16_t`, `int32_t` to `float64_t`, and `unsigned int` to `long` are
each ill-formed. It holds equally between two same-width types, such as
`int` and `int32_t`, or `float` and `float32_t`, which §16.1(2) makes
distinct. — end note]

(2) A conversion between two scalar types is requested explicitly, by
`static_cast<T>(expr)` or by `(T)expr`, where `T` is a scalar type and
`expr` is of scalar type. Every such conversion is well-formed. The
resulting value is that specified by [conv.integral], [conv.double],
[conv.fpint], and [conv.bool].

(3) In overload resolution, an argument of scalar type matches a
parameter of scalar type only if the two types are the same; no implicit
conversion sequence ([over.best.ics]) other than the identity conversion
exists for such an argument.

[Note: matching is therefore exact, and a call cannot be ambiguous by
reason of a conversion between scalar types. — end note]

(4) No conversion is applied to a value in order to obtain a `bool`. The
condition of a selection statement, of an iteration statement, and of a
conditional expression, the operand of `!`, and each operand of `&&` and
of `||` shall be of type `bool`.

[Note: for a scalar `x`, `if (x)` is ill-formed and
`if (static_cast<bool>(x))` is well-formed. For a pointer `p`, both
`if (p)` and `static_cast<bool>(p)` are ill-formed: SCPP26 has no
pointer-to-`bool` conversion at all. This differs from [conv.bool],
under which every arithmetic, unscoped enumeration, pointer, and
pointer-to-member type converts contextually to `bool`. — end note]

(5) There is no conversion, implicit or explicit, between a scalar type
and a pointer type, a function pointer type, or `nullptr_t`.

## 16.4 The null pointer literal and its type [basic.fundamental.scpp.nullptr]

(1) Except as modified by this subclause, [lex.nullptr] applies unchanged
to an SCPP26 program.

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

(5) `nullptr_t` is not a scalar type: it is not named in Table 1, and
§16.3 does not apply to it. A `static_cast` from a prvalue of type
`nullptr_t` to a scalar type is ill-formed.

[Note: `static_cast<int>(nullptr)` is ill-formed. — end note]

## 16.5 Conversions from the null pointer type [conv.ptr.scpp]

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
pointer-to-`bool` conversion at all (§16.3(4)), so admitting the
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

## 16.6 The null pointer type in the standard library [conv.ptr.scpp.lib]

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
