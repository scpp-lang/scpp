cmake_minimum_required(VERSION 3.25)
project(duplicatelib LANGUAGES NONE)
include(ScppPackages)

scpp_add_package(NAME shared SOURCES src/one.scpp)
scpp_add_package(NAME shared SOURCES src/two.scpp)
