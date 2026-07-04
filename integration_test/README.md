# scpp integration tests

This directory is a **black-box** integration test suite for the `scpp`
compiler. It is maintained independently from `src/` (the implementation)
and `docs/book/` (the language specification): tests here are written
purely by reading `docs/book/` and invoking the built `scpp` CLI binary as
an external tool, exactly the way any user of the language would -- there
is no dependency on, or knowledge of, scpp's internal compiler modules.

## How it works

- `cases/<NN_category>/<name>.cpp` -- a small scpp program illustrating one
  documented language rule (cited in a comment at the top of the file,
  pointing at the relevant `docs/book/en/chXX-*.md` section).
- `cases/<NN_category>/<name>.expected` -- the outcome that program *should*
  produce if `scpp` correctly implements the spec. Three forms:
  1. **A number on the first line**: `scpp build` must succeed, and running
     the resulting executable must exit with this code (0-255, POSIX
     `WEXITSTATUS`/shell `$?` semantics -- a process killed by a signal is
     normalized to `128+signum`, e.g. SIGABRT -> 134). Anything after the
     first line is the expected stdout, compared byte-for-byte.
  2. **`COMPILE_ERROR`**: `scpp build` must fail with a clean, positive exit
     status (a real diagnostic, not a crash). The exact message text is
     never checked -- the spec doesn't pin down wording.
  3. **`NO_ABORT`**: used only for the handful of cases where a
     scpp-inserted runtime check (span bounds, overflow) is deliberately
     *skipped* (inside `unsafe { }`/a native function, per ch01 §1.3), so
     the resulting value is genuine, unspecified garbage that can't be
     pinned down -- but the process must still still terminate normally,
     not be killed by a signal.

The runner itself (`run_tests.cpp`) is a small, dependency-free C++
program -- POSIX `fork`/`exec` + `<filesystem>` only, no third-party
libraries, no scpp modules linked in. It has its own standalone
`CMakeLists.txt` (independent of the top-level scpp project -- no
`add_subdirectory`, no LLVM/module dependency). Build it once with:

```sh
cmake -S . -B build
cmake --build build
```

Then run the whole suite (auto-discovers the `scpp` binary at
`../build/scpp`, baked in at configure time, so it works regardless of the
working directory it's run from):

```sh
./build/run_tests
```

Run one category, or filter by substring:

```sh
./build/run_tests 05_span
./build/run_tests bool_and_char
```

Pass `--scpp-bin <path>` to point at a different build.

## Testing philosophy

- Every `.cpp` file is written to be **valid per `docs/book/`** -- if a test
  fails, check the cited doc section first. If the test itself turns out to
  violate the spec, fix the test. If it's genuinely spec-conformant and
  still fails, that's an implementation bug, logged here for the `src/`
  maintainer to fix -- this suite does not modify `src/` to work around
  failures.
- Programs favor observing behavior through the **process exit code**
  (`main`'s return value) and, where real C interop is being exercised,
  real libc calls (`puts`, `printf`) declared via `extern "C"` -- both are
  fully documented mechanisms. Internal-only test scaffolding used by
  `tests/test_source` (e.g. `print_int`/`print_bool`/`print_char`) is
  deliberately **not** used here since it isn't part of the documented
  language surface.
- Every function body (including `void`-returning ones) needs an explicit
  `return` statement -- scpp currently has no implicit fall-off-the-end
  return, even though this isn't called out explicitly in `docs/book/`.
- `docs/book/` can occasionally lag behind `src/` (the two are maintained
  independently) -- a chapter note saying a feature is "not yet
  implemented" is not always still accurate. When in doubt, a quick probe
  with `scpp build` settles it empirically.

## Known failures as of this writing

These are believed to be genuine implementation gaps, not test bugs (see
each `.cpp`'s comment for the specific doc citation):

- `03_unique_ptr/returning_unique_ptr_and_binding_to_new_local_is_allowed.cpp`
  -- binding a factory function's `std::unique_ptr<T>` return value
  directly to a new local (`T x = factory();`) is rejected; only
  `std::move(place)` and `std::make_unique<T>(...)` are currently
  recognized as unique_ptr initializer forms. Passing the same call
  directly as a by-value argument (no intermediate local) does work --
  see `passing_factory_function_result_directly_as_argument_is_allowed.cpp`.
- `10_bool_and_char/bool_variable_initialized_from_int_literal_is_rejected.cpp`
  -- `bool b = 5;` should be a compile error (ch06: no implicit
  int<->bool conversion) but currently compiles, and the resulting binary
  **segfaults** at runtime. This is the more serious of the two bool/int
  findings.
- `10_bool_and_char/int_variable_initialized_from_bool_literal_is_rejected.cpp`
  -- `int x = true;` should likewise be a compile error but is currently
  silently accepted (and runs, returning the expected `1`).
