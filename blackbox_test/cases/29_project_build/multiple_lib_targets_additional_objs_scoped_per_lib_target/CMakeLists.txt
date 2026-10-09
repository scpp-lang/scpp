cmake_minimum_required(VERSION 3.25)
project(multiobjs LANGUAGES CXX)
include(ScppPackages)

add_library(nativea_shim OBJECT src/nativea_value.cpp)
add_library(nativeb_shim OBJECT src/nativeb_value.cpp)
scpp_add_package(NAME nativea SOURCES src/nativea.scpp NATIVE_OBJECTS nativea_shim)
scpp_add_package(NAME nativeb SOURCES src/nativeb.scpp NATIVE_OBJECTS nativeb_shim)
scpp_add_executable(NAME multiobjs SOURCES src/app_main.scpp DEPENDS nativea nativeb
    OUTPUT bin/multiobjs ALL)
