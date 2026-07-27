# Recoverable Errors with `std::expected<T, E>`

Section 9.2 used `std::optional<T>` for operations that may or may not produce a
value. That works well when “missing” is the whole story.

Sometimes the caller needs more than that. A parse can fail for one reason, a
lookup can fail for another, and a factory can reject invalid input with a
specific error code. In those cases, today's scpp uses `std::expected<T, E>`:
"success holds a `T`; failure holds an `E`."

For each runnable example below, save the file as `expected.scpp`, then build
and run it like this:

```sh
scpp expected.scpp -o expected
./expected
```

For examples that are supposed to be rejected, save the file under the
descriptive filename shown in the diagnostic block if you want the compiler
output to match byte for byte.

For examples that are supposed to abort, the program compiles successfully, but
terminates when it asks an `expected` for the wrong side of the result.

## Return `std::expected<T, E>` when failure needs an explanation

A function can return either a real value or a domain-specific error code.

```cpp
import std;

enum class divide_error { division_by_zero };

std::expected<int, divide_error> divide_checked(int total, int count) {
    if (count == 0) {
        std::unexpected<divide_error> err{divide_error::division_by_zero};
        std::expected<int, divide_error> result{err};
        return std::move(result);
    }
    std::expected<int, divide_error> result{total / count};
    return std::move(result);
}

int main() {
    auto good = divide_checked(42, 7);
    auto bad = divide_checked(42, 0);

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad.has_value() && bad.error() == divide_error::division_by_zero) {
        std::println("bad divisor");
    }
    return 0;
}
```

Output:

```text
6
bad divisor
```

The difference from `std::optional<T>` is the second channel. The caller does
not just learn “there is no `int`”; it learns *which* recoverable problem
happened.

## Library factories can return a ready-to-use value or an error enum

This pattern is not only for toy examples. Current library code already uses it.

```cpp
import std;
import scpp;

int main() {
    auto ready = scpp::rand::uniform_int_distribution<int>::make(4, 9);
    auto broken = scpp::rand::uniform_int_distribution<int>::make(9, 4);

    if (!ready.has_value()) {
        return 1;
    }
    std::println("{} {}", ready.value().min(), ready.value().max());

    if (!broken.has_value() && broken.error() == scpp::rand::error::empty_range) {
        std::println("empty range");
    }
    return 0;
}
```

Output:

```text
4 9
empty range
```

The first call returns a working distribution object. The second returns the
specific `scpp::rand::error::empty_range` code instead. That is the normal
`std::expected<T, E>` shape: the caller branches once, then continues with the
appropriate state.

## Discarding an `expected` result is rejected

Recoverable-error results are meant to be acknowledged explicitly. Current
`std::expected<T, E>` is `[[nodiscard]]`, so throwing the result away is a
compile-time error.

```cpp
import std;

enum class divide_error { division_by_zero };

std::expected<int, divide_error> divide_checked(int total, int count) {
    if (count == 0) {
        std::unexpected<divide_error> err{divide_error::division_by_zero};
        std::expected<int, divide_error> result{err};
        return std::move(result);
    }
    std::expected<int, divide_error> result{total / count};
    return std::move(result);
}

int main() {
    divide_checked(42, 0);
    return 0;
}
```

Compiler output:

```text
expected-discard-fail.scpp:16:5: error: discarded return value of nodiscard type 'std::expected.int.divide_error': expected results must be checked
```

So the caller cannot silently ignore the success-or-error state. Even if the
program chooses to collapse several error cases into one response, it must still
inspect the returned `expected` deliberately.

## `value()` and `error()` are both checked accesses

An `expected` only makes failure recoverable if code first checks which state it
holds. Asking for a value from the error state still aborts.

```cpp
import std;

enum class divide_error { division_by_zero };

int main() {
    std::unexpected<divide_error> err{divide_error::division_by_zero};
    std::expected<int, divide_error> result{err};
    return result.value();
}
```

Behavior when run: the program aborts because `result` currently holds an error,
not an `int`.

The same checked rule applies in the other direction too: calling `error()` on a
value state aborts symmetrically.

## The working rules for `std::expected<T, E>` today

- use `std::optional<T>` when the only question is whether a `T` exists;
- use `std::expected<T, E>` when callers also need a reason for failure;
- construct the success state from a `T`, and the error state from
  `std::unexpected<E>`;
- branch on `has_value()` before calling `value()` or `error()`;
- discarded `expected` results are rejected at compile time;
- today's style is explicit manual branching, not hidden exception flow.

With `std::optional<T>` and `std::expected<T, E>`, today's scpp already has two
clear recoverable-error tools: one for “maybe a value,” and one for “either a
value or a reason.” A future section can build on that by looking at larger
multi-step APIs and result propagation patterns.

---

[← Previous: Recoverable Errors Today](ch09-02-recoverable-errors-today.md) · [Table of Contents](README.md) · [Next: Propagating `std::expected<T, E>` Through Multi-Step APIs →](ch09-04-propagating-expected-results.md)
