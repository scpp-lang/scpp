# stdlib/string/CMakeLists.txt
#
# Builds and tests the String demo end-to-end -- the concrete
# demonstration of scpp calling into a real C/C++ library (see README.md):
#
#   1. scpp_string_wrapper.cpp (real C++, real std::string) is built as an
#      ordinary CMake static library target, entirely independent of the
#      scpp compiler itself -- exactly like any other existing C/C++
#      library a real project would link against.
#   2. String.cpp (the scpp `class String`) and demo.cpp (a scpp program
#      that uses it) are concatenated into one file (concat.cmake) --
#      scpp v0.1 has no multi-file/include mechanism yet, so a single
#      `scpp build` invocation needs its input as one file -- then
#      built+linked by `scpp build ... --link`, against the wrapper
#      library from step 1 (the `--link` flag exists for exactly this;
#      see src/driver.cppm's link_executable).
#
# The resulting `string_demo` executable is produced as part of the
# ordinary ALL target (like any other build product in this project); the
# `stdlib_string_demo` ctest case just runs it and diffs its stdout
# against expected_output.txt.

add_library(scpp_string_wrapper STATIC scpp_string_wrapper.cpp)
target_compile_features(scpp_string_wrapper PRIVATE cxx_std_23)

set(combined_source "${CMAKE_CURRENT_BINARY_DIR}/combined.cpp")
add_custom_command(
    OUTPUT "${combined_source}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSRC1=${CMAKE_CURRENT_SOURCE_DIR}/String.cpp"
            "-DSRC2=${CMAKE_CURRENT_SOURCE_DIR}/demo.cpp"
            "-DOUT=${combined_source}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/concat.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/String.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/demo.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/concat.cmake"
    COMMENT "stdlib/string: concatenating String.cpp + demo.cpp (scpp has no multi-file build yet)"
    VERBATIM)

set(string_demo_exe "${CMAKE_CURRENT_BINARY_DIR}/string_demo")
add_custom_command(
    OUTPUT "${string_demo_exe}"
    COMMAND "$<TARGET_FILE:scpp>" build "${combined_source}" -o "${string_demo_exe}"
            --link "$<TARGET_FILE:scpp_string_wrapper>"
    DEPENDS scpp scpp_string_wrapper "${combined_source}"
    COMMENT "stdlib/string: scpp build --link (String.cpp+demo.cpp against scpp_string_wrapper)"
    VERBATIM)

add_custom_target(stdlib_string_demo_exe ALL DEPENDS "${string_demo_exe}")

add_test(NAME stdlib_string_demo
    COMMAND ${CMAKE_COMMAND} -E env bash -c
        "\"${string_demo_exe}\" | diff - \"${CMAKE_CURRENT_SOURCE_DIR}/expected_output.txt\"")
