# Fixed-Size Arrays

The previous section ended chapter 7 with one module spread across several
files. This chapter shifts from project layout back to ordinary data: fixed-size
arrays, character buffers, and borrowed views over contiguous storage.

A fixed-size array stores a known number of elements inline. It owns those
values itself, and its length is part of its type. When you already know how
many elements you need, an array is the simplest sequence type scpp has today.

For each runnable example below, save the file as `arrays.scpp`, then build and
run it like this:

```sh
scpp arrays.scpp -o arrays
./arrays
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## Declaring an array with a fixed number of elements

Use `T[N]` for “an array of `N` elements of type `T`”.

```cpp
import std;

int main() {
    int scores[4]{};
    scores[0] = 10;
    scores[1] = 20;
    scores[2] = 30;
    scores[3] = 40;

    std::println("{} {}", scores[0], scores[3]);
    return 0;
}
```

Output:

```text
10 40
```

`scores` owns four `int` values directly. The `[4]` is not just a comment for
human readers; it is part of the variable's type, and every valid index has to
fit that fixed size.

The empty braces matter too: today, `T[N]{}` is the reliable way to start with a
fully initialized array, then fill the elements you want.

## Range-based `for` with `auto&` can update each element in place

Section 3.5 already used a range-based `for` loop to read each element of an
array. Using `auto&` instead gives direct mutable access to each element.

```cpp
import std;

int main() {
    int values[3]{};
    int next = 1;

    for (auto& value : values) {
        value = next * 10;
        next = next + 1;
    }

    for (int value : values) {
        std::println("{}", value);
    }

    return 0;
}
```

Output:

```text
10
20
30
```

Each `value` in the first loop is a reference to the real array element, not a
copy. Writing through that reference updates the array itself.

## An array bound can come from an earlier `constexpr`

The size does not have to be written as a raw literal. Any earlier constant
expression that resolves to a positive size works too.

```cpp
import std;

int main() {
    constexpr int count = 4;
    int values[count]{};
    values[3] = 9;

    std::println("{} {}", count, values[3]);
    return 0;
}
```

Output:

```text
4 9
```

This is often clearer than repeating the same literal in several places. The
important part is not that the name is spelled `count`; it is that `count` is a
`constexpr`, so the compiler can resolve the bound before the program ever runs.

## The bound must be a constant expression

An ordinary runtime variable cannot be used as the bound.

```cpp
int main() {
    int count = 4;
    int values[count]{};
    return 0;
}
```

Compiler output:

```text
array_nonconst_bound_fail.scpp: error: 3:16: expression is not a constant expression: identifier 'count' is not available
```

scpp does not have variable-length local arrays here. The compiler needs the
full size of `values` up front, so the bound has to be known during
compilation, not discovered later at runtime.

## A constant out-of-bounds index is rejected immediately

Because the bound is part of the type, a definitely-wrong constant index can be
rejected at compile time.

```cpp
int main() {
    int values[3]{};
    return values[3];
}
```

Compiler output:

```text
array_out_of_bounds_compile.scpp:3:12: error: array subscript 3 is out of bounds for array of size 3
 3 |     return values[3];
   |            ^
```

The valid indices for `int values[3]` are `0`, `1`, and `2`. Writing `3`
directly in the source gives the compiler enough information to reject the
program before it ever runs.

## Today, fill arrays element by element instead of with a list of values

In ordinary C++, `int scores[4]{10, 20, 30, 40};` would be a natural way to
spell the first example. In today's scpp, that multi-element brace initializer
is not supported yet.

```cpp
int main() {
    int scores[4]{10, 20, 30, 40};
    return 0;
}
```

Compiler output:

```text
array_brace_init.scpp:2:5: error: brace-initialization of this member requires exactly one expression
 2 |     int scores[4]{10, 20, 30, 40};
   |     ^
```

That is why every runnable example in this section started from `T[N]{}` and
then assigned the elements one by one.

## The fixed-size-array rules so far

- `T[N]` declares an array of exactly `N` elements of type `T`;
- the array owns those elements directly, and `N` is part of the type;
- range-based `for` over `auto&` lets you mutate each element in place;
- the bound can come from an earlier `constexpr`, but not from an ordinary
  runtime variable;
- obviously out-of-bounds constant indices are rejected at compile time;
- today, zero-initialize with `{}` and then fill elements explicitly.

The next section stays with arrays, but narrows the element type to `char` and
uses that fixed-size storage as a C-compatible text buffer.

---

[← Previous: Separating Modules into Different Files](ch07-05-separating-modules-into-different-files.md) · [Table of Contents](README.md)
