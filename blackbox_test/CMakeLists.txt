cmake_minimum_required(VERSION 3.20)

# A small, standalone CMake project for the black-box test runner --
# deliberately independent from the top-level scpp project (no
# add_subdirectory linkage, no C++20 modules, no LLVM dependency): the
# runner never links against scpp's own compiler modules, it only ever
# invokes the built `scpp` CLI binary as an external tool. Build with:
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

add_executable(run_tests run_tests.cpp)
target_compile_definitions(run_tests PRIVATE
    # Baked in at configure time so the runner can find its fixtures and a
    # default `scpp` binary regardless of the working directory it's run
    # from, or where the build system places the compiled executable.
    SCPP_BLACKBOX_TEST_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases"
    SCPP_DEFAULT_SCPP_BINARY="${CMAKE_CURRENT_SOURCE_DIR}/../build/scpp")

