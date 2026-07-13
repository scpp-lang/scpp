# Rust-like gaps noticed while rewriting the book

This file is an internal backlog, not reader-facing book content.

## Language / standard-library gaps

- No `std::string_view`-style borrowed text view yet, which makes it harder to
  teach Rust-like “borrowed slice of text” examples.
- No ordinary member-call syntax such as `.size()` yet; the book currently has
  to explain `.size` as a computed field on `std::span`.
- `std::span` can currently be constructed only from fixed-size arrays; there is
  no broader construction path from other container types yet.
- `std::span` cannot be rebound after construction, so it behaves more like a
  permanently-bound borrow than a freely reassignable view value.
- No separate `operator->` model; `x->y` is only sugar through `operator*` /
  `std::unique_ptr`, which narrows the “smart pointer” design space compared
  with C++ and some Rust wrapper patterns.
- Cross-function named lifetime groups are designed in the book, but the current
  compiler still implements only a small subset of that story.
- No language-level enums, pattern matching, `if let`, or `let...else` yet, so
  TRPL-style chapters about tagged alternatives must be reframed or postponed.
- No settled rule yet for enum zero-initialization / default-construction when an
  enum has no declared `0` enumerator. Real C++ still permits a zero bit-pattern
  value here (for example, `std::errc()` as the “success” idiom even though
  `std::errc` has no explicit zero/success enumerator), but that now sits in
  tension with scpp's stricter enum-safety direction: if int→enum conversion is
  allowed only through checked `scpp::enum_cast<T>`, letting `SomeEnum{}` or an
  equivalent zero-initialization path silently materialize a non-enumerator value
  would be an inconsistent loophole. Rust's model is the opposite extreme: enums
  have no implicit zero/default state at all, and any opted-in default must be a
  real declared variant rather than an out-of-band sentinel. This needs a later
  design decision, including what scpp should do with C++-style cases such as
  `std::errc`.
- No growable standard collections such as `std::vector` or hash-map
  equivalents yet.
- `const T&` parameter binding still rejects `double` literals, so generic APIs that
  want literal-friendly scalar calls currently have to prefer by-value parameters;
  one visible consequence is that `std::print`/`std::println` accept a bare
  `std::string` lvalue only via `std::move(s)` or a fresh temporary for now.
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
