# Propagating `std::expected<T, E>` Through Multi-Step APIs

Section 9.3 introduced `std::expected<T, E>` as the basic "value or error"
result type. The next question is how larger functions should use it.

Today's scpp does not add exceptions or a dedicated propagation operator on top
of `expected`. Instead, multi-step code is explicit: inspect each result, return
an error early when needed, and keep going only from the success state.

For each runnable example below, save the file as `expected-propagation.scpp`,
then build and run it like this:

```sh
scpp expected-propagation.scpp -o expected-propagation
./expected-propagation
```

All examples in this section are supposed to compile and run successfully.

## Wrap lower-level status APIs in one `expected`-returning helper

A low-level library function may report failure in some other shape. One common
pattern is to convert that boundary once, then let the rest of the program work
with `std::expected<T, E>` directly.

```cpp
import std;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class count_error {
    empty,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, count_error> parse_count(const std::string& text) {
    if (text.size() == 0) {
        std::unexpected<count_error> err{count_error::empty};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<count_error> err{count_error::invalid};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<count_error> err{count_error::out_of_range};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<count_error> err{count_error::trailing_text};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    std::expected<int, count_error> result{value};
    return std::move(result);
}

int main() {
    auto good = parse_count("128");
    auto bad = parse_count("12x");

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad.has_value() && bad.error() == count_error::trailing_text) {
        std::println("trailing text");
    }
    return 0;
}
```

Output:

```text
128
trailing text
```

`std::from_chars` itself reports through its `ec` and `ptr` fields. The helper
turns that into one `expected<int, count_error>` boundary, so callers can stay
in a single result style after that point.

## Return early to propagate the same error enum upward

If an outer function uses the same error type as the helper it calls, explicit
propagation is usually just "check, then return the error case unchanged."

```cpp
import std;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class count_error {
    empty,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, count_error> parse_count(const std::string& text) {
    if (text.size() == 0) {
        std::unexpected<count_error> err{count_error::empty};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<count_error> err{count_error::invalid};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<count_error> err{count_error::out_of_range};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<count_error> err{count_error::trailing_text};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    std::expected<int, count_error> result{value};
    return std::move(result);
}

std::expected<int, count_error> double_count(const std::string& text) {
    auto parsed = parse_count(text);
    if (!parsed.has_value()) {
        std::unexpected<count_error> err{parsed.error()};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    std::expected<int, count_error> result{parsed.value() * 2};
    return std::move(result);
}

int main() {
    auto good = double_count("21");
    auto bad = double_count("7x");

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad.has_value() && bad.error() == count_error::trailing_text) {
        std::println("trailing text");
    }
    return 0;
}
```

Output:

```text
42
trailing text
```

This is the manual propagation pattern available today. There is no hidden jump
or exception path here; the function spells out exactly where failure returns to
its caller.

## Translate lower-level errors at a wider API boundary

Sometimes a larger API should expose a simpler error vocabulary than the helpers
inside it. In that case, the outer layer checks each lower-level result and
maps it into the error codes it wants to promise publicly.

```cpp
import std;
import scpp;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class count_error {
    empty,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, count_error> parse_count(const std::string& text) {
    if (text.size() == 0) {
        std::unexpected<count_error> err{count_error::empty};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<count_error> err{count_error::invalid};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<count_error> err{count_error::out_of_range};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<count_error> err{count_error::trailing_text};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    std::expected<int, count_error> result{value};
    return std::move(result);
}

enum class request_error {
    bad_count,
    bad_range,
};

std::expected<int, request_error> load_width(const std::string& text) {
    auto parsed = parse_count(text);
    if (!parsed.has_value()) {
        std::unexpected<request_error> err{request_error::bad_count};
        std::expected<int, request_error> result{err};
        return std::move(result);
    }

    auto distribution = scpp::rand::uniform_int_distribution<int>::make(1, parsed.value());
    if (!distribution.has_value()) {
        std::unexpected<request_error> err{request_error::bad_range};
        std::expected<int, request_error> result{err};
        return std::move(result);
    }

    int width = distribution.value().max() - distribution.value().min() + 1;
    std::expected<int, request_error> result{width};
    return std::move(result);
}

int main() {
    auto good = load_width("6");
    auto bad_count = load_width("6x");
    auto bad_range = load_width("0");

    if (good.has_value()) {
        std::println("{}", good.value());
    }
    if (!bad_count.has_value() && bad_count.error() == request_error::bad_count) {
        std::println("bad count");
    }
    if (!bad_range.has_value() && bad_range.error() == request_error::bad_range) {
        std::println("bad range");
    }
    return 0;
}
```

Output:

```text
6
bad count
bad range
```

The outer function keeps the detailed parsing logic private. Callers only need
to know whether the request text was bad or the requested range was invalid.

## The working rules for `expected` propagation today

- turn low-level status-style APIs into one `std::expected<T, E>` boundary when
  you can;
- if two layers share the same error enum, check `has_value()` and return that
  error upward explicitly;
- if a wider API wants fewer or different errors, translate them at the outer
  boundary;
- keep success-path code after the `has_value()` check, and touch `value()` only
  on that path;
- today there is no special propagation operator: result flow is written as
  ordinary control flow.

That explicit style is a good match for current scpp. The control flow stays
visible, the recoverable cases stay typed, and each function chooses how much of
a lower layer's error detail it wants to expose.

---

[← Previous: Recoverable Errors with `std::expected<T, E>`](ch09-03-using-std-expected.md) · [Table of Contents](README.md)
