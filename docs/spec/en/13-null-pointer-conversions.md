# 16 Null pointer conversions

## 16.1 The null pointer literal and its type [conv.ptr.scpp]

(1) Except as modified by this subclause, [lex.nullptr], [conv.ptr], and
[basic.fundamental] apply unchanged to an SCPP26 program.

(2) The keyword `nullptr` denotes the **null pointer literal**. Its type
is `nullptr_t`, which is a distinct fundamental type.

(3) `nullptr_t` is known to the implementation independently of any
imported module. It may be named as `nullptr_t` or as `std::nullptr_t`;
the two spellings denote the same type.

[Note: C++26 leaves the type itself unspellable and provides only the
library alias `std::nullptr_t` over `decltype(nullptr)`. SCPP26 instead
gives the type a keyword spelling, as it already does for `size_t` and
`ptrdiff_t`, so that a program that never imports a module can still
name it. — end note]

(4) `nullptr_t` has exactly one value, the null pointer value. Two
prvalues of type `nullptr_t` compare equal.

## 16.2 Conversions [conv.ptr.scpp.conv]

(1) A prvalue of type `nullptr_t` can be implicitly converted to:

  (1.1) any pointer type, including a pointer to `void` and a pointer to
  a const-qualified type;

  (1.2) any function pointer type;

  (1.3) `nullptr_t` itself; and

  (1.4) a class type that declares a constructor whose sole parameter is
  of type `nullptr_t`, by ordinary constructor overload resolution.

(2) A conversion from `nullptr_t` to any other type is ill-formed. In
particular, `nullptr_t` does not convert to `bool` and does not convert
to any integral type, whether implicitly or by an explicit cast.

[Note: this differs from C++26, which permits a `nullptr_t` prvalue to
convert to `bool` under direct-initialization. SCPP26 has no
pointer-to-`bool` conversion at all ([conv.bool] as modified by the
scalar-conversion rules): `if (p)` is ill-formed for a pointer `p`, and
`static_cast<bool>(p)` is likewise ill-formed. Admitting `bool b(nullptr)`
alone would make the null literal more `bool`-convertible than the
pointers it exists to denote; the spelling C++26 uses for it,
parenthesized direct-initialization, is in any case not valid SCPP26. A
program tests a pointer for null by comparing it: `p == nullptr`. — end
note]

(3) The conversions in (1) apply uniformly at every boundary at which a
value is given to a declared type: the initializer of a variable, an
argument of a function call, an argument of a constructor call, and the
operand of a `return` statement.

(4) The equality operators `==` and `!=` accept a `nullptr_t` operand
against an operand of pointer type, of function pointer type, or of type
`nullptr_t`. No other operator accepts a `nullptr_t` operand.

## 16.3 Ownership and lifetime [conv.ptr.scpp.own]

(1) The null pointer literal designates no object. It therefore
introduces no borrow, and a value initialized from it has no lifetime
source; a reference or pointer whose value came from `nullptr` can never
be diagnosed as dangling on that account.

(2) An object of class type constructed from `nullptr` by (1.4) is
initialized, and is destroyed at the end of its lifetime like any other
object of that type.

---

[← Previous: Modules and Namespaces](12-modules-and-namespaces.md) · [Table of Contents](README.md)
