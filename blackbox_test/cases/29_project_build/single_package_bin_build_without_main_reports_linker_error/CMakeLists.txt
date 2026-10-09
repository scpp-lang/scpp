cmake_minimum_required(VERSION 3.25)
project(nomain LANGUAGES NONE)
include(ScppPackages)

scpp_add_executable(NAME nomain SOURCES src/helper.scpp src/other.scpp OUTPUT bin/nomain ALL)
