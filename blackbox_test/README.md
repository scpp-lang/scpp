# scpp black-box tests

> 中文版: [README.zh.md](README.zh.md)

This directory is a **black-box** test suite for the `scpp` compiler. It is
maintained independently from `src/` (the implementation), `docs/book/` (the
reader-facing guide), and `docs/spec/` (the formal language specification):
tests here are written purely by reading the published language docs and
invoking the built `scpp` CLI binary as an external tool, exactly the way any
user of the language would -- there is no dependency on, or knowledge of,
scpp's internal compiler modules.

## How it works

- `cases/<NN_category>/<name>.scpp` -- a small scpp program illustrating one
  documented language rule (cited in a comment at the top of the file,
  pointing at the relevant `docs/book/` or `docs/spec/` section). scpp
  source files use the `.scpp` extension, not `.cpp` (ch08 Q7/Q13): since
  every function is checked unconditionally by default now, an ordinary
  `.cpp` file must never be silently fed to the scpp compiler and checked
  without its author asking for that.
- `cases/<NN_category>/<name>.expected` -- the outcome that program *should*
  produce if `scpp` correctly implements the spec. Three forms:
  1. **A number on the first line**: `scpp` must succeed, and running
     the resulting executable must exit with this code (0-255, POSIX
     `WEXITSTATUS`/shell `$?` semantics -- a process killed by a signal is
     normalized to `128+signum`, e.g. SIGABRT -> 134). Anything after the
     first line is the expected stdout, compared byte-for-byte.
  2. **`COMPILE_ERROR`**: `scpp` must fail with a clean, positive exit
     status (a real diagnostic, not a crash). The exact message text is
     never checked -- the spec doesn't pin down wording.
  3. **`NO_ABORT`**: used only for the handful of cases where a
     scpp-inserted runtime check (span bounds, overflow) is deliberately
     *skipped* inside an `[[scpp::unsafe]] { }` block (ch01 §1.1), so the resulting
     value is genuine, unspecified garbage that can't be pinned down --
     but the process must still terminate normally, not be killed by a
     signal.
- **Optional CLI-case sidecars** for black-boxing the CLI surface itself:
  - `<name>.argv` / `main.argv` -- one argv token per non-blank line,
    passed to `scpp` after placeholder substitution (`$INPUT`, `$OUTPUT`,
    `$TEMP`)
  - `<name>.mode` / `main.mode` -- `command-only` means assert on the CLI
    command's own exit/stdout instead of running a produced executable
  - `<name>.output` / `main.output` -- output file path relative to the
    per-case temp directory (default: `case.bin`); `*`/`**` globs are
    allowed for target-triple-dependent build outputs
  - `<name>.artifacts` / `main.artifacts` -- relative paths that must exist
    after the CLI command succeeds; prefix a path with `!` to assert that
    it must *not* exist
  - `<name>.stderr` / `main.stderr` -- exact expected stderr from the CLI
    command; `$TEMP` expands to the per-case temp directory
- **Multi-file (ch11 module) cases**: some rules (import/export across
  files, partitions, ...) genuinely need more than one source file. A
  directory containing a `main.scpp` file is instead treated as one
  *module test case*, named after the directory:
  - `main.scpp` -- the entry point, compiled and run exactly like an
    ordinary single-file case; `main.expected` is its outcome (same three
    forms as above).
  - `main.imports` (optional) -- one `module_name=relative_path` mapping
    per non-blank, non-`#`-comment line, passed to `scpp` as
    `--import module_name=path` (ch11 §11.14) -- list every module
    `main.scpp` needs, direct or transitive, since `main.scpp` is the only
    file actually compiled as the entry point.
  - any other `.scpp` files in the directory -- the modules referenced by
    `main.imports`; never scanned as their own standalone case.
  - if `main.argv` is present, the entire case directory is copied into the
    temp workspace before invoking `scpp`, so project-build fixtures can
    safely include `scpp.toml`, subpackages, and nested source trees.

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
| `14_classes` | constructors/destructors, default member initializers and constructor member-initializer lists, private access control, compiler-provided/user-defined copy construction and assignment, compiler-only move construction and assignment, method borrow checking, `this` |
| `15_function_overloading` | exact-type-match resolution, by-value/by-reference axis, const/non-const methods |
| `16_namespaces` | basic `namespace` declaration, qualified calls, nesting, same-namespace unqualified class lookup, and leading `::` global-scope lookup; `using namespace` rejected |
| `17_modules` | `export module`/`import`, namespace-matches-module-name (ch11 §11.6), cross-module import/export/re-export, bare `extern`, partitions, and a workspace/path-dependency build whose cross-module `.scppm` binary artifact must correctly round-trip a still-generic exported class's template-parameter-dependent array bound |
| `18_closures` | lambda expressions (ch05 §5.12): by-value/by-reference/init capture, blanket/mixed captures, lifetime-tracking of reference-capturing closures, explicit `this`/`*this` capture, `mutable`, trailing return types, generic lambdas |
| `19_scalar_types` | the full scalar family beyond `bool`/`int`/`char` (ch06), explicit scalar-to-scalar casts, and comparison rules for same-type vs mixed-type scalars |
| `20_generic_functions` | ch05 §5.11 revisions: full header form (bare/concept-constrained/multi-param/return-type-only), abbreviated bare `auto`, concept-constrained parameter packs |
| `21_generic_types` | generic `struct`/`class` types (ch05 §5.14): bare/concept-constrained type parameters, per-method `requires`, variadic types via recursive inheritance, non-type template parameters, base-class-deduction indexed access |
| `22_lifetime_any_parameters` | `[[scpp::lifetime(any)]]` (ch05 §5.13): reserved lifetime group, call-site exemption for closures accepting a callee-chosen lifetime, and `[[scpp::lifetime(...)]]` on `requires(...)` probe parameters constraining concept satisfaction (any/named-group matching, untagged no-op, non-reference rejection) |
| `23_thread_safety_attributes` | `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]` (ch05 §5.15): structural derivation and manual override |
| `24_function_pointers` | function pointers (ch05 §5.16): real C/C++ syntax, the unsafe-qualified/not-unsafe-qualified type split, automatic address-type selection (ordinary / `[[scpp::unsafe]]` / bodyless `extern "C"` / with-body `extern "C"`), one-directional conversion, struct-member legality, copyability, `&overloaded_name` target-type resolution |
| `25_function_wrappers` | `std::function` / `std::move_only_function` (ch05 §5.18): copyable vs move-only targets, cv/ref-qualified signatures, moved-from behavior |
| `26_threads` | `std::thread` / `std::jthread`: thread-movable constructor constraint, join/detach/joinable transitions, `jthread` destructor auto-join |
| `27_unions_packed_layout` | union member unsafe-gating and `[[scpp::packed]]` layout/FFI behavior, including the Linux `epoll_event` / `epoll_data_t` pattern |
| `28_cli_invocation` | CLI surface: direct `scpp file.scpp` builds, default/custom output names, removed `build` keyword, and surviving `lex`/`parse`/`build-module` subcommands |
| `29_project_build` | manifest-based project builds: single-package `build`, workspace/path dependencies, direct-dependency visibility, package selection, and rejection of deferred manifest features |
| `30_constant_evaluation` | formal-spec-driven `constexpr`/`consteval` coverage: required constant evaluation, `if consteval` / `if !consteval`, unsupported v1 operations, and the later-pack-to-earlier-parameter deduction rule |
| `31_enum_class` | scoped enumerations: `enum class` declaration, scoped enumerator access, enum-type separation, explicit casts, and explicit underlying types/values |
| `32_sizeof_storage_lifetime` | `sizeof(type)` / `sizeof(expr)`, the `alignas`-qualified raw-`char`-array idiom for max-sized/aligned storage (replacing the removed `std::storage_for<T, ...>` builtin), placement-new, explicit destructor-call syntax, and polymorphic-class `sizeof`/`alignof` accounting for the implicit vtable pointer |
| `33_nodiscard` | `[[nodiscard]]` / `[[nodiscard("reason")]]` on functions and types, including discard diagnostics and allowed non-discarding uses |
| `34_expected_and_cstdlib` | `std::expected<T, E>` / `std::unexpected<E>` state behavior, misuse aborts, and `std::abort()` itself |
| `35_random` | `std::random_device`, `std::mt19937`, and `scpp::rand::uniform_int_distribution<int>` |
| `36_charconv` | `std::from_chars` integer parsing: success, partial consumption, errors, signs, and explicit base |
| `37_for_loops` | classic `for` loops, range-based `for` over arrays/`std::span`, and iteration-mode mutation rules |
| `38_lifetime_groups` | spec-driven coverage for cross-function named lifetime groups: named-vs-any groups, return-group matching, storage/escape rejection, member functions, templates, and thread-safety interaction |
| `39_ordinary_virtual_dispatch` | ordinary virtual dispatch: override selection across base/derived references and pointers, including chained forwarding through helpers |
| `40_operator_arrow` | `operator->`: recursive arrow chaining, cv-correct selection, ordinary-vs-unsafe call gating, and raw-pointer leaf requirements |
| `41_global_variables` | file-scope/global variable declarations: plain globals, const globals, cross-function mutation, and `alignas` acceptance/rejection rules |
| `42_array_bound_expressions` | array declarators (ch05 §9.4): literal/`sizeof`/`alignof`/arithmetic/global-`constexpr`-named-constant bounds applied uniformly at local-variable, struct/class-field, and function-parameter sites; rejection of non-constant, zero, negative, and self-referential-incomplete-type bounds; a generic type's `sizeof(T)`-dependent bound resolved independently per instantiation; a ternary bound over two template parameters' `sizeof`s resolving correctly for both branches (each selected by a different instantiation); and the current local-`constexpr`-as-later-local-bound scope boundary |
| `43_forward_declarations` | ordinary function forward declarations at namespace/module scope, exported-forward-declaration reconciliation, struct/class forward declarations, tag-kind mismatch diagnostics, and the deliberate permissive by-value-use-before-later-definition behavior |

## Testing philosophy

- Every `.scpp` file is written to be **valid per the published language
  docs** (`docs/book/` and, where newer work has only landed formally so
  far, `docs/spec/`) -- if a test fails, check the cited doc section first.
  If the test itself turns out to violate the spec, fix the test. If it's
  genuinely spec-conformant and still fails, that's an implementation bug,
  logged here for the `src/` maintainer to fix -- this suite does not modify
  `src/` to work around failures.
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
- **One `17_modules` case genuinely does compile a module to `.scppm`
  first**: a workspace/path-dependency build (`scpp build --workspace`,
  `main.argv` invoking it rather than `main.imports`) is the *only*
  mechanism in this suite that produces and imports a real `.scppm`
  binary artifact -- `src/project.cppm`'s manifest-build pipeline writes
  one to disk for each path dependency and points the importing package's
  module resolution at that compiled file, not at raw source. This is
  what's needed to exercise cross-module binary (de)serialization bugs at
  all (verified: an ordinary `--import name=path` case, per the note
  above, never round-trips through `.scppm`).
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
  When in doubt, a quick probe with `scpp file.scpp` or another direct CLI
  invocation settles it empirically.
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

- **534 cases total**
- **534/534 passing**
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
  - move-only class returns now also cover `return T{...};`, bare local
    implicit move, and bare by-value-parameter passthrough
  - `std::string` now has dedicated fresh-value return coverage too
  - the new implicit-move rule's scope boundaries stay pinned down:
    member access, reference parameters, differently-typed locals, and
    globals still reject as required
- **Required-initialization semantics now have dedicated black-box coverage**:
  bare local declarations are rejected, valid local forms (`{}`, `{args}`,
  and `= value`) succeed, constructors may mix in-class defaults with
  member-initializer lists, reference members bind correctly, completeness
  is checked per constructor, and member initialization follows declaration
  order rather than initializer-list order
- **`for` loops and reborrows now have dedicated black-box coverage too**:
  classic `for` loops count up/down, skip when initially false, nest, and
  accept both existing-variable and declaration init-clauses; range-based
  `for` covers arrays and `std::span` in by-value / `auto&` / `const auto&`
  forms; and reborrows now directly cover the lender-read-allowed rule, the
  lender-write/further-reborrow rejections, and lender reuse after the
  child borrow's last use
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
- **Union / packed-layout coverage now exists in direct black-box form**:
  unsafe-gated union member access, raw-byte packed-struct layout, and the
  real Linux `epoll_event` / `epoll_data_t` FFI declaration shape
- **Low-level size/storage/lifetime building blocks now have direct
  black-box coverage too**:
  `sizeof(type)` / `sizeof(expr)`, the `alignas`-qualified raw-array
  storage idiom, placement-new, explicit destructor calls, and leading
  `::` global-scope lookup
- **`[[nodiscard]]` now has direct black-box coverage too**:
  function-level and type-level nodiscard, reason-string diagnostics, and
  ordinary consuming uses that must stay accepted
- **`std::expected` / `std::abort` now have direct black-box coverage too**:
  success/error construction, inline storage with non-default-constructible
  values, bad-access aborts, and the direct `std::abort()` process abort path
- **Enum conversions now cover the checked integer-to-enum path too**:
  `scpp::enum_cast<T>(value)` success/error results, while ordinary
  int-to-enum casts stay rejected and explicit enum-to-int casts still work
- **Random support now has direct black-box coverage too**:
  `std::mt19937` same-seed reproducibility, and
  `scpp::rand::uniform_int_distribution<int>::min()` / `.max()` accessors
- **`std::from_chars` now has direct black-box coverage too**:
  full-input success, trailing unconsumed characters, invalid-argument and
  out-of-range errors, negative numbers, rejected leading `+`, and both
  the 3-argument and 4-argument overloads
- **CLI invocation now has direct black-box coverage too**:
  bare `scpp file.scpp`, `-o custom_name`, rejection of the removed
  `build` keyword, and spot-checks that `lex`, `parse`, and
  `build-module` still work explicitly
- **Manifest-based project builds now have direct black-box coverage too**:
  single-package lib/bin builds, workspace/path-dependency builds,
  `-p` package selection, direct-only compile-time visibility, and
  rejection of still-deferred manifest features like registry deps,
  `[workspace.dependencies]`, and `[native]`
- **Array bound constant-expressions (ch05 §9.4) now have dedicated
  black-box coverage**: literal/`sizeof`/`alignof`/arithmetic/global-named-
  constant bounds accepted uniformly across local-variable, struct/class-
  field, and function-parameter declarator sites; non-constant (not a
  VLA), zero, negative, and direct/indirect self-referential-incomplete-
  type bounds rejected with clean diagnostics; a generic class's
  `sizeof(T)`-dependent bound resolved independently and correctly across
  four distinct instantiations; and the current, deliberate scope
  boundary around a local `constexpr` constant used as a later local
  array's bound in the same function
- **Three new regression cases cover compiler bugs found and fixed while
  replacing `std::storage_for<T, ...>` with the `alignas`-qualified
  raw-`char`-array idiom**:
  - `32_sizeof_storage_lifetime/polymorphic_class_sizeof_and_alignof_account_for_implicit_vtable_pointer`:
    a polymorphic class's `sizeof`/`alignof` now correctly accounts for
    its implicit leading vtable pointer (previously silently
    under-reported for any class with fields following the vtable slot),
    including a case where the vtable pointer alone determines the
    class's overall alignment
  - `42_array_bound_expressions/ternary_array_bound_over_two_template_parameters_resolves_both_branches_per_instantiation`:
    a ternary array bound over two template parameters' `sizeof`s now
    resolves correctly regardless of which branch a given instantiation
    selects (previously, only the `?:` then-branch was substituted
    during generic monomorphization; the else-branch silently failed to
    resolve)
  - `17_modules/cross_module_generic_array_bound_with_polymorphic_type_argument`:
    a still-generic exported class's template-parameter-dependent array
    bound now survives the `.scppm` cross-module binary round trip when
    instantiated with a polymorphic type argument across a module
    boundary (previously, only the resolved integer was serialized,
    never the unresolved expression tree needed to resolve it
    per-instantiation on the importing side -- silently freezing the
    array bound at 0)
