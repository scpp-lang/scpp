# 6 Ownership, Initialization, and Move

## 6.1 Zero-initialization [dcl.init]

(1) An object of automatic, static, thread, or member storage duration
that has no *initializer* ([dcl.init]) is zero-initialized rather than
left with an indeterminate value, regardless of its type: a scalar
object's value is `0`, `false`, or `0.0` as its type requires; a pointer
object's value is a null pointer value; and each subobject of an object
of array or class type is, recursively, zero-initialized by this same
rule.

[Note: unlike the C++ standard, under which an object of automatic
storage duration and no initializer is left with an indeterminate value
([dcl.init]) unless every subobject is of a type with a user-provided
default constructor, this document requires zero-initialization
unconditionally, for every type. There is consequently no notion, in a
SCPP26 program, of reading an object with an indeterminate value: every
object is well-defined from the moment its lifetime begins
([basic.life]), and no flow analysis is needed to establish that every
execution path initializes an object before it is used. — end note]

## 6.2 Ownership and move state [basic.life]

(1) At every point in a program's execution, an object of automatic,
static, thread, or member storage duration is in exactly one of two
states: **initialized** or **moved-out**.

(2) An object is in the initialized state throughout its lifetime
([basic.life]), except as modified by (3) and (4).

(3) An expression of the form `std::move(E)`, where `E` is an
*id-expression* ([expr.prim.id]) designating an object *obj*, places
*obj* in the moved-out state immediately upon the expression's
evaluation, regardless of whether or how the expression's result is
subsequently used.

[Note: unlike an ordinary invocation of the function template
`std::move` declared in `<utility>` -- which only performs a
value-preserving conversion to an rvalue reference and has, by itself,
no effect on *obj*'s stored value or state -- this document attaches
the state transition in (3) to the syntactic form `std::move(E)`
itself, evaluated for this effect alone; a future clause enumerates
other existing syntax this document similarly reinterprets. — end note]

(4) An assignment to *obj* ([expr.assign]), or another operation this
document defines elsewhere as reinitializing *obj*, discards *obj*'s
current state and value -- whether initialized or moved-out -- and
places *obj* in the initialized state holding the newly assigned value.

(5) A *use* of *obj* is an occurrence of an *id-expression* designating
*obj*, other than: as the operand `E` of an expression of the form
`std::move(E)` under (3); or as the object being reinitialized under
(4).

(6) A program that contains a use (5) of an object at a point in the
program's execution where that object is in the moved-out state is
ill-formed.

[Note: this document does not, in this clause, define a state for a
subobject (a class member, an array element) independent of its
complete object's own state (2)-(4): whether, and under what
conditions, a program may move a subobject out while its complete
object remains otherwise initialized is not yet specified by this
document. — end note]

## 6.3 Destruction [class.dtor]

(1) At the end of an object's storage duration, if the object is in the
initialized state (6.2), its destructor, if any, is invoked, exactly as
the C++ standard requires for an object of that storage duration. If
the object is in the moved-out state, no destructor is invoked for it.

[Note: this document does not modify when an object's storage duration
ends, or any other requirement the C++ standard imposes on destruction;
it modifies only whether the destructor is invoked, based on the
object's ownership/move state (6.2). — end note]

---

[← Previous: The `unsafe` Compound Statement](01-unsafe.md) · [Table of Contents](README.md)
