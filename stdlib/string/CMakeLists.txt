# stdlib/string/CMakeLists.txt
#
# Builds and tests the std::string demo end-to-end -- the concrete
# demonstration of both scpp calling into a real C/C++ library and
# scpp's own multi-file module system, module partitions included (ch11,
# see README.md):
#
#   1. scpp_string_wrapper.cpp (real C++, real std::string) is built as an
#      ordinary CMake static library target, entirely independent of the
#      scpp compiler itself -- exactly like any other existing C/C++
#      library a real project would link against.
#   2. demo.cpp (`import std;` then `std::string`) is built via a single
#      `scpp build ... --import std=std.cpp --import std:string=std_string.cpp
#      --link <wrapper>` invocation: std.cpp is the "std" module's primary
#      interface unit (`export module std; export import :string;`),
#      aggregating std_string.cpp -- the `:string` partition, `export
#      module std:string;`, holding `namespace std { export class string
#      { ... }; }` -- which is where std::string's actual declarations
#      live. Both are resolved, compiled together into one object file
#      for the whole "std" module (ch11 §11.4: partitions compile
#      together with their module's primary unit, never separately), and
#      linked together with demo.cpp's own object file and the wrapper
#      library -- real separate compilation, not a textual concatenation
#      (see src/driver.cppm's compile_to_executable).
#
# The resulting `string_demo` executable is produced as part of the
# ordinary ALL target (like any other build product in this project); the
# `stdlib_string_demo` ctest case just runs it and diffs its stdout
# against expected_output.txt.

add_library(scpp_string_wrapper STATIC scpp_string_wrapper.cpp)
target_compile_features(scpp_string_wrapper PRIVATE cxx_std_23)

set(string_demo_exe "${CMAKE_CURRENT_BINARY_DIR}/string_demo")
add_custom_command(
    OUTPUT "${string_demo_exe}"
    COMMAND "$<TARGET_FILE:scpp>" build "${CMAKE_CURRENT_SOURCE_DIR}/demo.cpp" -o "${string_demo_exe}"
            --import "std=${CMAKE_CURRENT_SOURCE_DIR}/std.cpp"
            --import "std:string=${CMAKE_CURRENT_SOURCE_DIR}/std_string.cpp"
            --link "$<TARGET_FILE:scpp_string_wrapper>"
    DEPENDS scpp scpp_string_wrapper "${CMAKE_CURRENT_SOURCE_DIR}/demo.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/std.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/std_string.cpp"
    COMMENT "stdlib/string: scpp build --import std=std.cpp --import std:string=std_string.cpp --link (demo.cpp against std module + scpp_string_wrapper)"
    VERBATIM)

add_custom_target(stdlib_string_demo_exe ALL DEPENDS "${string_demo_exe}")

add_test(NAME stdlib_string_demo
    COMMAND ${CMAKE_COMMAND} -E env bash -c
        "\"${string_demo_exe}\" | diff - \"${CMAKE_CURRENT_SOURCE_DIR}/expected_output.txt\"")
