# 11 Inheritance and Interfaces

## 11.1 General [class.derived]

(1) Except as modified by this clause, [class.derived], [class.mi],
[class.virtual], [class.member.lookup], [namespace.udecl], and the
ordinary C++ rules for access control and derived-to-base conversion
apply unchanged to inheritance in an SCPP26 program.

(2) A class is an **interface** if and only if the declaration that
defines it is marked with the attribute-token `scpp::interface` in an
*attribute-specifier-seq* ([dcl.attr.grammar]) appertaining to that
class definition. A class whose definition is not so marked is an
**ordinary class**, even if it happens to declare no non-static data
members.

(3) A class definition is ill-formed if its direct base-specifier-list
contains more than one ordinary class. A class may, in addition to at
most one ordinary direct base class, have any number of direct base
classes that are interfaces.

(4) This clause adds multiple inheritance only through interfaces under
(2). It does not otherwise relax SCPP26's existing rule that ordinary
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

class TagOnly {
public:
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
};  // ill-formed: two ordinary direct base classes under (3)
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

(5) A program is ill-formed if it would form an object of interface
type in any object-forming context, including:

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
access control. — end note]

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

class Storage {};

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

(2) Rule (1) applies whether `D` is itself an interface or an ordinary
class.

(3) The access-specifier of an interface base is not otherwise
restricted by this document. `public`, `protected`, and `private`
interface inheritance retain their ordinary C++ meanings.

(4) If an interface base is inherited `public`, the ordinary C++
derived-to-base conversion to that interface type is available wherever
access control permits it. If an interface base is inherited
`protected` or `private`, the corresponding conversion is denied or
permitted exactly as the ordinary C++ access-control rules require.

(5) For each interface base `I` that is reachable from a most-derived
object through one or more inheritance paths, all of which are virtual
because of (1), the observable semantics shall match those of ordinary
C++ virtual inheritance for that same source: all valid conversions of
that most-derived object to `I` denote one shared `I` base identity,
and virtual dispatch through `I` selects the unique final overrider.

(6) SCPP26 need not realize the guarantee in (5) with the same ABI or
object layout mechanism used by any particular C++ compiler. It is
sufficient that the observable semantics named in (5) are preserved.
This permission applies only to SCPP26's implementation of the
required-virtual interface inheritance rules in this clause; it does
not alter the observable semantics required for other C++ constructs.

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

class SecretMover : private virtual IMovable {
public:
    ~SecretMover() override = default;
    void move_it() override {}
    IMovable& expose_inside() { return *this; }   // OK: conversion allowed here
};

void take_movable(IMovable&);

void demo(Duck& duck, SecretMover& secret) {
    take_movable(duck);      // OK: public interface inheritance
    // take_movable(secret); // ill-formed: private base conversion denied
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
    void f(int) {}
};

class [[scpp::interface]] IDoubleOps {
public:
    void f(double) {}
};

class CombinedOps : public virtual IIntOps, public virtual IDoubleOps {
public:
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

## 11.5 Polymorphic destruction and explicit overriding [class.dtor], [class.virtual]

(1) A class is **polymorphic**, for the purposes of this subclause, if
it declares any virtual member function or inherits any virtual member
function.

(2) A polymorphic class shall declare a destructor explicitly, and that
destructor shall be virtual. A complete class definition that violates
this rule is ill-formed.

(3) Rule (2) applies uniformly to interfaces and ordinary classes
alike, including a class that merely inherits a virtual member function
and declares no new virtual member function of its own.

(4) SCPP26 does not implicitly synthesize, promote, or reinterpret a
destructor as virtual merely because the class is polymorphic. If the
programmer does not declare the virtual destructor explicitly, the
program is ill-formed.

(5) If a member function declaration or destructor declaration
overrides a virtual member function or destructor of any base class,
the declaration shall include the `override` virt-specifier. A program
is ill-formed if such an overriding declaration omits `override`.

(6) If a declaration includes the `override` virt-specifier but does
not in fact override any base virtual member function or destructor,
the program is ill-formed exactly as in ordinary C++.

(7) Rule (5) applies to destructors with no exception. A destructor of
a derived class that overrides a virtual base destructor shall be
declared, for example, as `~D() override = default;` or
`~D() override { ... }`.

(8) A `using`-declaration is not an overriding declaration and neither
satisfies nor violates (5) by itself.

[Note: an explicit destructor required by (2) is a user-declared
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

class MissingDtor : public Base {
public:
    void run() override {}
};  // ill-formed: polymorphic class lacks an explicit virtual destructor

class MissingOverride : public Base {
public:
    virtual ~MissingOverride() = default;   // ill-formed: overrides `Base::~Base` but omits `override`
    void run() {}                           // ill-formed: overrides `Base::run` but omits `override`
};
```

---

[← Previous: Iteration statements](10-iteration-statements.md) · [Table of Contents](README.md)
