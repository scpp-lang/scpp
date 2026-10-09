cmake_minimum_required(VERSION 3.25)
project(buildalllibs LANGUAGES NONE)
include(ScppPackages)

scpp_add_package(NAME buildalllibs SOURCES src/alpha.scpp src/beta.scpp)
scpp_add_executable(NAME buildalllibs SOURCES src/app_main.scpp DEPENDS buildalllibs OUTPUT bin/buildalllibs)
