# stdlib/string — a `class String` backed by real `std::string`

This directory is scpp's first `stdlib` module, and its concrete
demonstration of calling into a real C/C++ library from scpp (see
[ch04 §4.2](../../docs/book/en/ch04-struct-vs-class.md) for the `class`
feature this relies on, and
[ch01 §1.3](../../docs/book/en/ch01-safety-context.md)/
[ch02](../../docs/book/en/ch02-boundary-rules.md) for the
`extern "C"`/`unsafe {}` native-function-call rules).

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
- `String.cpp` is the scpp side: a `class String` whose only job is
  calling those `extern "C"` functions, each wrapped in `unsafe { }`
  exactly like any other native-function call from safe code. All of
  `String`'s actual behavior (growth, copying bytes, etc.) is real
  `std::string` code; scpp contributes only the safe, borrow-checked
  surface around it (RAII construction/destruction, access control on the
  `handle` field, `this`-borrow-checked methods).

## Files

| File | Language | Role |
|---|---|---|
| `scpp_string_wrapper.h` | C/C++ | `extern "C"` function declarations (the ABI contract) |
| `scpp_string_wrapper.cpp` | C++ | Implementation, forwards onto real `std::string` |
| `String.cpp` | scpp | `class String`, calling the wrapper's functions |
| `demo.cpp` | scpp | A `main()` exercising `String` (construct/append/length/c_str/equals) |
| `CMakeLists.txt` | CMake | Builds the wrapper, concatenates+builds+links the demo, registers the ctest case (see below) |
| `concat.cmake` | CMake script | Concatenates `String.cpp`+`demo.cpp` (invoked from `CMakeLists.txt`) |
| `expected_output.txt` | — | Recorded expected stdout of `demo.cpp`, checked by the `stdlib_string_demo` ctest case |

`String.cpp` has no `main()` of its own — it's a library source, meant to
be built together with a consumer. scpp v0.1 has no multi-file/include
mechanism yet (`scpp build` compiles exactly one input file), so
`concat.cmake` concatenates `String.cpp` and `demo.cpp` into one file
before `CMakeLists.txt` invokes `scpp build` on the result. A real
multi-file scpp program would still need exactly this concatenation (or a
future `import`/`#include` equivalent) until that's designed.

## Building and running

This directory is a normal part of the top-level CMake build (via
`add_subdirectory(stdlib/string)`) — no separate build step:

```sh
cmake --build build          # builds libscpp_string_wrapper.a and string_demo
ctest --test-dir build -R stdlib_string_demo --output-on-failure
build/stdlib/string/string_demo   # run it directly, if you just want the output
```

`stdlib/string/CMakeLists.txt` does three things, in order:

1. Builds `scpp_string_wrapper.cpp` as an ordinary CMake static library
   target (`scpp_string_wrapper`) — entirely independent of the scpp
   compiler itself, exactly as a real project's existing C/C++ library
   would be.
2. Concatenates `String.cpp` + `demo.cpp` via `concat.cmake`, then runs
   `scpp build <combined> -o string_demo --link <path/to/libscpp_string_wrapper.a>`
   as a custom command (depending on both the `scpp` target and the
   `scpp_string_wrapper` target, so it always rebuilds against a fresh
   compiler/library). The `--link` flag (added to `scpp build` alongside
   this demo — see `src/driver.cppm`'s `link_executable`) forwards extra
   paths straight to the system linker after scpp's own object file,
   exactly like listing extra `.o`/`.a` files on an ordinary `cc`/`clang`
   command line. Since the wrapper is real C++ (needs libstdc++'s
   runtime), `scpp build` automatically adds `-lstdc++` to the link line
   whenever `--link` is used at all.
3. Registers `stdlib_string_demo` as a ctest case that runs the resulting
   `string_demo` executable and diffs its stdout against
   `expected_output.txt` — part of the normal `ctest --test-dir build`
   run, not a separate manual step.

## Known limitations (consistent with `class`'s own v0.1 scope)

- **No copy semantics**: `String` cannot be reassigned after construction,
  passed by value as a parameter, or returned by value — movecheck rejects
  all three (see `src/movecheck.cppm`'s class-typed-local/parameter/
  return-type checks). This isn't specific to `String`; it applies to
  every `class` type in this version, since scpp has no copy-constructor
  concept yet (ch04 §4.2). Pass `const String&`/`String&` instead.
- **`c_str()`'s validity window** matches real `std::string::c_str()`:
  valid only until the next mutating call (`append`) or destruction of the
  same `String`.
- Only the operations needed for a meaningful demo are wrapped
  (construct/destroy/length/c_str/append/equals) — not `std::string`'s
  full API. Extending `scpp_string_wrapper.h`/`.cpp` with more `extern "C"`
  functions and matching `String` methods is a straightforward, purely
  additive follow-up.
