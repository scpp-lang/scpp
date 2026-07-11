# stdlib/CMakeLists.txt
#
# Builds the native helper libraries required by scpp's `std` module
# partitions. The standard library's behavioral verification lives in the
# project's real test suites (`tests/` and `blackbox_test/`), not in demo
# executables under `stdlib/`.

add_library(scpp_string_wrapper STATIC string/scpp_string_wrapper.cpp)
target_compile_features(scpp_string_wrapper PRIVATE cxx_std_23)

add_library(scpp_thread_wrapper STATIC thread/scpp_thread_wrapper.cpp)
target_compile_features(scpp_thread_wrapper PRIVATE cxx_std_23)

add_library(scpp_print_wrapper STATIC print/scpp_print_wrapper.cpp)
target_compile_features(scpp_print_wrapper PRIVATE cxx_std_23)

add_library(scpp_random_wrapper STATIC random/scpp_random_wrapper.cpp)
target_compile_features(scpp_random_wrapper PRIVATE cxx_std_23)

function(add_scpp_module_artifacts target_name source_path interface_path archive_path)
    get_filename_component(interface_dir "${interface_path}" DIRECTORY)
    add_custom_command(
        OUTPUT "${interface_path}" "${archive_path}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${interface_dir}"
        COMMAND $<TARGET_FILE:scpp> build-module "${source_path}" --interface-out "${interface_path}" --archive-out "${archive_path}"
        DEPENDS scpp ${ARGN}
        VERBATIM)
    add_custom_target(${target_name} ALL DEPENDS "${interface_path}" "${archive_path}")
endfunction()

set(SCPP_STDLIB_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/std.scppm")
set(SCPP_STDLIB_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libstd.scppa")
add_scpp_module_artifacts(scpp_std_artifacts
    "${CMAKE_CURRENT_SOURCE_DIR}/std.scpp"
    "${SCPP_STDLIB_INTERFACE_PATH}"
    "${SCPP_STDLIB_ARCHIVE_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/functional/std_functional.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/format/std_format.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/std_memory.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/print/std_print.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/random/std_random.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thread/std_thread.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/string/std_string.scpp")
