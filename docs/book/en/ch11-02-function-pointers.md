# Function Pointers

The previous section used closures as small local callable objects. Another
common callable form is older and simpler: a pointer to a named function.

In scpp today, function pointers reuse ordinary C and C++ syntax. That means
you can pass them around, store them in objects, and choose an overload by the
target pointer type.

For each runnable example below, save the file as `function-pointers.scpp`, then
build and run it like this:

```sh
scpp function-pointers.scpp -o function-pointers
./function-pointers
```

## A function name can initialize a matching function pointer

An ordinary function name decays to a function-pointer value of the matching
type.

```cpp
import std;

int add(int a, int b) {
    return a + b;
}

int main() {
    int (*fp)(int, int) = add;
    int value = fp(2, 3);
    std::println("{}", value);
    return 0;
}
```

Output:

```text
5
```

The key idea is that `fp` is just a value holding “which function to call
later”. Calling through it uses ordinary function-call syntax.

## A function pointer can be stored in a class field

Because a function pointer is just another value, a class can keep one and use
it later.

```cpp
import std;

int add(int a, int b) {
    return a + b;
}

class Holder {
public:
    virtual ~Holder() { return; }

private:
    int (*fp_)(int, int) = nullptr;

public:
    Holder(int (*fp)(int, int)) : fp_{fp} {
        return;
    }

    int run(int x, int y) const {
        return this->fp_(x, y);
    }
};

int main() {
    Holder h{&add};
    int value = h.run(4, 5);
    std::println("{}", value);
    return 0;
}
```

Output:

```text
9
```

Using `&add` here is equivalent to the bare `add` form above. Some code prefers
the explicit address-taking spelling when passing a function onward.

## The target pointer type can select one overload

If several functions share one name, the target pointer type decides which one
you mean.

```cpp
import std;

int encode(int value) {
    return value + 100;
}

char encode(char value) {
    return value;
}

int main() {
    int (*pick_int)(int) = &encode;
    char (*pick_char)(char) = &encode;
    int first = pick_int(23);
    char second = pick_char('Q');
    std::println("{}", first);
    std::println("{}", second);
    return 0;
}
```

Output:

```text
123
Q
```

So `&encode` by itself is not ambiguous once the destination type is known. The
pointer declaration tells overload resolution which concrete function to pick.

## The working model for function pointers in today's scpp

So far, the practical rules are:

- function pointers use ordinary C/C++ pointer-to-function syntax;
- a matching function name, or `&function_name`, can initialize one;
- calling through a function pointer uses ordinary call syntax;
- function pointers are plain values, so they can be stored in objects and
  passed through APIs;
- when the name is overloaded, the target pointer type selects the matching
  overload.

That gives Chapter 11 a second callable-building block alongside closures. The
next section can add owning wrappers such as `std::function` and
`std::move_only_function`.

---

[← Previous: Closures and Captures](ch11-01-closures-and-captures.md) · [Table of Contents](README.md)
