# Calling `extern "C"` and Using Raw Pointers

The previous section introduced `[[scpp::unsafe]]` as the narrow gate for a
small set of explicitly unchecked operations.

Two of the most common cases are:

- calling a bodyless `extern "C"` function; and
- dereferencing a raw pointer.

This section focuses on those two operations and on the small amount of type
information that still matters even inside unsafe code.

For each runnable example below, save the file as `raw-pointers.scpp`, then
build and run it like this:

```sh
scpp raw-pointers.scpp -o raw-pointers
./raw-pointers
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## Forming a raw pointer is safe

Taking an address with `&value` is ordinary safe code. What requires
`[[scpp::unsafe]]` is *trusting* that pointer and dereferencing it.

```cpp
import std;

int main() {
    int value{1};
    int* pointer = &value;
    [[scpp::unsafe]] {
        *pointer = 9;
    }
    std::println("{}", value);
    return 0;
}
```

Output:

```text
9
```

That split is deliberate. Safe code may prepare raw pointers for low-level APIs,
but the actual dereference stays behind an explicit unsafe boundary.

## Dereferencing a raw pointer without unsafe is rejected

If you try to dereference a raw pointer directly in safe code, the compiler
stops you.

```cpp
int read_value(int* pointer) {
    return *pointer;
}

int main() {
    int value{42};
    return read_value(&value);
}
```

Compiler output:

```text
raw_pointer_unsafe_fail.scpp:2:12: error: cannot dereference raw pointer 'pointer': requires '[[scpp::unsafe]] { }' (spec ch01 §1.3/ch02)
 2 |     return *pointer;
   |            ^
```

## Calling a bodyless `extern "C"` function

An `extern "C"` declaration without a body is another unchecked boundary. scpp
cannot inspect its implementation, so calling it requires unsafe context too.

```cpp
import std;

extern "C" int abs(int x);

int main() {
    [[scpp::unsafe]] {
        std::println("{}", abs(-7));
    }
    return 0;
}
```

Output:

```text
7
```

This is the same design pattern as raw pointers: declaring the boundary is
fine, but actually trusting it requires `[[scpp::unsafe]]`.

## Calling that `extern "C"` function in safe code is rejected

If the call happens outside an unsafe context, the compiler rejects it.

```cpp
extern "C" int abs(int x);

int main() {
    return abs(-7);
}
```

Compiler output:

```text
calling_extern_c_requires_unsafe_fail.scpp:4:12: error: cannot call 'extern "C"' function 'abs' outside '[[scpp::unsafe]] { }': no scpp compiler ever sees its real implementation to check it (spec ch01 §1.3/ch02)
 4 |     return abs(-7);
   |            ^
```

## Mutable pointers widen to `const` pointers

Ordinary pointer typing rules still apply. A mutable `T*` can be passed where
`const T*` is expected.

```cpp
import std;

int read(const int* pointer) {
    [[scpp::unsafe]] {
        return *pointer;
    }
}

int main() {
    int value{7};
    int* pointer = &value;
    std::println("{}", read(pointer));
    return 0;
}
```

Output:

```text
7
```

So `[[scpp::unsafe]]` does not erase the type system. It only gates particular
operations.

## Writing through a `const` pointer is still a type error

Even inside an unsafe block, a `const int*` is still read-only.

```cpp
int main() {
    int value{5};
    const int* pointer = &value;
    [[scpp::unsafe]] {
        *pointer = 10;
    }
    return value;
}
```

Compiler output:

```text
write_through_const_pointer_fail.scpp:5:9: error: cannot assign to this place: it is reached through a read-only (const) reference
 5 |         *pointer = 10;
   |         ^
```

## Taking the address of a read-only place yields `const T*`

The same rule shows up when you form a pointer from a read-only place. If the
source is only reachable through `const`, the resulting pointer type must be
`const T*`, not `T*`.

```cpp
int read(const int& value) {
    int* pointer = &value;
    return 0;
}

int main() {
    int number{1};
    return read(number);
}
```

Compiler output:

```text
address_of_const_ref_fail.scpp:2:20: error: cannot assign '&' of a read-only-reachable place to 'pointer' (a mutable 'T*'): would need 'const T*', which 'pointer' isn't declared as
 2 |     int* pointer = &value;
   |                    ^
```

So raw pointers in scpp are low-level, but they are not untyped. Whether a
pointer is mutable or `const` still matters everywhere.

The next section will stay in this unsafe chapter, but shift from the mechanics
of individual calls and dereferences to the larger question of how to keep trust
localized in real programs.

---

[← Previous: What `[[scpp::unsafe]]` Does and Does Not Do](ch06-01-what-scpp-unsafe-does-and-does-not-do.md) · [Table of Contents](README.md)
