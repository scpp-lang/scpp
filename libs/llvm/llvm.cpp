// llvm.cpp
//
// `llvm`: the primary interface unit of a single, unified C++20 module
// consolidating what used to be six separate, standalone, top-level
// modules (`llvm.core`, `llvm.types`, `llvm.debug_info`, `llvm.target`,
// `llvm.target_machine`, `llvm.analysis` -- introduced one at a time
// across PRs #290-295) into six partitions (`:core`, `:types`,
// `:debug_info`, `:target`, `:target_machine`, `:analysis`) of this one
// module instead. Concrete surface area lives in those dedicated interface
// partitions (same directory: core.cpp, types.cpp, debug_info.cpp,
// target.cpp, target_machine.cpp, analysis.cpp), which this file simply
// re-exports for consumers of `import llvm;` -- exactly the same
// primary-interface-unit-plus-partitions shape already established
// elsewhere in this repo for `import std;` (libs/std/std.scpp) and
// `import scpp;` (libs/scpp/scpp.scpp), and for this compiler's own
// internal modules (e.g. src/compiler/codegen/codegen.cppm,
// src/compiler/movecheck/movecheck.cppm).
//
// This is a plain, ordinary C++ file (a `.cpp`, not a `.scpp`), compiled
// only by real clang++ (see the single `llvm` CMake target in
// libs/llvm/CMakeLists.txt, which compiles this file together with all six
// `llvm:*` partition `.cpp` files below into one module) and never fed to
// the scpp compiler itself -- there is no aspiration here for this
// specific file to also be scpp-parseable today, unlike std.scpp/scpp.scpp
// above. `export module llvm;` below is nonetheless unrestricted, standard
// C++20 module syntax -- real ISO C++, nothing scpp-specific about it --
// so the resulting compiled module interface will still be `import`able
// the same way from scpp-compiled code, once these files themselves
// eventually get rewritten in scpp; only this file's own *source* needs
// never be scpp-parseable.
//
// Why one module instead of six: every one of this project's own
// `src/*.cppm` consumer files (ten of them -- src/driver.cppm and nine
// files under src/compiler/codegen/) that needed more than one of the six
// pieces had to spell out each one separately (e.g.
// `import llvm.core; import llvm.debug_info; import llvm.analysis;` in
// orchestration.cppm) even though, from any consumer's point of view, all
// six were always just "the official-LLVM-C surface this project's own
// codegen/driver needs" -- a single, cohesive whole, not six independent
// libraries that happen to be developed together. Collapsing them into
// partitions of one module lets every consumer instead write a single
// `import llvm;` and transparently get whichever subset it actually calls
// (unused declarations cost nothing extra -- same as today, since none of
// these partitions define any function *bodies* of their own, only
// declarations LLVM's own compiled libraries resolve at final link time;
// see each partition's own header comment). The inter-partition dependency
// graph below (`:core` and `:debug_info`/`:target`/`:target_machine`/
// `:analysis` all depending on `:types`; `:target_machine` also depending
// on `:target`) is unchanged from the six modules' own prior inter-module
// `import`/`export import` graph -- only the syntax changed, from
// `import llvm.types;`/`export import llvm.types;` to the relative
// partition-import syntax `import :types;`/`export import :types;` (see
// each partition's own header comment for exactly which form it uses and
// why).
export module llvm;

export import :core;
export import :types;
export import :debug_info;
export import :target;
export import :target_machine;
export import :analysis;
