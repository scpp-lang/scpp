// target_machine.cpp
//
// `llvm.target_machine`: a fresh, standalone, top-level module -- not a
// partition or nested submodule of either `scpp` (this project's own
// compiler-internal modules, e.g. `scpp.ast`, `scpp.compiler.codegen`),
// `scpp.llvm` (the separate, ergonomic RAII wrapper package at
// libs/scpp_llvm/), or `llvm.core`/`llvm.types`/`llvm.debug_info`/
// `llvm.target` (the four sibling modules in this same directory). Its
// only job is to give this compiler's own real-C++ `src/*.cppm` files a
// way to reach official LLVM-C's `llvm-c/TargetMachine.h` surface via
// `import llvm.target_machine;` instead of
// `#include <llvm-c/TargetMachine.h>` -- scpp (the language) has no
// preprocessor/#include at all, so any raw #include left in this
// compiler's own sources is a hard blocker for eventual self-hosting.
//
// This is a plain, ordinary C++ file (a `.cpp`, not a `.scpp`), compiled
// only by real clang++ (see the `llvm_target_machine` CMake target in
// libs/llvm/CMakeLists.txt) and never fed to the scpp compiler itself --
// exactly like `llvm.core`'s core.cpp, `llvm.types`'s types.cpp,
// `llvm.debug_info`'s debug_info.cpp, and `llvm.target`'s target.cpp,
// there is no aspiration here for this specific file to also be
// scpp-parseable today. `export module llvm.target_machine;` below is
// nonetheless unrestricted, standard C++20 module syntax -- real ISO
// C++, nothing scpp-specific about it -- so the resulting compiled
// module interface will still be `import`able the same way from
// scpp-compiled code, once these files themselves eventually get
// rewritten in scpp; only this file's own *source* needs never be
// scpp-parseable.
//
// Scope: only the specific TargetMachine.h declarations actually
// referenced today by src/driver.cppm -- and *only* by src/driver.cppm,
// the sole remaining consumer of llvm-c/TargetMachine.h anywhere in this
// project -- surveyed function-by-function and signature-by-signature
// against that one file and real llvm-c/TargetMachine.h, not a blanket
// re-declaration of TargetMachine.h's much larger surface (real
// TargetMachine.h also declares e.g. `LLVMGetFirstTarget`/
// `LLVMGetNextTarget`, `LLVMGetTargetFromName`, `LLVMGetTargetName`,
// `LLVMGetTargetDescription`, `LLVMTargetHasJIT`/
// `LLVMTargetHasTargetMachine`/`LLVMTargetHasAsmBackend`, the whole
// `LLVMTargetMachineOptionsRef`/`LLVMCreateTargetMachineOptions`/
// `LLVMTargetMachineOptionsSet*` family and its `...WithOptions`
// constructor, `LLVMGetTargetMachineTarget`/`...Triple`/`...CPU`/
// `...FeatureString`, `LLVMSetTargetMachineAsmVerbosity`/`...FastISel`/
// `...GlobalISel`/`...GlobalISelAbort`/`...MachineOutliner`,
// `LLVMTargetMachineEmitToMemoryBuffer`, `LLVMNormalizeTargetTriple`,
// `LLVMGetHostCPUName`/`...Features`, and `LLVMAddAnalysisPasses`, none
// of which src/driver.cppm needs). The other still-`#include`d
// llvm-c/*.h header referenced by this project's own sources
// (Analysis.h, in src/compiler/codegen/orchestration.cppm, a different
// file entirely) is deliberately untouched -- out of scope for this
// change, left for its own later, equally narrow follow-up. This
// module's own introduction leaves that single header as the only
// `#include <llvm-c/*.h>` left anywhere in this project's own compiler
// *sources* still awaiting a migration like this one
// (`native_target_init.cpp`, same directory, keeps its own separate,
// deliberate, permanent `#include <llvm-c/Target.h>` -- see its own
// header comment -- but that one is never `import`ed by anything and was
// always meant to be a permanent bridge, not a temporary stepping stone
// like this file).
//
// Depends on `llvm.types` (types.cpp, same directory) for `LLVMModuleRef`
// (the module handle `LLVMTargetMachineEmitToFile` below emits) and
// `LLVMBool` (the shared boolean-result convention `LLVMGetTargetFromTriple`
// and `LLVMTargetMachineEmitToFile` below both return): those are
// genuinely llvm-c/Types.h's own declarations, not TargetMachine.h's --
// exactly the same reasoning `llvm.core`, `llvm.debug_info`, and
// `llvm.target` already rely on for their own `import llvm.types;` (see
// core.cpp/debug_info.cpp/target.cpp). Also depends on `llvm.target`
// (target.cpp, same directory) for `LLVMTargetDataRef` (the data-layout
// handle `LLVMCreateTargetDataLayout` below returns): that one is
// genuinely llvm-c/Target.h's own declaration (`typedef struct
// LLVMOpaqueTargetData *LLVMTargetDataRef;`), not TargetMachine.h's --
// real llvm-c/TargetMachine.h itself only reaches it by its own
// `#include "llvm-c/Target.h"`, exactly mirroring how this module reaches
// it by `import llvm.target;` instead of re-declaring a second, private
// copy. Like `llvm.debug_info` and `llvm.target` themselves, this module
// does *not* `export import` either dependency: src/driver.cppm, its one
// current consumer, already reaches every one of these three aliases
// through its own separate `import llvm.core;` (which re-exports
// `llvm.types`) and `import llvm.target;` (declared directly), so
// re-exporting the same names a second time here would only widen this
// module's own public surface with no consumer needing it through this
// path -- a plain `import` still makes every one of those aliases
// nameable within this file's own module purview below (which is all
// this file itself needs), and reachable to any importer of this module
// that sees one of the `export`ed function signatures below mentioning
// them.
//
// `LLVMTargetRef` and `LLVMTargetMachineRef`, unlike every alias reused
// above, are genuinely TargetMachine.h's *own* declarations
// (`typedef struct LLVMTarget *LLVMTargetRef;` and `typedef struct
// LLVMOpaqueTargetMachine *LLVMTargetMachineRef;`) -- so, mirroring how
// `LLVMTargetDataRef`/`LLVMOpaqueTargetData` stays in `llvm.target`
// rather than moving to `llvm.types` (see target.cpp's own header
// comment), they are declared here, in this module, rather than added to
// `llvm.types` or `llvm.target`. Note the real header's own naming
// inconsistency, kept byte-for-byte here since each tag name must match
// upstream exactly for the two declarations to denote the same type:
// `LLVMTargetRef`'s pointee tag is spelled plain `LLVMTarget` (no
// `Opaque` prefix, unlike almost every other opaque handle across any of
// these five modules), while `LLVMTargetMachineRef`'s pointee tag is
// spelled `LLVMOpaqueTargetMachine` (with the usual prefix) -- exactly
// the same kind of upstream quirk `llvm.types` already documents for its
// own `LLVMOpaqueAttributeRef` (see types.cpp's own header comment).
// `LLVMCreateTargetMachineOptions`'s own pointee tag,
// `LLVMOpaqueTargetMachineOptions`, is not declared at all: the whole
// `LLVMTargetMachineOptionsRef` family is out of this module's scope (see
// above), since src/driver.cppm calls the older, simpler
// `LLVMCreateTargetMachine` overload directly instead.
//
// Contrast with libs/scpp_llvm/: that package wraps LLVM-C in ergonomic,
// RAII scpp classes (Context/Module/Type/Value) for scpp *user* programs,
// and deliberately declares every opaque handle as a shared `void*`
// underneath those classes -- real type safety there comes from the
// wrapper classes, not the raw handles. This module has no such wrapper:
// its raw `LLVM*Ref` declarations *are* the public surface
// src/driver.cppm calls directly, exactly as it did through the real
// header, so `LLVMTargetRef` and `LLVMTargetMachineRef` (declared by this
// module) are each their own distinct pointer type, and distinct from
// every handle kind reused from `llvm.types`/`llvm.target` too (never a
// shared `void*`) -- otherwise the compiler could no longer catch e.g. an
// `LLVMTargetRef` accidentally passed where an `LLVMTargetMachineRef` was
// expected, silently trading a compile error for a runtime bug.
//
// Module-attachment note: both `LLVMTarget` and `LLVMOpaqueTargetMachine`
// -- the two new opaque handle struct tags this module introduces -- live
// in this file's own *global module fragment* (the `module;` ...
// declarations ... before `export module llvm.target_machine;` below),
// deliberately *not* in the module purview, and are never `export`ed
// directly (a global module fragment cannot export anything; only
// `LLVMTargetRef`/`LLVMTargetMachineRef`, the pointer aliases further
// below, are exported). This mirrors exactly the same rule `llvm.types`
// and `llvm.target` already established for their own struct tags (see
// types.cpp's/target.cpp's own header comments for the full "two
// declarations of the same struct tag denote the same entity only if
// both are attached to the same named module, or neither is attached to
// any named module" rationale) -- applied here even though, unlike every
// prior module's introduction, src/driver.cppm's own edit (see below)
// removes the *last* `#include <llvm-c/*.h>` in that file, leaving no
// other, competing, unattached declaration of either tag anywhere in that
// translation unit any more. An unattached global-module-fragment
// declaration is well-formed and self-sufficient on its own, whether or
// not some other unattached copy of the same tag also happens to exist
// elsewhere in the same translation unit, so this placement remains
// correct (and future-proofs this module the same way `llvm.target`'s own
// tag is future-proofed, against any not-yet-existing future consumer
// that might combine `import llvm.target_machine;` with some other real,
// still-`#include`d header transitively reaching the same tags
// unattached) even though the specific historical trigger -- a
// still-`#include`d TargetMachine.h/Target.h transitively reaching the
// same tags unattached in the *same* file that also imports the module --
// no longer applies to src/driver.cppm today. Verified directly against
// this project's own real build, not just reasoned about in isolation.
//
// This *does*, however, surface a new wrinkle none of the four prior
// modules ever hit: unlike every existing consumer of any of those four
// modules -- which only ever spell each opaque handle kind through its
// exported pointer-alias name (`LLVMContextRef`, `LLVMTargetDataRef`,
// ...), never the bare struct tag itself -- src/driver.cppm's own
// pre-existing code spells `LLVMOpaqueTargetMachine` directly, as the
// pointee type of a `std::unique_ptr`
// (`std::unique_ptr<LLVMOpaqueTargetMachine, void (*)(LLVMTargetMachineRef)>`),
// to name the type `LLVMDisposeTargetMachine` (the deleter) operates on.
// A global-module-fragment declaration is reachable but, unlike an
// exported declaration, is not *visible* to ordinary unqualified name
// lookup in an importer that has no other route to the same name -- so,
// once src/driver.cppm's own `#include <llvm-c/TargetMachine.h>` is gone,
// plainly writing the bare `LLVMOpaqueTargetMachine` identifier there
// would no longer resolve to anything, breaking the build. Rather than
// exporting the tag directly from this module's purview (which would
// abandon the module-attachment safety net above for no real benefit,
// since nothing else needs the bare tag nameable), src/driver.cppm's own
// one call site is updated instead, from the bare tag name to
// `std::remove_pointer_t<LLVMTargetMachineRef>` -- the identical type,
// computed from the exported (and therefore genuinely visible) alias
// instead of the unattached (reachable-only) tag, needing no unqualified
// lookup of the tag's own name at all. `std::remove_pointer_t` comes from
// `<type_traits>`, already reachable via src/driver.cppm's own
// pre-existing `import std;`. Verified directly against this project's
// own real build, not just reasoned about in isolation -- exactly the
// same empirical discipline every one of the four prior modules already
// applied.
module;

struct LLVMOpaqueTargetMachine;
struct LLVMTarget;

export module llvm.target_machine;

import llvm.types;
import llvm.target;

// ---------------------------------------------------------------------
// Opaque handle pointer aliases (llvm-c/TargetMachine.h)
// ---------------------------------------------------------------------
// Unlike the struct tags above, these pointer aliases *are* declared in
// the module purview and `export`ed directly: a type-alias redeclaration
// is always well-formed as long as every redeclaration in scope denotes
// the exact same type, with none of the struct tags' cross-module
// attachment restriction above.
export using LLVMTargetMachineRef = LLVMOpaqueTargetMachine*;
export using LLVMTargetRef = LLVMTarget*;

// ---------------------------------------------------------------------
// Enums (llvm-c/TargetMachine.h)
// ---------------------------------------------------------------------
// Real llvm-c/TargetMachine.h declares each of these as an anonymous C
// `enum` via `typedef enum { ... } LLVMFoo;`, whose enumerators every
// existing call site -- and the real header itself -- reaches
// unqualified, with no `LLVMFoo::` (or similar) prefix. Named here only
// so each type itself has a spellable name for use as a parameter/return
// type below, while keeping the same unqualified enumerator lookup and
// the exact real ABI/value contract, with no call-site changes needed
// beyond the include-to-import swap this task calls for.
//
// Only the enumerators actually referenced by src/driver.cppm are
// declared below, each given its exact real, auto-increment-derived
// numeric value explicitly (never left to auto-increment here, so that
// any omitted enumerator never shifts the value of one that is
// declared) -- confirmed empirically against real llvm-c/TargetMachine.h
// (a small standalone program #include-ing the real header and printing
// each enumerator's value), not just read off the header's declaration
// order.

export enum LLVMCodeGenOptLevel {
    LLVMCodeGenLevelNone = 0,
    LLVMCodeGenLevelLess = 1,
    LLVMCodeGenLevelDefault = 2,
    LLVMCodeGenLevelAggressive = 3,
};

// Only `LLVMRelocPIC` (position-independent code, the one relocation mode
// src/driver.cppm ever requests) is reproduced below; real
// llvm-c/TargetMachine.h's own `LLVMRelocMode` also declares
// LLVMRelocDefault/Static/DynamicNoPic/ROPI/RWPI/ROPI_RWPI, none of which
// this project's driver needs.
export enum LLVMRelocMode {
    LLVMRelocPIC = 2,
};

// Only `LLVMCodeModelDefault` (src/driver.cppm never requests a
// non-default code model) is reproduced below; real
// llvm-c/TargetMachine.h's own `LLVMCodeModel` also declares
// JITDefault/Tiny/Small/Kernel/Medium/Large.
export enum LLVMCodeModel {
    LLVMCodeModelDefault = 0,
};

// Only `LLVMObjectFile` (src/driver.cppm always emits a native object
// file, never a textual assembly file) is reproduced below; real
// llvm-c/TargetMachine.h's own `LLVMCodeGenFileType` also declares
// LLVMAssemblyFile.
export enum LLVMCodeGenFileType {
    LLVMObjectFile = 1,
};

// ---------------------------------------------------------------------
// Functions (llvm-c/TargetMachine.h)
// ---------------------------------------------------------------------
// A single `extern "C" { ... }` block, matching real
// llvm-c/TargetMachine.h's own linkage-specification for every one of
// these symbols -- they are implemented by, and this project ultimately
// links against, LLVM's own compiled static libraries (see LLVM_LIBS in
// root CMakeLists.txt), not by this module. Real C++ exports every
// declaration nested inside an `export extern "C" { ... }` block, so no
// per-line `export` repetition is needed for each of the 6 declarations
// below to be part of this module's public interface.
export extern "C" {

// Target
LLVMBool LLVMGetTargetFromTriple(const char* Triple, LLVMTargetRef* T, char** ErrorMessage);

// Target Machine
LLVMTargetMachineRef LLVMCreateTargetMachine(LLVMTargetRef T, const char* Triple, const char* CPU,
                                             const char* Features, LLVMCodeGenOptLevel Level, LLVMRelocMode Reloc,
                                             LLVMCodeModel CodeModel);
void LLVMDisposeTargetMachine(LLVMTargetMachineRef T);
LLVMTargetDataRef LLVMCreateTargetDataLayout(LLVMTargetMachineRef T);
LLVMBool LLVMTargetMachineEmitToFile(LLVMTargetMachineRef T, LLVMModuleRef M, const char* Filename,
                                     LLVMCodeGenFileType codegen, char** ErrorMessage);

// Triple
char* LLVMGetDefaultTargetTriple(void);

} // extern "C"
