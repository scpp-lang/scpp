cmake_minimum_required(VERSION 3.20)

# A small, standalone CMake project for the black-box test runner --
# deliberately independent from the top-level scpp project (no
# add_subdirectory linkage, no C++20 modules, no LLVM *CMake package*
# dependency: no find_package(LLVM), no LLVM include dirs/CMake targets
# anywhere here): the runner never links against scpp's own compiler
# modules or LLVM itself, it only ever invokes the built `scpp` CLI binary
# as an external tool. Build with:
#   cmake -S . -B build
#   cmake --build build
#   ./build/run_tests
project(scpp_blackbox_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# `libs/scpp/scpp.toml`'s "scpp" module is one merged object for *all*
# four of its partitions (rand/io/enum_cast/llvm -- no per-partition
# object granularity), so any case here that imports "scpp" for e.g.
# rand/io transitively needs :llvm's own real-LLVM-C symbols satisfied at
# final link time too, exactly like tests/llvm_lib_test.cpp does one
# directory up. Resolved via a plain llvm-config lookup (not
# find_package(LLVM)) to keep this project's own build free of any LLVM
# CMake package/target dependency -- it only ever needs the resulting
# library file path(s) as literal strings to forward as `--link` flags to
# the invoked scpp CLI below (see default_std_build_args() in
# run_tests.cpp), never anything to build or link against here.
find_program(SCPP_BLACKBOX_LLVM_CONFIG NAMES llvm-config-22 llvm-config REQUIRED)
execute_process(
    COMMAND "${SCPP_BLACKBOX_LLVM_CONFIG}" --libfiles core support target native
    OUTPUT_VARIABLE SCPP_LLVM_NATIVE_LIBRARY_FILES
    OUTPUT_STRIP_TRAILING_WHITESPACE)

add_executable(run_tests run_tests.cpp)
target_compile_definitions(run_tests PRIVATE
    # Baked in at configure time so the runner can find its fixtures and a
    # default `scpp` binary regardless of the working directory it's run
    # from, or where the build system places the compiled executable.
    SCPP_BLACKBOX_TEST_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases"
    SCPP_DEFAULT_SCPP_BINARY="${CMAKE_CURRENT_SOURCE_DIR}/../build/scpp"
    SCPP_STDLIB_STD_MODULE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../libs/std/std.scpp"
    SCPP_STDLIB_SCPP_MODULE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../libs/scpp/scpp.scpp"
    SCPP_STDLIB_STD_STRING_MODULE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../libs/std/string/std_string.scpp"
    SCPP_STDLIB_STD_MEMORY_MODULE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../libs/std/memory/std_memory.scpp"
    SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../libs/std/functional/std_functional.scpp"
    SCPP_STDLIB_STD_THREAD_MODULE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../libs/std/thread/std_thread.scpp"
    SCPP_LLVM_NATIVE_LIBRARY_FILES="${SCPP_LLVM_NATIVE_LIBRARY_FILES}")
