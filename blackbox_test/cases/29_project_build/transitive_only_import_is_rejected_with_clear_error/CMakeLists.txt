cmake_minimum_required(VERSION 3.25)
project(app LANGUAGES NONE)
include(ScppPackages)

add_subdirectory(libmath)
add_subdirectory(libutil)
scpp_add_executable(NAME app SOURCES main.scpp DEPENDS libutil OUTPUT bin/app ALL)
