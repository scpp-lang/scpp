# Methods and `this`

In the previous section, we grouped related data into a checked `class` and
used free functions such as `area(rectangle)` and `can_hold(outer, inner)`.

Those functions worked well, but they were also obviously *about* one type.
Methods let us move that behavior onto the type itself.

For each runnable example below, save the file as `methods.scpp`, then build and
run it like this:

```sh
scpp methods.scpp -o methods
./methods
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## Defining a method

A method is a function written inside a `struct` or `class` definition. The
receiver object is the value before the dot.

```cpp
import std;

class Rectangle {
private:
    std::string name;
    int width{};
    int height{};

public:
    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    int area() const {
        return this->width * this->height;
    }

    bool can_hold(const Rectangle& other) const {
        return this->width > other.width && this->height > other.height;
    }

    const char* label() const {
        return this->name.c_str();
    }
};

int main() {
    Rectangle frame{"frame", 30, 50};
    Rectangle card{"card", 10, 40};
    std::println("{} area = {}", frame.label(), frame.area());
    std::println("{} holds {} = {}", frame.label(), card.label(), frame.can_hold(card));
    return 0;
}
```

Output:

```text
frame area = 1500
frame holds card = true
```

Instead of writing `area(frame)`, we now write `frame.area()`. That style makes
it clearer that the operation belongs to `Rectangle`.

## `this` refers to the receiver

Inside a method, `this` means “the object this method was called on.” You will
often write `this->field` to make that explicit, especially when a parameter
name would otherwise collide with a field name.

```cpp
import std;

class Rectangle {
private:
    int width{};
    int height{};

public:
    Rectangle(int width, int height) : width{width}, height{height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    void resize(int width, int height) {
        this->width = width;
        this->height = height;
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int main() {
    Rectangle rect{2, 3};
    rect.resize(4, 5);
    std::println("{}", rect.area());
    return 0;
}
```

Output:

```text
20
```

Here the parameters are also named `width` and `height`, so `this->width` means
“the field on the receiver,” not the local parameter.

## `const` methods only read the receiver

When a method should only observe the object, mark it `const`. Then the method
can be called through a shared borrow such as `const Rectangle&`.

```cpp
import std;

class Rectangle {
private:
    int width{};
    int height{};

public:
    Rectangle(int width, int height) : width{width}, height{height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }

    int area() const {
        return this->width * this->height;
    }
};

int describe(const Rectangle& rectangle) {
    return rectangle.area();
}

int main() {
    Rectangle rect{6, 7};
    std::println("{}", describe(rect));
    return 0;
}
```

Output:

```text
42
```

This is the same borrowing story from Chapter 4. A `const` method works with a
shared, read-only view of the receiver.

## Non-`const` methods need a mutable receiver

If a method might update the object, leave off `const`. Then it cannot be
called through a `const` reference.

```cpp
class Counter {
private:
    int value{};

public:
    Counter(int start) : value{start} {
        return;
    }

    virtual ~Counter() {
        return;
    }

    void increment() {
        this->value = this->value + 1;
        return;
    }
};

void tick(const Counter& counter) {
    counter.increment();
    return;
}

int main() {
    Counter counter{5};
    tick(counter);
    return 0;
}
```

Compiler output:

```text
nonconst_method_on_const_ref_fail.scpp:21:5: error: cannot call non-const member function 'increment' through a read-only (const) receiver
 21 |     counter.increment();
    |     ^
```

So the `const` on a method is not decoration. It changes how the receiver is
borrowed and what calls are allowed.

## Calling a method borrows the receiver

scpp checks a method call the same way it checks any other borrow. A mutating
method needs mutable access to the whole receiver object.

```cpp
class Counter {
public:
    int value{};

    Counter(int start) : value{start} {
        return;
    }

    virtual ~Counter() {
        return;
    }

    void increment() {
        this->value = this->value + 1;
        return;
    }
};

int main() {
    Counter counter{5};
    int& value_ref = counter.value;
    counter.increment();
    return value_ref;
}
```

Compiler output:

```text
public_field_borrow_conflict.scpp:22:5: error: cannot use 'counter' while it is mutably borrowed
 22 |     counter.increment();
    |     ^
```

The borrow of `counter.value` is still live at the final `return`, so
`counter.increment()` cannot also take mutable access to the same receiver.

The next section will keep using methods, but focus more directly on ownership
boundaries with `[[scpp::unsafe]]`.

---

[← Previous: An Example Program Using a Checked Class](ch05-02-an-example-program-using-a-checked-class.md) · [Table of Contents](README.md)
