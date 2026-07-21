// debug_info.cpp
//
// `llvm:debug_info`: the `:debug_info` partition of module `llvm` (see
// llvm.cpp, the primary module interface unit, same directory) -- not a
// standalone, top-level module of its own any more (see types.cpp's own
// "Module consolidation / attachment note" for the general background).
// Its only job is to give this compiler's own real-C++ `src/*.cppm` files
// a way to reach official LLVM-C's `llvm-c/DebugInfo.h` surface via
// `import llvm;` (which re-exports every partition, this one included)
// instead of `#include <llvm-c/DebugInfo.h>` -- scpp (the language) has no
// preprocessor/#include at all, so any raw #include left in this
// compiler's own sources is a hard blocker for eventual self-hosting.
//
// This is a plain, ordinary C++ file (a `.cpp`, not a `.scpp`), compiled
// only by real clang++ (see the single `llvm` CMake target in
// libs/llvm/CMakeLists.txt, which compiles this file together with
// llvm.cpp and every other `llvm:*` partition `.cpp` file into one module)
// and never fed to the scpp compiler itself -- exactly like `llvm:core`'s
// core.cpp and `llvm:types`'s types.cpp, there is no aspiration here for
// this specific file to also be scpp-parseable today. `export module
// llvm:debug_info;` below is nonetheless unrestricted, standard C++20
// module-partition syntax -- real ISO C++, nothing scpp-specific about it
// -- so the resulting compiled module interface will still be
// `import`able the same way from scpp-compiled code, once these files
// themselves eventually get rewritten in scpp; only this file's own
// *source* needs never be scpp-parseable.
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
// record, and dozens more, none of which either file needs).
//
// Depends on `llvm:types` (types.cpp, same directory) for every opaque
// handle pointer alias used below (LLVMContextRef, LLVMModuleRef,
// LLVMValueRef, LLVMBasicBlockRef, LLVMMetadataRef, LLVMDIBuilderRef,
// LLVMDbgRecordRef) plus LLVMBool: those are genuinely llvm-c/Types.h's own
// declarations, not DebugInfo.h's -- exactly the same reasoning `llvm:core`
// already relies on for its own `import :types;` (see core.cpp).
// `LLVMDbgRecordRef` specifically (the return type of
// `LLVMDIBuilderInsertDeclareRecordAtEnd` below) is declared by
// `llvm:types` rather than declared privately here, so that partition
// remains the single, real source of truth for every Types.h declaration
// any of these `llvm:*` partitions need -- see types.cpp's own header
// comment for the full rationale. Unlike `llvm:core`, this partition does
// *not* `export import :types;`: both current consumers
// (orchestration.cppm, debug.cppm) already reach every one of those
// aliases through their own single `import llvm;` (which brings in every
// partition, `:core` -- itself re-exporting `:types` -- included), so
// re-exporting the same names a second time here would only widen this
// partition's own public surface with no consumer needing it through this
// path -- a plain `import :types;` still makes every one of those aliases
// nameable within this file's own module purview below (which is all this
// file itself needs), and reachable to any importer of this module that
// sees one of the `export`ed function signatures below mentioning them.
//
// This module declares no RAII wrapper of its own: its raw `LLVM*Ref`
// declarations *are* the public surface both orchestration.cppm and
// debug.cppm call directly, exactly as they did through the real header,
// so every opaque handle kind involved (declared by `llvm:types`, reused
// here via plain `import`) is its own distinct pointer type (never a
// shared `void*`) -- otherwise the compiler could no longer catch e.g. an
// `LLVMMetadataRef` accidentally passed where an `LLVMValueRef` was
// expected, silently trading a compile error for a runtime bug.
//
// Module-attachment note: this partition's own global module fragment
// below is empty, and needs no tag placement decision of its own: the one
// opaque handle struct tag its own declarations need beyond what
// `llvm:types` already declares (`LLVMOpaqueDbgRecord`, for
// `LLVMDbgRecordRef`) is a genuine Types.h symbol declared once, by
// `llvm:types` itself (see types.cpp's own "Module consolidation /
// attachment note" for the full rationale of why that tag lives in its
// module purview, attached to module `llvm`, rather than a global module
// fragment) -- not a second, private copy declared here. The four small
// enum-like types this file *does* declare directly below in its own
// module purview (`LLVMDIFlags`, `LLVMDWARFSourceLanguage`,
// `LLVMDWARFEmissionKind`, `LLVMDWARFTypeEncoding`) never needed any
// global-module-fragment treatment of their own even before this
// consolidation: unlike Types.h's struct tags, they are genuinely
// DebugInfo.h's own declarations, and every one of the ten consumer files
// under src/ has migrated its last `#include <llvm-c/*.h>` to an
// `import`, so there is no other, competing, unattached declaration of
// these four types anywhere in any translation unit for this partition's
// own attached copies to conflict with. Verified directly against this
// project's own real build, not just in isolation -- exactly the same
// empirical discipline `llvm:core` and `llvm:types` already applied.
module;

export module llvm:debug_info;

import std;
import :types;

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
// `llvm:types`/`llvm:core`), not a class/struct/enum, so it has none of the
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
// public interface.
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
