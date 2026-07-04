# 11. Modules & Libraries (design finalized, not yet implemented)

Today the compiler only ever handles one file at a time: `driver.cppm`'s
`emit_object_file` parses a single source string into one `Program` and
lowers it alone -- there's no notion of one scpp file calling into
another, and no way to distribute a scpp API for other scpp code to
consume. This chapter specifies how scpp programs span multiple files,
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
   facts* (is it `safe`? what `[[scpp::lifetime(name)]]` groups does it
   have?) are part of what a caller's borrow check depends on. A
   hand-copied declaration that lies about either would be a silent
   soundness hole. This directly contradicts
   [ch00](ch00-design-philosophy.md)'s "soundness over compatibility". A
   module interface is compiled once from ground truth and *imported*,
   never retyped, so this entire class of bug is structurally impossible
   rather than merely discouraged.
2. **No preprocessor needed.** scpp has never implemented one -- there is
   no `#define`, no macro expansion, no conditional compilation anywhere
   in the lexer today. C++20 modules need none of that either.
3. **Reuse known syntax.** `export module`/`import` is real, modern,
   idiomatic C++. It's also, pleasantly, already what the compiler's own
   implementation is written in: every `src/*.cppm` file is itself an
   `export module scpp.xxx;` with an `export namespace scpp { ... }`
   surface.

## 11.2 What a "library" is in scpp (no new keyword)

"Library" is not a language keyword and needs none: it's the ordinary,
build-level packaging of one or more `export module` interfaces plus
their compiled payloads, distributed together as one `.scppm` archive
(see [§11.11](#1111-the-scppm-library-archive-format)) -- exactly how
"library" already works in real C++ (Boost is "a library" as a social and
build-system convention; nothing in the C++ grammar defines the word). A
package manager, registry, or dependency-resolution tool is out of scope
for this chapter (see [§11.14](#1114-out-of-scope-for-v1-backlog)).

## 11.3 Export surface and the interface/implementation split

```cpp
// mylib_math.scpp -- the module's interface. Shareable, human-readable.
export module mylib.math;

namespace mylib::math {
    extern safe int square(int x);          // exported, bodyless -- see §11.6
    export struct Point { int x; int y; };  // exported, always has "a body" (it's data)
}

safe int internal_helper(int x) { return x * 2; }  // private, has a body -- namespace irrelevant, not exported
```

```cpp
// mylib_math_impl.scpp -- an implementation unit: not exported, not
// shared. Automatically sees mylib.math's own interface without import.
module mylib.math;

safe int mylib::math::square(int x) { return x * internal_helper(x) / 2; }
```

- A file becomes a module's **primary interface unit** by starting with
  `export module name;` (at most one per module, exactly like real
  C++20). A file with no module declaration is an ordinary,
  non-exporting file, exactly like every scpp file today.
- A file starting with `module name;` (no `export`) is an
  **implementation unit**: it contributes additional code to that same
  module, and automatically has access to the module's own interface
  without needing to `import` it. Building a module means compiling its
  primary interface unit together with all of its implementation units.
- `export` prefixing an individual declaration (or grouping several
  inside `export { ... }`) marks it visible to importers; anything else
  is private to the module.
- v0.1's exportable surface is exactly v0.1's safe-subset surface
  ([§6](ch06-safe-subset.md)): free functions (`safe` or not) and
  `struct` definitions. `class`, templates, etc. gain export support
  automatically whenever they exist. Every exported declaration must
  additionally live inside a namespace matching the module's own name --
  see [§11.5](#115-exported-declarations-must-live-in-a-namespace-matching-the-module-name).
- **This revises an earlier scoping call.** Multi-file modules were
  originally deferred entirely as out of scope for v1; supporting
  compiled-payload distribution (not just source distribution) turns out
  to require at least the interface-unit/implementation-unit split, so
  that split is in scope for v1 after all. **Module partitions**
  (`export module foo:part;`, a finer-grained subdivision *within* one
  module) remain deferred -- see
  [§11.14](#1114-out-of-scope-for-v1-backlog).

## 11.4 Namespaces

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
  placeholder -- it costs nothing today (scpp has no function overloading
  yet either), but the alternative would reintroduce exactly the
  "an unrelated new import silently changes what an existing call means"
  class of spooky action ADL is well known for in real C++, which
  conflicts with [ch00](ch00-design-philosophy.md) §2/§6.
- **Namespace aliases reuse real C++'s actual alias syntax**:
  `namespace cmath = org::lotx::cmath;` -- deliberately **not** a
  `using`-declaration. `using X = Y;` is a *type* alias in real C++, and a
  namespace is not a type; spelling a namespace alias that way would not
  survive stripping `safe`/`unsafe` and handing the result to a real C++
  compiler ([ch00](ch00-design-philosophy.md) §6). This is a third,
  orthogonal mechanism alongside `using foo::bar;` (imports one *name*,
  not a whole namespace) and `import name as local;`
  ([§11.7](#117-import-visibility-re-exports-and-renaming), renames a
  *module* at the import statement, doesn't shorten any in-code path) --
  all three can be used together freely.
- **Namespace and module are otherwise orthogonal**, exactly as in real
  C++: a namespace is a purely logical grouping of names; a module is a
  physical compilation/import boundary. The one deliberate exception is
  described next.

## 11.5 Exported declarations must live in a namespace matching the module name

```cpp
export module org.lotx.cmath;

namespace org::lotx::cmath {
    export safe double sqrt(double x);   // OK -- namespace matches module name
}

safe double helper(double x) { return x; } // OK -- not exported, namespace irrelevant
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
  [§11.9](#119-symbol-identity-linkage-and-mangling)'s existing
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
  [§11.9](#119-symbol-identity-linkage-and-mangling)'s convention example
  uses this order.
- **Qualified-name resolution across imported modules**: given a
  reference like `org::lotx::cmath::sqrt(...)`, resolution walks the
  name's segments and finds the **longest prefix that exactly equals an
  imported module's dotted name** (checked only against modules the
  current file actually imports, never every module that merely exists on
  the search path, [§11.13](#1113-importlibrary-search-path)); the
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

## 11.6 Bare `extern` for module-linkage bodyless declarations

`extern` without a `"C"` string declares a function with **ordinary scpp
linkage** (not C ABI), whose implementation lives in a separate
implementation unit or a separately-distributed payload
([§11.11](#1111-the-scppm-library-archive-format)):

```cpp
extern safe int square(int x);       // ordinary scpp linkage, may be safe
extern "C" int printf(const char*, ...); // C ABI, per §2.1, always unsafe
```

Unlike `extern "C"` -- which is **always** implicitly `unsafe` because no
scpp compiler ever sees the C implementation to check it (§2.1) -- a bare
`extern` declaration **can** be marked `safe`. The trust model is
different: when the module's author builds the primary interface unit
together with its implementation unit(s), the compiler checks that every
implementation-unit definition's signature matches its interface
declaration *exactly*, once, at that build. This is the same
declaration-matches-definition trust ordinary C++ already places in
separately-compiled translation units (assumed, rarely mechanically
verified in plain C++) -- scpp's version is actually checked, at least
once, by a real scpp compiler, which is strictly *more* than plain C++
guarantees, not less.

## 11.7 Import visibility, re-exports, and renaming

- `import name;` is **private, non-transitive**: it makes `name`'s
  exports visible in the importing file, but does not forward them to
  whoever imports *that* file in turn.
- `export import name;` **re-exports**, transitively.
- `import name as local_name;` -- **new syntax, not present in real
  C++20** -- lets the importing file refer to `name` under a local alias.
  This solves a purely source-level problem (two imported modules
  happening to share a human-readable name) and is analogous to Python's
  `import x as y` or Rust's dependency renaming; it does **not** by
  itself resolve link-level symbol collisions (see
  [§11.10](#1110-collision-handling)) -- that's a separate mechanism.

## 11.8 Soundness: cross-module signatures are all the checker needs

[§5.3](ch05-static-checks.md) already establishes that v0.1's borrow
checking is **intraprocedural**: checking a call to `g` inside `f` only
ever consults `g`'s *signature* (`FunctionSignature` -- param types,
return type, `Function::is_safe`, its `[[scpp::lifetime(name)]]`
groups), never `g`'s body. This means modules require **zero new checker
logic**. The `Signatures` map movecheck already builds once per `Program`
just needs to be seeded, before checking the current file, with the
`FunctionSignature` entries recovered from each imported module's
interface -- same map, same lookups, same checks, only a second source of
entries.

Struct layout carries over the same way: [§4.3](ch04-struct-vs-class.md)
already pins a struct's layout to a pure function of (field list, target
triple) -- an imported struct's field list, recovered from the interface,
is all codegen needs to reproduce a byte-identical layout.

## 11.9 Symbol identity: linkage and mangling

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
    [§11.5](#115-exported-declarations-must-live-in-a-namespace-matching-the-module-name)
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
  - **Reserved, not yet used**: v0.1 has no function overloading (the
    `Signatures` map is one entry per name today), but real C++ does, and
    scpp is explicitly modeled on looking like C++. Mirroring the reason
    *C has no mangling and C++ always has*, the mangling scheme reserves
    room to append a parameter-type encoding (`P<count>_<encoded types>`)
    now, so that whenever overload support lands, it doesn't require an
    incompatible mangling-format change to every already-published
    `.scppm`. Return type is deliberately **not** encoded (C++'s own rule:
    you cannot overload on return type alone).
  - **No Rust-style crate disambiguator hash.** Rust bakes an extra hash
    into every mangled symbol specifically to let *the same crate name at
    two different versions* coexist safely in one build -- scpp v0.1 does
    not support that (whichever `.scppm` is found first on the search
    path wins, full stop; see [§11.13](#1113-importlibrary-search-path)),
    so the problem that hash exists to solve doesn't arise here. The
    residual risk -- two unrelated libraries picking the literal same
    module name -- is handled exactly like C/C++ has always handled
    global symbol collisions: a hard error, fixed by renaming (see
    [§11.10 below](#1110-collision-handling)), not a cryptographic
    workaround.
  - **Choosing a domain-qualified name is a convention, not
    compiler-enforced** -- e.g. `org.lotx.cmath`, read
    outermost-to-innermost/reversed-domain style like a Java package (not
    forward/URL style, `cmath.lotx.org` -- see
    [§11.5](#115-exported-declarations-must-live-in-a-namespace-matching-the-module-name)
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
    [§11.5](#115-exported-declarations-must-live-in-a-namespace-matching-the-module-name).
- `extern "C"` symbols are **never** mangled by this scheme -- they use
  the bare, unmangled name, which is the entire point of C linkage. The
  two schemes are orthogonal.

## 11.10 Collision handling

- Two `import`s of differently-named-but-colliding modules **directly**
  in the same file: caught at **compile time**, cleanly, before codegen
  even runs (the existing rule: pick different names, or use
  `import ... as ...`).
- Two modules **indirectly** pulled into the same final link (neither
  imported directly by the same file, e.g. both are dependencies of a
  third module that doesn't re-export either): if their compiled exported
  symbols collide, the **system linker** reports an ordinary duplicate-symbol
  error at link time. This is exactly how plain C/C++ has always
  behaved for colliding global symbols -- scpp isn't trying to be more
  clever than that, on purpose (see [§11.9](#119-symbol-identity-linkage-and-mangling)'s
  reasoning for not adopting Rust's disambiguator hash).

## 11.11 The `.scppm` library archive format

A `.scppm` file is a **7z archive** that can hold an entire library --
many modules, many target-triple variants, one distributable unit a user
downloads once:

```
mylib.scppm  (7z archive)
├── MANIFEST.json
├── modules/
│   ├── mylib.math.scpp          # interface file (§11.3), shareable source
│   └── mylib.collections.scpp
├── payloads/
│   ├── mylib.math/
│   │   ├── x86_64-linux-gnu.bc  # LLVM bitcode, default/recommended kind
│   │   ├── aarch64-linux-gnu.bc
│   │   └── x86_64-linux-gnu.o   # native object, alternative kind
│   └── mylib.collections/...
└── SIGNATURE/
    ├── DIGESTS                  # sha256 of every other file, one per line
    └── DIGESTS.asc              # detached OpenPGP signature(s) over DIGESTS
```

`MANIFEST.json` sketch:

```json
{
  "format_version": 1,
  "library": { "name": "mylib", "version": "1.2.0" },
  "hash_algorithm": "sha256",
  "modules": {
    "mylib.math": {
      "interface": "modules/mylib.math.scpp",
      "dependencies": ["otherlib.util"],
      "native_link_requirements": ["m"],
      "payloads": [
        { "target_triple": "x86_64-linux-gnu", "kind": "llvm-bitcode",
          "path": "payloads/mylib.math/x86_64-linux-gnu.bc" },
        { "target_triple": "x86_64-linux-gnu", "kind": "native-object",
          "path": "payloads/mylib.math/x86_64-linux-gnu.o" }
      ]
    }
  }
}
```

- **`format_version` is checked first**, before anything else is parsed,
  so an old toolchain reading a newer archive fails cleanly instead of
  crashing on unrecognized structure.
- **`dependencies`/`native_link_requirements`** record what else a
  module's payload itself needs (other scpp modules, or `-l` system
  libraries) so the final link step knows what to bring in transitively.
- **Payload `kind` is tagged per entry, not hardwired to LLVM bitcode.**
  `llvm-bitcode` is the recommended default (enables cross-module LTO on
  the consumer's own machine, and per-target-triple variants -- scpp's
  codegen already fixes a target's `DataLayout` before generating code,
  per [§4.3](ch04-struct-vs-class.md), so today's bitcode is
  target-triple-specific, not universally portable; an author wanting to
  support several triples ships one `.bc` per triple). `native-object`/
  `native-archive` remain valid alternate kinds for cases wanting zero
  LLVM-toolchain dependency on the consumer side.
- **Interface files may have per-function bodies, optionally.** A
  function written with a full body in `modules/*.scpp` compiles on any
  target directly from source (no payload needed for it at all); a
  bodyless (`extern`, §11.6) one relies on a matching `payloads/` entry
  for whichever target triple the consumer is building for. This is a
  per-function choice, not an all-or-nothing one.
- **Signing**: `DIGESTS` lists a SHA-256 of every other file in the
  archive; `DIGESTS.asc` is one or more detached OpenPGP signatures over
  it -- the same "release tarball + `.asc`" pattern the open-source world
  has used for decades, not a bespoke scheme. Verifying means
  recomputing every listed hash (catches tampering/corruption of
  payloads) and checking the PGP signature (establishes who vouches for
  the bundle). **Enforcement is a compiler flag, off by default in v1**
  (e.g. `--require-signed-modules`); the format always has room for it
  regardless of whether it's turned on. **"No `SIGNATURE/` at all" and
  "`SIGNATURE/` present but verification fails" must not be treated the
  same way** -- the latter is hard evidence of tampering or corruption and
  should fail loudly even under a permissive policy; only the former is
  the ordinary "this library ships unsigned" state.
- **Atomicity**: one `.scppm`'s interface files and payloads must always
  be treated as one indivisible unit produced by one build -- never mix a
  `modules/*.scpp` from one archive with a `payloads/` entry from
  another, even for a same-named module. When signing is enabled this is
  cryptographically enforced for free (`DIGESTS` already covers every
  file); without signing it's a tooling-level invariant (no supported
  operation ever extracts/recombines pieces across archives).
- Integrating a `.7z` reader/writer is a new dependency (nothing in
  today's CMakeLists.txt provides one); recommend hiding it behind a thin
  internal archive-I/O interface so the concrete container format could
  be swapped later without disturbing the rest of this design.

## 11.12 Multi-target bitcode and target-triple resolution

A single `.scppm` can bundle several `.bc` variants of the same module
(one per target triple the author chooses to support). At final link
time, the consumer's toolchain looks for an **exact target-triple match**
in `payloads/<module>/`; if none exists, it's a hard error naming which
triples the library does provide. If the interface file happens to
include a full body for the needed function (§11.11), compiling that
inlined source is an available fallback even without a matching payload.

## 11.13 Import/library search path

- `scpp build <file> --import name=path/to/library.scppm` -- explicit,
  unambiguous, always works (mirrors Clang's `-fmodule-file=name=path`
  and Rust's `--extern name=path`).
- `scpp build <file> -I <dir>` -- convenience search: to resolve
  `import mylib.math;`, look in each `-I` directory (in order) for a
  `.scppm` archive that **contains** a module literally named
  `mylib.math`, then look up `modules/mylib.math.scpp` inside it. First
  directory (in the order given) that contains a match wins -- no
  ambiguity error, matching C's own `-I` header-search convention. The
  dot in a module name carries no directory-hierarchy meaning here either
  (consistent with [§11.3](#113-export-surface-and-the-interfaceimplementation-split)):
  the compiler looks for one flat archive containing that exact module
  name, not a nested path.
- Not found via either mechanism: compile error naming the missing
  module.
- At final link time, the same resolved `.scppm` also supplies whichever
  `payloads/` entry matches the consumer's target triple
  ([§11.12](#1112-multi-target-bitcode-and-target-triple-resolution)).

## 11.14 Out of scope for v1 (backlog)

- **Module partitions** (`export module foo:part;`) -- deferred; v1 only
  has the primary-interface-unit/implementation-unit split
  ([§11.3](#113-export-surface-and-the-interfaceimplementation-split)).
- **Automatic C-header ingestion** (a `bindgen`-style tool, or
  `import <cheader>;`) -- v1's entire FFI story is hand-written
  `extern "C"` ([§2.1](ch02-boundary-rules.md)); automating that needs a
  real preprocessor and is a tooling problem layered on the language, not
  part of this chapter.
- **Package management / dependency resolution / registries** -- entirely
  a build-ecosystem concern
  ([§11.2](#112-what-a-library-is-in-scpp-no-new-keyword)), not part of
  the language.
- **Reproducible builds** (byte-identical `.bc` from the same source) --
  would strengthen the signing story further (independent verification
  that a payload matches public source) but isn't required for v1.
- **Key revocation / trust-root management** -- a CLI/tooling policy layer
  (like apt's separate trusted-keyring files), not part of the `.scppm`
  format itself.
- **Multi-version coexistence** of the same module name (Rust's main
  reason for its disambiguator hash) -- not supported; see
  [§11.9](#119-symbol-identity-linkage-and-mangling).
- **Interop with existing, unmodified C++ libraries** (real classes,
  templates, exceptions, RTTI) -- explicitly not pursued; `extern "C"` is
  the only interop mechanism scpp provides (see
  [§8](ch08-open-questions.md) item 6).

## 11.15 Implementation shape (for whoever builds this)

- **Parser**: `module`, `export`, `import`, `namespace` keywords (none
  lexed yet); grammar for a module declaration, an import declaration
  (with optional `as name`), a namespace declaration (including the
  C++17 one-line nested form `namespace a::b::c { ... }`), a namespace
  alias (`namespace a = b::c;`), and `export`-prefixed (or
  `export { }`-grouped) top-level declarations, including bodyless
  (`extern`) exported functions.
- **A new build artifact**: extend the `scpp` CLI
  (`lex`/`parse`/`build`, [§7](ch07-compilation-pipeline.md)) with
  something like `scpp build-module <interface> [<impl>...] -o
  <name>.scppm`, compiling interface + implementation units together
  (checking every definition against its declaration once, per
  [§11.6](#116-bare-extern-for-module-linkage-bodyless-declarations)),
  emitting one `.bc` per requested `--target` triple, and packing
  everything into the 7z-based archive from
  [§11.11](#1111-the-scppm-library-archive-format).
- **Export/namespace check**: a new, purely syntactic validation pass
  (needs no borrow-checker involvement) that rejects any `export`-marked
  declaration whose enclosing namespace path doesn't start with the
  current module's own dotted name, per
  [§11.5](#115-exported-declarations-must-live-in-a-namespace-matching-the-module-name).
- **Qualified-name resolution**: resolving `a::b::c::...` against the
  current file's `import`ed module set needs the longest-prefix-match
  algorithm from [§11.5](#115-exported-declarations-must-live-in-a-namespace-matching-the-module-name),
  including its ambiguous-match compile error -- this is new lookup logic,
  layered in front of (not replacing) the existing per-name `Signatures`
  map lookups from [§11.8](#118-soundness-cross-module-signatures-are-all-the-checker-needs).
- **Codegen**: an imported module's exported functions are declared
  (LLVM `declare`, external linkage, mangled per
  [§11.9](#119-symbol-identity-linkage-and-mangling), including the
  `N<len>_<segment>` blocks for namespace nesting beyond the module name)
  in the importing TU's module; private functions never cross this
  boundary at all (internal linkage). Linking the resulting `.bc`/`.o`
  files together is the system linker's job; `driver.cppm`'s
  `link_executable` already shells out to `cc` for this and just needs to
  accept multiple object inputs instead of one.

---

[← Previous: Reference Implementations](ch10-reference-implementations.md) · [Table of Contents](README.md)
