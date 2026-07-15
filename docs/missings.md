# Rust-like gaps noticed while rewriting the book

This file is an internal backlog, not reader-facing book content.

## Language / standard-library gaps

- No `std::string_view`-style borrowed text view yet, which makes it harder to
  teach Rust-like “borrowed slice of text” examples.
- `std::span` can currently be constructed only from fixed-size arrays; there is
  no broader construction path from other container types yet.
- `std::span` cannot be rebound after construction, so it behaves more like a
  permanently-bound borrow than a freely reassignable view value.
- No user-defined `operator->` support yet: declaring `operator->` is rejected
  (`expected ';' but found '->'`), and `x->y` on custom types currently works
  only through `operator*`, unlike real C++'s distinct `operator->`
  chaining/proxy model.
- Cross-function named lifetime groups remain largely unimplemented; today only
  a narrow subset of the `[[scpp::lifetime(...)]]` story works.
- No Rust-like tagged enums, pattern matching, `if let`, or `let...else` yet;
  today's `enum class` support only covers C++-style fieldless enums.
- No growable standard collections such as `std::vector` or hash-map
  equivalents yet.
- `const double&` parameter binding still rejects `double` literals, so
  literal-friendly floating-point APIs currently have to prefer by-value
  parameters.
- No virtual-function-based dynamic dispatch yet, even though basic single
  inheritance now works.
- No async/await, futures, or streams story yet, so TRPL's async chapter has no
  direct scpp counterpart today.
- No macro system yet, which removes a whole category of Rust metaprogramming
  examples from any near-term scpp book.
