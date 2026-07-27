# Using `std::expected<T, E>` at I/O Boundaries

Section 9.4 showed how `expected` moves through multi-step functions once data
is already inside the program. Real programs also need a boundary where outside
text first becomes typed values.

In today's scpp, that boundary is usually explicit too: read text with a helper
such as `scpp::io::getline()`, parse it, and convert the possible failures into
an error enum the rest of the program can use.

For each runnable example below, save the file as `expected-io.scpp`, then
build it like this:

```sh
scpp expected-io.scpp -o expected-io
```

Each example in this section uses a different sample input command, shown right
before its output.

## Wrap line input and parsing into one `expected` helper

A program often wants callers to ask for “one checked `int` from stdin” instead
of repeating line-reading and parsing logic everywhere.

```cpp
import std;
import scpp;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class input_error {
    eof,
    read_failed,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, input_error> read_count() {
    auto line = scpp::io::getline();
    if (!line.has_value()) {
        input_error code = input_error::read_failed;
        if (line.error() == scpp::io::error::eof) {
            code = input_error::eof;
        }
        std::unexpected<input_error> err{code};
        std::expected<int, input_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const std::string& text = line.value();
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<input_error> err{input_error::invalid};
        std::expected<int, input_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<input_error> err{input_error::out_of_range};
        std::expected<int, input_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<input_error> err{input_error::trailing_text};
        std::expected<int, input_error> result{err};
        return std::move(result);
    }

    std::expected<int, input_error> result{value};
    return std::move(result);
}

void show_one() {
    auto value = read_count();
    if (value.has_value()) {
        std::println("count = {}", value.value());
        return;
    }
    if (value.error() == input_error::eof) {
        std::println("input closed");
        return;
    }
    std::println("invalid input");
}

int main() {
    show_one();
    show_one();
    show_one();
    return 0;
}
```

Run it like this:

```sh
printf '12\n7x\n' | ./expected-io
```

Output:

```text
count = 12
invalid input
input closed
```

The rest of the program no longer needs to know about `scpp::io::getline()` or
`std::from_chars` directly. It only sees one success-or-error boundary.

## Treat EOF as ordinary control flow when the caller wants “zero or more values”

Not every input boundary should treat end-of-file as an error. Sometimes EOF is
just the normal signal that there are no more records to read.

```cpp
import std;
import scpp;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class input_error {
    read_failed,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<std::optional<int>, input_error> read_count_or_eof() {
    auto line = scpp::io::getline();
    if (!line.has_value()) {
        if (line.error() == scpp::io::error::eof) {
            std::optional<int> done{};
            std::expected<std::optional<int>, input_error> result{done};
            return std::move(result);
        }
        std::unexpected<input_error> err{input_error::read_failed};
        std::expected<std::optional<int>, input_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const std::string& text = line.value();
    const char* first = text.c_str();
    const char* last = text.c_str() + text.size();
    std::from_chars_result parsed = std::from_chars(first, last, value);

    if (parsed.ec == std::errc::invalid_argument) {
        std::unexpected<input_error> err{input_error::invalid};
        std::expected<std::optional<int>, input_error> result{err};
        return std::move(result);
    }
    if (parsed.ec == std::errc::result_out_of_range) {
        std::unexpected<input_error> err{input_error::out_of_range};
        std::expected<std::optional<int>, input_error> result{err};
        return std::move(result);
    }
    if (!same_ptr(parsed.ptr, last)) {
        std::unexpected<input_error> err{input_error::trailing_text};
        std::expected<std::optional<int>, input_error> result{err};
        return std::move(result);
    }

    std::optional<int> boxed{value};
    std::expected<std::optional<int>, input_error> result{boxed};
    return std::move(result);
}

int main() {
    int total = 0;
    while (true) {
        auto next = read_count_or_eof();
        if (!next.has_value()) {
            std::println("bad input");
            return 1;
        }
        if (!next.value().has_value()) {
            break;
        }
        total = total + next.value().value();
    }
    std::println("{}", total);
    return 0;
}
```

Run it like this:

```sh
printf '10\n20\n30\n' | ./expected-io
```

Output:

```text
60
```

Here `std::optional<int>` answers a different question from `std::expected`:
`expected` says whether the read itself succeeded, while `optional` says whether
that successful read produced another value or reached normal end-of-input.

## Translate input details into a simpler application-level error enum

An outer API may not want to expose every low-level input detail directly. It
can read and parse in one helper, then map those details into the smaller error
vocabulary that makes sense for the application.

```cpp
import std;
import scpp;

bool same_ptr(const char* lhs, const char* rhs) {
    [[scpp::unsafe]] {
        return lhs == rhs;
    }
}

enum class count_error {
    eof,
    read_failed,
    invalid,
    trailing_text,
    out_of_range,
};

std::expected<int, count_error> read_count() {
    auto line = scpp::io::getline();
    if (!line.has_value()) {
        count_error code = count_error::read_failed;
        if (line.error() == scpp::io::error::eof) {
            code = count_error::eof;
        }
        std::unexpected<count_error> err{code};
        std::expected<int, count_error> result{err};
        return std::move(result);
    }

    int value = 0;
    const std::string& text = line.value();
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

enum class config_error {
    missing_line,
    bad_number,
    number_out_of_range,
};

std::expected<int, config_error> read_port() {
    auto count = read_count();
    if (!count.has_value()) {
        config_error code = config_error::bad_number;
        if (count.error() == count_error::eof) {
            code = config_error::missing_line;
        }
        if (count.error() == count_error::out_of_range) {
            code = config_error::number_out_of_range;
        }
        std::unexpected<config_error> err{code};
        std::expected<int, config_error> result{err};
        return std::move(result);
    }

    int port = count.value();
    if (port < 1 || port > 65535) {
        std::unexpected<config_error> err{config_error::number_out_of_range};
        std::expected<int, config_error> result{err};
        return std::move(result);
    }

    std::expected<int, config_error> result{port};
    return std::move(result);
}

int main() {
    auto good = read_port();
    auto missing = read_port();

    if (good.has_value()) {
        std::println("port = {}", good.value());
    }
    if (!missing.has_value() && missing.error() == config_error::missing_line) {
        std::println("missing port");
    }
    return 0;
}
```

Run it like this:

```sh
printf '8080\n' | ./expected-io
```

Output:

```text
port = 8080
missing port
```

The inner helper knows about I/O closure, parse errors, and trailing text. The
outer helper exposes only the distinctions that matter for this configuration
API.

## The working rules for `expected` at input boundaries today

- let one helper own the raw boundary work of reading text and parsing it;
- turn low-level I/O and parse outcomes into a typed error enum as early as you
  can;
- treat EOF as either an error or a normal stop signal depending on the API you
  are designing;
- when that distinction matters, `std::expected<std::optional<T>, E>` can say
  both “did the operation succeed?” and “was there another value?”;
- keep the rest of the program on typed values and `expected` results instead of
  repeating raw input checks everywhere.

That pattern closes the loop for today's recoverable-error story: external text
comes in at an explicit boundary, turns into typed results immediately, and then
moves through the rest of the program as ordinary checked control flow.

---

[← Previous: Propagating `std::expected<T, E>` Through Multi-Step APIs](ch09-04-propagating-expected-results.md) · [Table of Contents](README.md)
