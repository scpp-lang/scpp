cmake_minimum_required(VERSION 3.28)

project(httpserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)

add_library(httpserver_lib STATIC)
target_sources(httpserver_lib
    PUBLIC
        FILE_SET CXX_MODULES FILES
            httpserver.cppm
            api/httpserver_api.cppm
            platform/httpserver_platform.cppm
            runtime/httpserver_runtime.cppm)

add_executable(httpserver main.cpp)
target_link_libraries(httpserver PRIVATE httpserver_lib)

enable_testing()

add_executable(httpserver_test tests/httpserver_test.cpp)
target_link_libraries(httpserver_test PRIVATE httpserver_lib)
add_test(NAME httpserver_test COMMAND httpserver_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
