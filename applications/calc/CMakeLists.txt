cmake_minimum_required(VERSION 3.28)

project(calc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)

add_library(mathlib STATIC)
target_sources(mathlib
    PUBLIC
        FILE_SET CXX_MODULES FILES
            math.cppm
            math_ops.cppm)

add_executable(calc main.cpp)
target_link_libraries(calc PRIVATE mathlib)
