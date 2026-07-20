// debug_info.cpp
//
// `llvm.debug_info`: a fresh, standalone, top-level module -- not a
// partition or nested submodule of either `scpp` (this project's own
// compiler-internal modules, e.g. `scpp.ast`, `scpp.compiler.codegen`),
// `scpp.llvm` (the separate, ergonomic RAII wrapper package at
// libs/scpp_llvm/), or `llvm.core`/`llvm.types` (the two sibling modules in
// this same directory). Its only job is to give this compiler's own
// real-C++ `src/*.cppm` files a way to reach official LLVM-C's
// `llvm-c/DebugInfo.h` surface via `import llvm.debug_info;` instead of
// `#include <llvm-c/DebugInfo.h>` -- scpp (the language) has no
// preprocessor/#include at all, so any raw #include left in this
// compiler's own sources is a hard blocker for eventual self-hosting.
//
// This is a plain, ordinary C++ file (a `.cpp`, not a `.scpp`), compiled
// only by real clang++ (see the `llvm_debug_info` CMake target in
// libs/llvm/CMakeLists.txt) and never fed to the scpp compiler itself --
// exactly like `llvm.core`'s core.cpp and `llvm.types`'s types.cpp, there
// is no aspiration here for this specific file to also be scpp-parseable
// today. `export module llvm.debug_info;` below is nonetheless
// unrestricted, standard C++20 module syntax -- real ISO C++, nothing
// scpp-specific about it -- so the resulting compiled module interface
// will still be `import`able the same way from scpp-compiled code, once
// these files themselves eventually get rewritten in scpp; only this
// file's own *source* needs never be scpp-parseable.
//
// Scope: only the specific DebugInfo.h declarations actually referenced
// today by src/compiler/codegen/{orchestration,debug}.cppm -- surveyed
// function-by-function and signature-by-signature against those two files
// and real llvm-c/DebugInfo.h, not a blanket re-declaration of DebugInfo.h's
// much larger surface (real DebugInfo.h also declares e.g.
// `LLVMDIBuilderCreateModule`, `LLVMDIBuilderCreateNameSpace`,
// `LLVMDIBuilderCreateStructType`, `LLVMDIBuilderCreateReplaceableCompositeType`,
// `LLVMDIBuilderCreateGlobalVariableExpression`, the whole
// `LLVMDIBuilderInsert*Record*` family beyond `...AtEnd` for a declare
// record, and dozens more, none of which either file needs). The other
// still-`#include`d llvm-c headers in those same two files (Target.h,
// Analysis.h) are deliberately untouched -- out of scope for this change,
// left for their own later, equally narrow follow-ups.
//
// Depends on `llvm.types` (types.cpp, same directory) for every opaque
// handle pointer alias used below (LLVMContextRef, LLVMModuleRef,
// LLVMValueRef, LLVMBasicBlockRef, LLVMMetadataRef, LLVMDIBuilderRef,
// LLVMDbgRecordRef) plus LLVMBool: those are genuinely llvm-c/Types.h's own
// declarations, not DebugInfo.h's -- exactly the same reasoning `llvm.core`
// already relies on for its own `import llvm.types;` (see core.cpp).
// `LLVMDbgRecordRef` specifically (the return type of
// `LLVMDIBuilderInsertDeclareRecordAtEnd` below) is newly added to
// `llvm.types` by this same change, rather than declared privately here,
// so that module remains the single, real source of truth for every
// Types.h declaration any of these `llvm.*` modules need -- see types.cpp's
// own header comment for the full rationale. Unlike `llvm.core`, this
// module does *not* `export import llvm.types;`: both current consumers
// (orchestration.cppm, debug.cppm) already reach every one of those aliases
// through their own separate `import llvm.core;` (which itself re-exports
// `llvm.types`), so re-exporting the same names a second time here would
// only widen this module's own public surface with no consumer needing it
// through this path -- a plain `import llvm.types;` still makes every one
// of those aliases nameable within this file's own module purview below
// (which is all this file itself needs), and reachable (though not
// separately nameable via this module alone) to any importer of this
// module that sees one of the `export`ed function signatures below
// mentioning them, exactly as reachability already works one hop further
// away for `llvm.core`'s own non-exported internals (see core.cpp's own
// closing comment on that point).
//
// Contrast with libs/scpp_llvm/: that package wraps LLVM-C in ergonomic,
// RAII scpp classes (Context/Module/Type/Value) for scpp *user* programs,
// and deliberately declares every opaque handle as a shared `void*`
// underneath those classes -- real type safety there comes from the
// wrapper classes, not the raw handles. This module has no such wrapper:
// its raw `LLVM*Ref` declarations *are* the public surface both
// orchestration.cppm and debug.cppm call directly, exactly as they did
// through the real header, so every opaque handle kind involved (declared
// by `llvm.types`, reused here via plain `import`) is its own distinct
// pointer type (never a shared `void*`) -- otherwise the compiler could no
// longer catch e.g. an `LLVMMetadataRef` accidentally passed where an
// `LLVMValueRef` was expected, silently trading a compile error for a
// runtime bug.
//
// Module-attachment note: this module's own global module fragment below
// is empty. The one opaque handle struct tag its own declarations need
// beyond what `llvm.types` already declared prior to this change
// (`LLVMOpaqueDbgRecord`, for `LLVMDbgRecordRef`) is a genuine Types.h
// symbol -- and Types.h's own struct tags are still pervasively,
// transitively `#include`d, unattached, by the other llvm-c headers still
// directly `#include`d alongside this module in orchestration.cppm
// (Analysis.h) and debug.cppm (Target.h), so (exactly as types.cpp's own
// header comment explains at length) that tag belongs in `llvm.types`'s
// global module fragment, not a second, private copy declared here -- see
// this file's own "Depends on `llvm.types`" paragraph above. The four
// small enum-like types this file *does* declare
// directly below in its own module purview (`LLVMDIFlags`,
// `LLVMDWARFSourceLanguage`, `LLVMDWARFEmissionKind`,
// `LLVMDWARFTypeEncoding`) do not need the same global-module-fragment
// treatment: unlike Types.h's struct tags, they are genuinely
// DebugInfo.h's own declarations, and neither Target.h nor Analysis.h (the
// only other llvm-c headers still `#include`d by these two files) declares
// or transitively `#include`s any of them -- so, once this change removes
// the only `#include <llvm-c/DebugInfo.h>` in each of the two files, there
// is no other, competing, unattached declaration of these four types left
// anywhere in either translation unit for this module's own attached
// copies to conflict with. Verified directly against this project's own
// real build, not just in isolation -- exactly the same empirical
// discipline `llvm.core` and `llvm.types` already applied.
module;

export module llvm.debug_info;

import std;
import llvm.types;

// ---------------------------------------------------------------------
// Enums and small typedefs (llvm-c/DebugInfo.h)
// ---------------------------------------------------------------------
// Real llvm-c/DebugInfo.h declares `LLVMDIFlags` as an anonymous C `enum`
// via `typedef enum { ... } LLVMDIFlags;`, whose enumerators every existing
// call site -- and the real header itself -- reaches unqualified, with no
// `LLVMDIFlags::` (or similar) prefix. Named (`enum LLVMDIFlags { ... };`)
// here only so the type itself has a spellable name for use as a parameter
// type below, while keeping the same unqualified enumerator lookup and the
// exact real ABI/value contract, with no call-site changes needed beyond
// the include-to-import swap this task calls for.
//
// Only the enumerators actually referenced by orchestration.cppm/debug.cppm
// are declared below, each given its exact real numeric value explicitly
// (never left to auto-increment), so that any omitted enumerator never
// shifts the value of one that is declared.
export enum LLVMDIFlags {
    LLVMDIFlagZero = 0,
    LLVMDIFlagPrototyped = 1 << 8,
};

// Real llvm-c/DebugInfo.h declares this as a large anonymous C `enum`
// (`typedef enum { LLVMDWARFSourceLanguageC89, ... } LLVMDWARFSourceLanguage;`)
// whose ~63 enumerators auto-increment from 0 in declaration order; only
// `LLVMDWARFSourceLanguageC_plus_plus_17` is referenced (as the DWARF source
// language of every compile unit this project's own codegen ever emits), so
// only it is reproduced here, given its exact real position-derived value
// (40) explicitly rather than left to depend on every other, undeclared
// enumerator's own position.
export enum LLVMDWARFSourceLanguage {
    LLVMDWARFSourceLanguageC_plus_plus_17 = 40,
};

// Real llvm-c/DebugInfo.h declares this as
// `typedef enum { LLVMDWARFEmissionNone = 0, LLVMDWARFEmissionFull,
// LLVMDWARFEmissionLineTablesOnly } LLVMDWARFEmissionKind;`; only
// `LLVMDWARFEmissionFull` (this project's codegen always emits full debug
// info, never line-tables-only or none) is referenced, so only it is
// reproduced here, with its exact real auto-incremented value (1) given
// explicitly.
export enum LLVMDWARFEmissionKind {
    LLVMDWARFEmissionFull = 1,
};

// Real llvm-c/DebugInfo.h declares `typedef unsigned LLVMDWARFTypeEncoding;`
// -- a plain type alias (like `LLVMBool`/`LLVMAttributeIndex` in
// `llvm.types`/`llvm.core`), not a class/struct/enum, so it has none of the
// struct tags' cross-module attachment restriction and can be declared
// directly here, exported, with no global-module-fragment placement needed.
export using LLVMDWARFTypeEncoding = unsigned;

// ---------------------------------------------------------------------
// Functions (llvm-c/DebugInfo.h)
// ---------------------------------------------------------------------
// A single `extern "C" { ... }` block, matching real llvm-c/DebugInfo.h's
// own linkage-specification for every one of these symbols -- they are
// implemented by, and this project ultimately links against, LLVM's own
// compiled static libraries (see LLVM_LIBS in root CMakeLists.txt), not by
// this module. Real C++ exports every declaration nested inside an
// `export extern "C" { ... }` block, so no per-line `export` repetition is
// needed for each of the 20 declarations below to be part of this module's
// public interface. (This deliberately differs from
// libs/scpp_llvm/core/scpp_llvm_core.scpp's own `extern "C"` block, which
// is intentionally *not* exported there -- that package hides its raw
// LLVM-C bindings behind ergonomic wrapper classes and only exports those;
// this module has no such wrapper; the raw bindings themselves are the
// public surface.)
export extern "C" {

// Debug metadata version
unsigned LLVMDebugMetadataVersion(void);

// DIBuilder construction / lifetime
LLVMDIBuilderRef LLVMCreateDIBuilder(LLVMModuleRef M);
void LLVMDisposeDIBuilder(LLVMDIBuilderRef Builder);
void LLVMDIBuilderFinalize(LLVMDIBuilderRef Builder);

// Compile unit / file
LLVMMetadataRef LLVMDIBuilderCreateCompileUnit(
    LLVMDIBuilderRef Builder, LLVMDWARFSourceLanguage Lang, LLVMMetadataRef FileRef, const char* Producer,
    std::size_t ProducerLen, LLVMBool isOptimized, const char* Flags, std::size_t FlagsLen, unsigned RuntimeVer,
    const char* SplitName, std::size_t SplitNameLen, LLVMDWARFEmissionKind Kind, unsigned DWOId,
    LLVMBool SplitDebugInlining, LLVMBool DebugInfoForProfiling, const char* SysRoot, std::size_t SysRootLen,
    const char* SDK, std::size_t SDKLen);
LLVMMetadataRef LLVMDIBuilderCreateFile(LLVMDIBuilderRef Builder, const char* Filename, std::size_t FilenameLen,
                                       const char* Directory, std::size_t DirectoryLen);

// Subprogram (function)
LLVMMetadataRef LLVMDIBuilderCreateFunction(LLVMDIBuilderRef Builder, LLVMMetadataRef Scope, const char* Name,
                                            std::size_t NameLen, const char* LinkageName,
                                            std::size_t LinkageNameLen, LLVMMetadataRef File, unsigned LineNo,
                                            LLVMMetadataRef Ty, LLVMBool IsLocalToUnit, LLVMBool IsDefinition,
                                            unsigned ScopeLine, LLVMDIFlags Flags, LLVMBool IsOptimized);
void LLVMSetSubprogram(LLVMValueRef Func, LLVMMetadataRef SP);

// Debug locations
LLVMMetadataRef LLVMDIBuilderCreateDebugLocation(LLVMContextRef Ctx, unsigned Line, unsigned Column,
                                                 LLVMMetadataRef Scope, LLVMMetadataRef InlinedAt);

// Types
LLVMMetadataRef LLVMDIBuilderCreateSubroutineType(LLVMDIBuilderRef Builder, LLVMMetadataRef File,
                                                  LLVMMetadataRef* ParameterTypes, unsigned NumParameterTypes,
                                                  LLVMDIFlags Flags);
LLVMMetadataRef LLVMDIBuilderCreateArrayType(LLVMDIBuilderRef Builder, std::uint64_t Size,
                                            std::uint32_t AlignInBits, LLVMMetadataRef Ty,
                                            LLVMMetadataRef* Subscripts, unsigned NumSubscripts);
LLVMMetadataRef LLVMDIBuilderCreateUnspecifiedType(LLVMDIBuilderRef Builder, const char* Name,
                                                   std::size_t NameLen);
LLVMMetadataRef LLVMDIBuilderCreateBasicType(LLVMDIBuilderRef Builder, const char* Name, std::size_t NameLen,
                                            std::uint64_t SizeInBits, LLVMDWARFTypeEncoding Encoding,
                                            LLVMDIFlags Flags);
LLVMMetadataRef LLVMDIBuilderCreatePointerType(LLVMDIBuilderRef Builder, LLVMMetadataRef PointeeTy,
                                               std::uint64_t SizeInBits, std::uint32_t AlignInBits,
                                               unsigned AddressSpace, const char* Name, std::size_t NameLen);

// Subranges / arrays of DI nodes
LLVMMetadataRef LLVMDIBuilderGetOrCreateSubrange(LLVMDIBuilderRef Builder, std::int64_t LowerBound,
                                                 std::int64_t Count);
LLVMMetadataRef LLVMDIBuilderGetOrCreateArray(LLVMDIBuilderRef Builder, LLVMMetadataRef* Data,
                                             std::size_t NumElements);

// Expressions
LLVMMetadataRef LLVMDIBuilderCreateExpression(LLVMDIBuilderRef Builder, std::uint64_t* Addr, std::size_t Length);

// Variables (parameters / locals) and their declare-record insertion
LLVMMetadataRef LLVMDIBuilderCreateParameterVariable(LLVMDIBuilderRef Builder, LLVMMetadataRef Scope,
                                                     const char* Name, std::size_t NameLen, unsigned ArgNo,
                                                     LLVMMetadataRef File, unsigned LineNo, LLVMMetadataRef Ty,
                                                     LLVMBool AlwaysPreserve, LLVMDIFlags Flags);
LLVMMetadataRef LLVMDIBuilderCreateAutoVariable(LLVMDIBuilderRef Builder, LLVMMetadataRef Scope, const char* Name,
                                                std::size_t NameLen, LLVMMetadataRef File, unsigned LineNo,
                                                LLVMMetadataRef Ty, LLVMBool AlwaysPreserve, LLVMDIFlags Flags,
                                                std::uint32_t AlignInBits);
LLVMDbgRecordRef LLVMDIBuilderInsertDeclareRecordAtEnd(LLVMDIBuilderRef Builder, LLVMValueRef Storage,
                                                       LLVMMetadataRef VarInfo, LLVMMetadataRef Expr,
                                                       LLVMMetadataRef DebugLoc, LLVMBasicBlockRef Block);

} // extern "C"
