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
