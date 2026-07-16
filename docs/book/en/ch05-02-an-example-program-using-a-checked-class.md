# An Example Program Using a Checked Class

In the previous section, we learned the syntax for defining `struct` and
`class`. Now let's use a `class` in a small program so the motivation feels more
concrete.

Suppose we want to work with rectangles. At first, we might keep the width and
height as separate variables and write free functions that take separate
arguments.

For each runnable example below, save the file as `checked-class.scpp`, then
build and run it like this:

```sh
scpp checked-class.scpp -o checked-class
./checked-class
```

## Starting with separate values

This works, but the function signature has to repeat the relationship between
the two values every time.

```cpp
import std;

int area(int width, int height) {
    return width * height;
}

int main() {
    int width{30};
    int height{50};
    std::println("area = {}", area(width, height));
    return 0;
}
```

Output:

```text
area = 1500
```

There is nothing wrong with this program. The problem is that `width` and
`height` obviously belong together, but the type system does not know that yet.

## Refactoring the data into one `class`

When related values belong together, putting them into one type makes the
program easier to read and harder to mix up by accident.

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

int area(const Rectangle& rectangle) {
    return rectangle.width * rectangle.height;
}

int main() {
    Rectangle frame{"frame", 30, 50};
    std::println("{} area = {}", frame.name.c_str(), area(frame));
    return 0;
}
```

Output:

```text
frame area = 1500
```

Now the program can name one thing—`Rectangle`—instead of carrying around three
separate values that must stay in sync.

This example also shows why `class` is sometimes the natural choice in scpp:
`Rectangle` stores a `std::string` name, so it cannot be a `struct`.

## Free functions can borrow the whole object

Once the data is grouped into one value, helper functions can take one
parameter instead of several.

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

int area(const Rectangle& rectangle) {
    return rectangle.width * rectangle.height;
}

bool can_hold(const Rectangle& outer, const Rectangle& inner) {
    return outer.width > inner.width && outer.height > inner.height;
}

int main() {
    Rectangle frame{"frame", 30, 50};
    Rectangle card{"card", 10, 40};
    std::println("{} area = {}", frame.name.c_str(), area(frame));
    std::println("{} holds {} = {}", frame.name.c_str(), card.name.c_str(), can_hold(frame, card));
    return 0;
}
```

Output:

```text
frame area = 1500
frame holds card = true
```

Notice the signatures:

- `area(const Rectangle& rectangle)`
- `can_hold(const Rectangle& outer, const Rectangle& inner)`

Those `const Rectangle&` parameters are shared borrows, just like the reference
types from Chapter 4. The function can read the rectangle without taking
ownership of it.

## Free functions can also mutate through `T&`

If a helper function should update the object, it can take `Rectangle&`.

```cpp
import std;

class Rectangle {
public:
    std::string name;
    int width{};
    int height{};

    Rectangle(const char* initial_name, int initial_width, int initial_height)
        : name{initial_name}, width{initial_width}, height{initial_height} {
        return;
    }

    virtual ~Rectangle() {
        return;
    }
};

void rename(Rectangle& rectangle, const char* next_name) {
    rectangle.name = std::string{next_name};
    return;
}

int main() {
    Rectangle frame{"draft", 30, 50};
    rename(frame, "published");
    std::println("{}", frame.name.c_str());
    return 0;
}
```

Output:

```text
published
```

So a checked `class` already gives us a useful place to keep related data,
while ordinary functions describe the operations around that data.

## Preparing for methods

Right now, `area`, `can_hold`, and `rename` are all free functions. That is
fine, and sometimes it is exactly what you want.

But they are also all *about* `Rectangle`. The next section will take this same
kind of program one step further and move those operations onto the type itself
with methods and `this`.

---

[← Previous: Defining and Instantiating `struct` and `class`](ch05-01-defining-and-instantiating-struct-and-class.md) · [Table of Contents](README.md)
