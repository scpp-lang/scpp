# What Is Ownership?

Ownership is the rule system that lets scpp clean up resources automatically without a garbage collector and without everyday manual `delete` calls.

That matters most once a value owns something outside the simplest scalar case: heap memory, a file handle, a socket, or any other resource whose cleanup has to happen exactly once.

For each short example below, save the file as `ownership.scpp`, then build and run it like this:

```sh
scpp ownership.scpp -o ownership
./ownership
```

## The stack and the heap

A small scalar such as `int` fits directly inside the local variable that stores it. A value like `std::string` is different: the local object is still small, but the text it manages lives elsewhere.

`std::string` manages text stored on the heap, which lets it hold text whose size can grow and does not need to be known at compile time.

This is the main reason ownership exists: somebody has to know when that heap allocation should be released.

A practical way to remember the model is:

- one live owning object is responsible for each owned resource;
- `std::move(x)` transfers that responsibility out of `x` and leaves `x` moved-out;
- when the current owner leaves scope, cleanup runs automatically.

## Scope is where cleanup happens

The most basic ownership idea is scope. A local object is valid from its declaration until the end of the block that contains it.

```cpp
import std;

class Note {
private:
    const char* name{};

public:
    Note(const char* text) : name{text} {
        std::println("start {}", this->name);
        return;
    }

    ~Note() {
        std::println("drop {}", this->name);
        return;
    }
};

int main() {
    std::println("before inner");
    {
        Note inner{"inner"};
        std::println("inside inner");
    }
    std::println("after inner");
    return 0;
}
```

Output:

```text
before inner
start inner
inside inner
drop inner
after inner
```

`inner` is created when execution reaches its declaration, and its destructor runs automatically when the inner block ends. That is scpp's ordinary RAII story: scope decides when cleanup happens.

## `std::string` owns heap data

`std::string` is a good first ownership example because its size can change at runtime.

```cpp
import std;

int main() {
    std::string title{"scpp"};
    title.append(" book");

    std::println("{} ({} bytes)", title.c_str(), title.length());
    return 0;
}
```

Output:

```text
scpp book (9 bytes)
```

The local variable `title` owns that `std::string` value. Because the string manages heap-allocated text, `title`'s destructor releases that memory when `title` leaves scope.

## Moving transfers ownership

When an owning value should become someone else's responsibility, use `std::move`.

```cpp
import std;

int main() {
    std::string first{"owner"};
    std::string second{std::move(first)};
    second.append("ship");

    std::println("{}", second.c_str());
    return 0;
}
```

Output:

```text
ownership
```

In scpp, `std::move(first)` is not just a library helper. The language treats that syntax as the operation that puts `first` into the moved-out state immediately. After that point, `first` is no longer usable, and `second` is the only live owner of that string object.

This is how scpp avoids double-destruction: once ownership has moved away, the old owner is not used again and is not destroyed as an initialized object at scope exit.

## Copying is separate from moving

Some values are copied instead of moved. Plain scalar values such as `int`, `bool`, `char`, and `double` behave this way.

```cpp
import std;

int main() {
    int x{5};
    int y{x};
    y = y + 1;

    std::println("x = {}", x);
    std::println("y = {}", y);
    return 0;
}
```

Output:

```text
x = 5
y = 6
```

Changing `y` does not affect `x` because the value was copied.

For class types, copying is not automatic. A class is copyable only if it defines copy behavior. `std::string` now supports deep-copying, so ordinary copy construction and copy assignment both make a new owning string value.

Brace-init such as `std::string second{first};` uses the copy constructor, and `third = first;` uses copy assignment:

```cpp
import std;

int main() {
    std::string first{"book"};
    std::string second{first};
    std::string third{"draft"};
    third = first;
    second.append(" chapter");
    third.append(" notes");

    std::println("first = {}", first.c_str());
    std::println("second = {}", second.c_str());
    std::println("third = {}", third.c_str());
    return 0;
}
```

Output:

```text
first = book
second = book chapter
third = book notes
```

`second` and `third` each own their own string values. Changing either copy does not affect `first`.

## Ownership and functions

Passing and returning by value follow the same ownership rules.

A class value can hand ownership back to the caller by returning by value. When the function returns `std::move(word)`, ownership is transferred out of the local and into the return value:

```cpp
import std;

std::string make_word() {
    std::string word{"hello"};
    return std::move(word);
}

int main() {
    std::string local{make_word()};
    std::println("{}", local.c_str());
    return 0;
}
```

Output:

```text
hello
```

`make_word()` returns an owning `std::string`, and `local` becomes the new owner in the caller.

The same thing happens when a class value is passed by value with `std::move`: the callee takes ownership of that argument.

```cpp
import std;

void print_word(std::string text) {
    std::println("{}", text.c_str());
    return;
}

int main() {
    std::string word{"hello"};
    print_word(std::move(word));
    return 0;
}
```

Output:

```text
hello
```

After `std::move(word)`, the parameter `text` is the live owner inside `print_word`.

If a class type *does* have copy behavior, passing an lvalue by value can create a new owning object by copying:


```cpp
import std;

class Label {
private:
    const char* text{};

public:
    Label(const char* value) : text{value} {
        return;
    }

    Label(const Label& other) : text{other.text} {
        std::println("copy {}", this->text);
        return;
    }

    const char* c_str() const {
        return this->text;
    }
};

Label echo_label(Label label) {
    return label;
}

int main() {
    Label first{"ticket"};
    Label second{echo_label(first)};
    std::println("{}", second.c_str());
    return 0;
}
```

Output:

```text
copy ticket
ticket
```

This run prints one `copy ticket`: passing `first` by value copies it into the parameter object. The return path can then move or reuse that local, but the important point is that copyable types let function boundaries create a new owner when the program copies.

That is enough for a first ownership model:

- scope ends an owner's lifetime;
- `std::move` transfers ownership and invalidates the old owner;
- plain scalars copy cheaply, while class types copy only when they actually define copy behavior;
- function boundaries either move ownership or create a copied owner, depending on the type.

The next section keeps the same ownership rules, but adds a new question: how can code use a value temporarily without taking ownership of it?

---

[← Previous: Control Flow](ch03-05-control-flow.md) · [Table of Contents](README.md) · [Next: References and Borrowing →](ch04-02-references-and-borrowing.md)
