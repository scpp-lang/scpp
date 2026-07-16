# Defining and Instantiating `struct` and `class`

A `struct` or `class` gives one name to a group of related fields. Instead of
passing several separate values around, you can define one type whose fields
belong together.

In scpp, `struct` and `class` are **not** interchangeable spellings:

- `struct` is the plain-data, non-polymorphic form. It may still have
  `public:` and `private:` sections, constructors, and ordinary non-virtual
  member functions.
- `class` is the form used for inheritance and polymorphism. It can also hold
  non-trivial class-typed fields such as `std::string`.

That split is deliberate. Choosing `struct` says “this type stays out of
inheritance, interfaces, and virtual dispatch.” Choosing `class` opts into that
world up front: every `class` must declare a `virtual` destructor, even if it
does nothing, so adding virtual functions or interface bases later does not
silently change the shape of the type, and scpp never leaves you with the
classic C++ mistake of forgetting a virtual destructor on a base class.

Later chapters will cover inheritance and interfaces in detail. For now, keep
one boundary line in mind: only `class` can participate in that system. A
`struct` cannot declare virtual members, cannot have a base-clause, and cannot
be marked `[[scpp::interface]]`; likewise, a type declared as `struct` cannot
be used as a base by some later `class`.

For each runnable example below, save the file as `records.scpp`, then build and
run it like this:

```sh
scpp records.scpp -o records
./records
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## Defining a basic `struct` with named fields

A field-only `struct` is the simplest way to group related data.

```cpp
import std;

struct User {
    int id{};
    const char* name{""};
};

int main() {
    User user{};
    user.id = 7;
    user.name = "Ada";
    std::println("{} {}", user.id, user.name);
    return 0;
}
```

Output:

```text
7 Ada
```

`User user{};` creates one `User` value with its fields default-initialized.
After that, the fields are read and written with ordinary dot syntax.

On current scpp, braces with arguments such as `User user{7, "Ada"};` do not
automatically fill public fields. If you want construction-time arguments,
define a constructor.

## A `struct` can still hide fields and define behavior

In scpp, you do **not** need `class` just to hide data or to define
constructors. A `struct` can still have `private:` sections, a default
constructor, parameterized constructors, and ordinary non-virtual member
functions.

```cpp
import std;

struct Size {
private:
    int width{};
    int height{};

public:
    Size() {
        return;
    }

    Size(int initial_width, int initial_height)
        : width{initial_width}, height{initial_height} {
        return;
    }

    void grow_width(int delta) {
        this->width = this->width + delta;
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int main() {
    Size empty{};
    Size window{3, 4};
    window.grow_width(1);
    std::println("{} {}", empty.area(), window.area());
    return 0;
}
```

Output:

```text
0 16
```

Here `Size` is still a `struct`, even though it hides its fields and defines
behavior around them. We will come back to method syntax in Section 5.3; for
now, the important point is that `struct` is still the ordinary tool for
non-virtual, non-inheriting data types.

## One-argument constructors can convert at call sites

A one-argument constructor can also be used as a converting constructor. That
means a function taking the type by value can accept the constructor argument
directly.

```cpp
import std;

struct Meters {
    int value{};

public:
    Meters(int initial_value) : value{initial_value} {
        return;
    }
};

int read(Meters meters) {
    return meters.value;
}

int main() {
    Meters direct{8};
    std::println("{} {}", read(5), direct.value);
    return 0;
}
```

Output:

```text
5 8
```

This is still ordinary construction. `read(5)` works because scpp constructs a
temporary `Meters` from `5` to satisfy the by-value parameter.

## A `struct`'s fields must stay plain data

A scpp `struct` is still restricted to plain-data field types. If one field
needs class behavior such as `std::string`, the enclosing type must be a
`class` instead.

```cpp
import std;

struct Bad {
    std::string name{"hi"};
};

int main() {
    Bad value{};
    return 0;
}
```

Compiler output:

```text
struct_string_field_fail.scpp: error: struct 'Bad' field 'name': a class type 'std::string' cannot be a struct field; use class instead (only scalars, pointers, trivial structs/unions, and fixed-size arrays of trivial types are allowed here; see spec ch04)
```

That restriction is another major difference from ordinary C++, where `struct`
and `class` usually differ only in default access.

## Defining and instantiating a `class`

At the use site, a `class` still looks familiar: you define fields, construct a
value with braces, and access public fields with dot syntax.

```cpp
import std;

class DisplayName {
public:
    std::string text;

    DisplayName(const char* initial_text) : text{initial_text} {
        return;
    }

    virtual ~DisplayName() {
        return;
    }
};

int main() {
    DisplayName name{"scpp"};
    std::println("{}", name.text.c_str());
    return 0;
}
```

Output:

```text
scpp
```

The visible syntax is simple, but the design choice is different from `struct`.
This type may hold `std::string`, and because it is a `class`, it is also in
the part of the language that later supports one ordinary base class plus any
number of interface bases.

## Every `class` must declare an explicit virtual destructor

If you omit that destructor, the program is ill-formed even if the class has no
other virtual members.

```cpp
class Account {
public:
    Account() {
        return;
    }
};

int main() {
    Account account{};
    return 0;
}
```

Compiler output:

```text
class_without_virtual_dtor_fail.scpp: error: class 'Account' must declare an explicit virtual destructor (spec §11.5(1))
```

So in scpp, choosing `class` is not just a style preference. It is the language
form reserved for inheritance and polymorphism, and the destructor requirement
is part of making that choice explicit and stable from the start.

## Default brace-initialization still needs a default constructor

`Type value{};` means “construct a value with zero constructor arguments.” If a
type declares only a parameterized constructor, that initialization is rejected
with a normal constructor-selection diagnostic.

```cpp
struct CtorOnly {
    int value;

public:
    CtorOnly(int x) : value{x} {
        return;
    }
};

int main() {
    CtorOnly value{};
    return 0;
}
```

Compiler output:

```text
struct_default_ctor_fail.scpp:11:5: error: type 'CtorOnly' has no default constructor; no constructor of 'CtorOnly' matches 0 arguments
 11 |     CtorOnly value{};
    |     ^
```

The same rule applies to `class`. If you want `Type value{};` to work, the type
must actually have a default constructor.

## A `struct` cannot declare virtual members

The opposite restriction also matters: a `struct` is never virtual.

```cpp
struct Plain {
    virtual void f() {
        return;
    }
};

int main() {
    Plain value{};
    return 0;
}
```

Compiler output:

```text
struct_virtual_member_fail.scpp:2:5: error: a declaration introduced by 'struct' shall not declare a virtual member function or virtual destructor (spec §11.1(2.3))
 2 |     virtual void f() {
   |     ^
```

If a type needs virtual behavior, it must be a `class`.

## A `struct` cannot inherit

Likewise, `struct` is not the inheritance form in scpp.

```cpp
class Base {
public:
    Base() {
        return;
    }

    virtual ~Base() {
        return;
    }
};

struct Derived : public Base {
    Derived() {
        return;
    }
};

int main() {
    Derived value{};
    return 0;
}
```

Compiler output:

```text
struct_inherit_fail.scpp:12:16: error: a declaration introduced by 'struct' shall not have a base-clause (spec §11.1(2.1))
 12 | struct Derived : public Base {
    |                ^
```

The same boundary applies the other way too: a later `class` cannot use a
`struct` as its base, and `struct` also cannot be marked
`[[scpp::interface]]`. If a type may ever participate in inheritance or
interfaces, define it as a `class` from the start.

## The rules of `struct` and `class`

So far, the working rules are:

- use a `struct` to group related data when the type should stay plain-data,
  non-virtual, and non-inheriting;
- a `struct` may still have `public:`/`private:` sections, constructors, and
  ordinary non-virtual member functions;
- one-argument constructors can be used as converting constructors at call
  sites;
- fields are accessed with dot syntax on both `struct` and `class` values;
- if you want `Type value{};`, the type must actually have a default
  constructor;
- a `struct` cannot hold ownership-tracked fields such as `std::string`, cannot
  declare virtual members, cannot have a base-clause, and cannot be an
  interface;
- every `class` must declare an explicit `virtual` destructor;
- only `class` participates in inheritance, virtual dispatch, and interface
  implementation in scpp.

The next section will build a small example program around a checked `class`.

---

[← Previous: `std::span` and Other Non-Owning Views](ch04-03-std-span-and-other-non-owning-views.md) · [Table of Contents](README.md)
