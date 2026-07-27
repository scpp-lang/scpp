# Generic Functions and Classes

Chapter 10 begins with the basic observation behind generic code: sometimes we
want the *same shape of program* to work for more than one concrete type.

In today's scpp, the starting point is familiar C++ template syntax. A generic
definition names one or more type parameters, and each concrete use instantiates
that definition with real types.

For each runnable example below, save the file as `generics.scpp`, then build
and run it like this:

```sh
scpp generics.scpp -o generics
./generics
```

## A generic function can abstract over more than one concrete argument type

A function template can describe one operation once and let different calls pick
different concrete types.

```cpp
import std;

template<typename T, typename U>
T first(T left, U right) {
    return left;
}

int main() {
    std::println("{} {}", first(42, true), first('S', 9));
    return 0;
}
```

Output:

```text
42 S
```

The two calls do not agree on one common argument type. The first uses `T = int`
and `U = bool`. The second uses `T = char` and `U = int`. One generic function
header covers both concrete cases.

## A generic class can store values of whichever concrete type instantiates it

Generic classes use the same `template<typename ...>` header and then mention
those parameters inside their fields and methods.

```cpp
import std;

template<typename T>
class Holder {
public:
    virtual ~Holder() { return; }

private:
    T item_;

public:
    Holder(const T& item) : item_{item} {
        return;
    }

    T get() const {
        return this->item_;
    }
};

int main() {
    Holder<int> number{42};
    Holder<char> initial{'G'};
    std::println("{} {}", number.get(), initial.get());
    return 0;
}
```

Output:

```text
42 G
```

`Holder<int>` and `Holder<char>` come from one class definition, but they are
still two different concrete instantiated types in the finished program.

## Each instantiation can resolve type-dependent layout separately

A generic class is not just “one runtime box with erased type information.” Each
instantiation can produce different concrete storage.

```cpp
import std;

template<typename T>
class Buffer {
public:
    virtual ~Buffer() { return; }

    char storage_[sizeof(T)]{};

    int size() const {
        return static_cast<int>(sizeof(this->storage_));
    }
};

struct OneByte {
    char value;
};

struct EightBytes {
    int left;
    int right;
};

int main() {
    Buffer<int> ints{};
    Buffer<OneByte> one{};
    Buffer<EightBytes> pair{};
    std::println("{} {} {}", ints.size(), one.size(), pair.size());
    return 0;
}
```

Output:

```text
4 1 8
```

So `Buffer<int>`, `Buffer<OneByte>`, and `Buffer<EightBytes>` are not all using
one identical layout. Each instantiation resolves `sizeof(T)` with its own
concrete `T`.

## The working model for generic code at the start of Chapter 10

So far, the practical rules are:

- write generic functions and classes with ordinary `template<typename ...>`
  headers;
- each concrete use substitutes real types for those parameters;
- one generic definition can serve many concrete calls or instantiated class
  types;
- type-dependent fields and method bodies are resolved separately for each
  instantiation;
- the next section adds concepts, which let APIs say not just “some type goes
  here,” but also “this type must support these operations.”

That gives us the base vocabulary for the rest of the chapter: generic function
families, generic class families, and concrete instantiations produced from
both.

---

[← Previous: Choosing Between Fail-Fast Checks, `std::optional<T>`, and `std::expected<T, E>`](ch09-06-choosing-between-fail-fast-optional-and-expected.md) · [Table of Contents](README.md)
