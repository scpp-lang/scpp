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
