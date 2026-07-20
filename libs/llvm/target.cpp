// target.cpp
//
// `llvm.target`: a fresh, standalone, top-level module -- not a partition
// or nested submodule of `scpp` (this project's own
// compiler-internal modules, e.g. `scpp.ast`, `scpp.compiler.codegen`) or
// `llvm.core`/`llvm.types`/`llvm.debug_info` (the three sibling modules
// in this same directory). Its only job is to give
// this compiler's own real-C++ `src/*.cppm` files a way to reach official
// LLVM-C's `llvm-c/Target.h` surface via `import llvm.target;` instead of
// `#include <llvm-c/Target.h>` -- scpp (the language) has no
// preprocessor/#include at all, so any raw #include left in this
// compiler's own sources is a hard blocker for eventual self-hosting.
//
// This is a plain, ordinary C++ file (a `.cpp`, not a `.scpp`), compiled
// only by real clang++ (see the `llvm_target` CMake target in
// libs/llvm/CMakeLists.txt) and never fed to the scpp compiler itself --
// exactly like `llvm.core`'s core.cpp, `llvm.types`'s types.cpp, and
// `llvm.debug_info`'s debug_info.cpp, there is no aspiration here for this
// specific file to also be scpp-parseable today. `export module
// llvm.target;` below is nonetheless unrestricted, standard C++20 module
// syntax -- real ISO C++, nothing scpp-specific about it -- so the
// resulting compiled module interface will still be `import`able the same
// way from scpp-compiled code, once these files themselves eventually get
// rewritten in scpp; only this file's own *source* needs never be
// scpp-parseable.
//
// Scope: only the specific Target.h declarations actually referenced
// today by src/driver.cppm and src/compiler/codegen/{layout,debug,
// expressions}.cppm -- surveyed function-by-function and
// signature-by-signature against those four files and real
// llvm-c/Target.h, not a blanket re-declaration of Target.h's much larger
// surface (real Target.h also declares e.g. `LLVMSetModuleDataLayout`,
// `LLVMCreateTargetData`, `LLVMAddTargetLibraryInfo`, `LLVMByteOrder`,
// `LLVMPointerSize`, the deprecated `LLVMIntPtrType`/`LLVMIntPtrTypeForAS`
// pair, `LLVMIntPtrTypeInContext`/`LLVMIntPtrTypeForASInContext`,
// `LLVMStoreSizeOfType`, `LLVMCallFrameAlignmentOfType`,
// `LLVMPreferredAlignmentOfType`, `LLVMPreferredAlignmentOfGlobal`,
// `LLVMElementAtOffset`, `LLVMOffsetOfElement`, the whole per-target/
// per-component `LLVMInitialize<Target>{TargetInfo,Target,TargetMC,
// AsmPrinter,AsmParser,Disassembler}` family and its `LLVMInitializeAll*`
// aggregates, and `LLVMByteOrdering`/`LLVMTargetLibraryInfoRef`, none of
// which any of the four files need). The other still-`#include`d
// llvm-c/*.h headers in those same files (TargetMachine.h, Analysis.h)
// are deliberately untouched -- out of scope for this change, left for
// their own later, equally narrow follow-ups.
//
// Depends on `llvm.types` (types.cpp, same directory) for every opaque
// handle pointer alias used below (LLVMModuleRef, LLVMTypeRef) plus
// LLVMBool: those are genuinely llvm-c/Types.h's own declarations, not
// Target.h's -- exactly the same reasoning `llvm.core` and
// `llvm.debug_info` already rely on for their own `import llvm.types;`
// (see core.cpp/debug_info.cpp). Unlike `llvm.core`, this module does
// *not* `export import llvm.types;`: every current consumer (driver.cppm,
// layout.cppm, debug.cppm, expressions.cppm) already reaches both aliases
// through its own separate `import llvm.core;` (which itself re-exports
// `llvm.types`), so re-exporting the same names a second time here would
// only widen this module's own public surface with no consumer needing it
// through this path -- exactly the same reasoning `llvm.debug_info`
// already applied (see debug_info.cpp's own header comment).
//
// LLVMTargetDataRef, unlike every alias `llvm.types` itself declares, is
// genuinely Target.h's *own* declaration (`typedef struct
// LLVMOpaqueTargetData *LLVMTargetDataRef;`, declared directly in
// llvm-c/Target.h, not llvm-c/Types.h) -- so, mirroring how
// `LLVMAttributeIndex` stays in `llvm.core` rather than moving to
// `llvm.types` (see core.cpp's own header comment), it is declared here,
// in this module, rather than added to `llvm.types`.
//
// This module declares no RAII wrapper of its own: its raw `LLVM*Ref`
// declarations *are* the public surface all four consumers call
// directly, exactly as they did through the real header, so
// `LLVMTargetDataRef` (declared by this module) is its own distinct
// pointer type from every other handle kind (never a shared `void*`) --
// otherwise the compiler could no longer catch e.g. an
// `LLVMTargetDataRef` accidentally passed where an `LLVMTypeRef` was
// expected, silently trading a compile error for a runtime bug.
//
// Module-attachment note: `LLVMOpaqueTargetData` -- the one new opaque
// handle struct tag this module introduces -- lives in this file's own
// *global module fragment* (the `module;` ... declaration ... before
// `export module llvm.target;` below), deliberately *not* in the module
// purview, and is never `export`ed directly (a global module fragment
// cannot export anything; only `LLVMTargetDataRef`, the pointer alias
// further below, is exported). This mirrors exactly the same rule
// `llvm.types` already established for its own struct tags (see
// types.cpp's own header comment for the full "two declarations of the
// same struct tag denote the same entity only if both are attached to the
// same named module, or neither is attached to any named module"
// rationale), applied here to a tag this module owns directly rather than
// one reused from `llvm.types`: at the time this module was introduced,
// src/driver.cppm, its one consumer, still combined `import llvm.target;`
// with a raw `#include <llvm-c/TargetMachine.h>` (itself transitively
// `#include`ing its own, unattached copy of llvm-c/Target.h, and
// therefore of `LLVMOpaqueTargetData` too), which would otherwise have
// failed to build with "declaration 'LLVMOpaqueTargetData' attached to
// named module 'llvm.target' cannot be attached to other modules" --
// exactly the failure mode `llvm.types`'s own header comment describes an
// earlier version of `llvm.core` hitting for its own tags. Declaring the
// tag unattached here instead kept both copies agreeing, denoting one and
// the same type. src/driver.cppm's own `#include <llvm-c/TargetMachine.h>`
// has since been replaced by `import llvm.target_machine;` (see that
// module, same directory), removing the specific live conflict described
// above -- but the unattached placement here is retained regardless, as
// the same uniform, defensive convention every `llvm.*` module applies to
// every opaque handle tag it introduces, independent of whether any
// current consumer happens to have a competing unattached copy today (see
// `llvm.target_machine`'s own header comment for the fuller version of
// this same argument, written for its own two new tags). Verified
// directly against this project's own real build, not just reasoned
// about in isolation.
//
// Native target/AsmPrinter initialization: real llvm-c/Target.h's
// `LLVMInitializeNativeTarget()`/`LLVMInitializeNativeAsmPrinter()` are
// `static inline` functions defined entirely *in that header*, expanding
// (via the LLVM_NATIVE_TARGET/LLVM_NATIVE_ASMPRINTER macros, themselves
// set by LLVM's own build configuration) to whichever concrete backend
// the host was actually built for -- e.g. LLVMInitializeX86Target() on
// this machine. Unlike every other function this module declares below,
// there is therefore no real, ABI-stable, exported
// `LLVMInitializeNativeTarget`/`LLVMInitializeNativeAsmPrinter` symbol in
// LLVM's own compiled libraries to declare `extern "C"` and link against
// directly -- confirmed empirically (`nm -D --defined-only
// libLLVM-22.so | grep LLVMInitializeNative` finds nothing, while the
// concrete-architecture symbol it would otherwise expand to on this host,
// `LLVMInitializeX86Target`, *is* a real, exported symbol). Declaring
// ordinary `extern "C"` prototypes for the two real names the normal way,
// as this module does for every other Target.h function below, would
// therefore fail to link. `native_target_init.cpp` (same directory)
// bridges this one gap: a small, deliberately plain, never-`import`ed
// `.cpp` that itself `#include`s the real `<llvm-c/Target.h>` (something
// this module's own source never does) to reach the two real inline
// function bodies, and re-exports each one's result under a new, distinct
// `extern "C"` name (`scpp_llvm_target_initialize_native_target`/
// `scpp_llvm_target_initialize_native_asm_printer` below) that this module
// can declare and link against normally, compiled into its own small
// static library (`llvm_target_native_init`, see
// libs/llvm/CMakeLists.txt) linked into this module's own compiled
// output. src/driver.cppm -- the one consumer needing "native, whatever
// this host is" initialization -- calls these two new names directly in
// place of the real ones; see native_target_init.cpp's own header comment
// for the full rationale, including why hard-coding one specific
// architecture's own concrete symbol names instead (the only alternative
// needing no shim at all) was rejected.
module;

struct LLVMOpaqueTargetData;

export module llvm.target;

import llvm.types;

// ---------------------------------------------------------------------
// Opaque handle pointer alias (llvm-c/Target.h)
// ---------------------------------------------------------------------
// Unlike the struct tag above, this pointer alias *is* declared in the
// module purview and `export`ed directly: a type-alias redeclaration is
// always well-formed as long as every redeclaration in scope denotes the
// exact same type, with none of the struct tag's cross-module attachment
// restriction above -- so src/driver.cppm, which sees both this module's
// `export using LLVMTargetDataRef = ...` and its own still-#include'd
// llvm-c/TargetMachine.h's transitive `typedef struct LLVMOpaqueTargetData
// *LLVMTargetDataRef;`, simply sees two harmless, agreeing redeclarations
// of the same alias, not a conflict.
export using LLVMTargetDataRef = LLVMOpaqueTargetData*;

// ---------------------------------------------------------------------
// Functions (llvm-c/Target.h)
// ---------------------------------------------------------------------
// A single `extern "C" { ... }` block, matching real llvm-c/Target.h's own
// linkage-specification for every one of these symbols -- they are
// implemented by, and this project ultimately links against, LLVM's own
// compiled libraries (see LLVM_LIBS in root CMakeLists.txt), not by this
// module. Real C++ exports every declaration nested inside an
// `export extern "C" { ... }` block, so no per-line `export` repetition is
// needed for each of the 7 declarations below to be part of this module's
// public interface.
export extern "C" {

// Target Data
LLVMTargetDataRef LLVMGetModuleDataLayout(LLVMModuleRef M);
void LLVMDisposeTargetData(LLVMTargetDataRef TD);
char* LLVMCopyStringRepOfTargetData(LLVMTargetDataRef TD);
unsigned LLVMPointerSizeForAS(LLVMTargetDataRef TD, unsigned AS);
unsigned long long LLVMSizeOfTypeInBits(LLVMTargetDataRef TD, LLVMTypeRef Ty);
unsigned long long LLVMABISizeOfType(LLVMTargetDataRef TD, LLVMTypeRef Ty);
unsigned LLVMABIAlignmentOfType(LLVMTargetDataRef TD, LLVMTypeRef Ty);

} // extern "C"

// ---------------------------------------------------------------------
// Native target/AsmPrinter initialization (llvm-c/Target.h) -- ABI gap
// ---------------------------------------------------------------------
// These two names are *not* real llvm-c/Target.h symbols -- there is no
// such exported symbol anywhere in LLVM's own compiled libraries, see this
// file's own header comment above for the full rationale. They are this
// module's own bridge to native_target_init.cpp's two, newly-defined, real
// `extern "C"` symbols (compiled into the separate `llvm_target_native_init`
// static library, linked into this module's own compiled output, see
// libs/llvm/CMakeLists.txt), one call deeper than the real (header-only,
// `static inline`) `LLVMInitializeNativeTarget`/`LLVMInitializeNativeAsmPrinter`
// this project's own driver needs.
export extern "C" {

LLVMBool scpp_llvm_target_initialize_native_target();
LLVMBool scpp_llvm_target_initialize_native_asm_printer();

} // extern "C"
