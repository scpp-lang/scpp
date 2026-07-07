# stdlib/CMakeLists.txt
#
# Builds the native helper libraries required by scpp's `std` module
# partitions. The standard library's behavioral verification lives in the
# project's real test suites (`tests/` and `blackbox_test/`), not in demo
# executables under `stdlib/`.

add_library(scpp_string_wrapper STATIC string/scpp_string_wrapper.cpp)
target_compile_features(scpp_string_wrapper PRIVATE cxx_std_23)
