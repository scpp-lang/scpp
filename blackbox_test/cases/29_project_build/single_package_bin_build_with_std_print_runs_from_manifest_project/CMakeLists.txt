cmake_minimum_required(VERSION 3.25)
project(printbin LANGUAGES NONE)
include(ScppPackages)

# `import std;` is resolved by the compiler from the stdlib built next to it;
# only its archives have to be linked.
set(SCPP_PACKAGES_PREBUILT_MODULES std)
cmake_path(GET SCPP_PACKAGES_COMPILER PARENT_PATH compiler_dir)
scpp_add_executable(NAME hello SOURCES main.scpp OUTPUT bin/hello ALL
    LINK_LIBRARIES "${compiler_dir}/libs/libstd.scppa" "${compiler_dir}/libs/libscpp.scppa")
