# 11. Modules & Libraries

scpp programs can span multiple files: there's a notion of one scpp file
calling into another, and a way to distribute a scpp API for other scpp
code to consume. This chapter specifies how scpp programs span multiple files,
and what a "library" is in scpp terms. It's a distinct problem from
[§2.1](ch02-boundary-rules.md)'s `extern "C"`: that mechanism bridges to
*actual C* (libc, or any other C library); this chapter is about
scpp-to-scpp code sharing. Interop with *existing, unmodified C++
libraries* specifically (arbitrary classes, templates, overloads,
exceptions, RTTI) is explicitly **not pursued** -- `extern "C"` is scpp's
only interop mechanism with the outside world (see
[§8](ch08-open-questions.md) item 6 for the reasoning).

## 11.1 Why C++20 modules, not `#include` headers

scpp reuses real C++20 module syntax verbatim -- `export module name;`,
`import name;`, `export` on individual declarations -- rather than the
classic `#include`-a-header model, and rather than inventing something
new. Three reasons, in order of importance:

1. **Soundness.** A header is a manually-written, textually-pasted
   forward declaration. Nothing stops it from silently drifting out of
   sync with the real definition in another translation unit -- and in
   scpp, unlike ordinary C++, a function's declared *safety-relevant
   facts* (its `[[scpp::lifetime(name)]]` groups) are part of what a
   caller's borrow check depends on. A
   hand-copied declaration that lies about them would be a silent
   soundness hole. This directly contradicts
   [ch00](ch00-design-philosophy.md)'s "soundness over compatibility". A
   module interface is compiled once from ground truth and *imported*,
   never retyped, so this entire class of bug is structurally impossible
   rather than merely discouraged.
2. **No preprocessor needed.** scpp has no preprocessor -- there is
   no `#define`, no macro expansion, no conditional compilation anywhere
   in the language. C++20 modules need none of that either.
3. **Reuse known syntax.** `export module`/`import` is real, modern,
   idiomatic C++. It's also, pleasantly, already what the compiler's own
   implementation is written in: every `src/*.cppm` file is itself an
   `export module scpp.xxx;` with an `export namespace scpp { ... }`
   surface.

## 11.2 What a "library" is in scpp (no new keyword)

"Library" is not a language keyword and needs none: modules distribute
together as one `.scppkg` package -- as raw `.scpp` interface source, or
as `.scppm` interfaces paired with their compiled `.scppa` archives
(themselves bundling `.scppo` objects, one per contributing file), or a
mix -- (see [§11.12](#1112-the-scppm-scppa-and-scppkg-formats)) --
exactly how "library" already works in real C++ (Boost is "a library" as
a social and build-system convention; nothing in the C++ grammar defines
the word). A package manager, registry, or dependency-resolution tool is
out of scope for this chapter (see [§11.15](#1115-out-of-scope-for-v1-backlog)).

## 11.3 Export surface and the interface/implementation split

```cpp
// mylib_math.scpp -- the module's interface. Shareable, human-readable.
export module mylib.math;

namespace mylib::math {
    extern int square(int x);               // exported, bodyless -- see §11.7
    export struct Point { int x; int y; };  // exported, always has "a body" (it's data)
}

int internal_helper(int x) { return x * 2; }  // private, has a body -- namespace irrelevant, not exported
```

```cpp
// mylib_math_impl.scpp -- an implementation unit: not exported, not
// shared. Automatically sees mylib.math's own interface without import.
module mylib.math;

int mylib::math::square(int x) { return x * internal_helper(x) / 2; }
```

- A file becomes a module's **primary interface unit** by starting with
  `export module name;` (at most one per module, exactly like real
  C++20). A file with no module declaration is an ordinary,
  non-exporting file, exactly like an ordinary scpp file with no module
  involvement at all.
- A file starting with `module name;` (no `export`) is an
  **implementation unit**: it contributes additional code to that same
  module, and automatically has access to the module's own interface
  without needing to `import` it. Building a module means compiling its
  primary interface unit together with all of its implementation units
  and partitions ([§11.4](#114-module-partitions)).
- `export` prefixing an individual declaration (or grouping several
  inside `export { ... }`) marks it visible to importers; anything else
  is private to the module.
- v0.1's exportable surface is exactly v0.1's supported subset
  ([§6](ch06-safe-subset.md)): free functions and
  `struct` definitions. `class`, templates, etc. gain export support
  automatically whenever they exist. Every exported declaration must
  additionally live inside a namespace matching the module's own name --
  see [§11.6](#116-exported-declarations-must-live-in-a-namespace-matching-the-module-name).
- **This revises an earlier scoping call.** Multi-file modules were
  originally deferred entirely as out of scope for v1; supporting
  compiled-payload distribution (not just source distribution) turns out
  to require at least the interface-unit/implementation-unit split, so
  that split is in scope for v1 after all. **Module partitions**
  (`export module foo:part;`, a finer-grained subdivision *within* one
  module) are in scope for the same reason -- see
  [§11.4](#114-module-partitions).

## 11.4 Module partitions

A module's own declarations can spread across more than one file --
without a preprocessor's textual `#include`, and without allowing more
than one primary interface unit ([§11.3](#113-export-surface-and-the-interfaceimplementation-split))
-- via **module partitions**, reused verbatim from real C++20:

```cpp
// mylib_math_trig.scpp -- an interface partition
export module mylib.math:trig;

namespace mylib::math {
    export double sin_deg(double degrees);
}
```

```cpp
// mylib_math_detail.scpp -- an implementation partition
module mylib.math:detail;

namespace mylib::math {
    double poly_approx(double x) { /* ... */ }
}
```

```cpp
// mylib_math.scpp -- the primary interface unit
export module mylib.math;

export import :trig;   // re-exports :trig's own exported declarations
import :detail;         // uses :detail internally, does not re-export it

namespace mylib::math {
    export double sqrt(double x);
}
```

- A partition file's module declaration names the module and, after a
  colon, the partition: `export module name:part;` (an **interface
  partition**, itself able to `export` declarations) or `module
  name:part;` (an **implementation partition**, never able to export
  anything to the outside). Each partition name designates exactly one
  file within the module.
- **Partitions are visible only from inside their own named module** --
  a file outside module `mylib.math` can never `import mylib.math:trig;`
  directly, exactly like real C++20. The only way a partition's content
  reaches an external importer is via the primary interface unit
  aggregating it (below).
- **Within the module**, any unit that imports a partition (`import
  :part;`) sees every declaration in it, exported or not -- a
  partition's own `export` only controls whether the *primary* interface
  unit is later allowed to re-export it, not whether sibling units of the
  same module can see it.
- **The primary interface unit aggregates partitions** in one of two
  ways: `export import :part;` re-exports an interface partition's own
  exported declarations to anyone importing the module as a whole; a
  plain `import :part;` uses a partition's declarations internally
  (visible to the primary and other partitions) without exposing them
  further. **Attempting `export import` on an implementation partition is
  a compile error** -- an implementation partition's content can never
  reach an external importer, by construction, matching real C++20.
- **Building a module** ([§11.3](#113-export-surface-and-the-interfaceimplementation-split))
  means compiling its primary interface unit together with all of its
  implementation units *and* partitions. Partitions are purely a
  source-organization mechanism for the module's own author: the
  distributed `.scppm` file remains exactly one file per module, holding
  the fully merged interface -- partition names never appear inside a
  `.scppm` file or a `.scppkg` manifest (see
  [The `.scppm` Module Interface Format](../../standards/en/scppm-format.md)).

## 11.5 Namespaces

scpp reuses real C++ `namespace` syntax verbatim, including C++17's
one-line nested-namespace definition (`namespace a::b::c { ... }`). A few
deliberate restrictions apply, each consistent with a decision already made
elsewhere in this spec:

- **No `using namespace`, anywhere** -- not even function-local. The only
  import form is a single-name using-declaration, `using foo::bar;` (the
  same rule already applies to ordinary names; namespace members follow
  it too). Blanket-importing every name in a namespace reintroduces
  exactly the "which `x` did that come from" ambiguity
  [ch00](ch00-design-philosophy.md) §2/§6 exists to design away.
- **No anonymous namespaces.** A module's own export surface (`export` on
  individual declarations, [§11.3](#113-export-surface-and-the-interfaceimplementation-split))
  already provides "private to this compilation unit"; an anonymous
  namespace would be a second, redundant mechanism for the same job.
- **No argument-dependent lookup (ADL), ever.** Every unqualified call
  resolves purely from lexical scope and explicit `using` declarations,
  never from an argument's type. This is a **permanent** decision, not a
  placeholder -- function overloading
  ([§5.10](ch05-static-checks.md)) doesn't need it either (its candidate
  set is exactly the same lexical-scope-and-`using`-declaration lookup
  ordinary names already use), but the alternative would reintroduce
  exactly the "an unrelated new import silently changes what an existing
  call means" class of spooky action ADL is well known for in real C++,
  which conflicts with [ch00](ch00-design-philosophy.md) §2/§6.
- **Namespace aliases reuse real C++'s actual alias syntax**:
  `namespace cmath = org::lotx::cmath;` -- deliberately **not** a
  `using`-declaration. `using X = Y;` is a *type* alias in real C++, and a
  namespace is not a type; spelling a namespace alias that way would not
  survive stripping `unsafe` and handing the result to a real C++
  compiler ([ch00](ch00-design-philosophy.md) §6). This is a second,
  orthogonal mechanism alongside `using foo::bar;` (imports one *name*,
  not a whole namespace) -- both can be used together freely.
- **Namespace and module are otherwise orthogonal**, exactly as in real
  C++: a namespace is a purely logical grouping of names; a module is a
  physical compilation/import boundary. The one deliberate exception is
  described next.

## 11.6 Exported declarations must live in a namespace matching the module name

```cpp
export module org.lotx.cmath;

namespace org::lotx::cmath {
    export double sqrt(double x);   // OK -- namespace matches module name
}

double helper(double x) { return x; } // OK -- not exported, namespace irrelevant
```

- **Rule**: an `export`-marked declaration only actually exports if it's
  lexically inside `namespace <M1>::<M2>::...::<Mn> { ... }`, where
  `M1.M2. ... .Mn` is exactly the current module's own dotted name (module
  names use `.`, namespace paths use `::` -- translate one to the other
  segment-for-segment). This is a **prefix** requirement, not an
  exact-match one: a module may nest arbitrarily deeper for its own
  internal organization (`org::lotx::cmath::trig`,
  `org::lotx::cmath::stats`, ...), and every such nested declaration is
  still exportable, since its namespace still starts with the required
  prefix. An `export` on a declaration anywhere else (wrong namespace, or
  no enclosing namespace at all) is a **compile error** -- `export` and
  "lives in the required namespace" are two independent gates, both
  mandatory. Non-exported declarations are unaffected by any of this and
  may live in any namespace (or none).
- **Deliberately no implicit/default namespace.** An earlier draft of this
  rule had the module declaration itself silently establish an implicit
  enclosing namespace, so an interface file wouldn't need to spell out the
  wrapper at all. **Rejected**: real C++ has no concept of "a namespace
  that's active without being textually written" -- if scpp invented one,
  a real C++ compiler fed the erased file would place the same declaration
  in the *global* namespace instead, silently disagreeing with scpp's own
  model of where the symbol lives. That's exactly the implicit,
  non-erasable magic [ch00](ch00-design-philosophy.md) §2/§6 rules out.
  The actual cost of the explicit version is one line per interface file,
  thanks to C++17's nested-namespace-definition syntax -- not the
  three-level pyramid older C++ would have required.
- **Why bother**: this turns real C++'s single worst include-hygiene pain
  point (`std::filesystem::path` -- which header? -- answered today only
  by IDE-maintained heuristic lookup tables) into a **mechanically
  guaranteed** fact in scpp: any fully-qualified name determines exactly
  one module to `import`, full stop. It also upgrades
  [§11.10](#1110-symbol-identity-linkage-and-mangling)'s existing
  "domain-qualified module names are a *recommended convention*, not
  compiler-enforced" note -- the namespace-matches-module-name half of
  that convention is now an enforced rule, not just a suggestion (the
  *choice* of a domain-qualified name at all, e.g. `org.lotx.cmath` over a
  bare `cmath`, remains a convention -- the compiler still can't stop two
  unrelated authors from both picking the bare name `cmath`).
- **Module names read outermost-to-innermost, reversed-domain style**
  (`org.lotx.cmath`, matching Java's package-naming convention), not
  forward/URL style (`cmath.lotx.org`) -- reading left to right must agree
  with how the resulting namespace path reads (`org::lotx::cmath::sqrt`,
  the specific library name innermost/last).
  [§11.10](#1110-symbol-identity-linkage-and-mangling)'s convention example
  uses this order.
- **Qualified-name resolution across imported modules**: given a
  reference like `org::lotx::cmath::sqrt(...)`, resolution walks the
  name's segments and finds the **longest prefix that exactly equals an
  imported module's dotted name** (checked only against modules the
  current file actually imports, never every module that merely exists on
  the search path, [§11.14](#1114-importlibrary-search-path)); the
  remaining suffix is then looked up as a namespace path nested inside
  that module's export surface. E.g. with both `org.lotx.cmath` and
  `org.lotx` imported, `org::lotx::cmath::sqrt` resolves against
  `org.lotx.cmath` (module) + `sqrt` (its own top-level export) --
  `org.lotx` isn't even consulted; only if `org.lotx.cmath` weren't
  imported would resolution fall back to `org.lotx` (module) +
  `cmath::sqrt` (a nested namespace within *its* surface). **If two
  different imported modules could both resolve the same qualified name**
  (e.g. `org.lotx.cmath` is imported and exports `sqrt` at its own top
  level, *and* `org.lotx` is also imported and separately has a nested
  `cmath::sqrt`) -- this is a **compile error ("ambiguous qualified
  name")**, not a silent longest-match-wins pick, for the same reason ADL
  is rejected above: an unrelated, later-added `import` should never
  silently change what an existing qualified name resolves to. If no
  imported module's name matches any prefix at all, it's a compile error
  naming the unresolved segment (a "did you forget to import X"
  diagnostic is the intended quality bar, not just "undeclared
  identifier").

## 11.7 Bare `extern` for module-linkage bodyless declarations

`extern` without a `"C"` string declares a function with **ordinary scpp
linkage** (not C ABI), whose implementation lives in a separate
implementation unit or a separately-distributed `.scppo` object file
([§11.12](#1112-the-scppm-scppa-and-scppkg-formats)):

```cpp
extern int square(int x);                // ordinary scpp linkage, checked like any other function
extern "C" int printf(const char*, ...); // C ABI, per §2.1, always requires unsafe { } to call
```

Unlike `extern "C"` -- whose implementation no
scpp compiler ever sees, so calling it **always** requires `unsafe { }`
(§2.1) -- calling a bare
`extern` declaration needs no `unsafe { }` at all. The trust model is
different: when the module's author builds the primary interface unit
together with its implementation unit(s), the compiler checks that every
implementation-unit definition's signature matches its interface
declaration *exactly*, once, at that build, and that definition is
checked ([§5](ch05-static-checks.md)) exactly like any other scpp
function. This is the same
declaration-matches-definition trust ordinary C++ already places in
separately-compiled translation units (assumed, rarely mechanically
verified in plain C++) -- scpp's version is actually checked, at least
once, by a real scpp compiler, which is strictly *more* than plain C++
guarantees, not less.

## 11.8 Import visibility and re-exports

- `import name;` is **private, non-transitive**: it makes `name`'s
  exports visible in the importing file, but does not forward them to
  whoever imports *that* file in turn.
- `export import name;` **re-exports**, transitively.

## 11.9 Soundness: cross-module signatures are all the checker needs

[§5.3](ch05-static-checks.md) already establishes that v0.1's borrow
checking is **intraprocedural**: checking a call to `g` inside `f` only
ever consults `g`'s *signature* (param types,
return type, its `[[scpp::lifetime(name)]]`
groups), never `g`'s body. This means modules require **no new checking
model**: the same signature lookup an ordinary same-file call already uses
works unchanged for a cross-module call, seeded with the
signature entries recovered from each imported module's
interface -- same lookups, same checks, only a second source of
entries.

Struct layout carries over the same way: [§4.3](ch04-struct-vs-class.md)
already pins a struct's layout to a pure function of (field list, target
triple) -- an imported struct's field list, recovered from the interface,
is all codegen needs to reproduce a byte-identical layout.

## 11.10 Symbol identity: linkage and mangling

Two different rules for two different cases, matching how much a symbol
actually needs to be visible outside its own compiled unit:

- **Module-private (non-exported) functions** are emitted with LLVM
  `internal` linkage (the same mechanism as C's `static` or C++'s
  anonymous namespace). Internal-linkage symbols are never unified across
  separately-compiled files by the linker, so two unrelated modules can
  each have their own private `internal_helper` with zero risk of
  collision -- **no mangling is needed for these at all**.
- **Exported functions** need external linkage (so other files can call
  them), which means the linker *does* need every such symbol to be
  globally unique. These get a module-qualified mangled name:
  ```
  _scppM<len>_<module name bytes>F<len>_<function name bytes>
  ```
  Length-prefixed (not delimiter-based) so no module/function name choice
  can ever produce an ambiguous encoding. This is purely an internal
  linker-visible detail -- nobody types it, so there's no need for it to
  be pretty.
  - **Namespace nesting beyond the module name**: since
    [§11.6](#116-exported-declarations-must-live-in-a-namespace-matching-the-module-name)
    requires every exported symbol's namespace to *start with* its own
    module's dotted name, the `<module name bytes>` segment above already
    encodes that shared prefix -- there's no need to re-encode it a second
    time. Only namespace nesting *beyond* the required prefix (e.g. module
    `org.lotx.cmath` additionally organizing its exports under
    `org::lotx::cmath::trig`) needs its own encoding: one `N<len>_<segment
    bytes>` block per extra nesting level, inserted between the module and
    function segments, e.g. `_scppM14_org.lotx.cmathN4_trigF3_sinF...` for
    `org.lotx.cmath`'s `trig::sin`. A symbol exported directly at the
    module's own required namespace (no extra nesting) has zero `N<len>_`
    blocks.
  - **Parameter-type encoding, now specified**: this slot was originally
    reserved with no encoding defined pending a function-overloading
    design; [§5.10](ch05-static-checks.md) now specifies function
    overloading, so the slot is filled in:
    `P<count>_` followed by one length-prefixed, verbatim type spelling
    per parameter (e.g. `7_int32_t`, `8_int32_t&`, `14_const int32_t&`) --
    consistent with this whole scheme's length-prefixed style, not a
    bespoke single-letter abbreviation table like the Itanium ABI's. E.g.
    `f(int32_t, const double&)` mangles its parameter segment as
    `P2_7_int32_t13_const double&`. This is enough on its own: since
    [§5.10](ch05-static-checks.md) resolves overloads by exact type match
    only (no implicit-conversion ranking, per [§6](ch06-safe-subset.md)'s
    no-implicit-scalar-conversion rule), the mangled name only ever needs
    to record each parameter's exact spelling, never a family of
    possible-conversion types. Return type is deliberately **not** encoded
    (C++'s own rule: you cannot overload on return type alone).
  - **No Rust-style crate disambiguator hash.** Rust bakes an extra hash
    into every mangled symbol specifically to let *the same crate name at
    two different versions* coexist safely in one build -- scpp v0.1 does
    not support that (whichever `.scppm` is found first on the search
    path wins, full stop; see [§11.14](#1114-importlibrary-search-path)),
    so the problem that hash exists to solve doesn't arise here. The
    residual risk -- two unrelated libraries picking the literal same
    module name -- is handled exactly like C/C++ has always handled
    global symbol collisions: a hard error, fixed by renaming (see
    [§11.11 below](#1111-collision-handling)), not a cryptographic
    workaround.
  - **Choosing a domain-qualified name is a convention, not
    compiler-enforced** -- e.g. `org.lotx.cmath`, read
    outermost-to-innermost/reversed-domain style like a Java package (not
    forward/URL style, `cmath.lotx.org` -- see
    [§11.6](#116-exported-declarations-must-live-in-a-namespace-matching-the-module-name)
    for why the direction matters once namespace-matching is involved).
    This is strongly recommended for anything meant to be shared, both
    because it makes accidental collisions astronomically unlikely
    (nobody else can claim your registered domain) and because it doubles
    as a locator (readers know where to find support), mirroring Go's
    `github.com/user/repo` import paths -- exactly like Java's
    package-naming convention, the compiler does not require or check
    *this part*; a flat name like `cmath` remains fully valid for anything
    not meant for wide distribution. What **is** compiler-enforced,
    unlike in real C++ or Java, is that *whichever* name a module picks,
    its exports must live in the namespace that name maps to -- see
    [§11.6](#116-exported-declarations-must-live-in-a-namespace-matching-the-module-name).
- `extern "C"` symbols are **never** mangled by this scheme -- they use
  the bare, unmangled name, which is the entire point of C linkage. The
  two schemes are orthogonal.

## 11.11 Collision handling

- Two `import`s of differently-named-but-colliding modules **directly**
  in the same file: caught at **compile time**, cleanly, before codegen
  even runs (the existing rule: pick different names -- exactly like
  real C++, scpp has no import-site renaming mechanism to fall back on).
- Two modules **indirectly** pulled into the same final link (neither
  imported directly by the same file, e.g. both are dependencies of a
  third module that doesn't re-export either): if their compiled exported
  symbols collide, the **system linker** reports an ordinary duplicate-symbol
  error at link time. This is exactly how plain C/C++ has always
  behaved for colliding global symbols -- scpp isn't trying to be more
  clever than that, on purpose (see [§11.10](#1110-symbol-identity-linkage-and-mangling)'s
  reasoning for not adopting Rust's disambiguator hash).

## 11.12 The `.scppm`, `.scppa`, and `.scppkg` formats

A module's interface packages as one `.scppm` file. Its compiled machine
code packages separately: one `.scppo` object per contributing file
(primary interface unit, implementation unit, or partition -- §11.3,
§11.4), bundled by the target platform's own native static-archive tool
into one `.scppa` file per target triple -- a real static library
(`.a`/`.lib`), not a format scpp invents. One or more modules -- as raw
`.scpp` interface source, or as `.scppm` interfaces paired with their
`.scppa` archives, or a mix -- package together as one `.scppkg` file for
distribution as a whole library. `.scppm`'s byte layout is specified in
[The `.scppm` Module Interface Format](../../standards/en/scppm-format.md);
`.scppkg`'s byte layout and manifest schema (versioning, dependency
records, and the per-module source/binary split) are specified in
[The `.scppkg` Package Format](../../standards/en/scppkg-format.md).

## 11.13 Linking: which objects get linked

Linking is decoupled from `import` resolution
([§11.14](#1114-importlibrary-search-path)) -- exactly like real C++20,
where `import` supplies declarations for compilation only and has no
bearing on what the linker sees (a real C++ build still needs its own
`target_link_libraries` line, or equivalent, regardless of which modules
a file `import`s). At link time, the scpp build tool links:

- every `.scppo` object produced by building the current project's own
  modules (compiled directly, same as any ordinary same-project
  multi-file build -- no archiving needed for code that isn't being
  distributed), and
- every `.scppa` archive bundled inside every `.scppkg` package the
  project depends on, transitively across the whole dependency graph --
  regardless of which specific modules any single file actually
  `import`s.

This is deliberately blunt, mirroring an ordinary `g++ *.o -lfoo -lbar`
build or a `target_link_libraries` line naming every dependency
unconditionally: the system linker already discards whatever isn't
actually referenced -- from loose `.o` files as well as from a linked-in
`.a`/`.lib` archive's own member index -- exactly as in any ordinary
C/C++ build. `.scppa` being a real static-archive format means the linker
reads it directly with zero scpp-specific handling; scpp does not need
its own, separate per-`import` object selection to achieve any of this.

For both the project's own objects and every dependency's archives, only
the one matching the **current build's target triple** is a candidate
for linking; if a needed module provides no `.scppo`/`.scppa` for that
triple at all, it's a hard error naming which triples it does provide
(unless the interface file happens to include a full body for the
function in question, in which case compiling that inlined source is an
available fallback).

## 11.14 Import/library search path

- `scpp build <file> --import name=path` -- explicit,
  unambiguous, always works (mirrors Clang's `-fmodule-file=name=path`
  and Rust's `--extern name=path`): `path` names either a `.scppm` file
  providing module `name` directly, or a `.scppkg` file whose own manifest
  lists a module named `name` (nested as a `.scppm` file or as raw
  `.scpp` source).
- `scpp build <file> -I <dir>` -- convenience search: to resolve
  `import mylib.math;`, look in each `-I` directory (in order) for a file
  named `mylib.math.scppm` (§11.12), or a `.scppkg` file whose own manifest
  lists a module named `mylib.math`. First directory (in the order given)
  that contains a match wins -- no ambiguity error, matching C's own `-I`
  header-search convention. The dot in a module name carries no
  directory-hierarchy meaning here either (consistent with
  [§11.3](#113-export-surface-and-the-interfaceimplementation-split)): the
  compiler looks for a file or manifest entry named exactly `mylib.math`,
  never a nested path.
- Not found via either mechanism: compile error naming the missing
  module.
- Both of the above resolve `import` for **compilation only** -- finding
  a `.scppm` (or raw `.scpp` source) to type-check against. Neither one
  selects what gets linked; see [§11.13](#1113-linking-which-objects-get-linked)
  for that separate, unconditional mechanism.

## 11.15 Out of scope for v1 (backlog)

- **Archive signing** -- not specified yet;
  [the `.scppkg` format](../../standards/en/scppkg-format.md) is designed
  so this can be added later as a trailing block, with no version break.
  `.scppm` itself carries no signature by design -- it is a language-level
  format, not a distribution format (see
  [The `.scppm` Module Interface Format §3](../../standards/en/scppm-format.md)).
- **Automatic C-header ingestion** (a `bindgen`-style tool, or
  `import <cheader>;`) -- v1's entire FFI story is hand-written
  `extern "C"` ([§2.1](ch02-boundary-rules.md)); automating that needs a
  real preprocessor and is a tooling problem layered on the language, not
  part of this chapter.
- **Package management / dependency resolution / registries** -- entirely
  a build-ecosystem concern
  ([§11.2](#112-what-a-library-is-in-scpp-no-new-keyword)), not part of
  the language.
- **Reproducible builds** (byte-identical `.scppo` from the same source)
  -- would strengthen the signing story further (independent verification
  that an object matches public source) but isn't required for v1.
- **Key revocation / trust-root management** -- a CLI/tooling policy layer
  (like apt's separate trusted-keyring files), not part of the archive
  formats themselves.
- **Multi-version coexistence** of the same module name (Rust's main
  reason for its disambiguator hash) -- not supported; see
  [§11.10](#1110-symbol-identity-linkage-and-mangling).
- **Interop with existing, unmodified C++ libraries** (real classes,
  templates, exceptions, RTTI) -- explicitly not pursued; `extern "C"` is
  the only interop mechanism scpp provides (see
  [§8](ch08-open-questions.md) item 6).

---

[← Previous: Reference Implementations](ch10-reference-implementations.md) · [Table of Contents](README.md)
