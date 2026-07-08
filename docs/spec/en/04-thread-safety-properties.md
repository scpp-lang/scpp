# 8 Thread-Safety Properties

## 8.1 Thread-safety attributes [dcl.attr.scpp.thread]

(1) This document defines, for every type, two boolean properties:
**thread-movable** and **thread-shareable**.

(2) The attribute-token `scpp::thread_movable` or
`scpp::thread_shareable` may appear in an *attribute-specifier-seq*
([dcl.attr.grammar]) appertaining to:

  (2.1) a parameter declaration; or

  (2.2) the declaration of a class or struct.

(3) If `[[scpp::thread_movable]]` appertains to a parameter declaration,
the program is ill-formed unless:

  (3.1) if the parameter's type is an rvalue reference to `U`, `U` is
  thread-movable; otherwise

  (3.2) the type determined for that parameter at the point of a call is
  thread-movable.

(4) If `[[scpp::thread_shareable]]` appertains to a parameter
declaration, the program is ill-formed unless:

  (4.1) if the parameter's type is an rvalue reference to `U`, `U` is
  thread-shareable; otherwise

  (4.2) the type determined for that parameter at the point of a call is
  thread-shareable.

(5) If `[[scpp::thread_movable]]` appertains to the declaration of a
class or struct type `T`, `T` is thread-movable irrespective of the
result structural derivation would otherwise produce for that property.

(6) If `[[scpp::thread_shareable]]` appertains to the declaration of a
class or struct type `T`, `T` is thread-shareable irrespective of the
result structural derivation would otherwise produce for that property.

[Note: in the parameter form, the attribute constrains a use of the
parameter's type at a call site; in the class/struct form, it is an
explicit override on the declared type itself. — end note]

## 8.2 Structural derivation [meta.thread.struct]

(1) If a type's own thread-movable or thread-shareable value is not
supplied by an override on that type under 8.1 or 8.4, the property's
value is determined structurally by the rules in this subclause.

(2) A scalar type is thread-movable and thread-shareable.

(3) An array type is thread-movable if its element type is
thread-movable, and thread-shareable if its element type is
thread-shareable.

(4) A reference type is never thread-movable.

(5) A reference to `T` is thread-shareable if and only if:

  (5.1) the referred-to type is `const T`; and

  (5.2) `T` is thread-shareable.

(6) A pointer type is neither thread-movable nor thread-shareable.

[Note: a raw pointer type already requires an explicit
`[[scpp::unsafe]]` context for dereference by
[§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe). This subclause
likewise assigns no stronger default thread-safety property to a raw
pointer in the absence of an explicit override on a surrounding type. —
end note]

(7) A class or struct type `T` is thread-movable if and only if:

  (7.1) `T` has no non-static data member of reference type; and

  (7.2) every non-static data member of `T` is thread-movable.

(8) A class or struct type `T` is thread-shareable if and only if:

  (8.1) no non-static data member of `T` is declared `mutable`; and

  (8.2) every non-static data member of `T` is thread-shareable.

[Note: a `mutable` member affects only thread-shareable. It does not, by
itself, prevent the containing type from being thread-movable. — end note]

(9) A closure type ([expr.prim.lambda.closure]) is thread-movable if
and only if:

  (9.1) it has no capture by lvalue reference; and

  (9.2) every by-value capture member's type is thread-movable.

(10) A closure type ([expr.prim.lambda.closure]) is thread-shareable if
and only if:

  (10.1) it has no capture by mutable lvalue reference;

  (10.2) every by-value capture member's type is thread-shareable; and

  (10.3) for every capture by `const` lvalue reference, the referent
  type is thread-shareable.

## 8.3 Builtin predicates [expr.prim.scpp.thread]

(1) The forms `scpp::is_thread_movable(T)` and
`scpp::is_thread_shareable(T)` are builtin predicates.

(2) In each such form, `T` shall name a type. The token sequence between
the parentheses is parsed as a type operand, not as an expression
operand of an ordinary function call.

(3) Each such form is a prvalue of type `bool` and may appear wherever a
boolean constant-expression is permitted.

(4) `scpp::is_thread_movable(T)` evaluates to `T`'s own thread-movable
value, determined by:

  (4.1) an override on `T` under 8.4, if present;

  (4.2) otherwise an override on `T` under 8.1(5), if present; or

  (4.3) otherwise the structural derivation result for `T` under 8.2.

(5) `scpp::is_thread_shareable(T)` evaluates to `T`'s own
thread-shareable value, determined by:

  (5.1) an override on `T` under 8.4, if present;

  (5.2) otherwise an override on `T` under 8.1(6), if present; or

  (5.3) otherwise the structural derivation result for `T` under 8.2.

[Note: the syntax is analogous to a compiler builtin trait such as
`__is_trivially_copyable(T)`, not to an ordinary function call taking a
value argument. — end note]

## 8.4 Conditional override [dcl.attr.scpp.thread.if]

(1) The attribute-token `scpp::thread_movable_if` may appear in an
*attribute-specifier-seq* ([dcl.attr.grammar]) appertaining to the
declaration of a class or struct.

(2) `[[scpp::thread_movable_if(a, b)]]` takes exactly two arguments.

(3) Each of `a` and `b` shall be a boolean constant-expression.

(4) For a non-template class or struct, `a` is that type's own
thread-movable value and `b` is that type's own thread-shareable value.

(5) For a class or struct template, `a` and `b` are evaluated
separately for each instantiation after substituting that
instantiation's template arguments; `a` is that instantiation's own
thread-movable value and `b` is that instantiation's own
thread-shareable value.

(6) The values established by (4) or (5) replace, for that type or
instantiation, the results structural derivation would otherwise
produce for both properties from the type's fields.

(7) This attribute is an ordinary attribute available to any user
declaration of a class or struct; the thread-movable or
thread-shareable value of a type is not determined by any distinguished
library type name.

[Note: the following declaration gives `unique_ptr<T>` the same two
properties as `T`, independently:

```cpp
template<typename T>
class [[scpp::thread_movable_if(
    scpp::is_thread_movable(T),
    scpp::is_thread_shareable(T)
)]] unique_ptr {
    // ...
};
```

The following declaration gives `shared_ptr<T>` both properties only
when `T` is both thread-movable and thread-shareable:

```cpp
template<typename T>
class [[scpp::thread_movable_if(
    scpp::is_thread_movable(T) && scpp::is_thread_shareable(T),
    scpp::is_thread_movable(T) && scpp::is_thread_shareable(T)
)]] shared_ptr {
    // ...
};
```

In both examples, the attribute is used exactly as it may be on any
other user-declared class template. — end note]

---

[← Previous: Dereference and Member Access](03-dereference-and-member-access.md) · [Table of Contents](README.md) · [Next: Union types and packed layout →](05-unions-and-packed-layout.md)
