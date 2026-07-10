# Rust-like gaps noticed while rewriting the book

This file is an internal backlog, not reader-facing book content.

## Language / standard-library gaps

- No `std::string_view`-style borrowed text view yet, which makes it harder to
  teach Rust-like “borrowed slice of text” examples.
- No ordinary member-call syntax such as `.size()` yet; the book currently has
  to explain `.size` as a computed field on `std::span`.
- `std::span` can currently be constructed only from fixed-size arrays, not from
  richer owned containers the way Rust can teach slices from `Vec<T>` or arrays.
- `std::span` cannot be rebound after construction, so it behaves more like a
  permanently-bound borrow than a freely reassignable view value.
- No separate `operator->` model; `x->y` is only sugar through `operator*` /
  `std::unique_ptr`, which narrows the “smart pointer” design space compared
  with C++ and some Rust wrapper patterns.
- Cross-function named lifetime groups are designed in the book, but the current
  compiler still implements only a small subset of that story.
- No language-level enums, pattern matching, `if let`, or `let...else` yet, so
  TRPL-style chapters about tagged alternatives must be reframed or postponed.
- No growable standard collections such as `std::vector`, `std::string`, or
  hash-map equivalents yet; the book currently has to teach fixed-size arrays
  and C-compatible buffers instead.
- No `for` / range-for loops yet; learner-facing iteration still has to center
  on `while` and explicit indices.
- No inheritance or virtual-function-based dynamic dispatch in v0.1, which rules
  out a direct translation of TRPL's object-oriented/trait-object discussions.
- No async/await, futures, or streams story yet, so TRPL's async chapter has no
  direct scpp counterpart today.
- No macro system yet, which removes a whole category of Rust metaprogramming
  examples from any near-term scpp book.

## Tooling gaps

- There is no mdBook-/Rustdoc-style automatic code-block extraction and
  compile-test harness for `docs/book/`; examples must currently be verified by
  separate manual scripts or external tests.
- No `rustup`-style installer or toolchain manager yet; today the getting
  started story is still "build from source, then optionally `cmake --install`
  it somewhere on your `PATH`."
- No Cargo/crates.io-style registry, package publishing, or dependency-fetch
  workflow yet; current project builds cover local manifests, path deps, and
  workspaces, but not a full package ecosystem.
