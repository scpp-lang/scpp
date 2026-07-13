# 14 Enumeration conversions

## 14.1 Conversions between enumerations and integers [conv.enum.scpp]

(1) Except as modified by this subclause, [dcl.enum], [expr.static.cast],
and [expr.cast] apply unchanged to enumeration types in an SCPP26
program.

(2) An expression of enumeration type can be explicitly converted to an
integral type if, and only if, the corresponding conversion is
well-formed in C++26. Such a conversion is permitted whether it is
spelled as a `static_cast` ([expr.static.cast]) or as an
explicit-type-conversion ([expr.cast]).

[Note: `static_cast<int>(color)` and `int(color)` are examples of the
conversions permitted by (2). — end note]

(3) An explicit conversion whose source expression is of integral type
and whose destination type is an enumeration type is ill-formed.

[Note: `static_cast<Color>(n)` and `Color(n)` are both rejected. — end
note]

(4) A program that needs to convert an integer value to an enumeration
type shall call `scpp::enum_cast<T>(value)`, where `T` denotes that
enumeration type. `scpp::enum_cast<T>(value)` shall return
`std::expected<T, scpp::enum_cast_error>`.

(5) `scpp::enum_cast<T>(value)` succeeds if and only if `value` is equal
to the underlying integer value of one of `T`'s enumerators. Otherwise it
shall return an error value of type `scpp::enum_cast_error`, whose
`invalid_value` enumerator denotes that failure. No other unchecked or
ordinary cast-based conversion from an integer value to an enumeration
type is provided.

---

[← Previous: Function template argument deduction](08-function-template-argument-deduction.md) · [Table of Contents](README.md) · [Next: Iteration statements →](10-iteration-statements.md)
