cmake_minimum_required(VERSION 3.25)
project(multibin LANGUAGES NONE)
include(ScppPackages)

scpp_add_package(NAME multibin SOURCES src/helper.scpp)
scpp_add_executable(NAME multibin SOURCES src/app.scpp DEPENDS multibin OUTPUT bin/multibin ALL)
