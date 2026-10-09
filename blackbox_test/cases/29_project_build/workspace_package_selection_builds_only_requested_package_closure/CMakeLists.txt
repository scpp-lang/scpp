cmake_minimum_required(VERSION 3.25)
project(workspace LANGUAGES NONE)
include(ScppPackages)

add_subdirectory(libmath)
add_subdirectory(libutil)
add_subdirectory(app)
add_subdirectory(tool)
