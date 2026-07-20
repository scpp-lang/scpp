// types.cpp
//
// `llvm.types`: a fresh, standalone, top-level module -- not a partition or
// nested submodule of `scpp`, `scpp.llvm`, or `llvm.core` (the sibling
// module in this same directory). Its only job is to give this compiler's
// own real-C++ `src/*.cppm` files, and `llvm.core` itself, a way to reach
// official LLVM-C's `llvm-c/Types.h` surface via `import llvm.types;`
// instead of `#include <llvm-c/Types.h>` -- scpp (the language) has no
// preprocessor/#include at all, so any raw #include left in this
// compiler's own sources is a hard blocker for eventual self-hosting.
//
// This is a plain, ordinary C++ file (a `.cpp`, not a `.scpp`), compiled
// only by real clang++ (see the `llvm_types` CMake target in
// libs/llvm/CMakeLists.txt) and never fed to the scpp compiler itself --
// exactly like `llvm.core`'s own core.cpp, there is no aspiration here for
// this specific file to also be scpp-parseable today. `export module
// llvm.types;` below is nonetheless unrestricted, standard C++20 module
// syntax -- real ISO C++, nothing scpp-specific about it -- so the
// resulting compiled module interface will still be `import`able the same
// way from scpp-compiled code, once these files themselves eventually get
// rewritten in scpp; only this file's own *source* needs never be
// scpp-parseable.
//
// History / scope: `llvm.core` originally (mis)declared its own private
// copy of every one of these opaque handle struct tags and pointer aliases
// directly inside itself, since at the time it was the only module needing
// them and Types.h's actual declarations were few enough to seem like an
// implementation detail of Core.h. That was always slightly wrong: these
// declarations are genuinely Types.h's own surface, not Core.h's, and
// `src/compiler/codegen/api.cppm` -- which never used any Core.h function,
// only raw opaque handle types as its own `Codegen` class's member types --
// had to `#include <llvm-c/Types.h>` a second, independent time to get
// them, duplicating `llvm.core`'s private copy instead of sharing it. This
// module fixes that: it is now the single, real source of truth for
// Types.h's declarations, and both `llvm.core` (via `import llvm.types;`,
// see core.cpp) and `src/compiler/codegen/api.cppm` (via the same import)
// depend on it instead of each keeping their own copy.
//
// Scope: only the specific Types.h declarations actually referenced today,
// across all of `llvm.core` (core.cpp) and `src/compiler/codegen/api.cppm`
// combined -- not a blanket re-declaration of Types.h's much larger surface
// (real Types.h also declares e.g. `LLVMMemoryBufferRef`,
// `LLVMNamedMDNodeRef`, `LLVMOperandBundleRef`, `LLVMDiagnosticInfoRef`,
// `LLVMComdatRef`, `LLVMPassManagerRef`, `LLVMBinaryRef`,
// `LLVMDbgRecordRef`, none of which either file needs). Every handle kind
// below except `LLVMOpaqueDIBuilder`/`LLVMDIBuilderRef` was already
// declared by `llvm.core` prior to this module's introduction (verified
// function-by-function and signature-by-signature against
// src/driver.cppm and src/compiler/codegen/{layout,orchestration,debug,
// functions,object_model,lifetime,statements,expressions}.cppm when
// `llvm.core` was first created); `LLVMOpaqueDIBuilder`/`LLVMDIBuilderRef`
// is newly added here because `src/compiler/codegen/api.cppm` declares a
// raw `LLVMDIBuilderRef dibuilder_` member on its `Codegen` class, and that
// type is genuinely from Types.h (`typedef struct LLVMOpaqueDIBuilder
// *LLVMDIBuilderRef;`), not Core.h -- `llvm.core` itself never calls any
// DebugInfo-builder function (those live in the separate, still out-of-
// scope llvm-c/DebugInfo.h, left for its own later `llvm.debuginfo`
// follow-up), so this handle kind was never part of `llvm.core`'s own
// prior survey.
//
// Contrast with libs/scpp_llvm/: that package wraps LLVM-C in ergonomic,
// RAII scpp classes (Context/Module/Type/Value) for scpp *user* programs,
// and deliberately declares every opaque handle as a shared `void*`
// underneath those classes -- real type safety there comes from the
// wrapper classes, not the raw handles. This module has no such wrapper:
// its raw `LLVM*Ref` declarations *are* the public surface `llvm.core` and
// `api.cppm` call/use directly, exactly as they did through the real
// header, so each opaque handle kind below is declared as its own
// distinct pointer type (never a shared `void*`) -- otherwise the compiler
// could no longer catch e.g. an `LLVMTypeRef` accidentally passed where an
// `LLVMValueRef` was expected, silently trading a compile error for a
// runtime bug.
// ---------------------------------------------------------------------
// Global module fragment: opaque handle struct tags (llvm-c/Types.h)
// ---------------------------------------------------------------------
// Each handle kind gets its own never-defined, bodyless forward
// declaration, exactly mirroring real llvm-c/Types.h's own pattern (e.g.
// `typedef struct LLVMOpaqueContext *LLVMContextRef;`, where
// `LLVMOpaqueContext` itself is never completed anywhere): nothing ever
// instantiates or defines these types; only pointers to them are formed.
//
// These tags live here, in this file's *global module fragment* (the
// `module;` ... declarations ... before `export module llvm.types;`
// below), deliberately *not* in the module purview, and are never
// `export`ed directly (a global module fragment cannot export anything;
// only the pointer aliases further below are exported). This placement is
// not cosmetic -- it is the one thing that makes this whole module work
// at all, and is a real ISO C++20 modules rule, unrelated to this file's
// own `.cpp`/`.scpp` extension or to scpp's own grammar: every file that
// depends on this module -- directly (`api.cppm`) or indirectly, through
// `llvm.core`'s own `import llvm.types;` (core.cpp) -- is, in turn,
// `import`ed by consumers (the nine files listed above) that still keep a
// *different* llvm-c header #include'd alongside `import llvm.core;`
// (Target.h, TargetMachine.h, DebugInfo.h, Analysis.h all transitively
// #include their own copy of llvm-c/Types.h), so the very same struct tags
// declared here are *also* declared, separately, by the real system
// header, unattached to any module, in each of those files. Two
// declarations of the same class/struct tag denote the same entity only
// if both are attached to the same named module, or neither is attached
// to any named module (a plain `#include`, wherever it appears, is always
// "attached to no module"). Declaring the tags here, in the global module
// fragment, keeps this module's own copies unattached too, exactly like
// the real header's, so the two agree and safely denote one and the same
// type everywhere both are visible in the same translation unit --
// including transitively, since a declaration's "attached to no module"
// status does not depend on which file's global module fragment it
// physically lives in, nor on how many `import`/re-`export` hops separate
// the declaring file from the file that ultimately sees it. Verified
// directly against this project's own real build below, not just in
// isolation: this is exactly the same rule `llvm.core` already relied on
// before this module existed, now spanning one additional module boundary
// (`llvm.core` importing `llvm.types` rather than declaring the tags
// itself), with no new conflict introduced by that extra hop.
module;

// NOTE: real llvm-c/Types.h spells this specific struct tag
// "LLVMOpaqueAttributeRef", not "LLVMOpaqueAttribute" like every sibling
// tag below -- an inconsistency in the upstream header itself, kept
// byte-for-byte here since the tag name must match upstream exactly for
// the two declarations to denote the same type when a file combines this
// import with a still-#include'd llvm-c header that also declares it.
struct LLVMOpaqueContext;
struct LLVMOpaqueModule;
struct LLVMOpaqueType;
struct LLVMOpaqueValue;
struct LLVMOpaqueBasicBlock;
struct LLVMOpaqueMetadata;
struct LLVMOpaqueBuilder;
struct LLVMOpaqueDIBuilder;
struct LLVMOpaqueUse;
struct LLVMOpaqueAttributeRef;

export module llvm.types;

// ---------------------------------------------------------------------
// Opaque handle pointer aliases (llvm-c/Types.h)
// ---------------------------------------------------------------------
// Unlike the struct tags above, these pointer aliases *are* declared in
// the module purview and `export`ed directly: a type-alias (or C
// `typedef`) redeclaration is always well-formed as long as every
// redeclaration in scope denotes the exact same type, with none of the
// struct tags' cross-module attachment restriction above -- so a file
// that sees both this module's `export using LLVMContextRef = ...` and a
// still-#include'd llvm-c header's own `typedef struct LLVMOpaqueContext
// *LLVMContextRef;` simply sees two harmless, agreeing redeclarations of
// the same alias, not a conflict. Each alias is still its own distinct
// pointer type from every other one below (never a shared `void*`), so
// the compiler continues to reject e.g. an `LLVMTypeRef` passed where an
// `LLVMValueRef` is expected, exactly as real LLVM-C's own headers do.
export using LLVMContextRef = LLVMOpaqueContext*;
export using LLVMModuleRef = LLVMOpaqueModule*;
export using LLVMTypeRef = LLVMOpaqueType*;
export using LLVMValueRef = LLVMOpaqueValue*;
export using LLVMBasicBlockRef = LLVMOpaqueBasicBlock*;
export using LLVMMetadataRef = LLVMOpaqueMetadata*;
export using LLVMBuilderRef = LLVMOpaqueBuilder*;
export using LLVMDIBuilderRef = LLVMOpaqueDIBuilder*;
export using LLVMUseRef = LLVMOpaqueUse*;
export using LLVMAttributeRef = LLVMOpaqueAttributeRef*;

// ---------------------------------------------------------------------
// LLVMBool (llvm-c/Types.h)
// ---------------------------------------------------------------------
// Real llvm-c/Types.h itself declares `typedef int LLVMBool;` -- unlike
// `LLVMAttributeIndex` (`typedef unsigned LLVMAttributeIndex;`), which real
// llvm-c/Core.h declares itself and therefore stays behind in `llvm.core`
// (see core.cpp), this one is genuinely Types.h's own declaration.
export using LLVMBool = int;
