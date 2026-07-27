# Unrecoverable Errors and Compiler-Inserted Checks

Earlier sections already showed two kinds of failure:

- some programs are rejected during compilation because the compiler can prove
  they are wrong; and
- some operations are accepted, but only after scpp inserts a runtime check that
  aborts the program if execution reaches an impossible or unsafe state.

This section focuses on that second category. Current scpp does not try to
recover from arithmetic overflow, bad runtime subscripts, or similar checked
failures. It stops the program immediately instead.

For each runnable example below, save the file as `fail-fast.scpp`, then build
and run it like this:

```sh
scpp fail-fast.scpp -o fail-fast
./fail-fast
```

For examples that are supposed to abort, the program compiles successfully, but
terminates via `std::abort()` when the bad state is reached.

## A runtime subscript is checked before the element is read

If the index is not a compile-time constant, scpp cannot reject the program up
front. Instead, it inserts a bounds check around the actual access.

```cpp
import std;

int read_at(std::span<const int> values, int index) {
    return values[index];
}

int main() {
    int numbers[4]{};
    numbers[0] = 10;
    numbers[1] = 20;
    numbers[2] = 30;
    numbers[3] = 40;
    std::println("{}", read_at(numbers, 2));
    return 0;
}
```

Output:

```text
30
```

The important part is not just that `2` happens to be valid. It is that the
function body uses ordinary `values[index]`, and scpp keeps that operation
checked even though the exact index arrives at runtime.

## The same checked subscript aborts on an out-of-range runtime value

If execution reaches that same access with a bad index, the inserted check stops
the program before any invalid read happens.

```cpp
import std;

int read_at(std::span<const int> values, int index) {
    return values[index];
}

int main() {
    int numbers[4]{};
    numbers[0] = 10;
    numbers[1] = 20;
    numbers[2] = 30;
    numbers[3] = 40;
    return read_at(numbers, 9);
}
```

Behavior when run: the program aborts instead of reading past the end of the
span.

That is the runtime counterpart to Section 8.1's compile-time out-of-bounds
example. When the compiler can prove the index is wrong, it rejects the source.
When only the running program knows the index, the compiler inserts a check.

## Signed integer overflow is checked by default

Ordinary integer arithmetic is also guarded. The program does not silently wrap
around on overflow.

```cpp
int main() {
    int limit = 2147483647;
    int next = limit + 1;
    return next;
}
```

Behavior when run: the program aborts when evaluating `limit + 1`.

So today's scpp chooses fail-fast checked arithmetic over C++-style overflow
surprises or undefined behavior at this boundary.

## Division checks the divisor before performing the operation

Division is checked too. A zero divisor aborts instead of slipping through to
machine-level undefined behavior.

```cpp
int main() {
    int total = 10;
    int count = 0;
    return total / count;
}
```

Behavior when run: the program aborts before performing the division.

This same inserted check also covers the other signed-division trap case,
`INT_MIN / -1`, for the same reason: the result would not fit in an `int`.

## The fail-fast rules today

So far, the practical rules are:

- if scpp can prove a bad operation from the source alone, it rejects the
  program during compilation;
- if correctness depends on a runtime value, scpp may insert a check that aborts
  the program when that value is invalid;
- checked subscripts, checked arithmetic overflow, and checked division are all
  examples of this fail-fast model;
- these are unrecoverable errors in today's language model, not exceptions to
  catch and continue from.

The next section stays in error handling, but shifts to the problems programs can
handle deliberately today, using explicit return values instead of fail-fast
termination.

---

[← Previous: Borrowed Views with `std::span`](ch08-03-borrowed-views-with-std-span.md) · [Table of Contents](README.md) · [Next: Recoverable Errors Today →](ch09-02-recoverable-errors-today.md)
