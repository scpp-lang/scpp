cmake_minimum_required(VERSION 3.28)

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

list(INSERT CMAKE_FIND_LIBRARY_SUFFIXES 0 ".a")
find_library(SQLITE3_LIB NAMES sqlite3 REQUIRED)
find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h REQUIRED)

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
            src/driver.cppm)
# driver.cppm itself `import llvm;` (for LLVMTargetDataRef and the
# native-target-init bridge from the `:target` partition, and for
# target-triple lookup, target-machine creation, and object-file emission
# from the `:target_machine` partition), so the single, consolidated
# `llvm` target is linked here explicitly, alongside ${LLVM_LIBS} for the
# real LLVM-C symbols every partition's own declarations ultimately
# resolve to.
target_link_libraries(scpp_driver PUBLIC scpp_ast scpp_parser scpp_codegen scpp_movecheck scpp_constexpr llvm ${LLVM_LIBS})

add_library(scpp_project)
target_sources(scpp_project
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/project.cppm)
target_include_directories(scpp_project PRIVATE ${SQLITE3_INCLUDE_DIR})
target_link_libraries(scpp_project PUBLIC scpp_lexer scpp_driver ${SQLITE3_LIB})

add_executable(scpp)

target_sources(scpp
    PRIVATE
        src/main.cpp
    PRIVATE
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/cli/cli.cppm)
target_link_libraries(scpp PRIVATE scpp_lexer scpp_ast scpp_parser scpp_codegen scpp_movecheck scpp_driver scpp_project)

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
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_STD_INTERFACE_PATH="${SCPP_STDLIB_STD_INTERFACE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}"
    SCPP_STDLIB_SCPP_INTERFACE_PATH="${SCPP_STDLIB_SCPP_INTERFACE_PATH}")

add_test(NAME driver_test COMMAND driver_test)

# Builds the shipped workspace (root scpp.toml + libs/std/scpp.toml,
# libs/scpp/scpp.toml, src/scpp.toml, applications/httpserver/scpp.toml)
# via scpp's own package manager, and copies the resulting std/scpp
# artifacts into build/libs. Library *behavior* itself is covered by
# tests/, not demos. (Named scpp_workspace_artifacts, not
# scpp_stdlib_artifacts: it covers the whole workspace now, not just the
# two stdlib packages -- src's ast.cppm and applications/httpserver's
# [[bin]] both build here too, see below.)
#
# Deliberately always-run (no file(GLOB_RECURSE ...)/CONFIGURE_DEPENDS
# tracking of scpp.toml or *.scpp/*.cpp sources, unlike earlier
# revisions of this target): `scpp build` already has a real,
# content-hash-based incremental build cache of its own (BuildDatabase,
# a SQLite db at .scpp/cache/build.db -- see src/project.cppm, keyed by
# a signature over each module's source digest, manifest digest,
# compiler-binary digest, target triple, and dependency digests).
# Maintaining a second, independent "which files matter" list here
# purely to decide whether to invoke `scpp build` at all would just be
# drift-prone duplication of information scpp's own cache already
# tracks more precisely -- e.g. it also reacts correctly to a rebuilt
# compiler or a changed target triple, which a plain CMake file glob
# never would have. So this target's COMMAND runs unconditionally on
# every `cmake --build`, trusting `scpp build` to make that a fast
# cache-hit no-op whenever nothing relevant actually changed (measured:
# roughly 1-3 seconds of overhead for the whole chdir+build+copy recipe
# on a fully cached, nothing-changed rebuild -- see the PR description
# for exact numbers).
#
# No `--lib`: this workspace now genuinely has a [[bin]] target
# (applications/httpserver), so omitting `--lib` (build_lib_only in
# src/project.cppm) is what makes that binary actually get built here,
# not a no-op left over from clarity/dead-ceremony cleanup like before --
# `libs/std`/`libs/scpp`/`src` still declare [[lib]] only, so this is
# purely additive for them.
#
# The artifact paths under .scpp/build/ contain only the LLVM
# target-triple as a path component now (.scpp/build/<triple>/
# <package>/...) -- scpp's manifest-driven `[profile.*]` build-profile
# system (dev/release/custom profiles) was removed as underdesigned, so
# there is no longer a profile path segment to account for here; static
# linking is unconditional and opt-level/debug are fixed constants (see
# kManifestBuildOptLevel/kManifestBuildEmitDebugInfo in project.cppm)
# rather than per-profile settings. The triple is `${LLVM_HOST_TRIPLE}`,
# a normal CMake variable already set by `find_package(LLVM REQUIRED
# CONFIG)` above in this same file/scope -- confirmed to match scpp's
# own host_target_triple() exactly on every platform this project
# targets, since both ultimately describe the same LLVM host. Using it
# directly means no `find`-based path search or `llvm-config` shell-out
# is needed to locate these files: plain, direct `${CMAKE_COMMAND} -E
# copy` commands suffice, consistent with this file's usual style.
#
# DEPENDS only the `scpp` target itself, purely for build ordering (the
# compiler binary must exist before this can shell out to it) -- not for
# staleness tracking, since a plain add_custom_target's COMMAND always
# reruns regardless of its DEPENDS. BYPRODUCTS declares the 4 real files
# this recipe still produces on disk, so Ninja correctly cleans them and
# so other targets that DEPENDS on this target *by name* (e.g.
# applications/httpserver's httpserver_app_artifact, below) get correct
# ordering even though this target has no file-level OUTPUT to track --
# the same established pattern httpserver_app_artifact itself uses for
# its own always-run copy of the workspace-built httpserver binary.
add_custom_target(scpp_workspace_artifacts ALL
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_SOURCE_DIR}" $<TARGET_FILE:scpp> build
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/.scpp/build/${LLVM_HOST_TRIPLE}/std/modules/std.scppm"
        "${SCPP_STDLIB_STD_INTERFACE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/.scpp/build/${LLVM_HOST_TRIPLE}/std/archives/libstd.scppa"
        "${SCPP_STDLIB_STD_ARCHIVE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/.scpp/build/${LLVM_HOST_TRIPLE}/scpp/modules/scpp.scppm"
        "${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/.scpp/build/${LLVM_HOST_TRIPLE}/scpp/archives/libscpp.scppa"
        "${SCPP_STDLIB_SCPP_ARCHIVE_PATH}"
    BYPRODUCTS
        "${SCPP_STDLIB_STD_INTERFACE_PATH}"
        "${SCPP_STDLIB_STD_ARCHIVE_PATH}"
        "${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
        "${SCPP_STDLIB_SCPP_ARCHIVE_PATH}"
    DEPENDS scpp
    VERBATIM)

# driver_test compiles real programs against the *built* stdlib artifacts
# (SCPP_STDLIB_STD_INTERFACE_PATH / SCPP_STDLIB_SCPP_INTERFACE_PATH above,
# i.e. build/libs/*.scppm), so it needs them current -- but nothing said
# so, and `cmake --build build --target driver_test` therefore skipped
# scpp_workspace_artifacts entirely and left whatever artifacts the
# previous compiler had produced. A `.scppm` written by an older compiler
# is now rejected outright by its own format-version guard, which turns
# that into a loud failure rather than a silent one; the missing edge is
# the actual defect and this is it.
#
# add_dependencies, not a file-level DEPENDS: scpp_workspace_artifacts is
# deliberately an always-run target with no OUTPUT to track (see its own
# comment above), because `scpp build` has its own content-hash cache.
# This adds ordering only and leaves that design untouched -- the whole
# ALL build already ran it; only targeted builds were missing it.
# httpserver_test needs no equivalent edge: it consumes the httpserver
# binary through httpserver_app_artifact, which already DEPENDS on
# scpp_workspace_artifacts.
add_dependencies(driver_test scpp_workspace_artifacts)

add_subdirectory(applications/httpserver)
