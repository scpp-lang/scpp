cmake_minimum_required(VERSION 3.28)

project(scpp
    VERSION 0.1.0
    DESCRIPTION "A compiler frontend for a C++-looking language with safe regions"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_executable(scpp)

target_sources(scpp
    PRIVATE
        src/main.cpp
    PRIVATE
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/cli.cppm)

target_compile_features(scpp PRIVATE cxx_std_23)
