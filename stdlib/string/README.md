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
- `std.cpp` is the scpp side: the "std" module's primary interface unit
  (`export module std;`), aggregating the actual `class string`
  declaration from a separate partition (`std_string.cpp`, `export
  module std:string;`, ch11 §11.4) via `export import :string;`. Every
  method's body calls those `extern "C"` functions, each wrapped in
  `unsafe { }` exactly like any other call to an `extern "C"` function
  (ch01/ch02). All of `string`'s actual behavior (growth, copying bytes,
  etc.) is real `std::string` code; scpp contributes only the checked,
  borrow-checked surface around it (RAII construction/destruction,
  access control on the `handle` field, `this`-borrow-checked methods).

## Files

| File | Language | Role |
|---|---|---|
| `scpp_string_wrapper.h` | C/C++ | `extern "C"` function declarations (the ABI contract) |
| `scpp_string_wrapper.cpp` | C++ | Implementation, forwards onto real `std::string` |
| `std.cpp` | scpp | The `std` module's primary interface unit (`export module std;`), aggregating `:string` via `export import :string;` |
| `std_string.cpp` | scpp | The `:string` partition (`export module std:string;`), exporting `class string` |
| `demo.cpp` | scpp | `import std;` then a `main()` exercising `std::string` (construct/append/length/c_str/equals) |
| `CMakeLists.txt` | CMake | Builds the wrapper, builds+links the demo against the `std` module via `scpp build --import`, registers the ctest case (see below) |
| `expected_output.txt` | — | Recorded expected stdout of `demo.cpp`, checked by the `stdlib_string_demo` ctest case |

Neither `std.cpp` nor `std_string.cpp` has a `main()` of its own — they're
a module's primary interface unit and one of its partitions (ch11
§11.3/§11.4), meant to be `import`ed by a consumer, not run directly.
Partitions are purely a source-organization mechanism for the module's
own author: from a consumer's point of view, nothing changes — `import
std;` is exactly the same whether `std::string` lives directly in
`std.cpp` or, as here, in a `:string` partition `std.cpp` aggregates.
Building the module means compiling its primary interface unit
*together* with every partition it aggregates (ch11 §11.4) -- `scpp build
demo.cpp --import std=std.cpp --import std:string=std_string.cpp` gives
the driver both files, which are move-checked and compiled together into
one object file for the whole "std" module, genuinely **separately**
from `demo.cpp` itself (see `src/driver.cppm`'s `compile_to_executable`)
-- no textual concatenation is involved anywhere in this pipeline.

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
2. Runs `scpp build demo.cpp -o string_demo --import std=std.cpp --import
   std:string=std_string.cpp --link <path/to/libscpp_string_wrapper.a>`
   as a custom command (depending on the `scpp` target, the
   `scpp_string_wrapper` target, and all three scpp source files, so it
   always rebuilds against a fresh compiler/library/source). `--import
   std=std.cpp` (ch11 §11.7/§11.14) resolves `demo.cpp`'s `import std;`
   against `std.cpp`'s source; `--import std:string=std_string.cpp`
   resolves `std.cpp`'s own `export import :string;` against the
   partition's source (same flag, just keyed as
   `<module>:<partition>`). The whole "std" module (both files combined)
   gets its own, separately-compiled object file automatically linked in
   alongside `demo.cpp`'s own and the wrapper library. Since the wrapper
   is real C++ (needs libstdc++'s runtime), `scpp build` automatically
   adds `-lstdc++` to the link line whenever `--link` is used at all.

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
- **No `.scppm`/`.scppa`/`.scppkg` packaging** (ch11 §11.12): `std.cpp`
  and `std_string.cpp` are consumed directly as source files via
  `--import name=path`, not as a compiled `.scppm` module interface
  paired with a `.scppa` archive, nor bundled into a distributable
  `.scppkg` package — deliberately out of scope for this module system's
  first pass (see ch11's own document, and
  `docs/standards/en/{scppm,scppkg}-format.md`, for the full design).
