// types.cpp
//
// `llvm:types`: the `:types` partition of module `llvm` (see llvm.cpp, the
// primary module interface unit, same directory) -- not a standalone,
// top-level module of its own any more (see "Module consolidation" below).
// Its only job is to give this compiler's own real-C++ `src/*.cppm` files,
// and `llvm:core` itself, a way to reach official LLVM-C's
// `llvm-c/Types.h` surface via `import llvm;` (which re-exports every
// partition, this one included) instead of `#include <llvm-c/Types.h>` --
// scpp (the language) has no preprocessor/#include at all, so any raw
// #include left in this compiler's own sources is a hard blocker for
// eventual self-hosting.
//
// This is a plain, ordinary C++ file (a `.cpp`, not a `.scpp`), compiled
// only by real clang++ (see the single `llvm` CMake target in
// libs/llvm/CMakeLists.txt, which compiles this file together with
// llvm.cpp and every other `llvm:*` partition `.cpp` file into one module)
// and never fed to the scpp compiler itself -- exactly like `llvm:core`'s
// own core.cpp, there is no aspiration here for this specific file to also
// be scpp-parseable today. `export module llvm:types;` below is
// nonetheless unrestricted, standard C++20 module-partition syntax -- real
// ISO C++, nothing scpp-specific about it -- so the resulting compiled
// module interface will still be `import`able the same way from
// scpp-compiled code, once these files themselves eventually get
// rewritten in scpp; only this file's own *source* needs never be
// scpp-parseable.
//
// History / scope: `llvm.core` (this partition's own module, before this
// same change consolidated it into `llvm:core`) originally (mis)declared
// its own private copy of every one of these opaque handle struct tags and
// pointer aliases directly inside itself, since at the time it was the
// only module needing them and Types.h's actual declarations were few
// enough to seem like an implementation detail of Core.h. That was always
// slightly wrong: these declarations are genuinely Types.h's own surface,
// not Core.h's, and `src/compiler/codegen/api.cppm` -- which never used
// any Core.h function, only raw opaque handle types as its own `Codegen`
// class's member types -- had to `#include <llvm-c/Types.h>` a second,
// independent time to get them, duplicating `llvm.core`'s private copy
// instead of sharing it. This module fixed that: it became the single,
// real source of truth for Types.h's declarations, and both `llvm:core`
// (via `import :types;`, see core.cpp) and
// `src/compiler/codegen/api.cppm` (via `import llvm;`) depend on it
// instead of each keeping their own copy.
//
// Scope: only the specific Types.h declarations actually referenced today,
// across all of `llvm:core` (core.cpp), `llvm:debug_info` (debug_info.cpp,
// same directory), and `src/compiler/codegen/api.cppm` combined -- not a
// blanket re-declaration of Types.h's much larger surface (real Types.h
// also declares e.g. `LLVMMemoryBufferRef`, `LLVMNamedMDNodeRef`,
// `LLVMOperandBundleRef`, `LLVMDiagnosticInfoRef`, `LLVMComdatRef`,
// `LLVMPassManagerRef`, `LLVMBinaryRef`, none of which any of the three
// need). Every handle kind below except `LLVMOpaqueDIBuilder`/
// `LLVMDIBuilderRef` and `LLVMOpaqueDbgRecord`/`LLVMDbgRecordRef` was
// already declared by `llvm.core` prior to this module's introduction
// (verified function-by-function and signature-by-signature against
// src/driver.cppm and src/compiler/codegen/{layout,orchestration,debug,
// functions,object_model,lifetime,statements,expressions}.cppm when
// `llvm.core` was first created); `LLVMOpaqueDIBuilder`/`LLVMDIBuilderRef`
// is added here because `src/compiler/codegen/api.cppm` declares a raw
// `LLVMDIBuilderRef dibuilder_` member on its `Codegen` class, and that
// type is genuinely from Types.h (`typedef struct LLVMOpaqueDIBuilder
// *LLVMDIBuilderRef;`), not Core.h -- `llvm:core` itself never calls any
// DebugInfo-builder function (those live in the separate llvm-c/DebugInfo.h,
// see `llvm:debug_info` below), so this handle kind was never part of
// `llvm:core`'s own prior survey. `LLVMOpaqueDbgRecord`/`LLVMDbgRecordRef`
// is added here for the same reason, but for `llvm:debug_info` instead:
// `LLVMDIBuilderInsertDeclareRecordAtEnd` (llvm-c/DebugInfo.h, called by
// src/compiler/codegen/debug.cppm) returns `LLVMDbgRecordRef`, and that
// type, too, is genuinely from Types.h (`typedef struct LLVMOpaqueDbgRecord
// *LLVMDbgRecordRef;`), not DebugInfo.h -- so it belongs here, this
// project's single real source of truth for Types.h's declarations, rather
// than as a private copy re-declared inside debug_info.cpp itself.
//
// This module declares no RAII wrapper of its own: its raw `LLVM*Ref`
// declarations *are* the public surface `llvm:core`, `llvm:debug_info`,
// and `api.cppm` call/use directly, exactly as they did through the real
// header, so each opaque handle kind below is declared as its own
// distinct pointer type (never a shared `void*`) -- otherwise the
// compiler could no longer catch e.g. an `LLVMTypeRef` accidentally
// passed where an `LLVMValueRef` was expected, silently trading a
// compile error for a runtime bug.
//
// Module consolidation / attachment note: before the six `llvm.<name>`
// modules (PRs #290-295) were merged into six partitions of one module
// `llvm` (see llvm.cpp), every opaque handle struct tag below lived in
// this file's *global module fragment* (a `module;` ... declarations ...
// block before the module-declaration), deliberately unattached to any
// module, rather than declared directly in the module purview -- and
// never cosmetically: two declarations of the same class/struct/enum tag
// denote the same entity only if both are attached to the same named
// module, or neither is attached to any named module, and at the time
// several consumers of `llvm.core`/`llvm.types` (the ten files listed
// above) still kept a *different* llvm-c header #include'd alongside
// their `import llvm.core;` (Target.h, TargetMachine.h, DebugInfo.h,
// Analysis.h each transitively #include their own, unattached copy of
// llvm-c/Types.h too), so the very same struct tags declared here were
// *also* declared, separately, unattached, by the real system header, in
// each of those files. Keeping this module's own copies unattached too was
// what made the two agree, rather than conflict as two different entities
// sharing a name.
//
// That specific trigger no longer applies, for two independent reasons.
// First, every one of those ten consumer files has since migrated its last
// `#include <llvm-c/*.h>` to an `import` (`grep -rn "^#include.*llvm-c"
// src/` returns zero matches repo-wide), so no translation unit in src/
// combines this module's own declarations with a competing, unattached
// copy of the same tags any more. Second, and unrelated to that migration:
// now that `llvm.core` and `llvm.types` are partitions (`:core`/`:types`)
// of the very same module `llvm` rather than two separate modules, a tag
// attached to partition `:types` is, per C++20's own module rules,
// considered attached to module `llvm` as a whole -- the same module every
// other partition (`:core`, `:debug_info`, `:target`, `:target_machine`,
// `:analysis`) is also part of -- so even a hypothetical future
// declaration of one of these same tags in another `llvm:*` partition's
// own purview would already agree with this one, with no global-module-
// fragment indirection needed to make that so. The one remaining
// `#include <llvm-c/Target.h>` anywhere in this project
// (native_target_init.cpp, same directory) does not change this: it is a
// deliberately plain, never-`import`ed `.cpp` that never imports this
// module (or any `llvm:*` partition) either, compiled as its own, wholly
// separate translation unit into its own small static library, so its own
// unattached copies of these same tags (reached transitively through its
// own #include) never coexist, in any single translation unit, with this
// module's own declarations.
//
// With both of those defenses now moot, the opaque handle struct tags
// below are declared directly in this partition's own module purview
// (attached to module `llvm`, like every other declaration in every
// `llvm:*` partition) rather than in a global module fragment -- simpler,
// with no remaining conflict to guard against. Verified empirically
// against this project's own full clean build (all six partitions plus
// every one of the ten consumer files), not just reasoned about in
// isolation.
module;

export module llvm:types;

// ---------------------------------------------------------------------
// Opaque handle struct tags (llvm-c/Types.h)
// ---------------------------------------------------------------------
// Each handle kind gets its own never-defined, bodyless forward
// declaration, exactly mirroring real llvm-c/Types.h's own pattern (e.g.
// `typedef struct LLVMOpaqueContext *LLVMContextRef;`, where
// `LLVMOpaqueContext` itself is never completed anywhere): nothing ever
// instantiates or defines these types; only pointers to them are formed.
//
// Declared here, in this partition's own module purview (attached to
// module `llvm`, see this file's own "Module consolidation / attachment
// note" above), but never `export`ed directly: nothing outside this file
// ever needs to *name* one of these raw tags (every consumer, whether
// another `llvm:*` partition or one of the ten files under src/, only
// ever spells the exported pointer alias below, e.g. `LLVMContextRef`,
// never the bare tag) -- only the pointer aliases further below are
// exported.
//
// NOTE: real llvm-c/Types.h spells this specific struct tag
// "LLVMOpaqueAttributeRef", not "LLVMOpaqueAttribute" like every sibling
// tag below -- an inconsistency in the upstream header itself, kept
// byte-for-byte here since the tag name must match upstream exactly,
// should this partition's own copy ever need to coexist with some other,
// unattached copy of the same tag again in the future.
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
struct LLVMOpaqueDbgRecord;

// ---------------------------------------------------------------------
// Opaque handle pointer aliases (llvm-c/Types.h)
// ---------------------------------------------------------------------
// Unlike the struct tags above (attached, but never exported themselves),
// these pointer aliases *are* exported directly: a type-alias (or C
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
export using LLVMDbgRecordRef = LLVMOpaqueDbgRecord*;

// ---------------------------------------------------------------------
// LLVMBool (llvm-c/Types.h)
// ---------------------------------------------------------------------
// Real llvm-c/Types.h itself declares `typedef int LLVMBool;` -- unlike
// `LLVMAttributeIndex` (`typedef unsigned LLVMAttributeIndex;`), which real
// llvm-c/Core.h declares itself and therefore stays behind in `llvm:core`
// (see core.cpp), this one is genuinely Types.h's own declaration.
export using LLVMBool = int;
