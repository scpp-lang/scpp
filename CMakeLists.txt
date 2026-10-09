cmake_minimum_required(VERSION 3.28)

# Determine bootstrap stage (default: 0)
# Can be specified via -DSTAGE=<N> or environment variable STAGE=<N>
if(DEFINED ENV{STAGE})
    set(STAGE "$ENV{STAGE}")
elseif(NOT DEFINED STAGE)
    set(STAGE 0)
endif()

message(STATUS "Configuring scpp bootstrap STAGE=${STAGE}")
if(NOT STAGE EQUAL 0 AND NOT STAGE EQUAL 1)
    message(FATAL_ERROR "Unsupported STAGE: '${STAGE}'. Supported stages are 0 and 1.")
endif()

# --- C++26 `import std;` setup -----------------------------------------------
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

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(scpp
    VERSION 26.0.0
    DESCRIPTION "A compiler frontend for a C++-looking language with safe regions"
    LANGUAGES C CXX)

# Make `import std;` available to every target using cxx_std_26 or newer
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

find_package(Threads REQUIRED)

if(STAGE EQUAL 0)

add_library(scpp_lexer)
target_sources(scpp_lexer
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/compiler/lexer.cppm)

add_library(scpp_ast)
target_sources(scpp_ast
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/compiler/ast.cppm)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scpp_ast PRIVATE -Wno-error=unknown-attributes)
endif()

add_library(scpp_parser)
target_sources(scpp_parser
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/compiler/parser.cppm)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scpp_parser PRIVATE -Wno-error=unknown-attributes)
endif()
target_link_libraries(scpp_parser PUBLIC scpp_lexer scpp_ast)

add_library(scpp_mir)
target_sources(scpp_mir
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/mir.cppm)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scpp_mir PRIVATE -Wno-error=unknown-attributes)
endif()
target_link_libraries(scpp_mir PUBLIC scpp_ast)

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# `llvm` (module `llvm`, with partitions `:core`/`:types`/`:debug_info`/
# `:target`/`:target_machine`/`:analysis`) -- see libs/llvm/CMakeLists.txt.
add_subdirectory(libs/llvm)

# Validates that every scpp-language stdlib `.scpp` source file under
# libs/std/ and libs/scpp/ is also valid, compilable real C++ -- see
# libs/CMakeLists.txt for the full rationale.
add_subdirectory(libs)

add_subdirectory(src/compiler/codegen)

add_subdirectory(src/compiler/movecheck)

add_library(scpp_constexpr)
target_sources(scpp_constexpr
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/compiler/constexpression.cppm)
# Same reason as scpp_ast and scpp_parser above: this file now spells one
# genuinely unsafe operation with `[[scpp::unsafe]]` (the stack-depth
# measurement in call_function), and a real C++ compiler warns that it
# does not know the attribute. Front-matter Clause 4 (2)'s note names
# `-Wno-unknown-attributes` as the expected accommodation, because an
# scpp attribute is by construction one C++26 "can already parse and
# silently ignore".
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scpp_constexpr PRIVATE -Wno-error=unknown-attributes)
endif()
target_link_libraries(scpp_constexpr PUBLIC scpp_ast)

add_library(scpp_driver)
target_sources(scpp_driver
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/driver/driver.cppm)
# driver.cppm itself `import llvm;` (for LLVMTargetDataRef and the
# native-target-init bridge from the `:target` partition, and for
# target-triple lookup, target-machine creation, and object-file emission
# from the `:target_machine` partition), so the single, consolidated
# `llvm` target is linked here explicitly, alongside ${LLVM_LIBS} for the
# real LLVM-C symbols every partition's own declarations ultimately
# resolve to.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scpp_driver PRIVATE -Wno-error=unknown-attributes)
endif()
target_link_libraries(scpp_driver PUBLIC scpp_ast scpp_parser scpp_codegen scpp_movecheck scpp_constexpr llvm ${LLVM_LIBS})

add_executable(scpp)

target_sources(scpp
    PRIVATE
        src/main.cpp
    PRIVATE
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/cli/cli.cppm)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scpp PRIVATE -Wno-error=unknown-attributes)
endif()
target_link_libraries(scpp PRIVATE scpp_lexer scpp_ast scpp_parser scpp_codegen scpp_movecheck scpp_driver)

target_compile_definitions(scpp PRIVATE
    SCPP_VERSION="${PROJECT_VERSION}")

install(TARGETS scpp
    RUNTIME DESTINATION bin)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/libs/"
    DESTINATION "share/scpp/libs"
    FILES_MATCHING
        PATTERN "llvm" EXCLUDE
        PATTERN "llvm/*" EXCLUDE
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

add_test(NAME lexer_test COMMAND lexer_test)

add_executable(parser_test tests/parser_test.cpp)
target_link_libraries(parser_test PRIVATE scpp_parser scpp_ast scpp_lexer)
target_compile_definitions(parser_test PRIVATE
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}")

add_test(NAME parser_test COMMAND parser_test)

add_executable(codegen_test tests/codegen_test.cpp)
target_link_libraries(codegen_test PRIVATE scpp_codegen scpp_movecheck scpp_constexpr scpp_parser scpp_ast scpp_lexer)
target_compile_definitions(codegen_test PRIVATE
    SCPP_CODEGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/codegentest_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}")

add_test(NAME codegen_test COMMAND codegen_test)

add_executable(movecheck_test tests/movecheck_test.cpp)
target_link_libraries(movecheck_test PRIVATE scpp_movecheck scpp_parser scpp_ast scpp_lexer)
target_compile_definitions(movecheck_test PRIVATE
    SCPP_MOVETEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/movetest_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}")

add_test(NAME movecheck_test COMMAND movecheck_test)

add_executable(driver_test tests/driver_test.cpp)
target_link_libraries(driver_test PRIVATE scpp_driver scpp_codegen scpp_movecheck scpp_parser scpp_ast scpp_lexer)
target_compile_definitions(driver_test PRIVATE
    SCPP_BINARY_PATH="$<TARGET_FILE:scpp>"
    SCPP_DRIVER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_test_source"
    SCPP_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source"
    SCPP_CMAKE_COMMAND="${CMAKE_COMMAND}"
    SCPP_PACKAGES_CMAKE_MODULE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_STD_INTERFACE_PATH="${SCPP_STDLIB_STD_INTERFACE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}"
    SCPP_STDLIB_SCPP_INTERFACE_PATH="${SCPP_STDLIB_SCPP_INTERFACE_PATH}")

add_test(NAME driver_test COMMAND driver_test)

# --- Packages written in scpp --------------------------------------------------
# The stdlib (`std`, `scpp`), the `llvm` binding module and the self-hosted
# compiler are compiled by the `scpp` built above, through its clang-like
# command line (`scpp build-module`, `scpp -c`) -- see
# cmake/ScppPackages.cmake. Every module interface, every implementation
# partition (e.g. the movecheck and codegen ones), every native object and
# every archive is its own build edge, so a build is parallel, shows
# per-file progress and rebuilds only what a change affects. A package's
# DEPENDS lists the packages whose modules it may import. Library
# *behavior* itself is covered by tests/, not demos.
include(cmake/ScppPackages.cmake)
set(SCPP_PACKAGES_COMPILER scpp)

# The C++ halves of the stdlib and of the `llvm` module: compiled by CMake
# and merged into the module's archive next to the scpp-compiled object.
function(scpp_add_native_objects target)
    add_library(${target} OBJECT ${ARGN})
    set_target_properties(${target} PROPERTIES CXX_MODULE_STD 0 CXX_SCAN_FOR_MODULES OFF)
    target_compile_options(${target} PRIVATE -O2)
endfunction()

scpp_add_native_objects(scpp_std_native
    libs/std/string/scpp_string_wrapper.cpp
    libs/std/charconv/scpp_charconv_wrapper.cpp
    libs/std/memory/scpp_shared_ptr_wrapper.cpp
    libs/std/thread/scpp_thread_wrapper.cpp
    libs/std/print/scpp_print_wrapper.cpp
    libs/std/random/scpp_random_wrapper.cpp
    libs/std/vector/scpp_vector_wrapper.cpp)

scpp_add_native_objects(scpp_scpp_native libs/scpp/io/scpp_io_wrapper.cpp)

# `.scpp` isn't a CMake-recognized extension; this one is plain C++ (see
# libs/llvm/native_target_init.scpp).
set_source_files_properties(libs/llvm/native_target_init.scpp PROPERTIES LANGUAGE CXX)
scpp_add_native_objects(scpp_llvm_native libs/llvm/native_target_init.scpp)
target_include_directories(scpp_llvm_native SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(scpp_llvm_native PRIVATE ${LLVM_DEFINITIONS_LIST})

# std and scpp land in build/libs: that is where the compiler looks for a
# prebuilt stdlib next to itself, and where SCPP_STDLIB_*_PATH point.
file(GLOB_RECURSE SCPP_STD_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libs/std/*.scpp")
file(GLOB_RECURSE SCPP_SCPP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libs/scpp/*.scpp")

scpp_add_package(NAME std
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/libs"
    SOURCES ${SCPP_STD_SOURCES}
    NATIVE_OBJECTS scpp_std_native)

scpp_add_package(NAME scpp
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/libs"
    SOURCES ${SCPP_SCPP_SOURCES}
    DEPENDS std
    NATIVE_OBJECTS scpp_scpp_native)

scpp_add_package(NAME llvm
    SOURCES
        libs/llvm/llvm.scpp
        libs/llvm/types.scpp
        libs/llvm/core.scpp
        libs/llvm/debug_info.scpp
        libs/llvm/target.scpp
        libs/llvm/target_machine.scpp
        libs/llvm/analysis.scpp
    DEPENDS std
    NATIVE_OBJECTS scpp_llvm_native)

scpp_add_package(NAME scpp-compiler
    SOURCES
        src/compiler/ast.cppm
        src/compiler/lexer.cppm
        src/compiler/parser.cppm
        src/compiler/constexpression.cppm
        src/mir.cppm
    DEPENDS std)

scpp_add_package(NAME scpp-movecheck
    SOURCES
        src/compiler/movecheck/movecheck.cppm
        src/compiler/movecheck/errors.cppm
        src/compiler/movecheck/state.cppm
        src/compiler/movecheck/types.cppm
        src/compiler/movecheck/signatures.cppm
        src/compiler/movecheck/generics_support.cppm
        src/compiler/movecheck/calls.cppm
        src/compiler/movecheck/threadsafety.cppm
        src/compiler/movecheck/interfaces.cppm
        src/compiler/movecheck/borrows.cppm
        src/compiler/movecheck/lambdas.cppm
        src/compiler/movecheck/dataflow.cppm
        src/compiler/movecheck/monomorphize.cppm
    DEPENDS std scpp-compiler)

scpp_add_package(NAME scpp-codegen
    SOURCES
        src/compiler/codegen/codegen.cppm
        src/compiler/codegen/errors.cppm
        src/compiler/codegen/api.cppm
        src/compiler/codegen/debug.cppm
        src/compiler/codegen/functions.cppm
        src/compiler/codegen/layout.cppm
        src/compiler/codegen/lifetime.cppm
        src/compiler/codegen/object_model.cppm
        src/compiler/codegen/orchestration.cppm
        src/compiler/codegen/statements.cppm
        src/compiler/codegen/semantics.cppm
        src/compiler/codegen/expressions.cppm
    DEPENDS std scpp-compiler llvm)

scpp_add_package(NAME scpp-driver SOURCES src/driver/driver.cppm
    DEPENDS std scpp-compiler scpp-movecheck scpp-codegen llvm)

scpp_add_package(NAME scpp-cli SOURCES src/cli/cli.cppm
    DEPENDS std scpp-compiler scpp-movecheck scpp-codegen scpp-driver llvm)

# Everything above: what `lib` and `stage0` stand for, and part of ALL.
get_property(SCPP_PACKAGE_TARGET_LIST GLOBAL PROPERTY SCPP_PACKAGE_TARGETS)
add_custom_target(scpp_packages ALL)
add_dependencies(scpp_packages ${SCPP_PACKAGE_TARGET_LIST})

# driver_test compiles real programs against the *built* stdlib artifacts
# (SCPP_STDLIB_STD_INTERFACE_PATH / SCPP_STDLIB_SCPP_INTERFACE_PATH above,
# i.e. build/libs/*.scppm), so it needs them current: without this edge
# `cmake --build build --target driver_test` would leave whatever artifacts
# the previous compiler had produced. A `.scppm` written by an older
# compiler is rejected outright by its own format-version guard, which turns
# that into a loud failure rather than a silent one.
add_dependencies(driver_test scpp_pkg_std scpp_pkg_scpp)

add_subdirectory(applications/httpserver)

# Custom target for building the library packages
add_custom_target(lib
    DEPENDS scpp_packages
    COMMENT "Building scpp library packages"
)

# Custom target for Stage 0 (builds host scpp compiler and the packages it compiles)
add_custom_target(stage0
    DEPENDS scpp scpp_packages
    COMMENT "Stage 0 bootstrap complete"
)

# What the Stage 1 tree takes from this one: the archives the stage-1 compiler
# links, in link order (importers before the modules they import; the stdlib
# `scpp` rides along as it always has), and the interfaces its main.cpp is
# compiled against.
scpp_module_closure(SCPP_STAGE1_LINK_MODULES scpp scpp.cli)
scpp_module_archives(SCPP_STAGE1_ARCHIVES ${SCPP_STAGE1_LINK_MODULES})
scpp_source_imports("${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp" SCPP_STAGE1_MAIN_IMPORTS)
scpp_module_closure(SCPP_STAGE1_MAIN_MODULES ${SCPP_STAGE1_MAIN_IMPORTS})
scpp_module_import_arguments(SCPP_STAGE1_MAIN_IMPORT_ARGUMENTS SCPP_STAGE1_MAIN_INTERFACES
    ${SCPP_STAGE1_MAIN_MODULES})
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stage1-inputs.cmake"
    "# Written by the Stage 0 configure step, read by the Stage 1 tree.\n"
    "set(SCPP_STAGE1_ARCHIVES \"${SCPP_STAGE1_ARCHIVES}\")\n"
    "set(SCPP_STAGE1_MAIN_IMPORT_ARGUMENTS \"${SCPP_STAGE1_MAIN_IMPORT_ARGUMENTS}\")\n"
    "set(SCPP_STAGE1_MAIN_INTERFACES \"${SCPP_STAGE1_MAIN_INTERFACES}\")\n")

# Custom target for Stage 1 (drives Stage 1 configure, build, and smoke tests)
if(NOT STAGE1_BUILD_DIR)
    set(STAGE1_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/stage1")
endif()

if(NOT CMAKE_CTEST_COMMAND)
    find_program(CMAKE_CTEST_COMMAND ctest REQUIRED)
endif()

set(STAGE1_CMAKE_ARGS
    -S "${CMAKE_CURRENT_SOURCE_DIR}"
    -B "${STAGE1_BUILD_DIR}"
    -G "${CMAKE_GENERATOR}"
    -DSTAGE=1
    -DSTAGE0_SCPP=$<TARGET_FILE:scpp>
    -DSTAGE0_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
)
if(CMAKE_BUILD_TYPE)
    list(APPEND STAGE1_CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
endif()
if(CMAKE_C_COMPILER)
    list(APPEND STAGE1_CMAKE_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
endif()
if(CMAKE_CXX_COMPILER)
    list(APPEND STAGE1_CMAKE_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
endif()
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND STAGE1_CMAKE_ARGS -A "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND STAGE1_CMAKE_ARGS -T "${CMAKE_GENERATOR_TOOLSET}")
endif()
if(LLVM_DIR)
    list(APPEND STAGE1_CMAKE_ARGS -DLLVM_DIR=${LLVM_DIR})
endif()

add_custom_target(stage1
    COMMAND ${CMAKE_COMMAND} ${STAGE1_CMAKE_ARGS}
    COMMAND ${CMAKE_COMMAND} --build "${STAGE1_BUILD_DIR}"
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${STAGE1_BUILD_DIR}" --output-on-failure
    DEPENDS stage0
    COMMENT "Configuring, building, and verifying Stage 1 self-hosted compiler"
    VERBATIM
)

elseif(STAGE EQUAL 1)

    # =========================================================================
    # Bootstrap Stage 1: Build Stage-1 scpp using Stage-0 scpp
    # =========================================================================
    if(NOT STAGE0_SCPP)
        if(DEFINED ENV{STAGE0_SCPP})
            set(STAGE0_SCPP "$ENV{STAGE0_SCPP}")
        elseif(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/scpp")
            set(STAGE0_SCPP "${CMAKE_CURRENT_BINARY_DIR}/scpp")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/scpp")
            set(STAGE0_SCPP "${CMAKE_CURRENT_SOURCE_DIR}/build/scpp")
        elseif(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/../build/scpp")
            set(STAGE0_SCPP "${CMAKE_CURRENT_BINARY_DIR}/../build/scpp")
        elseif(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/../scpp")
            set(STAGE0_SCPP "${CMAKE_CURRENT_BINARY_DIR}/../scpp")
        else()
            find_program(STAGE0_SCPP scpp)
        endif()
    endif()

    if(NOT STAGE0_SCPP OR NOT EXISTS "${STAGE0_SCPP}")
        message(FATAL_ERROR "Stage 0 scpp compiler not found! Please build stage 0 first or specify -DSTAGE0_SCPP=/path/to/scpp")
    endif()
    get_filename_component(STAGE0_SCPP "${STAGE0_SCPP}" ABSOLUTE)
    message(STATUS "Using Stage 0 compiler: ${STAGE0_SCPP}")

    # The packages the stage-1 compiler is made of were compiled by the Stage 0
    # build tree (see cmake/ScppPackages.cmake); it records what Stage 1 needs.
    if(NOT STAGE0_BUILD_DIR)
        get_filename_component(STAGE0_BUILD_DIR "${STAGE0_SCPP}" DIRECTORY)
    endif()
    if(NOT EXISTS "${STAGE0_BUILD_DIR}/stage1-inputs.cmake")
        message(FATAL_ERROR "No Stage 0 build tree in '${STAGE0_BUILD_DIR}'! Please build stage 0 first or specify -DSTAGE0_BUILD_DIR=/path/to/stage0/build")
    endif()
    include("${STAGE0_BUILD_DIR}/stage1-inputs.cmake")
    foreach(archive IN LISTS SCPP_STAGE1_ARCHIVES)
        if(NOT EXISTS "${archive}")
            message(FATAL_ERROR "Stage 0 package archive '${archive}' not found! Please build stage 0 first (cmake --build ${STAGE0_BUILD_DIR} --target stage0)")
        endif()
    endforeach()

    file(GLOB STAGE1_LLVM_STATIC_LIBS "${LLVM_LIBRARY_DIRS}/libLLVM*.a")
    set(STAGE1_MAIN_OBJ "${CMAKE_CURRENT_BINARY_DIR}/main_stage1.o")

    add_custom_command(
        OUTPUT "${STAGE1_MAIN_OBJ}"
        COMMAND "${STAGE0_SCPP}" -c "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
            -o "${STAGE1_MAIN_OBJ}"
            ${SCPP_STAGE1_MAIN_IMPORT_ARGUMENTS}
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
            "${STAGE0_SCPP}"
            ${SCPP_STAGE1_MAIN_INTERFACES}
        COMMENT "Compiling main.cpp with Stage-0 scpp"
        VERBATIM
    )

    set_source_files_properties("${STAGE1_MAIN_OBJ}" PROPERTIES
        EXTERNAL_OBJECT TRUE
        GENERATED TRUE
    )

    add_executable(scpp_stage1
        "${STAGE1_MAIN_OBJ}"
    )

    set_target_properties(scpp_stage1 PROPERTIES
        LINKER_LANGUAGE CXX
    )

    target_link_libraries(scpp_stage1 PRIVATE
        ${SCPP_STAGE1_ARCHIVES}
        "-Wl,--start-group"
        ${STAGE1_LLVM_STATIC_LIBS}
        "-Wl,--end-group"
        -lz3 -lz -lzstd -lrt -ldl -lm -lpthread
    )

    add_custom_command(
        TARGET scpp_stage1 POST_BUILD
        COMMAND "$<TARGET_FILE:scpp_stage1>" --version
        COMMAND "$<TARGET_FILE:scpp_stage1>" -c
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/codegentest_source/return_constant.scpp"
            -o "${CMAKE_CURRENT_BINARY_DIR}/stage1_smoke_test.o"
        COMMENT "Running Stage-1 smoke tests"
        VERBATIM
    )

    enable_testing()
    add_test(NAME stage1_version_smoke COMMAND scpp_stage1 --version)
    add_test(NAME stage1_compile_smoke COMMAND scpp_stage1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/codegentest_source/return_constant.scpp"
        -o "${CMAKE_CURRENT_BINARY_DIR}/stage1_smoke_test.o")

    add_custom_target(stage1 DEPENDS scpp_stage1)

    install(TARGETS scpp_stage1
        RUNTIME DESTINATION bin)

endif()
