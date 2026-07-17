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
- User-defined `operator->` is now specified (see `docs/spec/en/03-dereference-and-member-access.md`),
  but the implementation is still missing: declaring it is rejected
  (`expected ';' but found '->'`), and the current parser still implements the
  old invented blanket rewrite `x->y` → `(*x).y` for class types. The required
  fix is breaking-but-correcting: remove that non-standard fallback, implement
  real C++-style explicit `operator->` chaining/proxy semantics, and migrate
  shipped wrappers such as `std::unique_ptr` to declare `operator->()`
  explicitly.
- Cross-function named lifetime groups are still mostly absent.
  `[[scpp::lifetime(generic)]]` on an ordinary parameter works, but multi-group
  contracts such as separate `a`/`b` groups across a signature are still
  rejected.
- Coroutine/async language support is still absent: no `co_await`, `co_yield`,
  `co_return`, or coroutine lowering/runtime integration yet.
- There is still no C/C++-style macro/preprocessor story (`#define`, etc.).

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
