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
| `13_unsupported_robustness` | unsupported/not-yet-implemented syntax fails cleanly, never crashes |
| `14_classes` | constructors/destructors, private access control, no-copy-semantics, method borrow checking, `this` |
| `15_function_overloading` | exact-type-match resolution, by-value/by-reference axis, const/non-const methods |
| `16_namespaces` | basic `namespace` declaration, qualified calls, nesting; `using namespace` rejected |
| `17_modules` | `export module`/`import`, namespace-matches-module-name (ch11 §11.6), cross-module import/export/re-export, bare `extern`, partitions |
| `18_closures` | lambda expressions (ch05 §5.12): by-value/by-reference/init capture, blanket/mixed captures, lifetime-tracking of reference-capturing closures, explicit `this`/`*this` capture, `mutable`, trailing return types, generic lambdas |

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

**157 cases total, 154 passing.** 137/137 in `01_basics`-`17_modules`
(previously verified, see below) plus 17/20 in `18_closures` (ch05 §5.12
lambda expressions) are verified against `scpp` after `src/` implemented
generic functions/concepts (§5.11) and closures (§5.12) together.
`auto` local-variable/return-type deduction turned out to already work
fine, as assumed.

3 known failures in `18_closures`, all genuine implementation findings
(not test issues -- the 2 dependencies flagged when these cases were
written did *not* end up being the cause of any of them):

- `by_reference_capture_mutates_and_reads_after_last_use.scpp`: a
  by-reference-capturing closure's borrow does **not** get released at
  its last use the way §5.3's NLL model promises -- writing (or even just
  reading) the captured local directly, right after the closure's own
  last use, is rejected with "cannot use 'x' while it is mutably
  borrowed". Isolated empirically against `std::span` (explicitly compared
  to closures in ch05 §5.12's own text: "exactly like a class holding a
  `T&`/`const T&` field, or `std::span`"): the exact same
  create-once/use-once/touch-the-original-directly-afterward shape
  compiles fine for `std::span`, so span's own borrow correctly releases
  at last use while a closure's captured reference does not. Likely a
  gap in extending NLL release-at-last-use to a closure's internal
  reference-typed capture members specifically.
- `explicit_star_this_capture_is_allowed.scpp`: `[*this]` is rejected
  with a clear, deliberate diagnostic -- "capturing the enclosing object
  by value would need class copy semantics, which don't exist yet -- use
  `[this]` to capture a reference to it instead." This isn't a new gap:
  it's the same pre-existing "no copy semantics for class types"
  limitation `14_classes/class_by_value_parameter_is_rejected.scpp`
  already covers, now surfacing through closures too. `[this]` (by
  reference) and blanket/explicit-member forms are unaffected -- only
  `[*this]` specifically depends on this.
- `generic_lambda_with_auto_parameter.scpp`: a lambda's bare (i.e.
  unconstrained, no concept) `auto` parameter -- "expected a type name".
  Isolated empirically: this isn't lambda-specific -- a bare `auto`
  parameter on an *ordinary* (non-lambda) function fails identically,
  while a `Concept auto` (concept-constrained) parameter works fine (see
  the passing `passing_closure_to_concept_constrained_generic_function.scpp`,
  which uses exactly that form). So generic-function support currently
  only covers the concept-constrained case; the unconstrained C++14
  generic-lambda/generic-function form ch05 §5.12 also claims ("generic
  (C++14, `auto` parameter) lambdas") isn't covered yet.

No known failures in the previously-verified `01_basics`-`17_modules`
categories.

`import ... as` (module aliasing) is **not** a scpp feature -- it was
briefly documented in ch11 §11.8 but turned out to not be real C++20
syntax (verified against cppreference: only `import name;` and
`import name:part;` are standard) and was removed as a documentation bug
(`0413530`). Even though this is no longer a documented rule, it's still
worth confirming scpp rejects the syntax cleanly rather than crashing or
misparsing it -- exactly the kind of thing a Python/Rust programmer might
instinctively try -- so
`13_unsupported_robustness/import_as_aliasing_is_rejected_not_crashed.scpp`
was kept (re-targeted at "unsupported/nonexistent syntax must fail
cleanly" rather than "documented but not yet implemented").


Earlier fixes from the previous verification round, for reference:
- Two `07_extern_c` cases wrongly assumed a with-body `extern "C"`
  function could be called without `unsafe { }` -- ch02's boundary table
  draws no such exception, so both the call sites and (for
  `sum_point_by_pointer`) an internal raw-pointer dereference needed
  `unsafe { }` added.
- `namespace` support turned out to already be implemented (nesting,
  qualified calls) despite ch06 still listing it as backlog at the time --
  moved the wrong `13_unsupported_robustness` case into a new
  `16_namespaces` category of real passing cases, and added a
  `13_unsupported_robustness` case for the one piece confirmed still
  missing (`using foo::bar;` single-name import).
- 4 `17_modules` cases were originally written as single files combining
  `export module X;` with a top-level runnable `main()` -- this silently
  can't link (see the "module file can't also be the runnable program"
  note above). Split each into a 2-file module + plain-consumer pair.
- `17_modules/export_import_re_exports_transitively` initially failed to
  link with "undefined reference to `_scppM1_bF5_valueP0_`" -- a
  transitively re-exported symbol's mangled name incorrectly used the
  re-exporting module (`b`) instead of the defining module (`a`) as its
  prefix. Isolated empirically (calling the same function *indirectly*
  through a `b`-defined wrapper worked fine, isolating the bug to call
  sites reaching a symbol purely through a transitive re-export chain).
  Reported to `src/`; confirmed fixed on re-verification.


