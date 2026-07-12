cmake_minimum_required(VERSION 3.28)

project(scpp
    VERSION 26.0.0
    DESCRIPTION "A compiler frontend for a C++-looking language with safe regions"
    LANGUAGES C CXX)

set(SCPP_STDLIB_STD_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/std/std.scpp")
set(SCPP_STDLIB_STD_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/std.scppm")
set(SCPP_STDLIB_STD_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libstd.scppa")
set(SCPP_STDLIB_SCPP_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/scpp/scpp.scpp")
set(SCPP_STDLIB_SCPP_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/scpp.scppm")
set(SCPP_STDLIB_SCPP_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libscpp.scppa")
set(SCPP_STDLIB_CHARCONV_WRAPPER_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libscpp_charconv_wrapper.a")
set(SCPP_STDLIB_IO_WRAPPER_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libscpp_io_wrapper.a")
set(SCPP_STDLIB_STRING_WRAPPER_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libscpp_string_wrapper.a")
set(SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libscpp_thread_wrapper.a")
set(SCPP_STDLIB_PRINT_WRAPPER_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libscpp_print_wrapper.a")
set(SCPP_STDLIB_RANDOM_WRAPPER_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libs/libscpp_random_wrapper.a")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

add_library(scpp_codegen)
target_sources(scpp_codegen
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/codegen.cppm)
target_include_directories(scpp_codegen SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(scpp_codegen PUBLIC ${LLVM_DEFINITIONS_LIST})
target_link_libraries(scpp_codegen PUBLIC scpp_ast scpp_constexpr ${LLVM_LIBS})
target_compile_features(scpp_codegen PRIVATE cxx_std_23)

add_library(scpp_movecheck)
target_sources(scpp_movecheck
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/movecheck.cppm)
target_link_libraries(scpp_movecheck PUBLIC scpp_ast scpp_mir)
target_compile_features(scpp_movecheck PRIVATE cxx_std_23)

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
target_link_libraries(scpp_driver PUBLIC scpp_ast scpp_parser scpp_codegen scpp_movecheck scpp_constexpr ${LLVM_LIBS})
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
    "${SCPP_STDLIB_CHARCONV_WRAPPER_LIB_PATH}"
    "${SCPP_STDLIB_IO_WRAPPER_LIB_PATH}"
    "${SCPP_STDLIB_STRING_WRAPPER_LIB_PATH}"
    "${SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH}"
    "${SCPP_STDLIB_PRINT_WRAPPER_LIB_PATH}"
    "${SCPP_STDLIB_RANDOM_WRAPPER_LIB_PATH}"
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
target_link_libraries(codegen_test PRIVATE scpp_codegen scpp_movecheck scpp_parser scpp_ast scpp_lexer)
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
    SCPP_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_STD_INTERFACE_PATH="${SCPP_STDLIB_STD_INTERFACE_PATH}"
    SCPP_STDLIB_SCPP_MODULE_PATH="${SCPP_STDLIB_SCPP_MODULE_PATH}"
    SCPP_STDLIB_SCPP_INTERFACE_PATH="${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
    SCPP_STDLIB_CHARCONV_WRAPPER_LIB_PATH="${SCPP_STDLIB_CHARCONV_WRAPPER_LIB_PATH}"
    SCPP_STDLIB_IO_WRAPPER_LIB_PATH="${SCPP_STDLIB_IO_WRAPPER_LIB_PATH}"
    SCPP_STDLIB_STRING_WRAPPER_LIB_PATH="${SCPP_STDLIB_STRING_WRAPPER_LIB_PATH}"
    SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH="${SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH}"
    SCPP_STDLIB_PRINT_WRAPPER_LIB_PATH="${SCPP_STDLIB_PRINT_WRAPPER_LIB_PATH}"
    SCPP_STDLIB_RANDOM_WRAPPER_LIB_PATH="${SCPP_STDLIB_RANDOM_WRAPPER_LIB_PATH}")

add_test(NAME driver_test COMMAND driver_test)

# Builds library helper libraries (e.g. the native wrapper behind
# std::string). Library behavior itself is covered by tests/, not demos.
add_subdirectory(libs)
add_subdirectory(applications/httpserver)
