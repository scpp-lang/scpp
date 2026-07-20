# libs — scpp's library modules

This directory contains scpp's shipped library modules: the real-`std` module under `libs/std/`, plus scpp-specific extensions under `libs/scpp/`. Native helper libraries live here too. `libs/scpp_llvm/` is a standalone package (not a member of the `std`/`scpp` workspace below) providing ergonomic scpp bindings to LLVM; see its own section near the end of this file. `libs/llvm/` is a third, distinct category: plain C++20 modules compiled directly by real clang++ (never by scpp itself) that let this compiler's own `src/*.cppm` sources replace `#include <llvm-c/*.h>` with `import llvm.<name>;`; see its own section below.

The project convention is:

- expose library surface through scpp's real module system (`import std;`)
- keep each area in its own partition-oriented subdirectory (`string/`,
  `memory/`, future ones)
- use ordinary scpp code where possible
- use small native C/C++ wrapper libraries only when a partition needs to
  bridge to existing runtime functionality scpp does not yet implement by
  itself

## Layout

| Path | Role |
|---|---|
| `scpp.toml` | Workspace manifest that dogfoods `scpp build` for the shipped libraries |
| `std/scpp.toml` | `std` package manifest: `[[lib]]` source set plus `[additional_objs.std-native]` wrapper-object build step |
| `std/std.scpp` | Primary interface unit of module `std`; re-exports its partitions with `export import :...;` |
| `std/` | Real-C++-mirroring library partitions and native wrappers for module `std` |
| `scpp/scpp.toml` | `scpp` package manifest with a path dependency on `../std` |
| `scpp/scpp.scpp` | Primary interface unit of module `scpp`; re-exports scpp-specific partitions |
| `scpp/rand/` | `scpp:rand` partition with `scpp::rand::uniform_int_distribution<int>` |
| `CMakeLists.txt` | Only stages a temporary workspace under the build tree, runs `scpp build --lib`, and copies final artifacts back to stable `build/libs` paths |
| `scpp_llvm/scpp.toml` | `scpp-llvm` package manifest (standalone, not a workspace member yet -- see its own section below) |
| `scpp_llvm/scpp_llvm.scpp` | Primary interface unit of module `scpp.llvm`; re-exports its partitions |
| `scpp_llvm/core/` | `scpp.llvm:core` partition: ergonomic scpp wrapper classes binding directly to official LLVM-C |
| `llvm/types.cpp` | Module `llvm.types`: hand-written opaque handle struct tags/aliases mirroring the `llvm-c/Types.h` subset `llvm.core` and `src/compiler/codegen/api.cppm` use -- plain clang++-compiled, not a workspace member |
| `llvm/core.cpp` | Module `llvm.core`: hand-written `extern "C"` mirror of the `llvm-c/Core.h` subset this compiler's own codegen uses, depending on `llvm.types` for its opaque handle types -- plain clang++-compiled, not a workspace member |

## Manifest workspace

`libs/` now dogfoods the same manifest-based flow the book teaches:

- `libs/scpp.toml` is a two-member workspace (`std`, `scpp`)
- `libs/std/scpp.toml` defines the `std` library package
- `libs/scpp/scpp.toml` defines the `scpp` library package and depends on `std`

`libs/` now keeps wrapper compilation inside the manifest build itself:

- `[[lib]]` declares the scpp module source set
- `[additional_objs.std-native]` / `[additional_objs.scpp-native]` each run one `${CXX:-c++} -c ...`
  command that produces native `.o` files
- `additional_objs = "..."` attaches those outputs to the final `libstd.scppa`
  / `libscpp.scppa` archives

So the only non-manifest piece left is the tiny CMake staging layer that runs
the workspace build in a throwaway build-tree directory and copies the resulting
artifacts to the stable paths the rest of the top-level build already consumes.

## Consuming `std`

User code writes:

```cpp
import std;
```

and the build passes the module mappings explicitly, for example:

```sh
scpp app.scpp -o app \
  --import std=libs/std/std.scpp \
  --import scpp=libs/scpp/scpp.scpp
```

Notes:

- `libs/std/std.scpp` aggregates the `std` partitions; consumers never import
  `std:string` or `std:memory` directly in source.
- `libs/scpp/scpp.scpp` aggregates scpp-specific partitions; consumers explicitly
  opt in with `import scpp;`.
- Native helper objects are already packed into the shipped `libstd.scppa` /
  `libscpp.scppa` archives, so ordinary consumers do not pass separate wrapper
  `--link` flags anymore.
- Partitions compile together with the primary interface unit as one `std`
  module object; there is no textual concatenation.

## Current partitions

### `std:string`

- File: `std/string/std_string.scpp`
- Backed by: `std/string/scpp_string_wrapper.{h,cpp}`
- Provides a small `std::string` surface via `extern "C"` wrapper calls

### `std:memory`

- File: `std/memory/std_memory.scpp`
- Pure scpp implementation
- Provides `std::unique_ptr<T>` and `std::make_unique<T>(...)`

## The `scpp-llvm` package (module `scpp.llvm`)

The package/`[[lib]]` manifest name is `scpp-llvm` (hyphen), while the
module a consumer actually `import`s is the dotted `scpp.llvm` -- these are
deliberately different spellings of two independent identifiers in scpp's
build model (see `scpp_llvm/scpp.toml`'s own header comment): only the
module name is what an `import` statement resolves against and is subject
to scpp's module-name grammar (`Identifier ('.' Identifier)*`, no
hyphens); the manifest `name` is pure package metadata -- used only for
dependency-graph bookkeeping and this package's output archive filename --
and is parsed as an arbitrary TOML string with no identifier-grammar
restriction at all, the same way an npm/cargo package can be hyphenated
(`serde-json`) even though the language identifier it maps to cannot be
(`serde_json`). Both names are deliberately distinct from bare `llvm`, and
the module name's dotted form mirrors this project's own compiler-internal
module naming (`scpp.ast`, `scpp.compiler.codegen`, etc., see
`src/*.cppm`) -- specifically to avoid any naming collision/confusion with
official upstream LLVM itself when a consumer writes `import scpp.llvm;`
and uses `scpp::llvm::core::...` -- this is our own curated scpp binding
package *around* LLVM, not LLVM. (The package's directory and `.scpp`
filenames stay underscore-based, `scpp_llvm/`, unrelated to either naming
choice above -- see "Layout" above.)

Unlike `std`/`scpp` above, `scpp-llvm` is not (yet) a member of
`libs/scpp.toml`'s workspace -- see `scpp_llvm/scpp.toml`'s own header
comment for why (in short: its consumers need real LLVM's own native
library at final link time, and there is no manifest-level mechanism yet
for a package to propagate that requirement automatically). It is still a
real, independently buildable and importable scpp package today, just one
that (for now) requires a couple of extra explicit `--link` flags from its
consumers.

It exists to make scpp's from-scratch LLVM bindings (originally built for the
compiler's own codegen) directly reusable by any scpp program that wants to
do LLVM-based codegen/JIT/tooling, not just the compiler itself -- one
canonical implementation, at least two consumers.

Official LLVM-C functions (`llvm-c/*.h`) are themselves already a stable,
cross-language `extern "C"` interface, designed precisely for binding from
other languages -- so `scpp_llvm/core/scpp_llvm_core.scpp` (the
`scpp.llvm:core` partition) declares `extern "C"` bindings (ch06 SS6.2)
straight to those official `LLVM*` functions, with zero indirection through
any custom C++ shim of our own, and wraps them in idiomatic, RAII scpp
classes -- `scpp::llvm::core::Context`/`Type`/`Value`/`Module` today. This
partition, aggregated by `scpp_llvm/scpp_llvm.scpp`, is the only part of
this package ordinary scpp consumers ever interact with directly.

This "bind straight to LLVM-C" design isn't an assumption -- it's the
conclusion of a rigorous, function-by-function empirical audit of every
`llvm::`-namespaced operation the scpp compiler's own codegen uses (dozens of
`IRBuilder` methods, `Module`/`Function`/`Type`/`DIBuilder` construction, etc.
across `src/compiler/codegen/*.cppm` and `src/compiler/debug.cppm`), checked
against actual LLVM-C signatures and semantics (including compiled-and-run
test programs for the trickiest cases). That audit found LLVM-C fully covers
everything needed, with zero genuine gaps -- including
`Module::pointer_abi_alignment` below, which an earlier round of this
project mistakenly concluded needed a hand-written `extern "C"` gap-filler of
our own; empirical testing proved that wrong (`LLVMABIAlignmentOfType`
queried against an opaque pointer type for a given address space reads the
exact same `DataLayout` entry a direct per-address-space query would), so it
now simply composes two official LLVM-C calls instead. Should a genuine
coverage gap ever be found for some future piece of functionality, the plan
is to add a minimal, hand-written `extern "C"` C++ shim at that point
(analogous in spirit to rustc's `rustc_llvm` wrapper) covering only that
specific gap -- not a blanket re-wrap of everything LLVM-C already provides.

Current scope is deliberately small (a `Context`; an empty `Module`;
scalar `Type`s (`i1`/`i8`/`i16`/`i32`/`i64`/`float`/`double`/`void`) plus
pointer/array/(anonymous) struct/function derived `Type`s; simple `Value`
constants (`const_int`/`const_real`/`const_null`); `Module::print()`/
`Module::verify()`; and a `DataLayout`-backed size/alignment query
surface) -- a first, provable slice, not the full `IRBuilder`/`Function`/
`BasicBlock`/`GlobalVariable`/`DIBuilder` surface the compiler's own
codegen will eventually need. The scpp compiler's own codegen does not use
this package -- see `src/compiler/codegen/`'s own comments for that
separate, ongoing migration; it does, like this package, bind directly to
official LLVM-C for the DataLayout/Verifier queries it has converted so far.

Consuming it today looks like:

```sh
scpp app.scpp -o app \
  --import std=libs/std/std.scpp \
  --import scpp.llvm=libs/scpp_llvm/scpp_llvm.scpp \
  --link <path(s) to LLVM's own native library, e.g. from \`llvm-config --libfiles\`>
```

See `tests/llvm_lib_test.cpp` and `tests/llvm_lib_test_source/main.scpp` for
a complete, working example (built and run by `ctest`), and
`scpp_llvm/scpp.toml`'s own comments for exactly how those `--link` inputs are
resolved there.

## The `llvm.types` and `llvm.core` modules (modules `llvm.types`, `llvm.core`)

`libs/llvm/` is a third category, distinct from both `std`/`scpp` above
(scpp-buildable workspace members) and `scpp_llvm/` below (an ergonomic,
scpp-facing RAII wrapper package): it is one or more plain, ordinary C++20
modules, compiled directly by real clang++ via dedicated CMake targets
(`llvm_types`, `llvm_core`, see `libs/llvm/CMakeLists.txt`, wired into the
root via `add_subdirectory(libs/llvm)`), never by scpp itself -- unlike
`std`/`scpp` above, there is no aspiration for these files to also be
scpp-parseable. It exists solely so this compiler's own `src/*.cppm`
codegen/driver files can replace a raw `#include <llvm-c/*.h>` with
`import llvm.<name>;` -- scpp (the language) has no preprocessor/
`#include` at all, so any raw `#include` left in the compiler's own
sources is a hard blocker for eventual self-hosting.

- File: `llvm/types.cpp`, module `llvm.types` -- a fresh, standalone,
  top-level module, not nested inside or a partition of `scpp`/`std`/
  `scpp.llvm`/`llvm.core`. Contains hand-written opaque handle struct
  tags and pointer-alias `typedef`s mechanically mirroring the specific
  subset of real `llvm-c/Types.h` referenced by `llvm.core` (below) and by
  `src/compiler/codegen/api.cppm` (which needs raw handle types, e.g.
  `LLVMContextRef`, `LLVMDIBuilderRef`, for its `Codegen` class's members,
  but no Core.h function) -- not a blanket re-declaration of Types.h's
  much larger surface. This module used to not exist: `llvm.core`
  declared its own private copy of this same surface directly, and
  `api.cppm` got a second, independent copy via its own
  `#include <llvm-c/Types.h>`; `llvm.types` is now the single, real
  source of truth both depend on instead.
- File: `llvm/core.cpp`, module `llvm.core` -- a fresh, standalone,
  top-level module, not nested inside or a partition of `scpp`/`std`/
  `scpp.llvm`/`llvm.types`.
- Contains hand-written `extern "C"` declarations mechanically mirroring
  the specific subset of real `llvm-c/Core.h` actually referenced by
  `src/driver.cppm` and `src/compiler/codegen/{layout,orchestration,debug,
  functions,object_model,lifetime,statements,expressions}.cppm` today --
  not a blanket re-declaration of Core.h's much larger surface. `core.cpp`
  itself has zero `#include` of any real `llvm-c/*.h` header -- a
  deliberate choice, not a requirement of the `.cpp` extension -- so,
  together with the `llvm.types` module it depends on, its declarations
  are the single, self-contained source of truth this module exports,
  rather than a re-export of someone else's macros.
- Unlike `scpp_llvm/core/`'s RAII wrapper classes (which deliberately
  share a single `void*` underneath their own type-safe wrapper layer),
  neither module has any such wrapper: their raw `LLVM*Ref` declarations
  *are* the public surface those nine files (and `api.cppm`) call/use
  directly, exactly as they did through the real header, so every opaque
  handle kind (`LLVMContextRef`, `LLVMModuleRef`, `LLVMTypeRef`, ...) is
  declared as its own distinct pointer type, never a shared `void*`.
- The opaque handle struct tags live in `llvm.types`'s own global module
  fragment (before `export module llvm.types;`), not its purview, and the
  pointer aliases (`using LLVMContextRef = ...`) plus `LLVMBool` are
  `export`ed from the purview instead -- required so the same struct
  tags, also reachable unattached via whichever other `llvm-c/*.h` header
  a given consumer still `#include`s alongside `import llvm.core;`,
  denote one and the same type instead of two conflicting, "attached to
  different modules" entities (see `types.cpp`'s own header comment for
  the full rationale, including why this still holds transitively now
  that `llvm.core` reaches these tags through `import llvm.types;` plus
  its own `export import llvm.types;` re-export, rather than declaring
  them itself). This split is a real ISO C++20 modules requirement,
  independent of either file's own extension or of scpp's grammar --
  verified empirically against this project's own real build, not just
  reasoned about in isolation.
- `llvm.core` keeps only its genuinely Core.h-specific declarations
  (`LLVMAttributeIndex`, the ~118 functions, the 5 enums) and depends on
  `llvm.types` (`export import llvm.types;`) for every opaque handle type
  it uses in its own signatures -- see `core.cpp`'s own header comment.

The other `llvm-c/*.h` headers still `#include`d by those same nine files
today (`Target.h`, `TargetMachine.h`, `DebugInfo.h`, `Analysis.h`) are
deliberately untouched by these modules -- out of scope for now, left for
their own later, equally narrow `llvm.<name>` follow-ups.

## Testing policy

`libs/` is library source, not a demo area. Coverage belongs in the real
test suites:

- `tests/` for dev-agent-owned unit/integration coverage
- `blackbox_test/` for user-visible language/library behavior

Native helper libraries are built here because the tests and compiler need
them, but demo executables do not live here.

