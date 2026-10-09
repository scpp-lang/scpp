cmake_minimum_required(VERSION 3.25)
project(multilib LANGUAGES NONE)
include(ScppPackages)

scpp_add_package(NAME multilib SOURCES src/mathlib.scpp src/textlib.scpp ALL)
