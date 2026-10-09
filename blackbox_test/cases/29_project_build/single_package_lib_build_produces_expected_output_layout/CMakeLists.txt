cmake_minimum_required(VERSION 3.25)
project(minlib LANGUAGES NONE)
include(ScppPackages)

scpp_add_package(NAME minlib SOURCES src/minlib.scpp ALL)
