# Recoverable Errors Today

Section 9.1 covered failures that current scpp treats as unrecoverable: the
program aborts as soon as a checked operation detects a bad state.

Not every problem belongs in that category. Sometimes a function should simply
say “I could not produce a value,” and let the caller choose what to do next.
Today, the main standard tool for that style is `std::optional<T>`.

An optional either contains one `T` value or contains nothing. That makes it a
good fit for lookups, parsing steps, and other operations where “missing” is a
normal outcome rather than a fatal bug.

For each runnable example below, save the file as `optional.scpp`, then build
and run it like this:

```sh
scpp optional.scpp -o optional
./optional
```

For examples that are supposed to abort, the program compiles successfully, but
terminates when it asks an empty optional for a value it does not have.

## Return `std::optional<T>` when a function may or may not produce a value

A function can return an optional and let the caller branch on `has_value()`.

```cpp
import std;

std::optional<int> find_score(int id) {
    if (id == 7) {
        std::optional<int> full{99};
        return full;
    }
    std::optional<int> empty{};
    return empty;
}

int main() {
    std::optional<int> found = find_score(7);
    std::optional<int> missing = find_score(1);

    if (found.has_value()) {
        std::println("score = {}", found.value());
    }
    if (!missing.has_value()) {
        std::println("not found");
    }
    return 0;
}
```

Output:

```text
score = 99
not found
```

The important shift is who makes the decision. `find_score` does not abort when
it has no answer; it returns an empty optional, and the caller decides how to
handle that ordinary case.

## An empty optional does not need a default-constructible `T`

`std::optional<T>` can still be empty even when `T` itself requires constructor
arguments.

```cpp
import std;

class Ticket {
private:
    int id_{};

public:
    virtual ~Ticket() = default;

    Ticket(int id) : id_{id} {
        return;
    }

    Ticket(const Ticket& other) : id_{other.id_} {
        return;
    }

    int id() const {
        return this->id_;
    }
};

int main() {
    std::optional<Ticket> empty{};
    std::optional<Ticket> full{Ticket{42}};
    std::println("{} {}", empty.has_value(), full->id());
    return 0;
}
```

Output:

```text
false 42
```

`Ticket` itself has no zero-argument constructor, but the optional can still
represent “no ticket yet” by containing nothing.

## `reset()` clears the current value, and another optional can replace it

Once an optional has a value, you can clear it back to the empty state and later
replace it from another optional.

```cpp
import std;

int main() {
    std::optional<int> answer{7};
    std::println("{} {}", answer.has_value(), answer.value());

    answer.reset();
    std::println("{}", answer.has_value());

    std::optional<int> replacement{9};
    answer = replacement;
    std::println("{}", answer.value());
    return 0;
}
```

Output:

```text
true 7
false
9
```

That gives you a straightforward state machine: full, empty, then full again.
The key is that the caller stays explicit about each transition.

## `value()` on an empty optional is still a checked failure

`std::optional` only makes absence recoverable if code actually checks before
accessing the value. Calling `value()` on an empty optional aborts.

```cpp
import std;

int main() {
    std::optional<int> empty{};
    return empty.value();
}
```

Behavior when run: the program aborts because `empty` does not contain an `int`.

So `std::optional<T>` is not “nullable data with no rules.” The recoverable part
is the explicit branch on `has_value()`, not unchecked access.

## The recoverable-error pattern available today

So far, the practical rules are:

- use `std::optional<T>` when a function may legitimately have no `T` to return;
- check `has_value()` before calling `value()` or assuming a value is present;
- an optional can represent absence even when `T` has no default constructor;
- `reset()` returns the optional to the empty state;
- today's `std::optional<T>` tells you whether a value exists, but not why it is
  missing.

That last limitation is the natural bridge to the next section: preparing APIs
for result types that can carry both a value and an error explanation.

---

[← Previous: Unrecoverable Errors and Compiler-Inserted Checks](ch09-01-unrecoverable-errors-and-compiler-inserted-checks.md) · [Table of Contents](README.md)
