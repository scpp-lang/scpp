# libs — scpp's library modules

This directory contains scpp's shipped library modules: the real-`std` module under `libs/std/`, plus scpp-specific extensions under `libs/scpp/`. Native helper libraries live here too. `libs/llvm/` is a second, distinct category: a single, plain C++20 module (`llvm`, split into six partitions) compiled directly by real clang++ (never by scpp itself) that lets this compiler's own `src/*.cppm` sources replace `#include <llvm-c/*.h>` with a single `import llvm;`; see its own section below.

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
| `llvm/llvm.cpp` | Primary interface unit of module `llvm`; re-exports its six partitions with `export import :...;` -- plain clang++-compiled, not a workspace member |
| `llvm/types.cpp` | Partition `llvm:types`: hand-written opaque handle struct tags/aliases mirroring the `llvm-c/Types.h` subset `llvm:core`, `llvm:debug_info`, and `src/compiler/codegen/api.cppm` use -- plain clang++-compiled, not a workspace member |
| `llvm/core.cpp` | Partition `llvm:core`: hand-written `extern "C"` mirror of the `llvm-c/Core.h` subset this compiler's own codegen uses, depending on `llvm:types` for its opaque handle types -- plain clang++-compiled, not a workspace member |
| `llvm/debug_info.cpp` | Partition `llvm:debug_info`: hand-written `extern "C"` mirror of the `llvm-c/DebugInfo.h` subset this compiler's own codegen uses, depending on `llvm:types` for its opaque handle types -- plain clang++-compiled, not a workspace member |
| `llvm/target.cpp` | Partition `llvm:target`: hand-written `extern "C"` mirror of the `llvm-c/Target.h` subset this compiler's own driver/codegen uses, depending on `llvm:types` for its opaque handle types -- plain clang++-compiled, not a workspace member |
| `llvm/target_machine.cpp` | Partition `llvm:target_machine`: hand-written `extern "C"` mirror of the `llvm-c/TargetMachine.h` subset this compiler's own driver uses, depending on `llvm:types` and `llvm:target` for its opaque handle types -- plain clang++-compiled, not a workspace member |
| `llvm/analysis.cpp` | Partition `llvm:analysis`: hand-written `extern "C"` mirror of the `llvm-c/Analysis.h` subset this compiler's own codegen uses, depending on `llvm:types` for its opaque handle types -- plain clang++-compiled, not a workspace member |
| `llvm/native_target_init.cpp` | Plain, never-`import`ed native-init shim bridging the one confirmed ABI gap in `llvm-c/Target.h` (`LLVMInitializeNativeTarget`/`LLVMInitializeNativeAsmPrinter` have no real exported symbol -- see `llvm:target`'s own section below) -- compiled into its own small static library, not a workspace member |

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

## The `llvm` module (partitions `:core`, `:types`, `:debug_info`, `:target`, `:target_machine`, `:analysis`)

`libs/llvm/` is a distinct category from `std`/`scpp` above
(scpp-buildable workspace members): it is a single, plain, ordinary C++20
module, `llvm`, split into a primary module interface unit plus six
partitions, compiled directly by real clang++ via one dedicated CMake
target (`llvm`, see `libs/llvm/CMakeLists.txt`, wired into the root via
`add_subdirectory(libs/llvm)`), never by scpp itself -- unlike `std`/`scpp`
above, there is no aspiration for these files to also be scpp-parseable. It
exists solely so this compiler's own `src/*.cppm` codegen/driver files can
replace a raw `#include <llvm-c/*.h>` with a single `import llvm;` -- scpp
(the language) has no preprocessor/`#include` at all, so any raw
`#include` left in the compiler's own sources is a hard blocker for
eventual self-hosting.

`llvm` was originally six independent, standalone top-level modules
(`llvm.types`, `llvm.core`, `llvm.debug_info`, `llvm.target`,
`llvm.target_machine`, `llvm.analysis`, introduced incrementally across
several PRs), each requiring its own separate `import llvm.<name>;` in
every consumer file that needed it. They were consolidated into one
module with six partitions because they were never truly independent in
practice: five of the six already depended on the sixth (`:types`), and
several consumer files needed two or three of the pieces at once (e.g.
`orchestration.cppm` needed `:core`, `:debug_info`, and `:analysis`
together). A single `import llvm;` -- with the primary interface unit
(`llvm.cpp`) re-exporting all six partitions -- gives every consumer file
everything it needs in one line, while the six `.cpp` files themselves,
and the dependency graph between them, are otherwise unchanged from
before this consolidation.

- File: `llvm/llvm.cpp` -- the primary module interface unit. Declares
  `export module llvm;` and re-exports every partition directly
  (`export import :core;`, `:types;`, `:debug_info;`, `:target;`,
  `:target_machine;`, `:analysis;`), mirroring the same
  primary-interface-plus-partitions pattern already used elsewhere in this
  codebase (e.g. `libs/std/std.scpp`, `libs/scpp/scpp.scpp`, and this
  compiler's own `src/compiler/codegen/codegen.cppm`/
  `src/compiler/movecheck/movecheck.cppm`).
- File: `llvm/types.cpp`, partition `llvm:types` -- a leaf partition (no
  cross-partition imports of its own). Contains hand-written opaque
  handle struct tags and pointer-alias `typedef`s mechanically mirroring
  the specific subset of real `llvm-c/Types.h` referenced by `llvm:core`
  (below), `llvm:debug_info` (below), and `src/compiler/codegen/api.cppm`
  (which needs raw handle types, e.g. `LLVMContextRef`, `LLVMDIBuilderRef`,
  for its `Codegen` class's members, but no Core.h function) -- not a
  blanket re-declaration of Types.h's much larger surface.
- File: `llvm/core.cpp`, partition `llvm:core` -- `export import :types;`
  (re-exports `:types`, since every current consumer needing `llvm:core`
  also needs `llvm:types`'s aliases directly). Contains hand-written
  `extern "C"` declarations mechanically mirroring the specific subset of
  real `llvm-c/Core.h` actually referenced by `src/driver.cppm` and
  `src/compiler/codegen/{layout,orchestration,debug,
  functions,object_model,lifetime,statements,expressions}.cppm` today --
  not a blanket re-declaration of Core.h's much larger surface. `core.cpp`
  itself has zero `#include` of any real `llvm-c/*.h` header -- a
  deliberate choice, not a requirement of the `.cpp` extension -- so,
  together with the `llvm:types` partition it depends on, its declarations
  are the single, self-contained source of truth this partition exports,
  rather than a re-export of someone else's macros.
- File: `llvm/debug_info.cpp`, partition `llvm:debug_info` -- `import
  :types;` (plain, not re-exported). Contains hand-written `extern "C"`
  declarations mechanically mirroring the specific subset of real
  `llvm-c/DebugInfo.h` actually referenced by
  `src/compiler/codegen/{orchestration,debug}.cppm` today -- not a blanket
  re-declaration of DebugInfo.h's much larger surface. Like `core.cpp`,
  `debug_info.cpp` itself has zero `#include` of any real `llvm-c/*.h`
  header. Unlike `llvm:core`, `llvm:debug_info` only plainly
  `import :types;` rather than re-exporting it (`export import`): both
  current consumers already reach `llvm:types`'s aliases through their own
  single `import llvm;` (which brings in every partition, `:core` --
  itself re-exporting `:types` -- included), so there is no other
  consumer needing them a second time through this path -- see
  `debug_info.cpp`'s own header comment for the full rationale.
- File: `llvm/target.cpp`, partition `llvm:target` -- `import :types;`
  (plain, same reasoning as `llvm:debug_info`). Contains hand-written
  `extern "C"` declarations mechanically mirroring the specific subset of
  real `llvm-c/Target.h` actually referenced by `src/driver.cppm` and
  `src/compiler/codegen/{layout,debug,expressions}.cppm` today (the 7
  `LLVMTargetData`-related functions) -- not a blanket re-declaration of
  Target.h's much larger surface. Like `core.cpp` and `debug_info.cpp`,
  `target.cpp` itself has zero `#include` of any real `llvm-c/*.h`
  header. `LLVMTargetDataRef`/`LLVMOpaqueTargetData` is genuinely
  Target.h's own declaration (not Types.h's), so it is declared in this
  partition rather than added to `llvm:types` -- mirroring how
  `LLVMAttributeIndex` stays in `llvm:core` for the same reason.
  `llvm:target` also declares two names with no real Target.h counterpart
  at all -- `scpp_llvm_target_initialize_native_target`/
  `scpp_llvm_target_initialize_native_asm_printer` -- bridging a genuine
  ABI gap; see "Native target/AsmPrinter initialization" below.
- File: `llvm/target_machine.cpp`, partition `llvm:target_machine` --
  `import :types;`/`import :target;` (both plain, same reasoning as
  `llvm:debug_info`/`llvm:target`). Contains hand-written `extern "C"`
  declarations mechanically mirroring the specific subset of real
  `llvm-c/TargetMachine.h` actually referenced by `src/driver.cppm` today
  -- the sole consumer of this header anywhere in the project (target-
  triple lookup, target-machine creation/disposal, data-layout-from-
  target-machine, object-file emission, and the 4 supporting enums) --
  not a blanket re-declaration of TargetMachine.h's much larger surface
  (the whole `LLVMTargetMachineOptionsRef` family, `LLVMGetFirstTarget`/
  `NextTarget`/`FromName`, `LLVMTargetHasJIT`/`HasTargetMachine`/
  `HasAsmBackend`, `LLVMGetTargetMachineTarget`/`Triple`/`CPU`/
  `FeatureString`, `LLVMSetTargetMachine*`,
  `LLVMTargetMachineEmitToMemoryBuffer`, `LLVMNormalizeTargetTriple`,
  `LLVMGetHostCPUName`/`Features`, and `LLVMAddAnalysisPasses` are all
  deliberately not declared). Like `core.cpp`, `debug_info.cpp`, and
  `target.cpp`, `target_machine.cpp` itself has zero `#include` of any
  real `llvm-c/*.h` header. `LLVMTargetRef`/`LLVMTarget` and
  `LLVMTargetMachineRef`/`LLVMOpaqueTargetMachine` are genuinely
  TargetMachine.h's own declarations (not Types.h's or Target.h's), so
  they are declared in this partition rather than added to `llvm:types`
  or `llvm:target` -- mirroring how `LLVMTargetDataRef`/
  `LLVMOpaqueTargetData` stays in `llvm:target` rather than `llvm:types`
  for the same reason. Note real TargetMachine.h's own naming
  inconsistency, kept byte-for-byte here since each tag must match
  upstream exactly: `LLVMTargetRef`'s pointee tag is spelled plain
  `LLVMTarget` (no `Opaque` prefix, unlike almost every other opaque
  handle across any of these six partitions) -- the same kind of upstream
  quirk `llvm:types` already documents for its own
  `LLVMOpaqueAttributeRef`.
- File: `llvm/analysis.cpp`, partition `llvm:analysis` -- `import :types;`
  (plain, same reasoning as `llvm:debug_info`/`llvm:target`/
  `llvm:target_machine`). Contains a hand-written `extern "C"` declaration
  mechanically mirroring the one real `llvm-c/Analysis.h` function
  actually referenced, by `src/compiler/codegen/orchestration.cppm` --
  the sole consumer of this header anywhere in the project -- not a
  blanket re-declaration of Analysis.h's much larger surface (`LLVMVerifyFunction`
  and the debugging-only `LLVMViewFunctionCFG`/`LLVMViewFunctionCFGOnly`
  pair, which open a ghostview window on the current function's CFG, are
  deliberately not declared). Like every other partition, `analysis.cpp`
  itself has zero `#include` of any real `llvm-c/*.h` header.
  `LLVMVerifierFailureAction` is genuinely Analysis.h's own declaration,
  so it is declared in this partition rather than added to `llvm:types` --
  mirroring how `LLVMAttributeIndex` stays in `llvm:core` for the same
  reason.
- None of the six partitions declares any RAII wrapper of its own: their
  raw `LLVM*Ref` declarations *are* the public surface those ten files
  (and `api.cppm`) call/use directly, exactly as they did through the real
  header, so every opaque handle kind (`LLVMContextRef`, `LLVMModuleRef`,
  `LLVMTypeRef`, `LLVMTargetDataRef`, `LLVMTargetMachineRef`, ...) is
  declared as its own distinct pointer type, never a shared `void*`.
- Every opaque handle struct tag any partition introduces (the 11 in
  `llvm:types`, plus `LLVMOpaqueTargetData` in `llvm:target` and
  `LLVMTarget`/`LLVMOpaqueTargetMachine` in `llvm:target_machine`) is
  declared directly in that partition's own module purview (attached to
  module `llvm` as a whole, per C++20's own module-partition rules), not a
  global module fragment, with only each tag's pointer alias
  (`LLVMContextRef`, `LLVMTargetDataRef`, `LLVMTargetMachineRef`, ...)
  `export`ed. Before this consolidation, when these six pieces were still
  independent top-level modules, each of these same tags instead lived in
  its own file's *global module fragment* (unattached to any module) --
  required at the time because two declarations of the same struct tag
  denote the same entity only if both are attached to the same named
  module, or neither is attached to any named module, and some consumer
  files still combined `import llvm.<name>;` with a raw, competing
  `#include` of another `llvm-c/*.h` header that transitively reached the
  same tag, unattached. That scenario can no longer arise: every one of
  the ten consumer files under `src/` has since migrated its last
  `#include <llvm-c/*.h>` to an `import`, and now that every partition
  that could ever redeclare one of these tags is part of the very same
  module `llvm`, a declaration attached to one partition is, per C++20's
  own rules, already considered attached to module `llvm` as a whole --
  so it agrees with any redeclaration in another `llvm:*` partition with
  no global-module-fragment indirection required. `native_target_init.cpp`
  (below) does not change this either: its own `#include <llvm-c/Target.h>`
  is a deliberately plain, never-`import`ed `.cpp`, compiled as its own,
  wholly separate translation unit, that never imports the `llvm` module.
  This simplification was verified empirically against this project's own
  full clean build, not just reasoned about in isolation -- see each
  affected partition's own header comment (`types.cpp`, `target.cpp`,
  `target_machine.cpp`) for the fuller argument. One wrinkle from the
  prior, six-separate-modules design is preserved regardless of this
  attachment simplification: `src/driver.cppm` names
  `LLVMOpaqueTargetMachine` as the pointee type of a `std::unique_ptr`
  rather than only its exported pointer alias; since a
  module-purview declaration that is not `export`ed is reachable but not
  *visible* to ordinary unqualified name lookup (exportedness, not
  attachment, governs visibility), that call site still spells this type
  as `std::remove_pointer_t<LLVMTargetMachineRef>` (from `<type_traits>`,
  already reachable via `src/driver.cppm`'s own pre-existing
  `import std;`) instead -- see `target_machine.cpp`'s own header comment
  for the fuller argument.
- `llvm:core` keeps only its genuinely Core.h-specific declarations
  (`LLVMAttributeIndex`, the ~118 functions, the 5 enums) and depends on
  `llvm:types` (`export import :types;`) for every opaque handle type
  it uses in its own signatures -- see `core.cpp`'s own header comment.
  `llvm:debug_info` likewise keeps only its genuinely DebugInfo.h-specific
  declarations (`LLVMDIFlags`, `LLVMDWARFSourceLanguage`,
  `LLVMDWARFEmissionKind`, `LLVMDWARFTypeEncoding`, the 20 functions) and
  depends on `llvm:types` (plain `import :types;`, not re-exported -- see
  above) for every opaque handle type it uses in its own signatures,
  including `LLVMDbgRecordRef` -- see `debug_info.cpp`'s own header
  comment. `llvm:target` likewise keeps only its genuinely
  Target.h-specific declarations (`LLVMTargetDataRef`/
  `LLVMOpaqueTargetData`, the 7 functions) and depends on `llvm:types`
  (plain `import :types;`, not re-exported, same reasoning as
  `llvm:debug_info`) for every other opaque handle type it uses in its own
  signatures -- see `target.cpp`'s own header comment. `llvm:target_machine`
  likewise keeps only its genuinely TargetMachine.h-specific declarations
  (`LLVMTargetRef`/`LLVMTarget`, `LLVMTargetMachineRef`/
  `LLVMOpaqueTargetMachine`, the 4 enums, the 6 functions) and depends on
  both `llvm:types` and `llvm:target` (plain `import`s, not re-exported,
  same reasoning as `llvm:debug_info`/`llvm:target`) for every other
  opaque handle type it uses in its own signatures -- see
  `target_machine.cpp`'s own header comment. `llvm:analysis` likewise
  keeps only its genuinely Analysis.h-specific declaration
  (`LLVMVerifierFailureAction`, the 1 function) and depends on
  `llvm:types` (plain `import :types;`, not re-exported, same reasoning as
  the other non-`:core` partitions) for every other opaque handle type it
  uses in its own signature -- see `analysis.cpp`'s own header comment.

### Native target/AsmPrinter initialization -- an ABI gap in `llvm:target`

Real `llvm-c/Target.h` declares `LLVMInitializeNativeTarget()` and
`LLVMInitializeNativeAsmPrinter()` as `static inline` functions defined
entirely *in the header itself*, expanding (via the
`LLVM_NATIVE_TARGET`/`LLVM_NATIVE_ASMPRINTER` macros, set by LLVM's own
build configuration) to whichever concrete backend the host was actually
built for -- e.g. `LLVMInitializeX86Target()` on a typical x86 build.
Unlike every other function `llvm:target` declares, there is therefore no
real, ABI-stable, exported `LLVMInitializeNativeTarget`/
`LLVMInitializeNativeAsmPrinter` symbol in LLVM's own compiled libraries to
declare `extern "C"` and link against directly -- confirmed empirically
(`nm -D --defined-only libLLVM-22.so | grep LLVMInitializeNative` finds
nothing, while the concrete-architecture symbol it expands to on that
host, `LLVMInitializeX86Target`, *is* a real, exported symbol). `src/driver.cppm`
is the one file needing "native, whatever this host is" initialization.

`llvm/native_target_init.cpp` bridges this one gap: a small, deliberately
plain, never-`import`ed `.cpp` (not part of the consolidated `llvm`
target's own `FILE_SET cxx_modules`, and not dual-compilable the way
`libs/std/*.scpp` or `libs/scpp/*.scpp` are) that itself `#include`s the
real `<llvm-c/Target.h>` -- something `target.cpp` itself never does --
to reach the two real inline function bodies, and re-exports each one's
result under a new, distinct `extern "C"` name
(`scpp_llvm_target_initialize_native_target`/
`scpp_llvm_target_initialize_native_asm_printer`) that `target.cpp`
declares and `src/driver.cppm` calls directly in place of the real names.
It is compiled into its own small static library (`llvm_target_native_init`,
see `libs/llvm/CMakeLists.txt`), linked privately into the consolidated
`llvm` target's own compiled output. Hard-coding `llvm:target`'s own
declarations to one specific architecture's concrete symbol names instead
(the only alternative needing no shim at all) was rejected: it would
silently break on any host LLVM wasn't built natively for that one
architecture.

## Testing policy

`libs/` is library source, not a demo area. Coverage belongs in the real
test suites:

- `tests/` for dev-agent-owned unit/integration coverage
- `blackbox_test/` for user-visible language/library behavior

Native helper libraries are built here because the tests and compiler need
them, but demo executables do not live here.

