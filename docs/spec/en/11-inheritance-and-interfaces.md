# 11 Inheritance and Interfaces

## 11.1 General [class.derived]

(1) Except as modified by this clause, [class.derived], [class.mi],
[class.virtual], [class.member.lookup], [namespace.udecl], and the
ordinary C++ rules for access control and derived-to-base conversion
apply unchanged to inheritance in an SCPP26 program.

(2) A declaration introduced by the keyword `struct` shall not:

  (2.1) have a *base-clause* ([class.derived]);

  (2.2) be marked with the attribute-token `scpp::interface`; or

  (2.3) declare a virtual member function or virtual destructor.

(3) A *base-specifier* is ill-formed if it names a type declared with
the keyword `struct`.

(4) Rules (2) and (3) do not otherwise restrict a `struct`. A `struct`
may declare constructors, access-specifiers, non-static data members,
and non-virtual member functions exactly as the ordinary C++ rules
permit.

(5) A declaration introduced by the keyword `class` is an **interface**
if and only if the declaration that defines it is marked with the
attribute-token `scpp::interface` in an *attribute-specifier-seq*
([dcl.attr.grammar]) appertaining to that class definition. A
declaration introduced by the keyword `class` whose definition is not so
marked is an **ordinary class**, even if it happens to declare no
non-static data members.

(6) A class definition is ill-formed if its direct base-specifier-list
contains more than one ordinary class. A class may, in addition to at
most one ordinary direct base class, have any number of direct base
classes that are interfaces.

(7) This clause adds multiple inheritance only through interfaces under
(5). It does not otherwise relax SCPP26's existing rule that ordinary
implementation inheritance is single inheritance.

[Note: as a style convention, SCPP26 source code is encouraged to name
classes marked `[[scpp::interface]]` with a leading `I`, for example
`IReader` or `IMovable`. This is a non-normative recommendation only:
no program is ill-formed merely because an interface name does not
follow that convention. — end note]

```cpp
class [[scpp::interface]] IReader {
public:
    virtual ~IReader() = default;
    virtual void read() = 0;
};

struct PlainData {
private:
    int value{};
public:
    PlainData(int v) : value{v} {}
    int read() const { return value; }
};

class TagOnly {
public:
    virtual ~TagOnly() = default;
    void ping();
};

class FileReader : public virtual IReader {
public:
    ~FileReader() override = default;
    void read() override {}
};

class Bad : public FileReader, public TagOnly {
public:
    ~Bad() override = default;
};  // ill-formed: two ordinary direct base classes under (6)

struct BadStruct : public TagOnly {};  // ill-formed: a struct shall not inherit
```

## 11.2 Interface declarations [dcl.attr.scpp.interface]

(1) An interface shall declare no non-static data member. A class
definition marked `[[scpp::interface]]` is ill-formed if it declares a
non-static data member of any type.

(2) Rule (1) does not prohibit class-scope declarations that introduce
no per-object state, such as type aliases, enumerations, static data
members, static member functions, or other declarations that are not
non-static data members.

(3) Every direct base class of an interface shall itself be an
interface. An interface is ill-formed if any direct or transitive base
class of that interface is an ordinary class.

(4) An interface may declare virtual member functions either with a
function-body or with a pure-specifier. It may also declare non-virtual
member functions. A non-virtual member function declared in an
interface is not part of the dynamic dispatch contract of that
interface; it is called exactly as an ordinary non-virtual member
function.

[Note: this clause adds no special rule for constructors of an
interface. An interface may declare constructors exactly as an ordinary
class may, and the ordinary C++ rules for base-class and virtual-base
initialization apply unchanged. — end note]

(5) A program is ill-formed if it would form a complete object whose
most-derived type is an interface in any object-forming context,
including:

  (5.1) a variable definition by value;

  (5.2) a non-static data member declaration;

  (5.3) an array element type;

  (5.4) a `new`-expression;

  (5.5) a temporary object;

  (5.6) a function parameter of by-value type; or

  (5.7) a function return type by value.

(6) Rule (5) applies whether or not the interface has any pure virtual
member functions. An interface with only default virtual
implementations is still not directly instantiable.

[Note: rule (5) prevents object slicing of interface-implementing
objects into standalone interface objects. Passing or returning an
interface by reference or pointer remains well-formed, subject to the
ordinary C++ rules for reference binding, pointer conversion, and
access control. An interface base subobject within a larger most-derived
object is not, by itself, a complete object under (5). — end note]

[Note: copying or moving a most-derived object that contains interface
base subobjects is governed by [§6.4](02-ownership-and-move.md#64-move-construction-and-move-assignment-classcopyctor-classcopyassign)
and [§6.5](02-ownership-and-move.md#65-copy-construction-and-copy-assignment-classcopyctor-classcopyassign),
including those subclauses' treatment of base-class subobjects. — end note]

```cpp
class [[scpp::interface]] ILogger {
    static constexpr int version = 1;
public:
    virtual ~ILogger() = default;
    virtual void log() {
        helper();
    }
    void helper() {}
};

class [[scpp::interface]] IBadState {
    int counter{};
public:
    virtual ~IBadState() = default;
};  // ill-formed: non-static data member under (1)

class Storage {
public:
    virtual ~Storage() = default;
};

class [[scpp::interface]] IBadBase : public virtual Storage {
public:
    virtual ~IBadBase() = default;
};  // ill-formed: interface inheriting an ordinary class under (3)

void consume(ILogger& ref);   // OK
void copy(ILogger value);     // ill-formed: (5.6)
ILogger make_logger();        // ill-formed: (5.7)
```

## 11.3 Base-specifiers and interface identity [class.mi]

(1) If a class `D` directly inherits from an interface `I`, the
base-specifier naming `I` shall include the `virtual` keyword. A direct
interface base specified without `virtual` is ill-formed.

(2) If a class `D` directly inherits from an ordinary class `B`, the
base-specifier naming `B` shall not include the `virtual` keyword. A
direct ordinary-class base specified with `virtual` is ill-formed.

(3) Rule (1) applies whether `D` is itself an interface or an ordinary
class.

[Note: rule (2) removes no useful expressiveness in SCPP26. By
[§11.1](11-inheritance-and-interfaces.md#111-general-classderived),
a class has at most one ordinary direct base class, and by
[§11.2](11-inheritance-and-interfaces.md#112-interface-declarations-dclattrscppinterface)
(3), an interface may inherit only other interfaces. The ordinary-base
relationship therefore cannot branch into, or reconverge from, multiple
paths, so the duplicate-subobject problem that ordinary C++ virtual
inheritance solves cannot arise for an ordinary base in SCPP26. — end
note]

(4) An interface base may be inherited only with the access-specifier
`public` or `private`.

(5) If an interface base is inherited `public`, the derived-to-base
conversion to that interface type is available to ordinary external code
as well as within the deriving class, subject to any other access rule
of the program. If an interface base is inherited `private`, that
conversion is available only within the deriving class's own member
functions; for this rule, a member function of a nested class is not a
member function of the deriving class. A program is ill-formed if
arbitrary external code attempts the corresponding conversion.

(6) For each interface base `I` that is reachable from a most-derived
object through one or more inheritance paths, all of which are virtual
because of (1), the observable semantics shall match those of ordinary
C++ virtual inheritance for that same source: all valid conversions of
that most-derived object to `I` denote one shared `I` base identity,
and virtual dispatch through `I` selects the unique final overrider.

(7) A pointer or reference to a non-interface type is an *ordinary
representation*. It occupies one machine word and denotes only the
address of the referenced object. If a complete non-interface class
type `D` directly or transitively implements one or more interfaces,
those interface implementations contribute no additional per-object
storage to `D`; in particular, `sizeof(D)` is unchanged by adding or
removing interface bases while keeping `D`'s ordinary base class and
non-static data members otherwise the same.

(8) A pointer or reference to an interface type is an *interface
representation*. It occupies exactly two machine words, and therefore
exactly twice the size of the representation required by (7) on the
same target. One word denotes the address of the underlying
most-derived object. The other denotes dispatch information for the
referenced interface, sufficient to dispatch each virtual member
function declared by that interface for the concrete object currently
referenced.

(9) The dispatch information named in (8) shall be resolved when the
interface-typed pointer or reference value is formed. Thereafter, a
call through that value to a virtual member function declared by the
referenced interface shall use that carried dispatch information
directly and shall not require a search over the object's implemented
interfaces at the call site.

(10) Only pointer-to-interface types have null values. A
pointer-to-interface value produced by `nullptr`, zero-initialization,
or default-initialization of a pointer-typed member or variable is a
null interface pointer. In a null interface pointer, the object-address
word is zero. The value of the dispatch-information word is
unspecified, and the program's semantics shall not depend on that
word while the object-address word is zero.

(11) A nullness test on a pointer-to-interface value, including
comparison against `nullptr` and contextual conversion to `bool`,
depends only on whether the object-address word named in (10) is zero.
The dispatch-information word plays no part in such a test. In
particular, two null interface pointers remain null regardless of
whether their dispatch-information words are equal.

(12) No conversion, implicit or explicit, is provided from an
interface-typed pointer or reference value to any scalar type whose
representation is one machine word. This includes `void*`, any other
raw pointer type whose representation is one machine word, and integer
scalar types such as `uintptr_t` or `intptr_t` when those types are
one machine word on the target. A program that attempts such a
conversion is ill-formed.

[Note: When a program must pass an interface value through an API that
accepts only `void*`, `uintptr_t`, or another single-word scalar, it
can first store that interface value in an object with stable storage
and pass a pointer to that storage, or another application-defined
handle that preserves the needed information, instead. Any such pointer
conversion remains subject to
[§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe). — end note]

(13) SCPP26 need not realize the guarantees in (6), (8), (9), (10),
and (11) with the same ABI, word ordering, object layout, or
dispatch-table structure used by any particular C++ compiler or any
other implementation technique. The order of the two machine words
within an interface representation is unspecified, as is the internal
structure of the dispatch information named in (8). It is sufficient
that the observable semantics named in those paragraphs are preserved:
one shared interface identity under (6), one-machine-word ordinary
representations under (7), two-machine-word interface representations
under (8), correct dispatch without call-site search under (9), and
null interface-pointer semantics determined solely by the
object-address word under (10) and (11). This permission applies only
to SCPP26's implementation of the required-virtual interface
inheritance rules and interface-typed representations in this clause;
it does not alter the observable semantics required for other C++
constructs.

[Note: Consequently, an owning pointer specialization such as
`unique_ptr<I>`, where `I` is an interface, may need to store both
words of the interface representation so that ownership transfer
preserves the full interface value. The exact library mechanism is
outside this clause. — end note]

```cpp
class [[scpp::interface]] IMovable {
public:
    virtual ~IMovable() = default;
    virtual void move_it() = 0;
};

class [[scpp::interface]] IFlyable : public virtual IMovable {
public:
    ~IFlyable() override = default;
};

class [[scpp::interface]] ISwimmable : public virtual IMovable {
public:
    ~ISwimmable() override = default;
};

class Duck : public virtual IFlyable, public virtual ISwimmable {
public:
    ~Duck() override = default;
    void move_it() override {}
};

class BadDuck : public IFlyable {
public:
    ~BadDuck() override = default;
};  // ill-formed: direct interface base lacks `virtual`

class OrdinaryBase {
public:
    virtual ~OrdinaryBase() = default;
};

class BadVirtualOrdinary : public virtual OrdinaryBase {
public:
    ~BadVirtualOrdinary() override = default;
};  // ill-formed: direct ordinary-class base uses `virtual`

class SecretMover : private virtual IMovable {
public:
    ~SecretMover() override = default;
    void move_it() override {}
    IMovable& expose_inside() { return *this; }   // OK: conversion allowed here
};

void take_movable(IMovable&);

void take_userdata(void*);

struct CallbackState {
    IMovable* value;
};

void demo(Duck& duck, SecretMover& secret, CallbackState& state) {
    take_movable(duck);      // OK: public interface inheritance
    // take_movable(secret); // ill-formed: private base conversion denied

    IMovable* p = nullptr;
    if (p) {
        p->move_it();
    }

    state.value = &duck;
    // take_userdata(state.value); // ill-formed: interface pointer is not `void*`
    // auto bits = uintptr_t(state.value); // ill-formed: not a single-word scalar conversion
    take_userdata(&state);         // OK: pass pointer to stable storage instead
}
```

## 11.4 Interface members, lookup, and virtual dispatch [class.member.lookup], [namespace.udecl], [class.virtual]

(1) Unqualified member lookup in a derived class follows ordinary C++
rules. If two or more base classes make a member with the same name
reachable, and the derived class introduces no declaration that resolves
that lookup, the name is ambiguous exactly as in C++.

(2) Under (1), ambiguity is not silently resolved merely because one
candidate is declared in an ordinary base class and another in an
interface, or merely because one candidate is virtual and another is
not.

(3) Consequently, each of the following is ambiguous unless the program
resolves it explicitly by ordinary C++ means such as qualification,
declaration of a new overriding member where applicable, or a
`using`-declaration that introduces the intended base declaration into
the derived class:

  (3.1) two sibling interfaces each providing a default implementation
  of the same-signature member function;

  (3.2) an ordinary base class member function and an interface member
  function having the same name;

  (3.3) two unrelated base classes declaring overloads with the same
  name but different signatures.

(4) In the case described by (3.3), overload resolution does not begin
until name lookup has first been made unambiguous. A declaration of the
form `using B::f;` in the derived class introduces the selected base
declaration or declarations into the derived class's scope exactly as in
C++, after which ordinary overload resolution applies to the resulting
overload set.

(5) If two or more intermediate base classes each override the same
virtual function of a shared virtual base, and a most-derived class
inherits those intermediate classes, the most-derived class shall
declare its own overriding function to serve as the unique final
overrider. A qualification or `using`-declaration does not satisfy this
requirement.

(6) One declaration in the most-derived class may satisfy (5) for more
than one overridden base virtual function if that declaration genuinely
overrides each of them.

```cpp
class [[scpp::interface]] IPrintable {
public:
    virtual ~IPrintable() = default;
    virtual void print() {
        helper();
    }
    void helper() {}
};

class [[scpp::interface]] IDebuggable {
public:
    virtual ~IDebuggable() = default;
    virtual void print() {}
};

class Tool : public virtual IPrintable, public virtual IDebuggable {
public:
    ~Tool() override = default;
    // void print() override {}   // one valid explicit resolution
};

class Worker {
public:
    virtual ~Worker() = default;
    void start() {}
};

class [[scpp::interface]] IStartable {
public:
    virtual ~IStartable() = default;
    virtual void start() {}
};

class Machine : public Worker, public virtual IStartable {
public:
    ~Machine() override = default;
    // Machine m; m.start();      // ambiguous under (3.2)
};

class [[scpp::interface]] IIntOps {
public:
    virtual ~IIntOps() = default;
    void f(int) {}
};

class [[scpp::interface]] IDoubleOps {
public:
    virtual ~IDoubleOps() = default;
    void f(double) {}
};

class CombinedOps : public virtual IIntOps, public virtual IDoubleOps {
public:
    ~CombinedOps() override = default;
    using IIntOps::f;
    using IDoubleOps::f;
};

class [[scpp::interface]] ITick {
public:
    virtual ~ITick() = default;
    virtual void tick() = 0;
};

class [[scpp::interface]] ILeft : public virtual ITick {
public:
    ~ILeft() override = default;
    void tick() override {}
};

class [[scpp::interface]] IRight : public virtual ITick {
public:
    ~IRight() override = default;
    void tick() override {}
};

class Both : public virtual ILeft, public virtual IRight {
public:
    ~Both() override = default;
    void tick() override {}
};
```

## 11.5 Virtual destruction and explicit overriding [class.dtor], [class.virtual]

(1) Every class shall declare a destructor explicitly, and that
destructor shall be virtual. A complete class definition that violates
this rule is ill-formed.

(2) Rule (1) applies whether or not the class declares or inherits any
other virtual member function, whether or not it implements any
interface, and whether or not it is immediately used as a base class.

(3) SCPP26 does not implicitly synthesize, promote, or reinterpret a
destructor as virtual. If the programmer does not declare a virtual
destructor explicitly, the program is ill-formed.

(4) If a member function declaration or destructor declaration
overrides a virtual member function or destructor of any base class,
the declaration shall include the `override` virt-specifier. A program
is ill-formed if such an overriding declaration omits `override`.

(5) If a declaration includes the `override` virt-specifier but does
not in fact override any base virtual member function or destructor,
the program is ill-formed exactly as in ordinary C++.

(6) Rule (4) applies to destructors with no exception. A destructor of
a derived class that overrides a virtual base destructor shall be
declared, for example, as `~D() override = default;` or
`~D() override { ... }`.

(7) A `using`-declaration is not an overriding declaration and neither
satisfies nor violates (4) by itself.

[Note: by requiring (1) for every class, SCPP26 eliminates the latent
defect of a class later being used as a base without a virtual
destructor. A `struct` under [§11.1](11-inheritance-and-interfaces.md#111-general-classderived)
is instead the construct that never participates in inheritance or
virtual dispatch; it may still encapsulate data and behavior, but it
adds no hidden virtual-dispatch state. A class that declares virtual
member functions beyond the mandatory destructor is therefore making a
deliberate inheritance-related design choice. As a side effect, adding
further virtual member functions or interface bases later does not
newly introduce such state into a class that already satisfies (1). —
end note]

[Note: an explicit destructor required by (1) is a user-declared
destructor. The consequences that SCPP26 already assigns to a
user-declared destructor for implicit copy construction and copy
assignment therefore apply to interfaces exactly as they apply to any
other class; see [§6.5](02-ownership-and-move.md). This clause
introduces no exception for interfaces. — end note]

```cpp
class Base {
public:
    virtual void run() {}
    virtual ~Base() = default;
};

class Derived : public Base {
public:
    ~Derived() override = default;
    void run() override {}
};

class MissingDtor {
public:
    void ping() {}
};  // ill-formed: every class needs an explicit virtual destructor

class MissingOverride : public Base {
public:
    virtual ~MissingOverride() = default;   // ill-formed: overrides `Base::~Base` but omits `override`
    void run() {}                           // ill-formed: overrides `Base::run` but omits `override`
};

struct Packet {
private:
    int value{};
public:
    Packet(int v) : value{v} {}
    int read() const { return value; }
};

struct BadStructVirtual {
    virtual ~BadStructVirtual() = default;
};  // ill-formed: a struct shall not declare virtual members
```

---

[← Previous: Iteration statements](10-iteration-statements.md) · [Table of Contents](README.md) · [Next: Modules and Namespaces →](12-modules-and-namespaces.md)
