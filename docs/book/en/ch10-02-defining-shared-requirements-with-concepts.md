# Defining Shared Requirements with Concepts

The previous section showed that a generic definition can be instantiated for
many concrete types. The next question is: how does an API say *which kinds of
types* it expects?

In scpp today, that job belongs to `concept`s. A concept gives a reusable name
to a set of operations, and a generic API can require callers to supply a type
that actually supports them.

For each runnable example below, save the file as `concepts.scpp`, then build
and run it like this:

```sh
scpp concepts.scpp -o concepts
./concepts
```

## A concept names a structural requirement

A type satisfies a concept because it has the required operations. There is no
separate “register this type as implementing the concept” declaration.

```cpp
import std;

class Circle {
public:
    virtual ~Circle() { return; }

    int area() const {
        return 314;
    }
};

class Square {
public:
    virtual ~Square() { return; }

    int area() const {
        return 100;
    }
};

template<typename T>
concept Shape = requires(const T& t) {
    { t.area() } -> std::same_as<int>;
};

int area_value(const Shape auto& shape) {
    return shape.area();
}

int main() {
    Circle circle{};
    Square square{};
    int circle_area = area_value(circle);
    int square_area = area_value(square);
    std::println("{}", circle_area);
    std::println("{}", square_area);
    return 0;
}
```

Output:

```text
314
100
```

The concept says exactly what the function body needs: given `const T&`, calling
`area()` must be valid and must produce an `int`. Both `Circle` and `Square`
satisfy that requirement structurally, so both calls compile.

## The full header form is useful when the same constrained type appears more than once

The abbreviated `Concept auto` form is convenient for one parameter. When the
same type parameter must appear in several positions, the full template header
is clearer.

```cpp
import std;

class Meter {
public:
    virtual ~Meter() { return; }

private:
    int value_{};

public:
    Meter(int value) : value_{value} {
        return;
    }

    int get() const {
        return this->value_;
    }
};

template<typename T>
concept HasGet = requires(const T& t) {
    { t.get() } -> std::same_as<int>;
};

template<HasGet T>
int sum_pair(const T& left, const T& right) {
    return left.get() + right.get();
}

int main() {
    Meter first{19};
    Meter second{23};
    int total = sum_pair(first, second);
    std::println("{}", total);
    return 0;
}
```

Output:

```text
42
```

Here the concept still describes the shared requirement, but the function needs
one named `T` because both parameters must have the same concrete type.

## A generic class can leave some operations unconstrained and gate others with `requires`

Sometimes a generic class should accept many types, but only one method needs an
extra capability. In that case, put the requirement on the method itself.

```cpp
import std;

template<typename T>
concept Describable = requires(const T& t) {
    { t.magnitude() } -> std::same_as<int>;
};

class Circle {
public:
    virtual ~Circle() { return; }

private:
    int radius_{};

public:
    Circle(int radius) : radius_{radius} {
        return;
    }

    Circle(const Circle& other) : radius_{other.radius_} {
        return;
    }

    int magnitude() const {
        return this->radius_;
    }
};

template<typename T>
class Box {
public:
    virtual ~Box() { return; }

private:
    T item_;

public:
    Box(const T& item) : item_{item} {
        return;
    }

    const T& get() const {
        return this->item_;
    }

    int describe() const requires Describable<T> {
        return this->item_.magnitude();
    }
};

int main() {
    Box<int> number{5};
    Box<Circle> circle{Circle{9}};
    int number_value = number.get();
    int circle_value = circle.describe();
    std::println("{}", number_value);
    std::println("{}", circle_value);
    return 0;
}
```

Output:

```text
5
9
```

`Box<T>` itself stays broadly usable. `get()` works for any `T`, while
`describe()` is only available when that instantiated `T` satisfies
`Describable`.

## The working rules for concepts in today's scpp

So far, the practical rules are:

- define a concept with `template<typename T> concept Name = requires(...) { ... };`;
- a type satisfies that concept structurally, by supporting the required
  operations;
- use `Concept auto` when one constrained parameter is enough;
- use the full header form when you need the same constrained type parameter in
  several places;
- use `requires Concept<T>` on one method when only part of a generic class
  needs the extra capability.

That gives Chapter 10 its next piece of vocabulary: generic APIs can now state
not just “some type goes here,” but “some type with these operations goes here.”
The next section adds lifetimes, which let those shared requirements talk about
references safely too.

---

[← Previous: Generic Functions and Classes](ch10-01-generic-functions-and-classes.md) · [Table of Contents](README.md) · [Next: Validating References with Lifetimes →](ch10-03-validating-references-with-lifetimes.md)
