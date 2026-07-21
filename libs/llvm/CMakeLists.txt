# `llvm`: a single, real, directly-clang++-compiled C++20 module, split
# across a primary module interface unit (llvm.cpp) and six partitions
# (types.cpp, core.cpp, debug_info.cpp, target.cpp, target_machine.cpp,
# analysis.cpp -- each hand-declaring one specific slice of official
# LLVM-C's surface, see each file's own header comment), so this
# compiler's own driver/codegen sources can reach all of it via a single
# `import llvm;` instead of `#include <llvm-c/*.h>` -- scpp (the
# language) has no preprocessor/#include at all, so raw #include in this
# compiler's own sources is a self-hosting blocker. See llvm.cpp's own
# header comment for the full rationale of why this is one module with
# six partitions rather than six independent modules (as it originally
# was, across PRs #290-295, before this consolidation): in short, the six
# pieces were never independent in practice -- five of them already
# depended on the sixth (`:types`) -- so one `import llvm;` giving every
# consumer file everything it needs, instead of a separate
# `import llvm.<name>;` per piece, more accurately reflects that they were
# always one cohesive LLVM-C binding surface.
#
# Every one of the seven `.cpp` files below is a plain, ordinary C++ file
# (unlike `.scpp` sources elsewhere under libs/), so CMake recognizes each
# one's `.cpp` extension as C++ on its own -- no
# `set_source_files_properties(... LANGUAGE CXX)` needed. None of the six
# partitions declare any LLVM-C symbol *definitions* of their own (only
# extern "C" bindings and opaque-handle/enum declarations), so this
# target needs neither ${LLVM_INCLUDE_DIRS} nor ${LLVM_LIBS} itself --
# consumers (see src/compiler/codegen/CMakeLists.txt, root
# CMakeLists.txt) already link ${LLVM_LIBS} directly, which resolves
# every partition's declarations at final link time. The one exception is
# `:target`'s own two native-target-init bridge declarations, which
# resolve instead to `llvm_target_native_init` (below), linked in
# explicitly.
#
# A single FILE_SET cxx_modules lists the primary interface unit
# (llvm.cpp) together with all six partition files, exactly mirroring the
# established primary-interface-plus-partitions pattern already used
# elsewhere in this codebase for a single CMake target spanning multiple
# partition files (see src/compiler/codegen/CMakeLists.txt's own
# `scpp_codegen` target for `scpp.compiler.codegen`'s own eleven
# partitions, and src/compiler/movecheck/CMakeLists.txt's own
# `scpp_movecheck` target). Real module dependency scanning
# (clang-scan-deps, invoked automatically by CMake's own Ninja/Makefiles
# module support) determines each partition's own build order from its
# `import :name;`/`export import :name;` declarations, exactly the same
# mechanism this project already relies on elsewhere for `import std;` --
# no manual ordering of the FILES list below, nor any inter-partition
# target_link_libraries edge, is needed for that alone.
add_library(llvm)
target_sources(llvm
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            llvm.cpp
            core.cpp
            types.cpp
            debug_info.cpp
            target.cpp
            target_machine.cpp
            analysis.cpp)
target_compile_features(llvm PRIVATE cxx_std_23)

# `llvm_target_native_init`: a tiny, separate static library bridging the
# one confirmed gap in `:target`'s own official-LLVM-C surface (target.cpp,
# same directory) -- see native_target_init.cpp's own header comment for
# the full rationale. Unlike every `llvm:*` partition `.cpp` file above,
# native_target_init.cpp is a deliberately plain, never-`import`ed .cpp
# that itself `#include`s the real <llvm-c/Target.h> (needing
# ${LLVM_INCLUDE_DIRS}/${LLVM_DEFINITIONS_LIST} to do so, unlike every
# file in the single FILE_SET cxx_modules above), so it cannot be part of
# that FILE_SET -- it is compiled as an ordinary source file into its own
# small static library instead. LLVM_INCLUDE_DIRS and
# LLVM_DEFINITIONS_LIST are both resolved by the root CMakeLists.txt's
# find_package(LLVM REQUIRED CONFIG) (before this subdirectory is added),
# and are visible here via ordinary CMake directory-variable scoping.
add_library(llvm_target_native_init STATIC native_target_init.cpp)
target_include_directories(llvm_target_native_init SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(llvm_target_native_init PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_features(llvm_target_native_init PRIVATE cxx_std_23)
# native_target_init.cpp's compiled object code itself references real,
# concrete-architecture LLVM symbols (e.g. LLVMInitializeX86Target, via the
# LLVMInitializeNativeTarget()/LLVMInitializeNativeAsmPrinter() inline
# bodies it calls through to -- see that file's own header comment) that
# only ${LLVM_LIBS} (below, resolved by the root CMakeLists.txt's own
# llvm_map_components_to_libnames) ultimately define. This target never
# declared that real dependency explicitly before llvm_target_machine
# (this module's own predecessor to today's `:target_machine` partition)
# first existed, relying instead on every final executable's own,
# separately-specified link line happening to place ${LLVM_LIBS} after
# this target's archive -- true by accident of argument order, not by any
# CMake-tracked dependency edge. Introducing llvm_target_machine as a
# second, sibling path by which a consumer (scpp_driver, see root
# CMakeLists.txt) reaches this same target (llvm_target_machine also
# privately depended on llvm_target, which privately depended on this one)
# empirically changed CMake's own link-line linearization enough to
# violate that accidental ordering and break the real build -- confirmed
# by reverting to the pre-llvm_target_machine tree and observing the link
# succeed unchanged. Declaring the real dependency explicitly here instead
# makes CMake itself respect the required "consumer before provider"
# archive order no matter how many separate paths a consumer might use to
# reach this target -- still true today, now that every one of those
# former separate paths (llvm_core, llvm_target, llvm_target_machine, ...)
# is one and the same `llvm` target above.
target_link_libraries(llvm_target_native_init PRIVATE ${LLVM_LIBS})

# The consolidated `llvm` target's own `:target` partition (target.cpp)
# needs `llvm_target_native_init`'s two bridge symbols
# (scpp_llvm_target_initialize_native_target/_asm_printer) resolved at
# link time -- the same real dependency the former, separate `llvm_target`
# module target used to declare directly. PRIVATE: this dependency is an
# implementation detail of how `:target` resolves two of its own
# declarations, not part of this module's own public interface, so it
# should not propagate to `llvm`'s own consumers (scpp_codegen,
# scpp_driver) -- none of which ever names anything from
# `llvm_target_native_init` directly.
target_link_libraries(llvm PRIVATE llvm_target_native_init)
