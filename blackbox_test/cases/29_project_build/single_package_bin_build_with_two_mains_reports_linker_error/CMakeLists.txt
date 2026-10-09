cmake_minimum_required(VERSION 3.25)
project(twomains LANGUAGES NONE)
include(ScppPackages)

scpp_add_executable(NAME twomains SOURCES src/first.scpp src/second.scpp OUTPUT bin/twomains ALL)
