# scpp black-box tests

> 中文版: [README.zh.md](README.zh.md)

This directory is a **black-box** test suite for the `scpp` compiler. It is
maintained independently from `src/` (the implementation) and `docs/book/`
(the language specification): tests here are written purely by reading
`docs/book/` and invoking the built `scpp` CLI binary as an external tool,
exactly the way any user of the language would -- there is no dependency
on, or knowledge of, scpp's internal compiler modules.

## How it works

- `cases/<NN_category>/<name>.scpp` -- a small scpp program illustrating one
  documented language rule (cited in a comment at the top of the file,
  pointing at the relevant `docs/book/en/chXX-*.md` section). scpp source
  files use the `.scpp` extension, not `.cpp` (ch08 Q7/Q13): since every
  function is checked unconditionally by default now, an ordinary `.cpp`
  file must never be silently fed to the scpp compiler and checked without
  its author asking for that.
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
     *skipped* inside an `unsafe { }` block (ch01 §1.1), so the resulting
     value is genuine, unspecified garbage that can't be pinned down --
     but the process must still terminate normally, not be killed by a
     signal.

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

## Categories

| Directory | Covers |
|---|---|
| `01_basics` | M1: scalars, locals, `if`/`while`, functions, arithmetic, zero-init, unconditional-by-default checking |
| `02_structs` | `struct` triviality rules, zero-init, bitwise copy, forbidden members |
| `03_unique_ptr` | `std::make_unique`/`std::move`, move-out checking, arrow sugar |
| `04_references_borrow` | `T&`/`const T&`, alias-XOR-mutability, NLL release, lifetime elision |
| `05_span` | `std::span<T>` construction/indexing/bounds checks |
| `06_unsafe_blocks` | `unsafe { }` gating and scoping rules; §5.1-§5.4 staying active inside it |
| `07_extern_c` | `extern "C"` declarations/definitions, real libc interop |
| `08_address_of` | `&expr`, `const T*`/`T*` distinction |
| `09_integer_overflow` | checked-abort by default, wrapping in `unsafe`, div/mod special cases |
| `10_bool_and_char` | no implicit scalar conversions, short-circuit evaluation |
| `12_struct_vs_class` | `struct` vs `class` access-control divergence |
| `13_unsupported_robustness` | not-yet-implemented syntax fails cleanly, never crashes |
| `14_classes` | constructors/destructors, private access control, no-copy-semantics, method borrow checking, `this` |
| `15_function_overloading` | exact-type-match resolution, by-value/by-reference axis, const/non-const methods |
| `16_namespaces` | basic `namespace` declaration, qualified calls, nesting; `using namespace` rejected |

## Testing philosophy

- Every `.scpp` file is written to be **valid per `docs/book/`** -- if a
  test fails, check the cited doc section first. If the test itself turns
  out to violate the spec, fix the test. If it's genuinely spec-conformant
  and still fails, that's an implementation bug, logged here for the
  `src/` maintainer to fix -- this suite does not modify `src/` to work
  around failures.
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
- There is **no `safe` keyword** -- every function is checked
  unconditionally by default (ch01/ch08 Q13); `unsafe { }` is the only
  safety-context construct, and it only relaxes ch05 §5.5's fixed
  operation list (raw pointer deref, calling an `extern "C"` function,
  etc.) plus span-bounds/overflow checking -- ownership/move/alias/
  lifetime checking (§5.1-§5.4) keeps running unconditionally even inside
  `unsafe { }`. Calling an `extern "C"` function always needs `unsafe { }`
  regardless of the caller *and regardless of whether that function has a
  body* -- extern "C" linkage itself marks the FFI boundary (ch02's
  boundary table draws no distinction); a with-body extern "C" function's
  own internals are otherwise checked exactly like any other function
  (e.g. a raw-pointer dereference inside it still needs its own
  `unsafe { }`).
- `docs/book/` can occasionally lag behind `src/` (the two are maintained
  independently) -- a chapter note saying a feature is "not yet
  implemented" is not always still accurate (confirmed again this round:
  basic `namespace` declaration/qualified-lookup/nesting already work
  despite ch06's backlog still calling all of ch11 "design only so far";
  see `16_namespaces`). When in doubt, a quick probe with `scpp build`
  settles it empirically.

## Status

**123/123 passing**, verified against `scpp` after `src/` implemented the
`safe`-keyword removal (ch01/ch08 Q13: every function checked
unconditionally by default; `unsafe { }` is the only safety-context
construct; file extension `.scpp`, not `.cpp`). Getting here from the
docs-only rewrite took two more rounds of fixes once actually compiled:
- Two `07_extern_c` cases wrongly assumed a with-body `extern "C"`
  function could be called without `unsafe { }` -- ch02's boundary table
  draws no such exception, so both the call sites and (for
  `sum_point_by_pointer`) an internal raw-pointer dereference needed
  `unsafe { }` added.
- `namespace` support turned out to already be implemented (nesting,
  qualified calls) despite ch06 still listing it as backlog -- moved the
  wrong `13_unsupported_robustness` case into a new `16_namespaces`
  category of real passing cases, and added a `13_unsupported_robustness`
  case for the one piece confirmed still missing (`using foo::bar;`
  single-name import).

Re-run `./build/run_tests` after any further `src/`/`docs/book/` change to
catch regressions.
