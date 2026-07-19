# `scpp` module target (compiler-only build): compiles exactly two files --
# scpp_llvm.scpp (the real `:llvm` partition implementation, ~2600 lines of
# Context/Module/Type/Value/Builder/Function/BasicBlock/GlobalVariable/
# Attribute/Intrinsic/DIBuilder/Target/TargetMachine/DataLayout classes) and
# scpp_llvm_single_module.cpp (a two-line, CMake-only primary interface unit:
# `export module scpp; export import :llvm;`) -- together as a real C++20
# module target literally named "scpp", but containing *only* the `:llvm`
# partition. This lets the compiler's own CMake-built C++ targets
# (scpp_codegen, scpp_driver) `import scpp;` directly -- exactly like
# `import std;` -- instead of `#include "llvm-c/*.h"`. Module dependency
# scanning (CMake + Ninja + clang-scan-deps) discovers the `:llvm` partition
# automatically from scpp_llvm_single_module.cpp's own `export import :llvm;`,
# the same mechanism already used project-wide for `import std;`.
#
# This is one of *two* entirely separate, never-linked-together
# compilations of scpp_llvm.scpp, by design: this target builds only the
# `:llvm` partition, decoupled at build time from libs/scpp/scpp.toml's own
# full package build (which independently compiles this same
# scpp_llvm.scpp file, alongside :io/:rand/:enum_cast, for ordinary scpp
# users via the scpp CLI's own --import mechanism -- see llvm_lib_test in
# the root CMakeLists.txt, which exercises that path and proves the library
# also works standalone there). Nested under libs/scpp/ purely for
# filesystem tidiness -- this target has zero build-time dependency on
# libs/scpp's own rand/io/enum_cast content compiling successfully.
#
# scpp_llvm.scpp declares its own `extern "C"` bindings straight to LLVM-C
# (deliberately zero #include of llvm-c/*.h, so it stays dual-compilable
# under scpp itself, which has no preprocessor at all), so -- unlike
# scpp_codegen -- the module sources in this target need neither
# LLVM_INCLUDE_DIRS nor LLVM_DEFINITIONS_LIST, only LLVM_LIBS at link time
# for the real LLVM-C symbols those extern "C" declarations resolve to.
# -Wno-unknown-attributes silences real clang++'s (correct) warning about
# the scpp-only `[[scpp::unsafe]]` attribute sprinkled through method
# bodies there -- a real, standard-grammar vendor attribute
# ([[vendor::attr]]) that plain C++ silently accepts and ignores, but
# -Wall still flags as unrecognized.
#
# native_target_init.cpp (an ordinary, non-dual-compiled, never-imported
# .cpp -- see its own top-of-file comment) bridges this module's one
# confirmed gap (LLVMInitializeNative* are header-only `static inline`
# functions, not real exported symbols): it needs LLVM_INCLUDE_DIRS/
# LLVM_DEFINITIONS_LIST to #include <llvm-c/Target.h>, unlike
# scpp_llvm.scpp itself just below. It's built as its own tiny static
# library, separate from scpp_llvm itself, since it needs to reach *two*
# independent consumers that otherwise share no other build machinery: the
# scpp_llvm target right below (an ordinary target_link_libraries away),
# and, in the root CMakeLists.txt, llvm_lib_test's own scpp-CLI-driven
# recompile-from-source of these same .scpp files (which needs this
# prebuilt library's file path as one more --link input, since a fresh
# scpp-compiled object for scpp_llvm.scpp has no way to pull in a plain
# C++ .cpp's compiled code itself). This mirrors the *other* copy of
# native_target_init.cpp's own compilation folded into libscpp.scppa via
# libs/scpp/scpp.toml's `additional_objs` mechanism for the ordinary
# scpp-package/CLI path -- two separate compilations of the same source
# file, exactly like scpp_llvm.scpp itself, never linked together.
#
# LLVM_LIBS, LLVM_INCLUDE_DIRS, and LLVM_DEFINITIONS_LIST are all resolved
# by the root CMakeLists.txt's find_package(LLVM REQUIRED CONFIG) (before
# this subdirectory is added), and are visible here via ordinary CMake
# directory-variable scoping -- no need to re-resolve them.
add_library(scpp_llvm_native_target_init STATIC native_target_init.cpp)
target_include_directories(scpp_llvm_native_target_init SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(scpp_llvm_native_target_init PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_features(scpp_llvm_native_target_init PRIVATE cxx_std_23)

add_library(scpp_llvm)
set_source_files_properties(
    scpp_llvm.scpp
    PROPERTIES LANGUAGE CXX)
target_sources(scpp_llvm
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            scpp_llvm_single_module.cpp
            scpp_llvm.scpp)
target_compile_options(scpp_llvm PRIVATE -Wno-unknown-attributes)
target_link_libraries(scpp_llvm PUBLIC ${LLVM_LIBS} PRIVATE scpp_llvm_native_target_init)
target_compile_features(scpp_llvm PRIVATE cxx_std_23)
