# `std::span` and Other Non-Owning Views

Ownership explains who cleans up resources. References explain how code can
temporarily use one value without taking that responsibility away.
`std::span<T>` applies that same borrowing idea to a whole contiguous sequence
of elements.

In today's scpp, `std::span` is the main standard non-owning view type. You can
think of a span as a small view value that pairs a pointer to the first element
with a length. The span does not own those elements; the array owner still
does.

For each runnable example below, save the file as `span.scpp`, then build and
run it like this:

```sh
scpp span.scpp -o span
./span
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

## Constructing a span from a fixed-size array

Today, the normal construction path is from a fixed-size array.

```cpp
import std;

int main() {
    int numbers[4]{};
    numbers[0] = 7;
    numbers[1] = 8;
    numbers[2] = 9;
    numbers[3] = 10;
    std::span<int> view = numbers;
    int length = view.size;
    std::println("{}", length);
    std::println("{}", view[2]);
    return 0;
}
```

Output:

```text
4
9
```

`view` is borrowing the array. Constructing the span did not copy the four
elements, and it did not transfer ownership away from `numbers`.

## Passing a span to read without copying the elements

A function that only needs to read a sequence can take `std::span<const T>`.

```cpp
import std;

int sum(std::span<const int> values) {
    int total = 0;
    for (int value : values) {
        total = total + value;
    }
    return total;
}

int main() {
    int numbers[4]{};
    numbers[0] = 10;
    numbers[1] = 20;
    numbers[2] = 30;
    numbers[3] = 40;
    std::println("sum = {}", sum(numbers));
    return 0;
}
```

Output:

```text
sum = 100
```

The call `sum(numbers)` constructs the span view at the call site. Passing the
span by value copies only that small view object, not the underlying array
elements.

## Mutable spans can update the caller's array

If a function should mutate existing elements in place, take `std::span<T>`.

```cpp
import std;

void double_all(std::span<int> values) {
    for (auto& value : values) {
        value = value * 2;
    }
    return;
}

int main() {
    int numbers[3]{};
    numbers[0] = 3;
    numbers[1] = 4;
    numbers[2] = 5;
    double_all(numbers);
    for (int value : numbers) {
        std::println("{}", value);
    }
    return 0;
}
```

Output:

```text
6
8
10
```

`double_all` still does not own the array. It receives a mutable non-owning
view, writes through that view, and the caller keeps ownership the whole time.

## `std::span<const T>` is read-only

Making the element type `const` gives a shared, read-only view.

```cpp
import std;

int main() {
    int numbers[3]{};
    std::span<const int> view = numbers;
    view[0] = 99;
    return 0;
}
```

Compiler output:

```text
span_const_write_fail.scpp:6:10: error: cannot assign to this place: it is reached through a read-only (const) reference
 6 |     view[0] = 99;
   |          ^
```

The rule is the same as with `const T&` in the previous section: a shared
borrow lets you read, but not write.

## Span borrows follow the same liveness rules as references

The borrowing model from Section 4.2 still applies. Once a shared span has been
used for the last time, a mutable span borrow of the same array can begin.

```cpp
import std;

int main() {
    int numbers[3]{};
    numbers[0] = 5;
    numbers[1] = 6;
    numbers[2] = 7;
    std::span<const int> reader = numbers;
    int first = reader[0];
    std::span<int> writer = numbers;
    writer[1] = 9;
    std::println("{} {}", first, numbers[1]);
    return 0;
}
```

Output:

```text
5 9
```

That `writer` borrow is accepted because `reader` was already used for the last
time at `int first = reader[0];`.

But overlapping shared and mutable span borrows are rejected:

```cpp
import std;

int main() {
    int numbers[3]{};
    std::span<int> writer = numbers;
    std::span<const int> reader = numbers;
    return writer[0] + reader[0];
}
```

Compiler output:

```text
span_borrow_conflict_fail.scpp:6:5: error: cannot borrow 'numbers': it is already mutably borrowed
 6 |     std::span<const int> reader = numbers;
   |     ^
```

So spans are not an escape hatch around ownership checking. They are views, but
they are still borrows.

## Current limitations today

Two current limitations matter when you design APIs around spans.

First, construction is currently limited to fixed-size arrays:

```cpp
import std;

int main() {
    int value{1};
    std::span<int> view = value;
    return 0;
}
```

Compiler output:

```text
span_non_array_fail.scpp:5:27: error: std::span<T> can currently only be constructed from a fixed-size array in this version
 5 |     std::span<int> view = value;
   |                           ^
```

Second, a span cannot currently be rebound after it is initialized:

```cpp
import std;

int main() {
    int first[2]{};
    int second[2]{};
    std::span<int> view = first;
    view = second;
    return 0;
}
```

Compiler output:

```text
span_reassign_fail.scpp:7:5: error: std::span 'view' cannot be reassigned after initialization in this version
 7 |     view = second;
   |     ^
```

So today `std::span` behaves more like a permanently bound borrow than a freely
reassignable view value.

## The rules of `std::span`

So far, the working rules are:

- `std::span<T>` is a non-owning view over contiguous elements;
- constructing or passing a span does not copy the underlying elements;
- `std::span<const T>` is read-only, while `std::span<T>` allows mutation;
- the same borrow and liveness rules from Section 4.2 still apply to spans;
- today spans are constructed from fixed-size arrays, and they cannot be
  rebound after construction.

The arrays chapter will return to buffers and views in more detail.

---

[← Previous: References and Borrowing](ch04-02-references-and-borrowing.md) · [Table of Contents](README.md)
