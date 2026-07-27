# Choosing Between Fail-Fast Checks, `std::optional<T>`, and `std::expected<T, E>`

At this point, Chapter 9 has three different error-handling tools on the table:

- compiler-inserted fail-fast checks for bad operations,
- `std::optional<T>` for ordinary absence, and
- `std::expected<T, E>` for ordinary failure with an explanation.

The remaining design question is not *how* each tool works, but *when* an API
should choose one over the others.

For each runnable example below, save the file as `api-shapes.scpp`, then build
and run it like this:

```sh
scpp api-shapes.scpp -o api-shapes
./api-shapes
```

For examples that are supposed to abort, the program compiles successfully, but
terminates when execution reaches the invalid state.

## Use fail-fast checked operations for broken caller contracts and impossible states

Some functions are only meaningful if their preconditions already hold. In those
cases, today's scpp often relies on ordinary checked operations and lets a bad
call abort instead of turning it into a recoverable return value.

```cpp
import std;

int items_per_group(int total_items, int groups) {
    return total_items / groups;
}

int main() {
    std::println("{}", items_per_group(12, 3));
    return 0;
}
```

Output:

```text
4
```

Here `groups == 0` is not presented as an expected business outcome. It is a
broken caller contract. If execution does reach `items_per_group(12, 0)`, the
program aborts at the checked division instead of pretending that the caller can
continue normally.

## Use `std::optional<T>` when “no value” is ordinary and needs no explanation

If the only question is whether a value exists, `std::optional<T>` is usually
the simplest shape.

```cpp
import std;

std::optional<int> find_channel(int id) {
    if (id == 7) {
        std::optional<int> found{42};
        return found;
    }
    std::optional<int> missing{};
    return missing;
}

int main() {
    auto first = find_channel(7);
    auto second = find_channel(1);

    if (first.has_value()) {
        std::println("{}", first.value());
    }
    if (!second.has_value()) {
        std::println("missing");
    }
    return 0;
}
```

Output:

```text
42
missing
```

The caller does not need a code saying *why* channel `1` was absent. It only
needs to know that no channel mapping exists.

## Use `std::expected<T, E>` when callers need a typed reason for failure

If callers should branch differently on different failure causes, return an
`expected` and make that reason part of the type.

```cpp
import std;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class port_error {
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, port_error> parse_port(const std::string& text) {
    int value = 0;
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<port_error> err{port_error::invalid};
        std::expected<int, port_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<port_error> err{port_error::out_of_range};
        std::expected<int, port_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<port_error> err{port_error::trailing_text};
        std::expected<int, port_error> result{err};
        return std::move(result);
    }

    if (value < 1 || value > 65535) {
        std::unexpected<port_error> err{port_error::out_of_range};
        std::expected<int, port_error> result{err};
        return std::move(result);
    }

    std::expected<int, port_error> result{value};
    return std::move(result);
}

int main() {
    auto good = parse_port("8080");
    auto bad = parse_port("80x");

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad.has_value() && bad.error() == port_error::trailing_text) {
        std::println("trailing text");
    }
    return 0;
}
```

Output:

```text
8080
trailing text
```

This is the main reason to choose `expected` over `optional`: the caller can see
that failure happened *and* distinguish what kind of failure it was.

## The practical API-design rule for today's scpp

The chapter's decision tree is now fairly small:

- if reaching the bad state means a bug, broken precondition, or impossible
  execution path, rely on ordinary checked operations and fail fast;
- if absence is normal and no extra reason matters, return `std::optional<T>`;
- if failure is normal and callers need a typed reason, return
  `std::expected<T, E>`;
- keep those choices visible in the function signature instead of hiding them
  behind exceptions.

That is the main shape of error handling in today's scpp: checked fail-fast
execution for bugs, explicit return values for recoverable cases, and type
signatures that tell callers which kind of situation they are dealing with.

---

[← Previous: Using `std::expected<T, E>` at I/O Boundaries](ch09-05-using-expected-at-io-boundaries.md) · [Table of Contents](README.md) · [Next: Generic Functions and Classes →](ch10-01-generic-functions-and-classes.md)
