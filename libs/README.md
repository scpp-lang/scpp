# libs — scpp's library modules

This directory contains scpp's shipped library modules: the real-`std` module under `libs/std/`, plus scpp-specific extensions under `libs/scpp/`. Native helper libraries live here too. `libs/scpp/llvm/` holds the `scpp:llvm` partition -- a genuine part of the `scpp` package/workspace below (ordinary scpp consumers reach it via `import scpp;`, exactly like `:io`/`:rand`/`:enum_cast`), providing ergonomic scpp bindings to LLVM. It is *also*, independently, compiled a second way by the scpp compiler's own CMake build, as a standalone module dependency fully decoupled from the rest of the `scpp` package; see its own section near the end of this file.

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
| `scpp/scpp.toml` | `scpp` package manifest with a path dependency on `../std`; `[[lib]].sources` lists each partition directory, including `llvm/` (see the `scpp:llvm` partition section below); `[additional_objs.scpp-native]` compiles both `io/scpp_io_wrapper.cpp` and `llvm/native_target_init.cpp` |
| `scpp/scpp.scpp` | Primary interface unit of module `scpp`; re-exports scpp-specific partitions, including `:llvm` |
| `scpp/rand/` | `scpp:rand` partition with `scpp::rand::uniform_int_distribution<int>` |
| `CMakeLists.txt` | Only stages a temporary workspace under the build tree, runs `scpp build --lib`, and copies final artifacts back to stable `build/libs` paths |
| `scpp/llvm/scpp_llvm.scpp` | `scpp:llvm` partition: ergonomic scpp wrapper classes (in `namespace scpp::llvm`) binding directly to official LLVM-C; discovered by `scpp/scpp.toml`'s own `sources` glob like any other partition |
| `scpp/llvm/scpp_llvm_single_module.cpp` | Minimal, CMake-only primary interface unit (`export module scpp; export import :llvm;`) used *only* by `scpp/llvm/CMakeLists.txt`, to compile just the `:llvm` partition as its own independent module for the compiler's own build -- never touched by the scpp package/workspace/CLI system |
| `scpp/llvm/native_target_init.cpp` | Ordinary (non-dual-compiled, never-imported) C++ shim bridging the one confirmed LLVM-C gap -- compiled twice, once via `scpp/scpp.toml`'s `[additional_objs.scpp-native]` and once via `scpp/llvm/CMakeLists.txt`'s own static-library target -- see the `scpp:llvm` partition section below |
| `scpp/llvm/CMakeLists.txt` | Defines the compiler-only `scpp_llvm` C++20-module target (`scpp_llvm.scpp` + `scpp_llvm_single_module.cpp`) and the `scpp_llvm_native_target_init` static library, pulled into the root build via `add_subdirectory(libs/scpp/llvm)` -- see the `scpp:llvm` partition section below |

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

## The `scpp:llvm` partition, and its second, independent build

`scpp:llvm` is a genuine partition of the `scpp` package, declared exactly
like `:io`/`:rand`/`:enum_cast`: `scpp/llvm/scpp_llvm.scpp` declares
`export module scpp:llvm;` with all of its implementation in `namespace
scpp::llvm { ... }`, and `scpp/scpp.scpp` re-exports it via `export import
:llvm;` alongside the others. Ordinary scpp consumers reach it the same way
as any other `scpp` partition -- `import scpp;`, then `scpp::llvm::...` --
and `scpp/scpp.toml`'s `[[lib]].sources` glob discovers
`llvm/scpp_llvm.scpp` the same way it discovers every other partition file,
with no special-casing. (The partition's directory lives at
`libs/scpp/llvm/` -- nested under `libs/scpp/` purely for filesystem
tidiness -- and its `.scpp` filename stays underscore-based,
`scpp_llvm.scpp`, matching this project's established convention; see
"Layout" above.)

It exists to make scpp's from-scratch LLVM bindings (originally built for the
compiler's own codegen) directly reusable by any scpp program that wants to
do LLVM-based codegen/JIT/tooling, not just the compiler itself -- one
canonical implementation, at least two consumers.

Official LLVM-C functions (`llvm-c/*.h`) are themselves already a stable,
cross-language `extern "C"` interface, designed precisely for binding from
other languages -- so `scpp/llvm/scpp_llvm.scpp` (the `scpp:llvm`
partition) declares `extern "C"` bindings (ch06 SS6.2) straight to those
official `LLVM*` functions for nearly all of its surface, and wraps them in
idiomatic, RAII scpp classes -- `scpp::llvm::Context`/`Type`/`Value`/
`Module`/`Builder`/`Function`/`BasicBlock`/`GlobalVariable`/`Attribute`/
`Intrinsic`/`DataLayout`/`DIBuilder`/`Target`/`TargetMachine`. This
partition is the only part of this surface ordinary scpp consumers ever
interact with directly.

This "bind straight to LLVM-C" design isn't an assumption -- it's the
conclusion of a rigorous, function-by-function empirical audit of every
`llvm::`-namespaced operation the scpp compiler's own codegen and driver use
(the full `IRBuilder` surface -- arithmetic/comparison/casts, memory/GEP,
control flow, calls, Phi --, `Module`/`Function`/`BasicBlock`/
`GlobalVariable`/`Type`/`DIBuilder`/`TargetMachine` construction, etc. across
`src/compiler/codegen/*.cppm` and `src/driver.cppm`), checked against actual
LLVM-C signatures and semantics (including compiled-and-run test programs for
the trickiest cases). That audit found LLVM-C covers nearly everything
needed directly -- including `Module::pointer_abi_alignment`, which an
earlier round of this project mistakenly concluded needed a hand-written
`extern "C"` gap-filler of our own; empirical testing proved that wrong
(`LLVMABIAlignmentOfType` queried against an opaque pointer type for a given
address space reads the exact same `DataLayout` entry a direct
per-address-space query would), so it now simply composes two official
LLVM-C calls instead.

One genuine, confirmed coverage gap has been found so far:
`LLVMInitializeNativeTarget`/`LLVMInitializeNativeAsmPrinter` (`llvm-c/
Target.h`) are header-only `static inline` functions that expand, via
build-config macros, to real architecture-specific exported symbols (e.g.
`LLVMInitializeX86Target`) -- not stable exported ABI symbols themselves.
Since `scpp_llvm.scpp` has no preprocessor and so cannot `#include` that
header to get the inline body, and hard-coding one architecture's concrete
symbol names would break portability, `scpp/llvm/native_target_init.cpp`
is the minimal, hand-written `extern "C"` C++ shim added for that one gap
(analogous in spirit to rustc's `rustc_llvm` wrapper) -- covering only that
specific gap, not a blanket re-wrap of everything LLVM-C already provides.
Should another genuine coverage gap ever be found for some future piece of
functionality, the same minimal-shim treatment is the plan. This one shim
file is compiled twice -- once folded into `libscpp.scppa` via
`scpp/scpp.toml`'s `[additional_objs.scpp-native]` (for ordinary scpp
consumers), and once via `scpp/llvm/CMakeLists.txt`'s own tiny
`scpp_llvm_native_target_init` static-library target (for the compiler's
own independent build, below) -- both compiling the exact same source,
never linked together.

This partition's surface now covers everything the scpp compiler's own
codegen and driver need: `Context`; `Module` (create/print/verify/
DataLayout/module-flag queries); scalar/pointer/array/(anonymous)
struct/function `Type`s; `Value` constants and the full instruction-level
query surface; the complete `Builder` (`IRBuilder`-equivalent) surface
(arithmetic, comparisons, casts, memory/GEP, control flow, calls, Phi);
`Function`/`BasicBlock`/`GlobalVariable`/`Attribute`/`Intrinsic`
construction and management; `DIBuilder`-equivalent debug-info generation;
and `Target`/`TargetMachine`-based object-file emission. The scpp
compiler's own codegen (`src/compiler/codegen/*.cppm`) and driver
(`src/driver.cppm`) `import scpp;` and route every LLVM operation through
`scpp::llvm::...` -- one canonical implementation, reused by both the
compiler itself and any other scpp program that wants to do LLVM-based
codegen/JIT/tooling.

Consuming it today looks like:

```sh
scpp app.scpp -o app \
  --import std=libs/std/std.scpp \
  --import scpp=libs/scpp/scpp.scpp \
  --link <path(s) to LLVM's own native library, e.g. from \`llvm-config --libfiles\`>
```

See `tests/llvm_lib_test.cpp` and `tests/llvm_lib_test_source/main.scpp` for
a complete, working example (built and run by `ctest`; it imports the
*prebuilt* `scpp.scppm` interface rather than raw source, so the scpp
compiler auto-links the co-located `libscpp.scppa` archive -- which already
has `native_target_init.cpp`'s object code folded in -- needing no separate
`--link` for that shim), and `scpp/scpp.toml`'s own comments for exactly how
the LLVM-library `--link` input is resolved there.

A second, related friction this surfaced: `scpp/scpp.toml`'s package build
compiles the *entire* "scpp" module -- all four partitions together -- into
one merged object (there is no per-partition object granularity yet), so
even a program that only imports "scpp" for `:rand`/`:io`/`:enum_cast` (not
touching `:llvm` at all) still transitively needs `:llvm`'s real LLVM-C
symbols satisfied at final link time, exactly like a genuine `:llvm`
consumer does. `tests/driver_test.cpp`'s `std_link_inputs()` and
`blackbox_test/run_tests.cpp`'s `default_std_build_args()` both add the
same `--link`/extra-link-input treatment unconditionally for this reason
(each resolves its own `SCPP_LLVM_NATIVE_LIBRARY_FILES` via CMake, the
latter with a standalone `llvm-config` lookup so its own CMakeLists.txt
still needs no `find_package(LLVM)`), so ordinary `ctest`/`blackbox_test`
runs need no per-case changes -- but any *other*, future consumer of
`scpp/scpp.toml`'s rand/io/enum_cast alone should expect the same until the
self-hosted build gains per-partition object output.

### The compiler's own, second and fully independent build of this same partition

The scpp compiler's own CMake build does *not* depend on `scpp/scpp.toml`'s
package build at all -- it never needs `:io`/`:rand`/`:enum_cast` to compile,
and (see below) `:rand` in particular cannot be compiled directly by real
clang++ in the first place. Instead, `scpp/llvm/CMakeLists.txt` defines its
own, entirely independent `scpp_llvm` CMake target (pulled into the root
build via `add_subdirectory(libs/scpp/llvm)`, the same
own-CMakeLists.txt-plus-`add_subdirectory` pattern `src/compiler/codegen`
and `src/compiler/movecheck` already use), which compiles exactly two
files together: `scpp_llvm.scpp` (the real `:llvm` partition implementation)
and `scpp_llvm_single_module.cpp` (a two-line, CMake-only primary interface
unit: `export module scpp; export import :llvm;`, never touched by the
scpp package/workspace/CLI system). Module dependency scanning (CMake +
Ninja + `clang-scan-deps`, the same mechanism already used for `import
std;` elsewhere in this project) discovers the `:llvm` partition
automatically from that primary interface unit's own `export import
:llvm;`. The result is a real C++20 module literally named `scpp`, but
containing *only* the `:llvm` partition -- which `scpp_codegen`/
`scpp_driver` then link against and `import scpp;` directly in real C++,
no `--import`/`--link` CLI flags involved, since this is an ordinary
CMake build-time module dependency, not a scpp-CLI-driven build.

This means `scpp_llvm.scpp` is compiled *twice*, completely separately,
and the two compiled modules are never linked into the same binary: once
by `scpp/scpp.toml`'s own package build (self-hosted, via `scpp build
--lib`, alongside `:io`/`:rand`/`:enum_cast`, producing the *full* `scpp`
module for ordinary scpp users), and once by `scpp/llvm/CMakeLists.txt`'s
own CMake target (real clang++ compiling just this one file plus
`scpp_llvm_single_module.cpp`, producing a *minimal* `scpp` module
containing only `:llvm`, for the compiler's own build). This split exists
because `scpp/scpp.toml`'s own full package build is driven by the scpp
compiler itself (which accepts scpp-specific sugar like `:rand`'s
distribution `.call()` syntax), while the compiler's own CMake target
compiles `.scpp` source directly with real clang++ (which does not) --
so real clang++ never needs to ingest `:rand`'s content at all under this
split, only `:llvm`'s, which is written to be valid under both front ends
simultaneously (see this project's core design principle: every `.scpp`
file must be simultaneously valid real C++ and valid scpp).

Crucially, `scpp_codegen`/`scpp_driver` link *only* against this
independent `scpp_llvm` CMake target -- not against `libs/scpp/scpp.toml`'s
own full package build (which has no CMake module target at all and is
consumed exclusively via the scpp CLI's `--import` mechanism, e.g. by
`driver_test`/`blackbox_test`'s rand/io test cases, and by `llvm_lib_test`
above for `:llvm` itself). The compiler's own build depends only on the one
independent piece it actually needs, with zero build-time coupling to
`:io`/`:rand`/`:enum_cast` compiling successfully.

## Testing policy

`libs/` is library source, not a demo area. Coverage belongs in the real
test suites:

- `tests/` for dev-agent-owned unit/integration coverage
- `blackbox_test/` for user-visible language/library behavior

Native helper libraries are built here because the tests and compiler need
them, but demo executables do not live here.

