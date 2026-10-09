cmake_minimum_required(VERSION 3.25)
project(workspace LANGUAGES CXX)
include(ScppPackages)

add_subdirectory(libnative)
add_subdirectory(app)
