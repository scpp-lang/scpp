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
| `01_basics` | M1: scalars, locals, `if`/`while`, functions, arithmetic, zero-init, unconditional-by-default checking |
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
| `16_namespaces` | basic `namespace` declaration, qualified calls, nesting; `using namespace` rejected |
| `17_modules` | `export module`/`import`, namespace-matches-module-name (ch11 §11.6), cross-module import/export/re-export, bare `extern`, partitions |
| `18_closures` | lambda expressions (ch05 §5.12): by-value/by-reference/init capture, blanket/mixed captures, lifetime-tracking of reference-capturing closures, explicit `this`/`*this` capture, `mutable`, trailing return types, generic lambdas |
| `19_scalar_types` | the full scalar family beyond `bool`/`int`/`char` (ch06), and explicit scalar-to-scalar casts |
| `20_generic_functions` | ch05 §5.11 revisions: full header form (bare/concept-constrained/multi-param/return-type-only), abbreviated bare `auto`, concept-constrained parameter packs |
| `21_generic_types` | generic `struct`/`class` types (ch05 §5.14): bare/concept-constrained type parameters, per-method `requires`, variadic types via recursive inheritance, non-type template parameters, base-class-deduction indexed access |
| `22_lifetime_generic_parameters` | `[[scpp::lifetime(generic)]]` (ch05 §5.13): reserved lifetime group, call-site exemption for closures accepting a callee-chosen lifetime |
| `23_thread_safety_attributes` | `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]` (ch05 §5.15): structural derivation and manual override |

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

**218 cases total, 197 passing.** 137/137 in `01_basics`-`17_modules` plus
17/20 in `18_closures` were previously verified (see below for both
rounds' details); the prior round additionally verified 179/197 after a
large, simultaneous doc + `src/` update (`[[scpp::unsafe]]`, public class
members, generic functions/types, lifetime-generic parameters, and
thread-safety attributes -- see git history for that round's full
writeup). **This round** caught `14_classes` up to a large ch04/ch08
doc update defining `class` copy/move construction and assignment
semantics: move construction and move assignment are always
compiler-synthesized and never user-declarable, for every `class`
unconditionally (ch04 §4.2, ch08 Q14); copy construction and copy
assignment, unlike move, may be user-written, but are compiler-provided
only when a class declares **none** of a copy constructor, a copy
assignment operator, or a destructor itself -- declaring any *one* of
the three suppresses the *other* special member function's automatic
generation too, a deliberate "no mixed state" tightening stricter than
real C++ (ch08 Q15); a class with a reference-typed member may have a
compiler-provided copy/move constructor but never a compiler-provided
copy/move assignment operator. 21 new cases were added to `14_classes`
covering all of this (compiler-provided and user-defined copy
construction/assignment, the "no mixed state" suppression rule in both
directions, user-declared move rejection, the reference-member
carve-outs for both copy and move, self-move-assignment safety, and
destructor-runs-exactly-once after a move, all observed via process
exit code and, where needed, `printf` rather than internal test
scaffolding) -- plus 4 pre-existing cases whose comments cited an
outdated blanket "no copy semantics" rule that ch04 §4.2 has now
superseded (their COMPILE_ERROR outcomes were unaffected: by-value
parameter passing and by-value return remain unconditionally rejected
for a class type regardless of copy-eligibility, confirmed empirically
against a plain compiler-copyable class -- a separate, still-standing
restriction ch04 §4.2's new rules don't relax).

**The single most important finding of the previous round, by far**: most of the
scalar type family is currently unusable, and no explicit scalar-to-scalar
cast works at all (`19_scalar_types`, 8/8 failing) --
`int8_t`/`int16_t`/`int32_t`/`int64_t`, their `uint*_t` counterparts,
`long`, `unsigned int`, `unsigned long`, `float32_t`/`float64_t`/`float`/
`double`, `size_t`, and `ptrdiff_t` **all** fail to parse as a type name
at all (only `bool`, `int`, and `char` currently work), and neither
`static_cast<T>(expr)` nor a C-style `(T)expr` cast is accepted, even
between the two scalar types that do work. This was discovered
incidentally while probing generic non-type template parameters --
nothing in `docs/book/` suggests any of this is a known gap (ch06's
scalar table presents the whole family as unconditionally available, no
"not yet implemented" annotation anywhere).

21 known failures overall, all genuine implementation findings (tests are
spec-conformant, left unchanged; findings from earlier rounds are
summarized only briefly here, see git history for the full original
writeups):

- **`14_classes`** (3 new failures this round, out of 31 cases): (1)
  calling a method on a moved-out class object -- even a `const`,
  read-only getter -- is not rejected at all, even though a *direct
  field read* of the same moved-out object correctly is; confirmed to
  also affect a move-assignment source and a mutating method call, not
  just move-construction (`class_method_call_on_moved_out_object_is_rejected.scpp`).
  (2) a user-declared move *assignment* operator (`operator=(ClassName&&)`)
  compiles successfully, even though the analogous move *constructor*
  form is correctly rejected with a clean diagnostic -- ch08 Q14's
  rejection is only wired up for one of the two syntactic forms
  (`class_user_defined_move_assignment_is_rejected.scpp`). (3)
  dereferencing `this` (`*this`) is rejected outright ("only
  std::unique_ptr or a raw pointer... is supported"), even for a bare
  field read through it, contradicting ch05 §5.9's "`this->x`,
  `(*this).x`... is unchanged" -- this also blocks writing a
  user-defined copy/move assignment operator the idiomatic real-C++ way
  (`return *this;`); the new copy-assignment cases work around it with
  a `void` return instead (`this_dereference_via_star_this_is_allowed.scpp`).
  Also worth noting, not a failure: a class-typed copy-construction
  ineligibility (e.g. a destructor-only class) is currently caught much
  later than the analogous copy-assignment ineligibility -- an LLVM
  module-verification failure rather than a clean sema-level diagnostic
  -- still a clean positive exit status either way, so this doesn't
  affect any test's pass/fail outcome, just diagnostic quality.
- **`19_scalar_types`** (8/8 failing): see above.
- **`20_generic_functions`** (4/6 passing): the full header form (bare,
  concept-constrained, multi-type-parameter, return-type-only) all work
  correctly. Two documented forms don't parse yet: the abbreviated bare
  `auto` parameter (`int f(auto x)` -- confirmed not lambda-specific,
  same gap `18_closures` found), and a concept-constrained parameter pack
  (`const Concept auto&... args`, "expected parameter name but found
  '...'").
- **`21_generic_types`** (7/9 passing): bare type parameters, per-method
  `requires` clauses, generic-struct concept-constraint enforcement,
  variadic types via recursive inheritance (the `Tuple`/`TupleImpl`
  patterns), and non-type parameters combined with a type-parameter pack
  all work correctly. Two findings: (1) a bare (unconstrained) generic
  type parameter incorrectly allows calling a method on it -- e.g.
  `this->item.doubled()` compiles inside `Holder<T>` even though `T` is
  completely unconstrained there, contradicting the once-at-definition
  checking principle §5.11/§5.14 both establish (no error appears until
  a caller instantiates with an incompatible type, which isn't how this
  is supposed to work); (2) a generic type with a *sole* non-type
  parameter (no accompanying type parameter, e.g. `template<int N> class
  X`) fails to instantiate ("expected a type name"), even though the same
  mechanism works fine combined with a type-parameter pack. Also noted,
  not a new failure: general (non-generic, non-variadic) class
  inheritance is still supposed to be deferred (ch04 §4.2) -- it's not
  cleanly rejected at the declaration, but using it produces a confusing
  "cannot access private member" error for an inherited *public* field,
  neither a clean rejection nor working inheritance.
- **`22_lifetime_generic_parameters`** (3/3 passing): the
  `[[scpp::lifetime(generic)]]` tag works fine on an ordinary function
  parameter, and the core "callee invents a value internally and calls a
  passed-in closure with it" pattern works when concept satisfaction only
  needs plain callability. One documented form doesn't parse: tagging a
  `requires`-expression's own probe parameter with the same attribute
  ("expected ')' but found '['").
- **`23_thread_safety_attributes`** (6/6 passing): every structural
  derivation rule tested (scalar types, reference-capturing vs.
  value-capturing closures, raw pointer fields with/without manual
  override) matches the spec exactly.
- **`18_closures`** (17/20 passing, from the previous round): a
  by-reference-capturing closure's borrow doesn't release at last use the
  way NLL promises (contrast `std::span`, which does); `[*this]` is
  rejected by a hardcoded "capturing the enclosing object by value would
  need class copy semantics, which don't exist yet" check -- **re-checked
  this round: still rejected verbatim, even though class copy semantics
  now genuinely exist** (ch04 §4.2) for a plain, compiler-copyable
  receiver like this test's `Box` -- the closure-capture code path simply
  hasn't been updated to use them yet, so this is now a distinct,
  narrower gap than originally described, not just "waiting on copy
  semantics to land"; a lambda's bare `auto` parameter doesn't parse
  (same root cause as the `20_generic_functions` finding above).
- **`12_struct_vs_class`** (3/4 passing, new this round): public member
  variables work correctly for ordinary read/write, but a live borrow of
  a public field does *not* correctly conflict with a call to a mutating
  method on the same object, even though whole-root-conservative
  field-vs-field borrow conflicts (two different public fields) do work
  -- the composition ch04 §4.2 describes ("both already record a borrow
  against the same root object") isn't fully wired up between the field
  path and the method-call path yet.
- **2 findings in `04_references_borrow`**, unrelated to any specific new
  section, discovered incidentally while constructing other tests: a
  `const T&` parameter cannot bind to a temporary/literal argument (only
  to a named lvalue -- `f(42)` fails, `f(v)` for a local `int v` works),
  and a plain `const`-qualified local variable declaration
  (`const int x = 5;`) is rejected outright ("'const' is only supported
  directly before a reference type or a pointer type").
- **1 finding in `06_unsafe_blocks`**: dereferencing the result of
  pointer arithmetic (`*(p + 1)`) isn't supported -- only a bare local
  variable/field dereference is, narrower than ch05 §5.5's listing of
  "indirection through, or pointer arithmetic on, a value of pointer
  type" as one gated operation.

No known failures in `01_basics`-`17_modules` (the categories verified in
earlier rounds).


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


