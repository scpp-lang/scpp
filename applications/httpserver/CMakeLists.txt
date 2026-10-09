# httpserver is a scpp package (module `httpserver` and its partitions) plus a
# `main.scpp` entry point, compiled by the `scpp` built in the top-level
# project like the stdlib and the compiler are -- see
# cmake/ScppPackages.cmake. The `httpserver` and `scpp_httpserver` targets
# below compile the same sources as real C++.
#
# SCPP_HTTPSERVER_MODULE_PATH is what httpserver_test needs: it does its own
# independent, on-the-fly compile of a generated main.scpp (see
# tests/httpserver_test.cpp) via the single-file CLI form.
set(SCPP_HTTPSERVER_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/httpserver.scpp")

set(HTTPSERVER_MODULE_SOURCES
    httpserver.scpp
    api/httpserver_api.scpp
    platform/httpserver_platform.scpp
    runtime/httpserver_runtime.scpp)

scpp_add_package(NAME httpserver SOURCES ${HTTPSERVER_MODULE_SOURCES} DEPENDS std)

# The fully static httpserver binary, at a stable, easy-to-find path for
# manual runs (and for website/Dockerfile).
set(SCPP_HTTPSERVER_BINARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/httpserver_app_bin")
scpp_add_executable(NAME httpserver_app_artifact ALL
    SOURCES main.scpp
    DEPENDS httpserver
    OUTPUT "${SCPP_HTTPSERVER_BINARY_PATH}"
    COMPILE_FLAGS -g -O0)

set_source_files_properties(
    ${HTTPSERVER_MODULE_SOURCES}
    PROPERTIES
        LANGUAGE CXX
        COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/libs/scpp_clang_preinclude.h")

add_library(scpp_httpserver STATIC)
target_sources(scpp_httpserver
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            ${HTTPSERVER_MODULE_SOURCES})

set_target_properties(scpp_httpserver PROPERTIES CXX_MODULE_STD 0)
target_link_libraries(scpp_httpserver PUBLIC scpp_std Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scpp_httpserver PRIVATE
        -Wno-error=unknown-attributes
        -Wno-error=reserved-module-identifier
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-pessimizing-move)
endif()
add_library(scpp_httpserver_cxx_validation ALIAS scpp_httpserver)

set_source_files_properties(
    main.scpp
    PROPERTIES
        LANGUAGE CXX
        COMPILE_DEFINITIONS "__SCPP_NON_MODULE__"
        COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/libs/scpp_clang_preinclude.h")

add_executable(httpserver main.scpp)
set_target_properties(httpserver PROPERTIES CXX_MODULE_STD 0)
target_link_libraries(httpserver PRIVATE scpp_httpserver scpp_std Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(httpserver PRIVATE
        -Wno-error=unknown-attributes
        -Wno-error=reserved-module-identifier
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-pessimizing-move)
endif()

add_executable(httpserver_test tests/httpserver_test.cpp)
target_compile_definitions(httpserver_test PRIVATE
    SCPP_BINARY_PATH="$<TARGET_FILE:scpp>"
    SCPP_HTTPSERVER_MODULE_PATH="${SCPP_HTTPSERVER_MODULE_PATH}"
    SCPP_HTTPSERVER_EXECUTABLE_PATH="$<TARGET_FILE:httpserver>"
    SCPP_HTTPSERVER_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    SCPP_HTTPSERVER_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-work")
add_dependencies(httpserver_test httpserver)
add_test(NAME httpserver_test COMMAND httpserver_test)
