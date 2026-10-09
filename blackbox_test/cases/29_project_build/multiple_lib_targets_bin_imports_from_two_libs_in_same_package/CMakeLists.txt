cmake_minimum_required(VERSION 3.25)
project(importtwolibs LANGUAGES NONE)
include(ScppPackages)

scpp_add_package(NAME importtwolibs SOURCES src/alpha.scpp src/beta.scpp)
scpp_add_executable(NAME importtwolibs SOURCES src/app_main.scpp DEPENDS importtwolibs
    OUTPUT bin/importtwolibs ALL)
