cmake_minimum_required(VERSION 3.25)
project(minbin LANGUAGES NONE)
include(ScppPackages)

scpp_add_executable(NAME hello SOURCES main.scpp OUTPUT bin/hello ALL)
