# `llvm.types`: a real, directly-clang++-compiled C++20 module (types.cpp)
# hand-declaring the specific official llvm-c/Types.h surface this
# compiler's own codegen sources (src/compiler/codegen/api.cppm) and
# `llvm.core` itself need, so they can `import llvm.types;` instead of
# `#include <llvm-c/Types.h>` -- scpp (the language) has no preprocessor/
# #include at all, so raw #include in this compiler's own sources is a
# self-hosting blocker. Declares only opaque handle struct tags/pointer
# aliases, no LLVM-C symbol *definitions* of its own, so it needs neither
# ${LLVM_INCLUDE_DIRS} nor ${LLVM_LIBS} itself.
#
# types.cpp is a plain, ordinary C++ file (unlike `.scpp` sources
# elsewhere under libs/), so CMake recognizes its `.cpp` extension as C++
# on its own -- no `set_source_files_properties(... LANGUAGE CXX)` needed.
add_library(llvm_types)
target_sources(llvm_types
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            types.cpp)
target_compile_features(llvm_types PRIVATE cxx_std_23)

# `llvm.core`: a real, directly-clang++-compiled C++20 module (core.cpp)
# hand-declaring the specific official llvm-c/Core.h surface this
# compiler's own codegen/driver sources need, so they can
# `import llvm.core;` instead of `#include <llvm-c/Core.h>` -- scpp (the
# language) has no preprocessor/#include at all, so raw #include in this
# compiler's own sources is a self-hosting blocker. Declares only
# extern "C" bindings, no LLVM-C symbol *definitions* of its own, so it
# needs neither ${LLVM_INCLUDE_DIRS} nor ${LLVM_LIBS} itself -- consumers
# (see src/compiler/codegen/CMakeLists.txt, root CMakeLists.txt) already
# link ${LLVM_LIBS} for the other, still-#include'd llvm-c/*.h headers,
# which resolves this module's declarations too at final link time.
#
# core.cpp is a plain, ordinary C++ file (unlike `.scpp` sources
# elsewhere under libs/), so CMake recognizes its `.cpp` extension as C++
# on its own -- no `set_source_files_properties(... LANGUAGE CXX)` needed.
# core.cpp itself `export import llvm.types;` (see its own header
# comment) for the opaque handle struct tags/aliases it used to declare
# directly, so `llvm_core` links PUBLIC against `llvm_types`: PUBLIC both
# gives CMake's module dependency scanner the BMI-ordering edge it needs
# (core.cpp's own BMI can't be built before types.cpp's) and propagates
# the re-export to `llvm_core`'s own consumers (scpp_codegen, scpp_driver)
# transitively, exactly mirroring how those consumers already link
# `llvm_core` PUBLIC themselves.
add_library(llvm_core)
target_sources(llvm_core
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            core.cpp)
target_link_libraries(llvm_core PUBLIC llvm_types)
target_compile_features(llvm_core PRIVATE cxx_std_23)

# `llvm.debug_info`: a real, directly-clang++-compiled C++20 module
# (debug_info.cpp) hand-declaring the specific official llvm-c/DebugInfo.h
# surface this compiler's own codegen sources
# (src/compiler/codegen/{orchestration,debug}.cppm) need, so they can
# `import llvm.debug_info;` instead of `#include <llvm-c/DebugInfo.h>` --
# scpp (the language) has no preprocessor/#include at all, so raw #include
# in this compiler's own sources is a self-hosting blocker. Declares only
# extern "C" bindings, no LLVM-C symbol *definitions* of its own, so it
# needs neither ${LLVM_INCLUDE_DIRS} nor ${LLVM_LIBS} itself -- consumers
# (see src/compiler/codegen/CMakeLists.txt, root CMakeLists.txt) already
# link ${LLVM_LIBS} for the other, still-#include'd llvm-c/*.h headers,
# which resolves this module's declarations too at final link time.
#
# debug_info.cpp is a plain, ordinary C++ file (unlike `.scpp` sources
# elsewhere under libs/), so CMake recognizes its `.cpp` extension as C++
# on its own -- no `set_source_files_properties(... LANGUAGE CXX)` needed.
# debug_info.cpp itself only `import llvm.types;` (not `export import`,
# unlike `llvm.core`'s own core.cpp -- see debug_info.cpp's own header
# comment for why), so `llvm_debug_info` links PRIVATE against
# `llvm_types`: this still gives CMake's module dependency scanner the
# BMI-ordering edge it needs (debug_info.cpp's own BMI can't be built
# before types.cpp's), without also propagating a re-export this module
# never performs to `llvm_debug_info`'s own consumers (scpp_codegen), which
# already reach every one of `llvm.types`'s aliases through their own
# separate, direct link to `llvm_core` (which does re-export `llvm.types`)
# or, for api.cppm specifically, `llvm_types` itself.
add_library(llvm_debug_info)
target_sources(llvm_debug_info
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            debug_info.cpp)
target_link_libraries(llvm_debug_info PRIVATE llvm_types)
target_compile_features(llvm_debug_info PRIVATE cxx_std_23)

# `llvm_target_native_init`: a tiny, separate static library bridging the
# one confirmed gap in `llvm.target`'s own official-LLVM-C surface below
# -- see native_target_init.cpp's own header comment for the full
# rationale. Unlike target.cpp, native_target_init.cpp is a deliberately
# plain, never-`import`ed .cpp that itself `#include`s the real
# <llvm-c/Target.h> (needing ${LLVM_INCLUDE_DIRS}/${LLVM_DEFINITIONS_LIST}
# to do so, unlike every other target in this file), so it cannot be part
# of a FILE_SET cxx_modules -- it is compiled as an ordinary source file
# into its own small static library instead. LLVM_INCLUDE_DIRS and
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
# (below) existed, relying instead on every final executable's own,
# separately-specified link line happening to place ${LLVM_LIBS} after
# this target's archive -- true by accident of argument order, not by any
# CMake-tracked dependency edge. Introducing llvm_target_machine as a
# second, sibling path by which a consumer (scpp_driver, see root
# CMakeLists.txt) reaches this same target (llvm_target_machine also
# privately depends on llvm_target, which privately depends on this one)
# empirically changed CMake's own link-line linearization enough to
# violate that accidental ordering and break the real build -- confirmed
# by reverting to the pre-llvm_target_machine tree and observing the
# link succeed unchanged. Declaring the real dependency explicitly here
# instead makes CMake itself respect the required "consumer before
# provider" archive order no matter how many separate paths a future
# consumer might use to reach this target.
target_link_libraries(llvm_target_native_init PRIVATE ${LLVM_LIBS})

# `llvm.target`: a real, directly-clang++-compiled C++20 module (target.cpp)
# hand-declaring the specific official llvm-c/Target.h surface this
# compiler's own driver/codegen sources
# (src/driver.cppm, src/compiler/codegen/{layout,debug,expressions}.cppm)
# need, so they can `import llvm.target;` instead of
# `#include <llvm-c/Target.h>` -- scpp (the language) has no preprocessor/
# #include at all, so raw #include in this compiler's own sources is a
# self-hosting blocker. Declares only extern "C" bindings, no LLVM-C
# symbol *definitions* of its own, so it needs neither ${LLVM_INCLUDE_DIRS}
# nor ${LLVM_LIBS} itself -- consumers (see src/compiler/codegen/CMakeLists.txt,
# root CMakeLists.txt) already link ${LLVM_LIBS} for the other, still-
# #include'd llvm-c/*.h headers, which resolves this module's declarations
# too at final link time. The two native-target-init bridge declarations
# are the one exception: they resolve instead to `llvm_target_native_init`
# (above), linked in explicitly below.
#
# target.cpp is a plain, ordinary C++ file (unlike `.scpp` sources
# elsewhere under libs/), so CMake recognizes its `.cpp` extension as C++
# on its own -- no `set_source_files_properties(... LANGUAGE CXX)` needed.
# target.cpp itself only `import llvm.types;` (not `export import`,
# same reasoning as `llvm.debug_info`'s own debug_info.cpp -- see that
# file's own header comment), so `llvm_target` links PRIVATE against
# `llvm_types`: this still gives CMake's module dependency scanner the
# BMI-ordering edge it needs (target.cpp's own BMI can't be built before
# types.cpp's), without also propagating a re-export this module never
# performs to `llvm_target`'s own consumers, which already reach every one
# of `llvm.types`'s aliases through their own separate, direct link to
# `llvm_core` (which does re-export `llvm.types`).
add_library(llvm_target)
target_sources(llvm_target
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            target.cpp)
target_link_libraries(llvm_target PRIVATE llvm_types llvm_target_native_init)
target_compile_features(llvm_target PRIVATE cxx_std_23)

# `llvm.target_machine`: a real, directly-clang++-compiled C++20 module
# (target_machine.cpp) hand-declaring the specific official
# llvm-c/TargetMachine.h surface this compiler's own driver source
# (src/driver.cppm, the sole consumer) needs, so it can
# `import llvm.target_machine;` instead of
# `#include <llvm-c/TargetMachine.h>` -- scpp (the language) has no
# preprocessor/#include at all, so raw #include in this compiler's own
# sources is a self-hosting blocker. Declares only extern "C" bindings, no
# LLVM-C symbol *definitions* of its own, so it needs neither
# ${LLVM_INCLUDE_DIRS} nor ${LLVM_LIBS} itself -- consumers (see root
# CMakeLists.txt) already link ${LLVM_LIBS} for the other, still-
# #include'd llvm-c/*.h headers, which resolves this module's declarations
# too at final link time.
#
# target_machine.cpp is a plain, ordinary C++ file (unlike `.scpp` sources
# elsewhere under libs/), so CMake recognizes its `.cpp` extension as C++
# on its own -- no `set_source_files_properties(... LANGUAGE CXX)` needed.
# target_machine.cpp itself only `import llvm.types;`/`import llvm.target;`
# (not `export import` either, same reasoning as `llvm.debug_info`'s own
# debug_info.cpp and `llvm.target`'s own target.cpp -- see that file's own
# header comment), so `llvm_target_machine` links PRIVATE against both
# `llvm_types` and `llvm_target`: this still gives CMake's module
# dependency scanner the BMI-ordering edge it needs (target_machine.cpp's
# own BMI can't be built before types.cpp's/target.cpp's), without also
# propagating a re-export this module never performs to
# `llvm_target_machine`'s own consumers, which already reach every one of
# `llvm.types`'s/`llvm.target`'s aliases through their own separate,
# direct links to `llvm_core` (which does re-export `llvm.types`) and
# `llvm_target`.
add_library(llvm_target_machine)
target_sources(llvm_target_machine
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            target_machine.cpp)
target_link_libraries(llvm_target_machine PRIVATE llvm_types llvm_target)
target_compile_features(llvm_target_machine PRIVATE cxx_std_23)

# `llvm.analysis`: a real, directly-clang++-compiled C++20 module
# (analysis.cpp) hand-declaring the specific official llvm-c/Analysis.h
# surface this compiler's own codegen source
# (src/compiler/codegen/orchestration.cppm, the sole consumer) needs, so it
# can `import llvm.analysis;` instead of `#include <llvm-c/Analysis.h>` --
# scpp (the language) has no preprocessor/#include at all, so raw #include
# in this compiler's own sources is a self-hosting blocker. This module's
# own introduction removes the last such #include anywhere in src/. Declares
# only an extern "C" binding, no LLVM-C symbol *definition* of its own, so
# it needs neither ${LLVM_INCLUDE_DIRS} nor ${LLVM_LIBS} itself -- and
# unlike `llvm.target`'s own native-target-init gap, LLVMVerifyModule is a
# real, exported symbol in LLVM's own compiled libraries with no
# native-init-style shim needed (confirmed via `nm -D --defined-only
# libLLVM-22.so | grep LLVMVerifyModule`) -- consumers (see
# src/compiler/codegen/CMakeLists.txt, root CMakeLists.txt) already link
# ${LLVM_LIBS} for the other, still-#include'd llvm-c/*.h headers (before
# this change) or their own still-`import`ed llvm.* modules (after), which
# resolves this module's declaration too at final link time.
#
# analysis.cpp is a plain, ordinary C++ file (unlike `.scpp` sources
# elsewhere under libs/), so CMake recognizes its `.cpp` extension as C++
# on its own -- no `set_source_files_properties(... LANGUAGE CXX)` needed.
# analysis.cpp itself only `import llvm.types;` (not `export import`, same
# reasoning as `llvm.debug_info`'s/`llvm.target`'s/`llvm.target_machine`'s
# own sources -- see that file's own header comment), so `llvm_analysis`
# links PRIVATE against `llvm_types`: this still gives CMake's module
# dependency scanner the BMI-ordering edge it needs (analysis.cpp's own BMI
# can't be built before types.cpp's), without also propagating a re-export
# this module never performs to `llvm_analysis`'s own consumers, which
# already reach every one of `llvm.types`'s aliases through their own
# separate, direct link to `llvm_core` (which does re-export `llvm.types`).
add_library(llvm_analysis)
target_sources(llvm_analysis
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            analysis.cpp)
target_link_libraries(llvm_analysis PRIVATE llvm_types)
target_compile_features(llvm_analysis PRIVATE cxx_std_23)
