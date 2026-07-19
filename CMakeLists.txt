cmake_minimum_required(VERSION 3.28)

# --- C++23 `import std;` setup -----------------------------------------------
# scpp (the language) has no preprocessor/#include at all, so scpp's own C++
# implementation is migrating off #include-based standard library usage and
# onto `import std;`.
#
# This project deliberately builds against the platform default standard
# library (libstdc++ on this Ubuntu/Clang toolchain) rather than libc++.
# Clang's `import std;` support works with either, but the prebuilt LLVM
# static libraries this project links against (see `find_package(LLVM)`
# below) were themselves compiled against libstdc++: their public API
# surface exposes `std::` types (e.g. `llvm::IRBuilderBase`'s
# `std::optional<...>` parameters) whose mangled names bake in the standard
# library's ABI. Building our own code with `-stdlib=libc++` therefore
# produces object code that cannot link against these prebuilt LLVM
# archives at all -- it's a hard ABI mismatch (different mangled symbol
# names), not a flag/link-order problem. Staying on libstdc++ (Clang's
# default here) keeps us ABI-compatible with LLVM while still getting a
# fully working `import std;`.
#
# All of this must be decided *before* `project()` triggers CXX toolchain/ABI
# detection -- CMake's own `import std;` support (CMAKE_CXX_MODULE_STD,
# CMAKE_EXPERIMENTAL_CXX_IMPORT_STD, and the CMAKE_CXX_STANDARD_LIBRARY it
# auto-detects) is still gated as an experimental feature; see
# Help/dev/experimental.rst in CMake's own source tree for the current
# activation UUID (it may change in future CMake releases -- re-check that
# file when upgrading CMake).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(scpp
    VERSION 26.0.0
    DESCRIPTION "A compiler frontend for a C++-looking language with safe regions"
    LANGUAGES C CXX)

# Make `import std;` available to every target using cxx_std_23 or newer
# (see CXX_MODULE_STD target property docs). Set once, project-wide, before
# any targets are created, since it only initializes each target's
# CXX_MODULE_STD property at the point the target is created.
set(CMAKE_CXX_MODULE_STD 1)

set(SCPP_STDLIB_STD_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/std/std.scpp")
set(SCPP_STDLIB_STD_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/std.scppm")
set(SCPP_STDLIB_STD_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libstd.scppa")
set(SCPP_STDLIB_SCPP_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/scpp/scpp.scpp")
set(SCPP_STDLIB_SCPP_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/scpp.scppm")
set(SCPP_STDLIB_SCPP_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libscpp.scppa")

add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
llvm_map_components_to_libnames(LLVM_LIBS core support target native)
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# Real, absolute path(s) to LLVM's own native library file(s), needed to run
# a scpp binary that links against libs/scpp/llvm/ (see llvm_lib_test
# below): its scpp bindings call official LLVM-C functions directly, so
# its final link needs LLVM's real libraries. CMake targets elsewhere in
# this project link via the LLVM_LIBS *CMake target names* above, but
# --link (scpp's own linker-input CLI flag) needs literal file paths
# instead, so resolve them via llvm-config here.
execute_process(
    COMMAND "${LLVM_TOOLS_BINARY_DIR}/llvm-config" --libfiles core support target native
    OUTPUT_VARIABLE SCPP_LLVM_NATIVE_LIBRARY_FILES
    OUTPUT_STRIP_TRAILING_WHITESPACE)
list(INSERT CMAKE_FIND_LIBRARY_SUFFIXES 0 ".a")
find_library(SQLITE3_LIB NAMES sqlite3 REQUIRED)
find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h REQUIRED)

# libs/scpp/llvm/'s own CMakeLists.txt defines the scpp_llvm module target
# (import scpp; -- see below and driver.cppm/codegen's *.cppm) and its
# scpp_llvm_native_target_init support library; see that file for the full
# rationale on both. Must come before any target that links against
# scpp_llvm -- scpp_codegen (add_subdirectory below) and scpp_driver
# (further down) both do. Nested under libs/scpp/ purely for filesystem
# tidiness -- this target compiles only scpp_llvm.scpp (the `:llvm`
# partition) plus its own tiny scpp_llvm_single_module.cpp primary
# interface, producing a module literally named "scpp" but containing
# *only* `:llvm` -- zero build-time coupling to libs/scpp's own
# rand/io/enum_cast content (that package's own full "scpp" module,
# including :llvm alongside them, is instead consumed only via the scpp
# CLI's own --import mechanism, e.g. by driver_test/blackbox_test's
# rand/io test cases and llvm_lib_test below -- see libs/scpp/scpp.toml).
add_subdirectory(libs/scpp/llvm)

add_library(scpp_lexer)
target_sources(scpp_lexer
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/lexer.cppm)
target_compile_features(scpp_lexer PRIVATE cxx_std_23)

add_library(scpp_ast)
target_sources(scpp_ast
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/ast.cppm)
target_compile_features(scpp_ast PRIVATE cxx_std_23)

add_library(scpp_parser)
target_sources(scpp_parser
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/parser.cppm)
target_link_libraries(scpp_parser PUBLIC scpp_lexer scpp_ast)
target_compile_features(scpp_parser PRIVATE cxx_std_23)

add_library(scpp_mir)
target_sources(scpp_mir
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/mir.cppm)
target_link_libraries(scpp_mir PUBLIC scpp_ast)
target_compile_features(scpp_mir PRIVATE cxx_std_23)

add_subdirectory(src/compiler/codegen)

add_subdirectory(src/compiler/movecheck)

add_library(scpp_constexpr)
target_sources(scpp_constexpr
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/constexpr.cppm)
target_link_libraries(scpp_constexpr PUBLIC scpp_ast)
target_compile_features(scpp_constexpr PRIVATE cxx_std_23)

add_library(scpp_driver)
target_sources(scpp_driver
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/driver.cppm)
target_link_libraries(scpp_driver PUBLIC scpp_ast scpp_parser scpp_codegen scpp_movecheck scpp_constexpr scpp_llvm ${LLVM_LIBS})
target_compile_features(scpp_driver PRIVATE cxx_std_23)

add_library(scpp_project)
target_sources(scpp_project
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/project.cppm)
target_include_directories(scpp_project PRIVATE ${SQLITE3_INCLUDE_DIR})
target_link_libraries(scpp_project PUBLIC scpp_lexer scpp_driver ${SQLITE3_LIB})
target_compile_features(scpp_project PRIVATE cxx_std_23)

add_executable(scpp)

target_sources(scpp
    PRIVATE
        src/main.cpp
    PRIVATE
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/cli/cli.cppm)
target_link_libraries(scpp PRIVATE scpp_lexer scpp_ast scpp_parser scpp_codegen scpp_movecheck scpp_driver scpp_project)

target_compile_features(scpp PRIVATE cxx_std_23)
target_compile_definitions(scpp PRIVATE
    SCPP_VERSION="${PROJECT_VERSION}")

install(TARGETS scpp
    RUNTIME DESTINATION bin)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/libs/"
    DESTINATION "share/scpp/libs"
    FILES_MATCHING
        PATTERN "*.scpp")

install(FILES
    "${SCPP_STDLIB_STD_INTERFACE_PATH}"
    "${SCPP_STDLIB_STD_ARCHIVE_PATH}"
    "${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
    "${SCPP_STDLIB_SCPP_ARCHIVE_PATH}"
    DESTINATION "share/scpp/libs")

enable_testing()

add_executable(lexer_test tests/lexer_test.cpp)
target_link_libraries(lexer_test PRIVATE scpp_lexer)
target_compile_features(lexer_test PRIVATE cxx_std_23)

add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_test tests/parser_test.cpp)
target_link_libraries(parser_test PRIVATE scpp_parser scpp_ast scpp_lexer)
target_compile_features(parser_test PRIVATE cxx_std_23)
target_compile_definitions(parser_test PRIVATE
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}")

add_test(NAME parser_test COMMAND parser_test)

add_executable(codegen_test tests/codegen_test.cpp)
target_link_libraries(codegen_test PRIVATE scpp_codegen scpp_movecheck scpp_constexpr scpp_parser scpp_ast scpp_lexer)
target_compile_features(codegen_test PRIVATE cxx_std_23)
target_compile_definitions(codegen_test PRIVATE
    SCPP_CODEGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/codegentest_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}")

add_test(NAME codegen_test COMMAND codegen_test)

add_executable(movecheck_test tests/movecheck_test.cpp)
target_link_libraries(movecheck_test PRIVATE scpp_movecheck scpp_parser scpp_ast scpp_lexer)
target_compile_features(movecheck_test PRIVATE cxx_std_23)
target_compile_definitions(movecheck_test PRIVATE
    SCPP_MOVETEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/movetest_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}")

add_test(NAME movecheck_test COMMAND movecheck_test)

add_executable(driver_test tests/driver_test.cpp)
target_link_libraries(driver_test PRIVATE scpp_driver scpp_codegen scpp_movecheck scpp_parser scpp_ast scpp_lexer)
target_compile_features(driver_test PRIVATE cxx_std_23)
target_compile_definitions(driver_test PRIVATE
    SCPP_BINARY_PATH="$<TARGET_FILE:scpp>"
    SCPP_DRIVER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_test_source"
    SCPP_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_STD_INTERFACE_PATH="${SCPP_STDLIB_STD_INTERFACE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}"
    SCPP_STDLIB_SCPP_INTERFACE_PATH="${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
    # The prebuilt libscpp.scppa this test links against (via
    # prebuilt_module_import_paths()/std_link_inputs() below) is one
    # merged object for the *whole* "scpp" module -- all four partitions,
    # including :llvm, compiled together (libs/scpp/scpp.toml has no
    # per-partition object granularity). So even a rand-/io-/enum_cast-only
    # test here transitively needs :llvm's own real-LLVM-C undefined
    # symbols satisfied at final link time, exactly like llvm_lib_test
    # does above -- reusing the same SCPP_LLVM_NATIVE_LIBRARY_FILES.
    SCPP_LLVM_NATIVE_LIBRARY_FILES="${SCPP_LLVM_NATIVE_LIBRARY_FILES}")

add_test(NAME driver_test COMMAND driver_test)

# Proves libs/scpp/llvm/ works end to end via the real, CLI-driven
# --import/--link workflow an external scpp consumer would follow -- see
# tests/llvm_lib_test.cpp and tests/llvm_lib_test_source/main.scpp.
# Deliberately depends on the scpp executable (not scpp_driver's in-process
# API, unlike driver_test above), and reuses SCPP_LLVM_NATIVE_LIBRARY_FILES
# (resolved above via llvm-config) for the same real LLVM .so path(s)
# --link needs at final link time. Imports the *prebuilt*
# SCPP_STDLIB_SCPP_INTERFACE_PATH (built by libs/ below, same artifact
# driver_test's prebuilt_module_import_paths() uses) rather than raw
# scpp.scpp source, so the scpp compiler auto-discovers and links the
# co-located libscpp.scppa archive -- which already has
# llvm/native_target_init.cpp's compiled object code folded in (see
# libs/scpp/scpp.toml's `[additional_objs.scpp-native]`), needing no
# separate --link input for that shim. This exercises libs/scpp/scpp.toml's
# own full package build (all four partitions, :llvm included), entirely
# independent from the scpp_llvm CMake module target above.
add_executable(llvm_lib_test tests/llvm_lib_test.cpp)
target_compile_features(llvm_lib_test PRIVATE cxx_std_23)
target_compile_definitions(llvm_lib_test PRIVATE
    SCPP_BINARY_PATH="$<TARGET_FILE:scpp>"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_SCPP_INTERFACE_PATH="${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
    SCPP_LLVM_NATIVE_LIBRARY_FILES="${SCPP_LLVM_NATIVE_LIBRARY_FILES}"
    SCPP_LLVM_LIB_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/llvm_lib_test_source"
    SCPP_LLVM_LIB_TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/llvm_lib_test-work")
add_dependencies(llvm_lib_test scpp)

add_test(NAME llvm_lib_test COMMAND llvm_lib_test)

# Builds the shipped std/scpp manifest workspace and copies its produced
# artifacts into build/libs. Library behavior itself is covered by tests/, not demos.
add_subdirectory(libs)
add_subdirectory(applications/httpserver)
