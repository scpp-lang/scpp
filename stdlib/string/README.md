# stdlib/string — `std::string` backed by real `std::string`

This directory is scpp's first `stdlib` module, and its concrete
demonstration of both calling into a real C/C++ library from scpp (see
[ch04 §4.2](../../docs/book/en/ch04-struct-vs-class.md) for the `class`
feature this relies on, and
[ch01 §1.3](../../docs/book/en/ch01-safety-context.md)/
[ch02](../../docs/book/en/ch02-boundary-rules.md) for the
`extern "C"`/`unsafe {}` boundary rules) *and* scpp's own
multi-file module system
([ch11](../../docs/book/en/ch11-modules-and-libraries.md)): a consumer
writes `import std;` then uses `std::string` directly, exactly like real
C++.

## Why a wrapper, not a native reimplementation

scpp does not (yet) have its own dynamic string type, and writing one from
scratch would just reimplement `std::string` badly. Instead:

- `scpp_string_wrapper.h`/`.cpp` is **ordinary C++**, compiled by an
  ordinary C++ compiler (clang++/g++) — nothing about it is scpp-specific.
  It exposes a small set of `extern "C"` functions, each a thin, direct
  forward onto a real, heap-allocated `std::string` referenced by an
  opaque `void*` handle. No C++ type (`std::string` itself, or anything
  from namespace `std`) ever crosses the `extern "C"` boundary — scpp only
  ever sees `void*`/`const char*`/`int`, types it already understands.
- `std.cpp` is the scpp side: `export module std;` with `namespace std {
  export class string { ... }; }`, whose only job is calling those
  `extern "C"` functions, each wrapped in `unsafe { }` exactly like any
  other call to an `extern "C"` function (ch01/ch02). All of `string`'s
  actual behavior (growth, copying bytes, etc.) is real `std::string`
  code; scpp contributes only the checked, borrow-checked surface around
  it (RAII construction/destruction, access control on the `handle`
  field, `this`-borrow-checked methods).

## Files

| File | Language | Role |
|---|---|---|
| `scpp_string_wrapper.h` | C/C++ | `extern "C"` function declarations (the ABI contract) |
| `scpp_string_wrapper.cpp` | C++ | Implementation, forwards onto real `std::string` |
| `std.cpp` | scpp | The `std` module (`export module std;`), exporting `class string` |
| `demo.cpp` | scpp | `import std;` then a `main()` exercising `std::string` (construct/append/length/c_str/equals) |
| `CMakeLists.txt` | CMake | Builds the wrapper, builds+links the demo against `std.cpp` via `scpp build --import`, registers the ctest case (see below) |
| `expected_output.txt` | — | Recorded expected stdout of `demo.cpp`, checked by the `stdlib_string_demo` ctest case |

`std.cpp` has no `main()` of its own — it's a module's primary interface
unit (ch11 §11.3), meant to be `import`ed by a consumer, not run directly.
Unlike scpp's earlier (pre-ch11) single-file limitation, `demo.cpp` and
`std.cpp` are genuinely **separately compiled** — `scpp build demo.cpp
--import std=std.cpp` parses and move-checks each independently, seeding
`demo.cpp`'s checker with `std.cpp`'s *exported* signatures only (ch11
§11.8), then emits and links two separate object files (see
`src/driver.cppm`'s `compile_to_executable`) — no textual concatenation
is involved.

## Building and running

This directory is a normal part of the top-level CMake build (via
`add_subdirectory(stdlib/string)`) — no separate build step:

```sh
cmake --build build          # builds libscpp_string_wrapper.a and string_demo
ctest --test-dir build -R stdlib_string_demo --output-on-failure
build/stdlib/string/string_demo   # run it directly, if you just want the output
```

`stdlib/string/CMakeLists.txt` does two things, in order:

1. Builds `scpp_string_wrapper.cpp` as an ordinary CMake static library
   target (`scpp_string_wrapper`) — entirely independent of the scpp
   compiler itself, exactly as a real project's existing C/C++ library
   would be.
2. Runs `scpp build demo.cpp -o string_demo --import std=std.cpp --link
   <path/to/libscpp_string_wrapper.a>` as a custom command (depending on
   the `scpp` target, the `scpp_string_wrapper` target, and both source
   files, so it always rebuilds against a fresh compiler/library/source).
   `--import std=std.cpp` (ch11 §11.7/§11.13) resolves `demo.cpp`'s
   `import std;` against `std.cpp`'s source, which gets its own,
   separately-compiled object file automatically linked in alongside
   `demo.cpp`'s own and the wrapper library. Since the wrapper is real
   C++ (needs libstdc++'s runtime), `scpp build` automatically adds
   `-lstdc++` to the link line whenever `--link` is used at all.

Registers `stdlib_string_demo` as a ctest case that runs the resulting
`string_demo` executable and diffs its stdout against
`expected_output.txt` — part of the normal `ctest --test-dir build` run,
not a separate manual step.

## Known limitations (consistent with `class`'s own v0.1 scope)

- **No copy semantics**: `std::string` cannot be reassigned after
  construction, passed by value as a parameter, or returned by value —
  movecheck rejects all three (see `src/movecheck.cppm`'s
  class-typed-local/parameter/return-type checks). This isn't specific to
  `std::string`; it applies to every `class` type in this version, since
  scpp has no copy-constructor concept yet (ch04 §4.2). Pass `const
  std::string&`/`std::string&` instead.
- **`c_str()`'s validity window** matches real `std::string::c_str()`:
  valid only until the next mutating call (`append`) or destruction of the
  same `std::string`.
- Only the operations needed for a meaningful demo are wrapped
  (construct/destroy/length/c_str/append/equals) — not `std::string`'s
  full API. Extending `scpp_string_wrapper.h`/`.cpp` with more `extern "C"`
  functions and matching `std::string` methods is a straightforward,
  purely additive follow-up.
- **No `.scppm` archive packaging** (ch11 §11.11): `std.cpp` is consumed
  directly as a source file via `--import std=std.cpp`, not packaged into
  a `.scppm` library archive with a compiled-payload/signature story —
  deliberately out of scope for this module system's first pass (see
  ch11's own document for the full design).
