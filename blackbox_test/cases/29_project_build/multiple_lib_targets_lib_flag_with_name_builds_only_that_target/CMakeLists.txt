cmake_minimum_required(VERSION 3.25)
project(selectlib LANGUAGES NONE)
include(ScppPackages)

scpp_add_package(NAME alpha SOURCES src/alpha.scpp)
scpp_add_package(NAME beta SOURCES src/beta.scpp)
