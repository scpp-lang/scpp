# Current capability gaps to track for scpp

This file is an internal backlog, not reader-facing book content.

It tracks gaps against scpp's own intended language/library/tooling story,
not against any single reference language. The guiding litmus test is simple:
if an ordinary C++ compiler (clang/gcc/msvc) accepts the exact syntax, scpp can
support that capability as a library facility and/or semantic layer; if the
syntax is not real C++, scpp should not invent it.

The list below was re-audited against current `main`; fixed items should be
removed instead of left behind as historical noise.

## Language/Syntax Gaps (current focus)

These items need real compiler work: parser, semantic analysis, borrow/type
rules, coroutine machinery, codegen, or preprocessor support.

- `std::span` still has language-level rough edges in its borrow/binding rules:
  the core fixed-size-local-array case works, but direct binding from a string
  literal still fails, and `std::span` still cannot be rebound after
  construction.
- Generic (template) `union` declarations are rejected outright, even
  though a templated union is ordinary, valid ISO C++ syntax and
  non-generic unions already work: a union's member types can never be
  parameterized by an enclosing template's own type parameters.
- Multi-file module builds are not usable end-to-end yet: a manifest-driven
  project build (`scpp build`) rejects any source file classified as a
  module implementation unit (a file starting with plain `module name;`,
  no `export`) with "module implementation units are not implemented in
  project builds yet", so a module's interface and implementation cannot
  really be split across files in a real build. The bare `extern`
  declaration form that exists precisely to let such a split happen (an
  exported, bodyless `extern int f(...);` whose body is meant to live in a
  separate implementation unit or `.scppo` object) has no working path to
  ever receive that body either: supplying one anywhere reachable in the
  same program -- the same namespace block, or a separately reopened one --
  is rejected as a redefinition rather than accepted as that declaration's
  own definition, and leaving it undefined instead fails to link with an
  undefined-reference error as soon as it's called.
- `inline` on a function declaration/definition is currently parser-only:
  scpp accepts the keyword so ordinary C++ signatures such as
  `[[nodiscard]] inline SourceLocation make_source_location(...)` parse, but
  it has no semantic effect whatsoever. It does not provide C++'s real
  "multiple definitions across translation units without an ODR violation"
  relaxation, and it does not act as any codegen/optimization inlining hint
  either: a function marked `inline` compiles and behaves identically to the
  same function without `inline`. This is a deliberate simplification to
  unblock self-hosting-preparation work on compiler-internal sources, and
  full `inline` semantics can wait until a real multi-translation-unit need
  appears.
- Coroutine/async language support is still absent: no `co_await`, `co_yield`,
  `co_return`, or coroutine lowering/runtime integration yet.

## Library/Stdlib Gaps (tracked, but not the current focus)

These items are real gaps, but they are achievable as libraries/runtime/tooling
on top of syntax that real C++ compilers already accept.

- No `std::string_view`-style borrowed string view yet.
- `std::span`'s library surface is still narrow beyond the core array case:
  there is still no `std::string` interop and no broader container/view
  construction story yet.
- No growable standard collections such as `std::vector` or hash-map
  equivalents yet.
- No `std::variant`/`std::visit`-style tagged-union library surface yet, so the
  underlying "sum type / tagged union" capability is still missing in valid
  C++ syntax even though scpp should pursue it as a library feature rather than
  invented new syntax.
- Concurrency support is still shallow: `std::thread`/`std::jthread` exist, but
  there is no `std::mutex`, atomics, condition-variable, or future/promise
  layer.
- File I/O and filesystem support are still minimal: `scpp::io::getline()`
  exists for stdin, but there is no general file open/read/write API or
  `std::filesystem`-style path layer.
- No JSON/document parsing/serialization library yet.
- No archive/compression/signature primitives yet.
- No crypto/hashing/TLS library surface yet.
- The shipped HTTP server is still a static-file server. Its public builder only
  mounts `/` to a filesystem root and toggles a few file-serving options; there
  is no general routing, request handling, or API framework yet.
- Self-hosting is still not realistic today: the compiler backend is directly
  tied to LLVM's C++ API, and the compiler implementation itself relies heavily
  on `std::shared_ptr` and other C++ standard-library facilities that scpp does
  not ship.
