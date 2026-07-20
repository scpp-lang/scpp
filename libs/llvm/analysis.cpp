// analysis.cpp
//
// `llvm.analysis`: a fresh, standalone, top-level module -- not a partition
// or nested submodule of either `scpp` (this project's own
// compiler-internal modules, e.g. `scpp.ast`, `scpp.compiler.codegen`),
// `scpp.llvm` (the separate, ergonomic RAII wrapper package at
// libs/scpp_llvm/), or `llvm.core`/`llvm.types`/`llvm.debug_info`/
// `llvm.target`/`llvm.target_machine` (the five sibling modules in this
// same directory). Its only job is to give this compiler's own real-C++
// `src/*.cppm` files a way to reach official LLVM-C's `llvm-c/Analysis.h`
// surface via `import llvm.analysis;` instead of
// `#include <llvm-c/Analysis.h>` -- scpp (the language) has no
// preprocessor/#include at all, so any raw #include left in this
// compiler's own sources is a hard blocker for eventual self-hosting.
//
// This is a plain, ordinary C++ file (a `.cpp`, not a `.scpp`), compiled
// only by real clang++ (see the `llvm_analysis` CMake target in
// libs/llvm/CMakeLists.txt) and never fed to the scpp compiler itself --
// exactly like `llvm.core`'s core.cpp, `llvm.types`'s types.cpp,
// `llvm.debug_info`'s debug_info.cpp, `llvm.target`'s target.cpp, and
// `llvm.target_machine`'s target_machine.cpp, there is no aspiration here
// for this specific file to also be scpp-parseable today. `export module
// llvm.analysis;` below is nonetheless unrestricted, standard C++20 module
// syntax -- real ISO C++, nothing scpp-specific about it -- so the
// resulting compiled module interface will still be `import`able the same
// way from scpp-compiled code, once these files themselves eventually get
// rewritten in scpp; only this file's own *source* needs never be
// scpp-parseable.
//
// Scope: only the specific Analysis.h declaration actually referenced today
// by src/compiler/codegen/orchestration.cppm -- and *only* by that one
// file, the sole remaining consumer of llvm-c/Analysis.h anywhere in this
// project -- surveyed function-by-function and signature-by-signature
// against that one file and real llvm-c/Analysis.h, not a blanket
// re-declaration of Analysis.h's much larger surface (real Analysis.h also
// declares `LLVMVerifyFunction` and the debugging-only
// `LLVMViewFunctionCFG`/`LLVMViewFunctionCFGOnly` pair, which open a
// ghostview window on the current function's CFG -- entirely unsuited to
// this compiler's own batch codegen -- none of which orchestration.cppm
// needs). This module's own introduction removes the *last*
// `#include <llvm-c/*.h>` anywhere in this project's own compiler sources:
// Analysis.h was the sixth and final llvm-c header still directly
// `#include`d by any `src/*.cppm` file, after `llvm.core`/`llvm.types`
// (Core.h/Types.h), `llvm.debug_info` (DebugInfo.h), and
// `llvm.target`/`llvm.target_machine` (Target.h/TargetMachine.h) each
// already migrated their own narrow slice (`native_target_init.cpp`, same
// directory, keeps its own separate, deliberate, permanent
// `#include <llvm-c/Target.h>` -- see its own header comment -- but that
// one is never `import`ed by anything, was always meant to be a permanent
// bridge rather than a temporary stepping stone like this file, and is not
// part of `src/` itself).
//
// Depends on `llvm.types` (types.cpp, same directory) for `LLVMModuleRef`
// (the module handle `LLVMVerifyModule` below checks) and `LLVMBool` (its
// return type): those are genuinely llvm-c/Types.h's own declarations, not
// Analysis.h's -- exactly the same reasoning `llvm.core`, `llvm.debug_info`,
// `llvm.target`, and `llvm.target_machine` already rely on for their own
// `import llvm.types;` (see core.cpp/debug_info.cpp/target.cpp/
// target_machine.cpp). Like `llvm.debug_info`, `llvm.target`, and
// `llvm.target_machine` themselves, this module does *not* `export import
// llvm.types;`: its one current consumer (orchestration.cppm) already
// reaches both aliases through its own separate `import llvm.core;` (which
// re-exports `llvm.types`), so re-exporting the same names a second time
// here would only widen this module's own public surface with no consumer
// needing it through this path -- a plain `import llvm.types;` still makes
// both aliases nameable within this file's own module purview below (which
// is all this file itself needs), and reachable (though not separately
// nameable via this module alone) to any importer of this module that sees
// the exported function signature below mentioning them.
//
// `LLVMVerifierFailureAction`, unlike `LLVMModuleRef`/`LLVMBool`, is
// genuinely Analysis.h's *own* declaration (a small, three-value anonymous
// C `enum`), so -- mirroring how `LLVMAttributeIndex` stays in `llvm.core`
// rather than moving to `llvm.types` (see core.cpp's own header comment) --
// it is declared directly here, in this module's own purview, rather than
// added to `llvm.types`. Unlike an opaque handle's struct tag, a plain enum
// declaration has none of the cross-module attachment restriction discussed
// at length in types.cpp's own header comment (that restriction is specific
// to class/struct/union tags, not enums), so it needs no
// global-module-fragment placement of its own. This module introduces no
// new opaque handle struct tag at all -- its one function below only takes/
// returns handle kinds `llvm.types` already declares -- so this file's own
// global module fragment is empty, exactly like `llvm.core`'s own core.cpp.
//
// Contrast with libs/scpp_llvm/: that package wraps LLVM-C in ergonomic,
// RAII scpp classes (Context/Module/Type/Value) for scpp *user* programs,
// and deliberately declares every opaque handle as a shared `void*`
// underneath those classes -- real type safety there comes from the
// wrapper classes, not the raw handles. This module has no such wrapper:
// its raw `LLVMVerifyModule` declaration *is* the public surface
// orchestration.cppm calls directly, exactly as it did through the real
// header.
module;

export module llvm.analysis;

import llvm.types;

// ---------------------------------------------------------------------
// Enums (llvm-c/Analysis.h)
// ---------------------------------------------------------------------
// Real llvm-c/Analysis.h declares this as an anonymous C `enum` via
// `typedef enum { LLVMAbortProcessAction, LLVMPrintMessageAction,
// LLVMReturnStatusAction } LLVMVerifierFailureAction;`, whose enumerators
// auto-increment from 0 in declaration order and every existing call site
// -- and the real header itself -- reaches unqualified, with no
// `LLVMVerifierFailureAction::` (or similar) prefix. Named here only so
// the type itself has a spellable name for use as a parameter type below,
// while keeping the same unqualified enumerator lookup and the exact real
// ABI/value contract, with no call-site changes needed beyond the
// include-to-import swap this task calls for.
//
// Only `LLVMReturnStatusAction` (this project's codegen always requests
// "just return a nonzero LLVMBool on failure" verification handling, never
// LLVMAbortProcessAction's abort() or LLVMPrintMessageAction's
// print-and-return) is referenced by orchestration.cppm, so only it is
// declared below, given its exact real, auto-increment-derived numeric
// value (2) explicitly -- confirmed empirically against real
// llvm-c/Analysis.h (a small standalone program #include-ing the real
// header and printing each enumerator's value), not just read off the
// header's declaration order -- rather than left to depend on every other,
// undeclared enumerator's own position.
export enum LLVMVerifierFailureAction {
    LLVMReturnStatusAction = 2,
};

// ---------------------------------------------------------------------
// Functions (llvm-c/Analysis.h)
// ---------------------------------------------------------------------
// Matches real llvm-c/Analysis.h's own linkage-specification for this
// symbol -- it is implemented by, and this project ultimately links
// against, LLVM's own compiled libraries (see LLVM_LIBS in root
// CMakeLists.txt), not by this module. Real C++ exports every declaration
// nested inside an `export extern "C" { ... }` block, so no per-line
// `export` repetition is needed for this one declaration to be part of
// this module's public interface. (This deliberately differs from
// libs/scpp_llvm/core/scpp_llvm_core.scpp's own `extern "C"` block, which
// is intentionally *not* exported there -- that package hides its raw
// LLVM-C bindings behind ergonomic wrapper classes and only exports those;
// this module has no such wrapper; the raw binding itself is the public
// surface.)
export extern "C" {

LLVMBool LLVMVerifyModule(LLVMModuleRef M, LLVMVerifierFailureAction Action, char** OutMessage);

} // extern "C"
