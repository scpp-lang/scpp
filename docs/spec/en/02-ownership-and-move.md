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

## 6.4 Move construction and move assignment [class.copy.ctor], [class.copy.assign]

(1) A program shall not declare a move constructor ([class.copy.ctor])
or a move assignment operator ([class.copy.assign]) for a class type; a
declaration the C++ standard would otherwise classify as either is
ill-formed.

(2) Every class type has an implicitly-defined move constructor with
exactly one parameter, of type rvalue reference to the class type,
irrespective of whether the C++ standard's own conditions for
implicitly declaring one ([class.copy.ctor]) are met.

(3) A class type has an implicitly-defined move assignment operator
with exactly one parameter, of type rvalue reference to the class type,
irrespective of whether the C++ standard's own conditions for
implicitly declaring one ([class.copy.assign]) are met, unless the
class has a non-static data member of reference type, in which case it
has no move assignment operator, exactly as the C++ standard's own
conditions ([class.copy.assign]) already provide.

(4) The implicitly-defined move constructor for a class X initializes
each non-static data member of the object being constructed with the
corresponding non-static data member of the constructor's parameter,
moved in the manner appropriate to that member's type, in declaration
order.

(5) The implicitly-defined move assignment operator for a class X
replaces the value of each non-static data member of the object denoted
by `*this` with the corresponding non-static data member of the
operator's parameter, moved in the manner appropriate to that member's
type, in declaration order, and returns `*this`.

[Note: (4) and (5) apply recursively where a non-static data member is
itself of class type: (2)/(3) give that member's own type an
implicitly-defined move constructor/move assignment operator, which (1)
guarantees is not a user declaration this document must instead
reconcile with. — end note]

[Note: [§6.2](02-ownership-and-move.md#62-ownership-and-move-state-basiclife) already places the
object denoted by an expression of the form `std::move(E)` in the
moved-out state upon that expression's evaluation, and
[§6.3](02-ownership-and-move.md#63-destruction-classdtor) already excuses an object in the
moved-out state from destruction; this subclause introduces no separate
rule for either effect for an object supplied as the argument
initializing (4)'s or (5)'s parameter. — end note]

```cpp
struct Inner { int* p; };
class Outer {
    Inner a;
    int b;
public:
    Outer(int* p, int b_) : a{p}, b(b_) {}
};

Outer x(new int(1), 2);
Outer y(std::move(x));   // (4): memberwise move-constructs y.a, y.b from x.a, x.b;
                          // x is thereafter in the moved-out state (§6.2) and its
                          // destructor, if declared, is not invoked for it (§6.3)
```

## 6.5 Copy construction and copy assignment [class.copy.ctor], [class.copy.assign]

(1) A program may declare a copy constructor ([class.copy.ctor]) or a
copy assignment operator ([class.copy.assign]) for a class type.

(2) A class type that has no user-declared copy constructor, no
user-declared destructor, and no user-declared copy assignment
operator has an implicitly-defined copy constructor with exactly one
parameter, of type `const` reference to the class type, irrespective
of whether the C++ standard's own conditions for implicitly declaring
one ([class.copy.ctor]) are met. A class type that has a user-declared
destructor or a user-declared copy assignment operator, and no
user-declared copy constructor, has no copy constructor.

(3) A class type that has no user-declared copy assignment operator, no
user-declared destructor, and no user-declared copy constructor has an
implicitly-defined copy assignment operator with exactly one parameter,
of type `const` reference to the class type, irrespective of whether
the C++ standard's own conditions for implicitly declaring one
([class.copy.assign]) are met, unless the class has a non-static data
member of reference type, in which case it has no copy assignment
operator, exactly as the C++ standard's own conditions
([class.copy.assign]) already provide. A class type that has a
user-declared destructor or a user-declared copy constructor, and no
user-declared copy assignment operator, has no copy assignment
operator.

(4) Whether a class type has a user-declared copy constructor is
independent of whether it has a user-declared copy assignment operator,
and conversely; a program may declare either without the other.

(5) The implicitly-defined copy constructor for a class X initializes
each non-static data member of the object being constructed with the
corresponding non-static data member of the constructor's parameter,
copied in the manner appropriate to that member's type, in declaration
order.

(6) The implicitly-defined copy assignment operator for a class X
replaces the value of each non-static data member of the object denoted
by `*this` with the corresponding non-static data member of the
operator's parameter, copied in the manner appropriate to that member's
type, in declaration order, and returns `*this`.

[Note: (5) and (6) apply recursively where a non-static data member is
itself of class type: that member's own type has, by this subclause,
either an implicitly-defined copy constructor/copy assignment operator,
a user-declared one, or none at all -- in the last case, (5) or (6),
respectively, is not satisfiable for X, and X consequently likewise has
no implicitly-defined copy constructor or copy assignment operator,
respectively. — end note]

[Note: unlike [§6.4](02-ownership-and-move.md#64-move-construction-and-move-assignment-classcopyctor-classcopyassign),
this subclause does not forbid a user-declared copy constructor or copy
assignment operator, and (5)/(6) leave the object denoted by the
constructor's or operator's parameter completely unaffected -- copying,
unlike moving, never changes the state of the object copied from,
whether the constructor or operator invoked is user-declared or
implicitly-defined. — end note]

[Note: the circumstances in (2) under which a class type has no
implicitly-defined copy constructor, and the circumstances in (3) under
which it has no implicitly-defined copy assignment operator, are
exactly the circumstances under which the C++ standard's own implicit
definition of the corresponding special member function is deprecated
rather than absent ([depr.impldec]). — end note]

[Note: because (2) and (3) preclude an implicitly-defined copy
constructor or copy assignment operator for a class type in the
circumstances given there, and (5)/(6) never modify the object denoted by
the parameter, an assignment of the form `x = x` through an
implicitly-defined copy assignment operator (3) is unconditionally
well-defined; this document imposes no corresponding guarantee on a
user-declared copy assignment operator (1), whose behavior for such an
assignment is exactly what its own definition gives it, as for any
other user-declared function. — end note]

```cpp
class RefCounted {
    int* count;
public:
    RefCounted(int* c) : count(c) {}
    // user-declared: this class has a destructor, so it would otherwise
    // have no copy constructor/assignment operator at all (2)/(3)
    RefCounted(const RefCounted& other) : count(other.count) { ++(*count); }
    RefCounted& operator=(const RefCounted& other) {
        if (this != &other) { count = other.count; ++(*count); }
        return *this;
    }
    ~RefCounted() { --(*count); }
};
```

---

[← Previous: The `[[scpp::unsafe]]` Attribute](01-unsafe.md) · [Table of Contents](README.md)
