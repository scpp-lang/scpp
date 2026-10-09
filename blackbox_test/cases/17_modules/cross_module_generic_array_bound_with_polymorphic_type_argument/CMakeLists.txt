cmake_minimum_required(VERSION 3.25)
project(workspace LANGUAGES NONE)
include(ScppPackages)

add_subdirectory(libbox)
add_subdirectory(app)
