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
     *skipped* inside an `[[scpp::unsafe]] { }` block (ch01 §1.1), so the resulting
     value is genuine, unspecified garbage that can't be pinned down --
     but the process must still terminate normally, not be killed by a
     signal.
- **Multi-file (ch11 module) cases**: some rules (import/export across
  files, partitions, ...) genuinely need more than one source file. A
  directory containing a `main.scpp` file is instead treated as one
  *module test case*, named after the directory:
  - `main.scpp` -- the entry point, compiled and run exactly like an
    ordinary single-file case; `main.expected` is its outcome (same three
    forms as above).
  - `main.imports` (optional) -- one `module_name=relative_path` mapping
    per non-blank, non-`#`-comment line, passed to `scpp build` as
    `--import module_name=path` (ch11 §11.14) -- list every module
    `main.scpp` needs, direct or transitive, since `main.scpp` is the only
    file actually compiled as the entry point.
  - any other `.scpp` files in the directory -- the modules referenced by
    `main.imports`; never scanned as their own standalone case.

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
| `01_basics` | M1: scalars, locals, `if`/`while`, functions, arithmetic, zero-init, unconditional-by-default checking, basic `break`/`continue`, ternary `?:`, ordinary forward declarations |
| `02_structs` | `struct` triviality rules, zero-init, bitwise copy, forbidden members |
| `03_unique_ptr` | `std::make_unique`/`std::move`, move-out checking, arrow sugar |
| `04_references_borrow` | `T&`/`const T&`, alias-XOR-mutability, NLL release, lifetime elision |
| `05_span` | `std::span<T>` construction/indexing/bounds checks |
| `06_unsafe_blocks` | `[[scpp::unsafe]] { }` gating and scoping rules; §5.1-§5.4 staying active inside it; function-level `[[scpp::unsafe]]` marker (ch01 §1.2, scpp's `unsafe fn`) |
| `07_extern_c` | `extern "C"` declarations/definitions, real libc interop |
| `08_address_of` | `&expr`, `const T*`/`T*` distinction |
| `09_integer_overflow` | checked-abort by default, wrapping in `[[scpp::unsafe]]`, div/mod special cases |
| `10_bool_and_char` | no implicit scalar conversions, short-circuit evaluation |
| `12_struct_vs_class` | `struct` vs `class` access-control divergence |
| `13_unsupported_robustness` | unsupported/not-yet-implemented syntax fails cleanly, never crashes |
| `14_classes` | constructors/destructors, private access control, compiler-provided/user-defined copy construction and assignment, compiler-only move construction and assignment, method borrow checking, `this` |
| `15_function_overloading` | exact-type-match resolution, by-value/by-reference axis, const/non-const methods |
| `16_namespaces` | basic `namespace` declaration, qualified calls, nesting, same-namespace unqualified class lookup; `using namespace` rejected |
| `17_modules` | `export module`/`import`, namespace-matches-module-name (ch11 §11.6), cross-module import/export/re-export, bare `extern`, partitions |
| `18_closures` | lambda expressions (ch05 §5.12): by-value/by-reference/init capture, blanket/mixed captures, lifetime-tracking of reference-capturing closures, explicit `this`/`*this` capture, `mutable`, trailing return types, generic lambdas |
| `19_scalar_types` | the full scalar family beyond `bool`/`int`/`char` (ch06), explicit scalar-to-scalar casts, and comparison rules for same-type vs mixed-type scalars |
| `20_generic_functions` | ch05 §5.11 revisions: full header form (bare/concept-constrained/multi-param/return-type-only), abbreviated bare `auto`, concept-constrained parameter packs |
| `21_generic_types` | generic `struct`/`class` types (ch05 §5.14): bare/concept-constrained type parameters, per-method `requires`, variadic types via recursive inheritance, non-type template parameters, base-class-deduction indexed access |
| `22_lifetime_generic_parameters` | `[[scpp::lifetime(generic)]]` (ch05 §5.13): reserved lifetime group, call-site exemption for closures accepting a callee-chosen lifetime |
| `23_thread_safety_attributes` | `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]` (ch05 §5.15): structural derivation and manual override |
| `24_function_pointers` | function pointers (ch05 §5.16): real C/C++ syntax, the unsafe-qualified/not-unsafe-qualified type split, automatic address-type selection (ordinary / `[[scpp::unsafe]]` / bodyless `extern "C"` / with-body `extern "C"`), one-directional conversion, struct-member legality, copyability, `&overloaded_name` target-type resolution |
| `25_function_wrappers` | `std::function` / `std::move_only_function` (ch05 §5.18): copyable vs move-only targets, cv/ref-qualified signatures, moved-from behavior |
| `26_threads` | `std::thread` / `std::jthread`: thread-movable constructor constraint, join/detach/joinable transitions, `jthread` destructor auto-join |

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
  unconditionally by default (ch01/ch08 Q13); `[[scpp::unsafe]]` is the
  only safety-context construct (an attribute, not a keyword -- see ch01
  §1.3, "redesigned from a bare `unsafe { }` block to
  `[[scpp::unsafe]] { }`" this round), and it only relaxes ch05 §5.5's
  fixed operation list (raw pointer deref, calling an `extern "C"`
  function, etc.) plus span-bounds/overflow checking -- ownership/move/
  alias/lifetime checking (§5.1-§5.4) keeps running unconditionally even
  inside `[[scpp::unsafe]] { }`. Calling an `extern "C"` function always
  needs `[[scpp::unsafe]] { }` regardless of the caller *and regardless
  of whether that function has a body* -- extern "C" linkage itself
  marks the FFI boundary (ch02's boundary table draws no distinction); a
  with-body extern "C" function's own internals are otherwise checked
  exactly like any other function (e.g. a raw-pointer dereference inside
  it still needs its own `[[scpp::unsafe]] { }`). A *new* mechanism this
  round: attaching `[[scpp::unsafe]]` directly to a function's own
  declaration (before its return type) makes the whole body an unsafe
  context *and* makes calling that function itself one of §5.5's gated
  operations -- scpp's equivalent of Rust's `unsafe fn` (ch01 §1.2).
- **`17_modules`'s `--import name=path` mechanics are now verified**:
  `path` does point directly at a module's raw `.scpp` interface source,
  compiled on the fly -- no separate "compile a module to `.scppm` first"
  step is needed. Confirmed empirically by 10 passing multi-file cases.
  Module partitions genuinely combining multiple files into one
  importable module still aren't expressible with this one-path-per-
  module-name model -- only the "an external file can't import a
  partition directly" restriction is covered; the
  primary-interface-unit-aggregates-partitions mechanism itself isn't
  exercised.
- **A module file can't also be the runnable program**: a file containing
  `export module name;` does not get its `main()` linked as the process
  entry point (discovered empirically via an "undefined reference to
  `main`" linker error). Every `17_modules` multi-file case therefore
  uses two files: a plain (non-moduled) `main.scpp` that does the
  `import`ing and calling, and a separate module file with no `main` of
  its own. This is a constraint on the test convention, not a documented
  language rule -- flagged here since it isn't called out anywhere in
  `docs/book/`.
- `docs/book/` can occasionally lag behind `src/` (the two are maintained
  independently) -- a chapter note saying a feature is "not yet
  implemented" is not always still accurate (confirmed again this round:
  basic `namespace` declaration/qualified-lookup/nesting already work).
  When in doubt, a quick probe with `scpp build` settles it empirically.
- **`18_closures` assumed `auto` local-variable and return-type deduction
  already work like real C++**, even though it's never explicitly called
  out as supported anywhere in `docs/book/` -- **confirmed correct** on
  verification, no cases failed because of this.
- **`18_closures`'s generic-function/generic-lambda cases were expected to
  depend on the separately-tracked generics/concepts gap** -- turned out
  to be half right: concept-constrained generic functions/lambdas are now
  implemented (confirmed by
  `passing_closure_to_concept_constrained_generic_function.scpp` passing),
  but a bare/unconstrained `auto` parameter is a distinct, still-open gap
  (see Status below) -- not the same thing as "generics aren't implemented
  at all" as originally guessed.

## Status

Current maintained baseline, rebuilt locally with CMake + Ninja and
re-run via `./build/run_tests`:

- **268 cases total**
- **268/268 passing**
- **`24_function_pointers`: 14/14 meaningfully verified** -- the parser
  now accepts real function-pointer declarators and the suite covers both
  the positive-path runtime cases and the `COMPILE_ERROR` safety rules
- **Move-assignment teardown is now covered in two shapes**:
  - a `std::unique_ptr`-owning class whose old target value is destroyed
    during move assignment, then the replacement value is destroyed again
    at scope-exit
  - a user-defined destructor-owning class with a manually-managed
    resource (`strdup`/`free`), verifying that the old target state is
    torn down before the moved-in state replaces it
- **Class by-value transport is now covered in both the positive and
  negative shapes**:
  - copyable classes passed/returned by value
  - move-only classes passed/returned by value via `std::move(...)`
  - non-copyable bare locals still rejected when a copy would be needed
- **Thread-trait overrides now cover the rewritten §5.15/§8 docs**:
  builtin trait predicates, conditional overrides on generic classes,
  unconditional generic override propagation, and `std::unique_ptr<T>`'s
  trait forwarding behavior
- **Function/thread wrappers now have direct black-box coverage**:
  `std::function`, `std::move_only_function`, `std::thread`, and
  `std::jthread` each have dedicated case directories exercising their
  current stdlib behavior
- **Recent retrospective gap-filling now covers several "too basic to fail"
  language corners that ordinary app code exposed first**:
  `break`/`continue`, ternary `?:`, ordinary forward declarations,
  same-namespace unqualified class lookup, and scalar-comparison
  rejection for mixed scalar types

No known implementation gaps remain in the full black-box suite at this
snapshot.
