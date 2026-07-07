cmake_minimum_required(VERSION 3.28)

project(scpp
    VERSION 0.1.0
    DESCRIPTION "A compiler frontend for a C++-looking language with safe regions"
    LANGUAGES CXX)

set(SCPP_STDLIB_STD_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/std.cpp")
set(SCPP_STDLIB_STD_STRING_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/string/std_string.cpp")
set(SCPP_STDLIB_STD_MEMORY_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/memory/std_memory.cpp")
set(SCPP_STDLIB_STRING_WRAPPER_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/stdlib/libscpp_string_wrapper.a")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
llvm_map_components_to_libnames(LLVM_LIBS core support target native)

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
target_link_libraries(scpp_codegen PUBLIC scpp_ast ${LLVM_LIBS})
target_compile_features(scpp_codegen PRIVATE cxx_std_23)

add_library(scpp_movecheck)
target_sources(scpp_movecheck
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/movecheck.cppm)
target_link_libraries(scpp_movecheck PUBLIC scpp_ast scpp_mir)
target_compile_features(scpp_movecheck PRIVATE cxx_std_23)

add_library(scpp_driver)
target_sources(scpp_driver
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/driver.cppm)
target_link_libraries(scpp_driver PUBLIC scpp_ast scpp_parser scpp_codegen scpp_movecheck ${LLVM_LIBS})
target_compile_features(scpp_driver PRIVATE cxx_std_23)

add_executable(scpp)

target_sources(scpp
    PRIVATE
        src/main.cpp
    PRIVATE
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/cli.cppm)
target_link_libraries(scpp PRIVATE scpp_lexer scpp_ast scpp_parser scpp_codegen scpp_movecheck scpp_driver)

target_compile_features(scpp PRIVATE cxx_std_23)

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
    SCPP_STDLIB_STD_STRING_MODULE_PATH="${SCPP_STDLIB_STD_STRING_MODULE_PATH}"
    SCPP_STDLIB_STD_MEMORY_MODULE_PATH="${SCPP_STDLIB_STD_MEMORY_MODULE_PATH}")

add_test(NAME parser_test COMMAND parser_test)

add_executable(codegen_test tests/codegen_test.cpp)
target_link_libraries(codegen_test PRIVATE scpp_codegen scpp_movecheck scpp_parser scpp_ast scpp_lexer)
target_compile_features(codegen_test PRIVATE cxx_std_23)
target_compile_definitions(codegen_test PRIVATE
    SCPP_CODEGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/codegentest_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_STD_STRING_MODULE_PATH="${SCPP_STDLIB_STD_STRING_MODULE_PATH}"
    SCPP_STDLIB_STD_MEMORY_MODULE_PATH="${SCPP_STDLIB_STD_MEMORY_MODULE_PATH}")

add_test(NAME codegen_test COMMAND codegen_test)

add_executable(movecheck_test tests/movecheck_test.cpp)
target_link_libraries(movecheck_test PRIVATE scpp_movecheck scpp_parser scpp_ast scpp_lexer)
target_compile_features(movecheck_test PRIVATE cxx_std_23)
target_compile_definitions(movecheck_test PRIVATE
    SCPP_MOVETEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/movetest_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_STD_STRING_MODULE_PATH="${SCPP_STDLIB_STD_STRING_MODULE_PATH}"
    SCPP_STDLIB_STD_MEMORY_MODULE_PATH="${SCPP_STDLIB_STD_MEMORY_MODULE_PATH}")

add_test(NAME movecheck_test COMMAND movecheck_test)

add_executable(driver_test tests/driver_test.cpp)
target_link_libraries(driver_test PRIVATE scpp_driver scpp_codegen scpp_movecheck scpp_parser scpp_ast scpp_lexer)
target_compile_features(driver_test PRIVATE cxx_std_23)
target_compile_definitions(driver_test PRIVATE
    SCPP_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source"
    SCPP_STDLIB_STD_MODULE_PATH="${SCPP_STDLIB_STD_MODULE_PATH}"
    SCPP_STDLIB_STD_STRING_MODULE_PATH="${SCPP_STDLIB_STD_STRING_MODULE_PATH}"
    SCPP_STDLIB_STD_MEMORY_MODULE_PATH="${SCPP_STDLIB_STD_MEMORY_MODULE_PATH}"
    SCPP_STDLIB_STRING_WRAPPER_LIB_PATH="${SCPP_STDLIB_STRING_WRAPPER_LIB_PATH}")

add_test(NAME driver_test COMMAND driver_test)

# Builds stdlib helper libraries (e.g. the native wrapper behind
# std::string). Library behavior itself is covered by tests/, not demos.
add_subdirectory(stdlib)
