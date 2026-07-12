# libs/CMakeLists.txt
#
# Builds the native helper libraries required by scpp's library modules and
# emits the compiled `std` / `scpp` module artifacts. Library behavior is
# verified in the project's real test suites (`tests/` and `blackbox_test/`),
# not in demo executables under `libs/`.

add_library(scpp_string_wrapper STATIC std/string/scpp_string_wrapper.cpp)
target_compile_features(scpp_string_wrapper PRIVATE cxx_std_23)

add_library(scpp_charconv_wrapper STATIC std/charconv/scpp_charconv_wrapper.cpp)
target_compile_features(scpp_charconv_wrapper PRIVATE cxx_std_23)

add_library(scpp_thread_wrapper STATIC std/thread/scpp_thread_wrapper.cpp)
target_compile_features(scpp_thread_wrapper PRIVATE cxx_std_23)

add_library(scpp_print_wrapper STATIC std/print/scpp_print_wrapper.cpp)
target_compile_features(scpp_print_wrapper PRIVATE cxx_std_23)

add_library(scpp_random_wrapper STATIC std/random/scpp_random_wrapper.cpp)
target_compile_features(scpp_random_wrapper PRIVATE cxx_std_23)

add_library(scpp_io_wrapper STATIC scpp/io/scpp_io_wrapper.cpp)
target_compile_features(scpp_io_wrapper PRIVATE cxx_std_23)

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

set(SCPP_STDLIB_STD_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/std.scppm")
set(SCPP_STDLIB_STD_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libstd.scppa")
add_scpp_module_artifacts(scpp_std_artifacts
    "${CMAKE_CURRENT_SOURCE_DIR}/std/std.scpp"
    "${SCPP_STDLIB_STD_INTERFACE_PATH}"
    "${SCPP_STDLIB_STD_ARCHIVE_PATH}"
    scpp_charconv_wrapper
    "${CMAKE_CURRENT_SOURCE_DIR}/std/charconv/std_charconv.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/cstdlib/std_cstdlib.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/expected/std_expected.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/functional/std_functional.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/format/std_format.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/memory/std_memory.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/print/std_print.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/random/std_random.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/thread/std_thread.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/string/std_string.scpp")

set(SCPP_STDLIB_SCPP_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/scpp.scppm")
set(SCPP_STDLIB_SCPP_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libscpp.scppa")
add_scpp_module_artifacts(scpp_scpp_artifacts
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/scpp.scpp"
    "${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
    "${SCPP_STDLIB_SCPP_ARCHIVE_PATH}"
    scpp_std_artifacts
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/scpp.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/io/scpp_io.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/rand/scpp_rand.scpp")
