# 6 Ownership, Initialization, and Move

## 6.1 Required initialization and zero-initialization [dcl.init]

(1) A definition of a non-array local variable shall include an
*initializer* ([dcl.init]). A definition of a local variable of non-array
type with no *initializer* is ill-formed, regardless of that variable's
type or storage duration.

[Note: `int x;` and `Counter c;` are ill-formed; `int x{};`,
`Counter c{};`, `Counter c{1, 2};`, and `Counter c = make_counter();`
are well-formed. This rule is purely syntactic: SCPP26 does not permit
an uninitialized local declaration whose later assignments are validated
by flow analysis. — end note]

(2) A non-static data member of a class or struct is initialized for a
given constructor by exactly one of the following:

  (2.1) an in-class default member initializer on that member's own
  declaration; or

  (2.2) a member-initializer naming that member in that constructor's
  member-initializer-list.

(3) A constructor definition may include a member-initializer-list
immediately after its parameter list and before its function-body. A
member-initializer-list is introduced by `:` and consists of one or more
member-initializers separated by `,`. Each member-initializer shall:

  (3.1) name a non-static data member of the constructor's class or
  struct type; and

  (3.2) supply that member with a *braced-init-list*
  ([dcl.init.list]).

A parenthesized *expression-list* in a member-initializer is
ill-formed.

(4) For each constructor definition of a class or struct, every
non-static data member shall be initialized either by:

  (4.1) a member-initializer in that constructor's own
  member-initializer-list; or

  (4.2) an in-class default member initializer on that member's
  declaration, provided that constructor's member-initializer-list does
  not name that member.

If neither (4.1) nor (4.2) applies to a member for a given constructor,
that constructor is ill-formed. A member shall not be named more than
once in the same member-initializer-list.

(5) A non-static data member of reference type shall satisfy (4) by a
well-formed reference binding. Because a reference has no empty state, a
constructor for a class or struct with a reference member is
ill-formed unless that member is initialized either by an in-class
default member initializer that binds it to an object, or by that
constructor's member-initializer-list.

(6) A variable definition that has no *initializer* ([dcl.init]), and is
not ill-formed by (1), is zero-initialized rather than left with an
indeterminate value, regardless of its type: a scalar object's value is
`0`, `false`, or `0.0` as its type requires; a pointer object's value is
a null pointer value; and each subobject of an object of array or class
type is, recursively, zero-initialized by this same rule.

(7) If an object definition uses an *initializer* ([dcl.init]) to supply
arguments for direct-initialization, that *initializer* shall be a
*braced-init-list* ([dcl.init.list]). A parenthesized
*expression-list* in that position does not initialize an object in
SCPP26; the program is ill-formed.

[Note: `Widget x{1, 2};` is well-formed; `Widget x(1, 2);` is
ill-formed. This rule affects object definitions only; it does not
modify the syntax of a constructor declaration such as `Widget(int,
int)` or of a function call. — end note]

[Note: `Widget(int x) : value{x} {}` is, however, a constructor
member-initializer under (3), not an object definition under (7). — end
note]

[Note: unlike the C++ standard, under which an object of automatic
storage duration and no initializer is left with an indeterminate value
([dcl.init]) unless every subobject is of a type with a user-provided
default constructor, SCPP26 rejects such local declarations outright by
(1), requires member-initialization completeness by (4), and requires
zero-initialization elsewhere by (6). There is
consequently no notion, in an SCPP26 program, of reading an object with
an indeterminate value, and no flow analysis is needed to establish that
every execution path initializes a local object before it is used. — end
note]

[Note: (1)-(5) do not alter the rules for union members or for array
declarations; those remain governed by other clauses or future design
work. — end note]

```cpp
int x{};                         // OK: (1)
int y = 1;                       // OK: (1)
int z;                           // ill-formed: (1)

struct Defaults {
    int a{};
    int b{5};
};

struct CtorOnly {
    int a;
    int b;
public:
    CtorOnly(int x, int y) : a{x}, b{y} {}
};

struct Mixed {
    int a{1};
    int b;
public:
    Mixed(int x) : b{x} {}
};

int global_target{};

struct RefBox {
    int& ref;
public:
    RefBox(int& r) : ref{r} {}
};

struct Bad {
    int a{};
    int b;
public:
    Bad(int x) : a{x} {}   // ill-formed: (4), b is initialized by neither path
};
```

## 6.2 Ownership, move state, and reborrows [basic.life]

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

(7) If a local variable or parameter of reference type, `std::span<T>`,
or `std::span<const T>` is used to initialize another local variable of
reference or span type, or to satisfy a parameter of reference or span
type, the new binding is a reborrow if it aliases the same underlying
object or range through that existing binding.

(8) For the purposes of (9), a reborrow is *live* at a program point if
the binding introduced by (7) may still be used on some control-flow
path forward from that point. A reborrow's liveness is not required to
extend to the end of its lexical scope if no further such use can occur.

(9) A mutable reborrow is well-formed only if the existing binding from
which it is formed is itself mutable. While a reborrow formed from a
mutable existing binding is live:

  (9.1) an operation that writes through that existing binding is
  ill-formed; and

  (9.2) a further reborrow formed from that existing binding is
  ill-formed.

A read through that existing binding is not, by itself, ill-formed
solely because the reborrow is live, provided that read does not mutate
the underlying object or range and does not violate any other rule of
this document. Once the reborrow is no longer live, the existing binding
may again be used for writes or reborrows.

(10) A shared reborrow does not make the program more permissive than the
binding from which it is formed: it may not be used to mutate an object
or range that is reachable only through a shared or `const` binding.

[Note: reborrowing under (7)-(10) requires an already-existing object or
range that is being aliased through an already-existing reference or span
binding. A binding that instead materializes a temporary under (11) does
not have such a lender object. — end note]

(11) If a local variable or parameter of type `const T&` is initialized
from either:

  (11.1) an expression that is an rvalue of type `T`, including a fresh
  value of type `T`; or

  (11.2) an expression from which a temporary object of type `T` is
  directly constructed by selecting a constructor of `T` that takes that
  expression as its single argument,

a temporary object of type `T` is materialized and the reference binds to
that temporary.

(12) A temporary materialized under (11) remains alive for the lifetime
of that reference binding. Such a binding is not a reborrow under
(7)-(10), and introduces no lender object for those rules, because it
aliases no pre-existing object or range.

[Note: this document does not, in this clause, define a state for a
subobject (a class member, an array element) independent of its
complete object's own state (2)-(4): whether, and under what
conditions, a program may move a subobject out while its complete
object remains otherwise initialized is not yet specified by this
document. Reborrowing under (7)-(10) is about aliases formed through an
already-existing reference or span binding; a binding that instead
materializes a temporary under (11)-(12) is not such an alias. Neither
kind of binding, by itself, places the complete object in the moved-out
state. — end note]


### Cross-function lifetime groups [dcl.attr.scpp.lifetime]

(13) The attribute-token `scpp::lifetime` may appear in an
*attribute-specifier-seq* ([dcl.attr.grammar]) appertaining to:

  (13.1) a parameter declaration, including a `requires(...)`
  expression's probe parameter declaration, whose type is a reference
  type, pointer type, `std::span<T>`, or `std::span<const T>`; or

  (13.2) the declarator of a function or member function whose return
  type is a reference type, pointer type, `std::span<T>`, or
  `std::span<const T>`.

If it appears elsewhere, the program is ill-formed.

(14) `[[scpp::lifetime(name)]]` takes exactly one argument. That
argument shall be an identifier.

(15) A user-written group name is any such identifier other than
`any`. User-written group names are local to one function or member
function declaration. The same spelling in two different
declarations denotes no relation. Within one such declaration, user-written occurrences with
the same spelling denote one named lifetime group; user-written
occurrences with different spellings denote different named lifetime
groups.

(16) The identifier `any` is reserved. Each parameter tagged
`[[scpp::lifetime(any)]]` denotes a fresh compiler-synthesized
lifetime group distinct from:

  (16.1) every user-written group; and

  (16.2) every other `any` occurrence, including another such
  parameter in the same declaration.

An `any` group does not introduce a name that may later be referred
to by a return annotation or by another parameter.

(17) If a parameter declaration bears `[[scpp::lifetime(name)]]` with a
user-written group name `name`, that parameter is a member of group
`name`. A parameter with no `scpp::lifetime` attribute belongs to no
named lifetime group.

(18) If the declarator of such a function or member function bears
`[[scpp::lifetime(name)]]`, the returned reference, pointer, or
span value is tied to group `name`.
The program is ill-formed if:

  (18.1) the return type is not an eligible type under (13.2);

  (18.2) `name` is `any`; or

  (18.3) neither:

    (18.3.1) some explicit parameter of that declaration is a member of
    group `name`; nor

    (18.3.2) the declaration is a non-static member function named
    `operator->` using the special implicit-object rule of (23).

(19) A value tied to group `name` shall be derived only from:

  (19.1) one or more explicit parameters that are members of group
  `name`;

  (19.2) for a non-static member function named `operator->` governed by
  (23), that call's implicit object parameter; or

  (19.3) a subobject, array element, base-class subobject, pointee, or
  contiguous range reachable through a value from (19.1) or (19.2).

It is ill-formed to return a value tied to group `name` if the returned
value is instead derived from:

  (19.4) an explicit parameter in a different named group;

  (19.5) an `any`-tagged parameter; or

  (19.6) a local object, temporary object, or other state whose
  lifetime is not proved to outlive the call.

(20) If several parameters belong to the same named group, the function
may return or forward a value derived from any of them wherever that
group is required. At a call site, a result tied to that group is
treated as no longer-lived than the shortest-lived actual argument
supplied to any parameter in that group. Parameters in different named
groups are lifetime-independent unless some other rule of this document
relates them.

(21) Lifetime-group identity constrains lifetime only. It does not relax
aliasing, mutability, thread-safety, or `[[scpp::unsafe]]`
requirements. In particular, annotating a raw pointer with
`[[scpp::lifetime(name)]]` does not permit dereference outside an
`[[scpp::unsafe]]` context.

(22) User-written group names are declaration-local and alpha-
equivalent. A call from one such declaration to another does not
compare lifetime-group names textually across declarations; the checker
instead uses the callee's own grouping relation to determine which
actual arguments may influence that callee's eligible return value under
(18)-(20). Whether a value derived from such a group may instead be
embedded into object state is governed separately by (24). The same
declaration-local, alpha-equivalent comparison is used when a
`requires(...)` expression tests a callable with probe parameters
bearing `[[scpp::lifetime(name)]]`. Such an annotation constrains
concept satisfaction; it is not merely syntactic sugar for an otherwise
ordinary test call. For such a satisfaction check:

  (22.1) a probe parameter tagged with a user-written group name
  requires the corresponding parameter of the selected callable
  declaration to be a member of some non-`any` group, and probe
  parameters in the same user-written group require corresponding
  parameters in that declaration to be members of one and the same
  group;

  (22.2) probe parameters in different user-written groups require
  corresponding parameters in that declaration to belong to different
  groups;

  (22.3) a probe parameter tagged
  `[[scpp::lifetime(any)]]` requires the corresponding parameter of
  the selected callable declaration to be tagged
  `[[scpp::lifetime(any)]]`; and

  (22.4) a probe parameter with no `scpp::lifetime` attribute imposes
  no lifetime-group constraint beyond the ordinary well-formedness and
  type requirements of the probe.

(23) A non-static member function may use named lifetime groups on its
explicit parameters under the same rules as a free function. For the
purposes of this subclause, a call to a non-static member function
supplies an implicit object parameter of reference type: `C&` for a
non-`const` member function and `const C&` for a `const` member
function; any borrow or reborrow through that implicit object parameter
is governed by 6.2(7)-(12). The implicit object parameter cannot bear
`[[scpp::lifetime(name)]]` on its own declaration. Except as provided
next for `operator->`, it does not by itself introduce a user-written
group name. Consequently, a member function with an explicit
`[[scpp::lifetime(name)]]` return annotation is ill-formed unless one of
its explicit parameters is a member of group `name`; a value derived
solely from `this` cannot satisfy that requirement. A non-static member
function named `operator->` may, however, use
`[[scpp::lifetime(name)]]` on its declarator to tie its returned value
directly to that call's implicit object parameter instead of to an
explicit parameter. In that special case, `name` remains a declaration-
local user-written group name, but for that one declaration it denotes
the implicit object parameter for the purposes of (18.3.2) and
(19.2)-(19.3).

(24) Constructing an object, closure, or other stored state from a
reference, pointer, or span derived from a named lifetime group does not
erase that group's lifetime obligation. This subclause defines lifetime-
group propagation only for the direct function or member-function return
value governed by (18)-(20); it defines no mechanism by which a class,
struct, union, array, closure, or other object type itself carries a
named lifetime-group parameter. Therefore, if a reference, pointer, or
span derived from a named group or from `[[scpp::lifetime(any)]]`
would be used to initialize or assign any subobject of such an object,
the program is ill-formed. This includes returning `Holder{x}` where
`Holder` contains a reference member initialized from `x`, storing such
a value into a data member or array element, or capturing it in a
closure. This prohibition does not forbid ordinary local reborrows under
6.2(7)-(12), nor passing such a value as an argument to another call.

(25) Lifetime-group annotations are permitted on function templates and
on members of class templates under the same rules as on non-templates.
Template argument substitution neither creates nor merges groups; it
instantiates the same declaration-local grouping relation for the
specialized signature.

(26) Two declarations of the same function or member function shall
agree in lifetime-group annotations after a consistent renaming of user-
written group names. Lifetime-group annotations are safety-relevant
function-signature facts, but overload resolution shall not
distinguish functions solely by different lifetime-group annotations.

(27) Lifetime-group annotations do not alter a type's layout,
triviality, thread-movable value, or thread-shareable value under §8. If
the same declaration also bears thread-safety attributes, both sets of
requirements apply independently.

The following declarations are well-formed:

```cpp
const int& get_x(
    const int& x [[scpp::lifetime(a)]],
    const int& y [[scpp::lifetime(b)]]
) [[scpp::lifetime(a)]] {
    return x;
}

const int& min_ref(
    const int& x [[scpp::lifetime(a)]],
    const int& y [[scpp::lifetime(a)]]
) [[scpp::lifetime(a)]] {
    return x < y ? x : y;
}

const int* pick_right(
    const int* left [[scpp::lifetime(a)]],
    const int* right [[scpp::lifetime(b)]]
) [[scpp::lifetime(b)]] {
    return right;
}

const int& keep_head(
    const int& head [[scpp::lifetime(head_life)]],
    int& scratch [[scpp::lifetime(any)]]
) [[scpp::lifetime(head_life)]] {
    scratch = 0;
    return head;
}
```

The following declarations are ill-formed:

```cpp
const int& bad_unknown(
    const int& x [[scpp::lifetime(a)]]
) [[scpp::lifetime(b)]] {
    return x;
}
// ill-formed: `b` is introduced by no parameter

const int& bad_mismatch(
    const int& x [[scpp::lifetime(a)]],
    const int& y [[scpp::lifetime(b)]]
) [[scpp::lifetime(a)]] {
    return y;
}
// ill-formed: the returned reference is derived from group `b`, not `a`

struct Holder {
    const int& ref;
};

Holder bad_named_store(const int& x [[scpp::lifetime(a)]]) {
    return Holder{x};
}
// ill-formed: this subclause provides no way for `Holder` to carry group `a`

const int& bad_any_return(
    const int& x [[scpp::lifetime(any)]]
) [[scpp::lifetime(any)]] {
    return x;
}
// ill-formed: `any` is reserved and cannot be named by the return

Holder bad_store(const int& x [[scpp::lifetime(any)]]) {
    return Holder{x};
}
// ill-formed: a value derived from `any` is stored in returned state
```

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

(4) The implicitly-defined move constructor for a class X
move-constructs each base-class subobject of the object being
constructed from the corresponding base-class subobject of the
constructor's parameter, in the order the C++ standard requires for the
construction of X. If X is a most-derived class and has a virtual base
class, that virtual base subobject is move-constructed exactly once by
X, exactly as in ordinary C++ construction.

(5) After the base-class subobjects required by (4), the
implicitly-defined move constructor for a class X initializes each
non-static data member of the object being constructed with the
corresponding non-static data member of the constructor's parameter,
moved in the manner appropriate to that member's type, in declaration
order.

(6) The implicitly-defined move assignment operator for a class X
applies move assignment to the base-class subobjects of the object
denoted by `*this` from the corresponding base-class subobjects of the
operator's parameter exactly as the C++ standard requires for X.

(7) After the base-class subobjects required by (6), the
implicitly-defined move assignment operator for a class X replaces the
value of each non-static data member of the object denoted by `*this`
with the corresponding non-static data member of the operator's
parameter, moved in the manner appropriate to that member's type, in
declaration order, and returns `*this`.

[Note: (4)-(7) apply recursively where a base-class subobject or a
non-static data member is itself of class type: (2)/(3) give that
subobject's or member's own type an implicitly-defined move
constructor/move assignment operator, which (1) guarantees is not a user
declaration this document must instead reconcile with. — end note]

[Note: [§6.2](02-ownership-and-move.md#62-ownership-and-move-state-basiclife) already places the
object denoted by an expression of the form `std::move(E)` in the
moved-out state upon that expression's evaluation, and
[§6.3](02-ownership-and-move.md#63-destruction-classdtor) already excuses an object in the
moved-out state from destruction; this subclause introduces no separate
rule for either effect for an object supplied as the argument
initializing (4)'s or (6)'s parameter. — end note]

```cpp
struct Inner { int* p; };
struct Outer {
    Inner a;
    int b;
public:
    Outer(int* p, int b_) : a{p}, b{b_} {}
};

Outer x{new int{1}, 2};
Outer y{std::move(x)};   // (5): memberwise move-constructs y.a, y.b from x.a, x.b;
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

(5) The implicitly-defined copy constructor for a class X
copy-constructs each base-class subobject of the object being
constructed from the corresponding base-class subobject of the
constructor's parameter, in the order the C++ standard requires for the
construction of X. If X is a most-derived class and has a virtual base
class, that virtual base subobject is copy-constructed exactly once by
X, exactly as in ordinary C++ construction.

(6) After the base-class subobjects required by (5), the
implicitly-defined copy constructor for a class X initializes each
non-static data member of the object being constructed with the
corresponding non-static data member of the constructor's parameter,
copied in the manner appropriate to that member's type, in declaration
order.

(7) The implicitly-defined copy assignment operator for a class X
applies copy assignment to the base-class subobjects of the object
denoted by `*this` from the corresponding base-class subobjects of the
operator's parameter exactly as the C++ standard requires for X.

(8) After the base-class subobjects required by (7), the
implicitly-defined copy assignment operator for a class X replaces the
value of each non-static data member of the object denoted by `*this`
with the corresponding non-static data member of the operator's
parameter, copied in the manner appropriate to that member's type, in
declaration order, and returns `*this`.

[Note: (5)-(8) apply recursively where a base-class subobject or a
non-static data member is itself of class type: that subobject's or
member's own type has, by this subclause, either an implicitly-defined
copy constructor/copy assignment operator, a user-declared one, or none
at all -- in the last case, (5)/(6) or (7)/(8), respectively, are not
satisfiable for X, and X consequently likewise has no implicitly-defined
copy constructor or copy assignment operator, respectively. — end note]

[Note: unlike [§6.4](02-ownership-and-move.md#64-move-construction-and-move-assignment-classcopyctor-classcopyassign),
this subclause does not forbid a user-declared copy constructor or copy
assignment operator, and (5)-(8) leave the object denoted by the
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
circumstances given there, and (5)-(8) never modify the object denoted by
the parameter, an assignment of the form `x = x` through an
implicitly-defined copy assignment operator (3) is unconditionally
well-defined; this document imposes no corresponding guarantee on a
user-declared copy assignment operator (1), whose behavior for such an
assignment is exactly what its own definition gives it, as for any
other user-declared function. — end note]

```cpp
struct RefCounted {
    int* count;
public:
    RefCounted(int* c) : count{c} {}
    // user-declared: this class has a destructor, so it would otherwise
    // have no copy constructor/assignment operator at all (2)/(3)
    RefCounted(const RefCounted& other) : count{other.count} { ++(*count); }
    RefCounted& operator=(const RefCounted& other) {
        if (this != &other) { count = other.count; ++(*count); }
        return *this;
    }
    ~RefCounted() { --(*count); }
};
```

## 6.6 Fresh values and function parameter binding [expr.call]

(1) For the purposes of this document, a **fresh value** of type `T` is:

  (1.1) an expression of the form `std::move(E)` where `E` designates an
  object of type `T`; or

  (1.2) a call expression whose type is `T`; or

  (1.3) an expression of the form `T{a1, ..., an}` that directly
  constructs a temporary object of type `T`.

(2) A fresh value of type `T` may be used wherever this document
requires a fresh value of type `T`, including this subclause and §6.7.

(3) If a function parameter has class type `T` and is not of reference
type, the parameter object is initialized at each call according to
(4)-(7).

(4) If the corresponding argument is an *id-expression* designating a
local object, including a parameter, whose type is exactly `T`, and `T`
has a copy constructor (6.5), the parameter object is copy-constructed
from that local object.

(5) Otherwise, the corresponding argument shall be a fresh value of type
`T`.

(6) If neither (4) nor (5) is satisfied, the program is ill-formed.

(7) After its initialization under (4) or (5), the parameter object is
an ordinary automatic object of type `T` within the callee, governed by
§6.2-§6.5 exactly as any other local object of class type is governed.

(8) A candidate function whose by-value class parameter cannot be
initialized as required by (3)-(7) is not viable for overload
resolution.

(9) If a function parameter has type `const T&` and the corresponding
argument satisfies either §6.2(11.1) or §6.2(11.2), the parameter binds
to the temporary materialized by §6.2(11), and that temporary's
lifetime is governed by §6.2(12).

(10) Otherwise, a function parameter of type `const T&` binds directly
to the argument's designated object under the ordinary rules for
reference binding. If that direct binding aliases an already-existing
reference or span binding, it is a reborrow governed by
§6.2(7)-(10).

(11) If neither (9) nor (10) is satisfied, the program is ill-formed.

(12) A candidate function whose `const T&` parameter cannot be
initialized as required by (9)-(11) is not viable for overload
resolution.

```cpp
struct Box {
public:
    int value;

    Box(int v) : value{v} {}
};

void consume(Box value);
int read_double(const double& x) { return x == 3.5 ? 0 : 1; }
int read_box(const Box& x) { return x.value; }
int read_text(const std::string& text) { return text.length(); }

int call_examples() {
    std::string greeting{"hello"};

    consume(Box{1});                        // OK: 6.6(1.3), 6.6(5)
    if (read_double(3.5) != 0) return 1;   // OK: 6.2(11.1), 6.6(9)
    if (read_box(Box{42}) != 42) return 2; // OK: 6.2(11.1), 6.6(9)
    if (read_text("hi") != 2) return 3;    // OK: 6.2(11.2), 6.6(9)
    if (read_text(std::move(greeting)) != 5) return 4; // OK: 6.2(11.1), 6.6(9)
    return 0;
}
```

## 6.7 By-value return of class type [stmt.return]

(1) If a function's return type is class type `T`, a `return` statement's
operand initializes the returned object according to this subclause.

(2) If the operand is exactly an unparenthesized *id-expression*
designating a local object, or a non-reference parameter object, of the
innermost enclosing function, whose type is exactly `T`, the operand is
treated, for the purposes of this subclause, as a fresh value of type
`T`. The returned object is move-constructed from that object.

[Note: under [§6.4](02-ownership-and-move.md#64-move-construction-and-move-assignment-classcopyctor-classcopyassign),
every class type has an implicitly-defined move constructor. Therefore,
for an operand satisfying (2), this subclause always selects move
construction; there is no copy-constructor fallback. The special
treatment in (2) applies only to `return` operands and does not make
such an *id-expression* a fresh value for the purposes of §6.6.
— end note]

(3) Otherwise, the operand shall be a fresh value of type `T` as defined
by §6.6(1). The returned object is move-constructed from that fresh
value.

(4) If neither (2) nor (3) is satisfied, the program is ill-formed.

```cpp
struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(const MoveOnly&) = delete;
};

struct Box {
    int value;
};

MoveOnly make_move_only() {
    MoveOnly local{};
    return local;                 // OK: 6.7(2), move-constructs from local
}

MoveOnly pass_through(MoveOnly param) {
    return param;                 // OK: 6.7(2), move-constructs from param
}

std::string greet() {
    return std::string{"hello"};   // OK: 6.6(1.3), 6.7(3)
}

Box make_box() {
    return Box{42};                // OK: 6.6(1.3), 6.7(3)
}
```

---

[← Previous: The `[[scpp::unsafe]]` Attribute](01-unsafe.md) · [Table of Contents](README.md) · [Next: Dereference and Member Access →](03-dereference-and-member-access.md)
