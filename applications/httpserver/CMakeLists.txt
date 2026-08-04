# httpserver is now a real workspace member (applications/httpserver/
# scpp.toml, a [[bin]] package) built by the same unified
# scpp_workspace_artifacts `scpp build` step as libs/std, libs/scpp, and
# src -- see the top-level CMakeLists.txt. The bespoke single-file
# `$<TARGET_FILE:scpp> main.scpp -o ... --import httpserver=...` custom
# target that used to live here is gone; that ad-hoc CLI invocation is
# exactly what adding a proper manifest replaces.
#
# SCPP_HTTPSERVER_MODULE_PATH is still needed below: httpserver_test does
# its own independent, on-the-fly compile of a generated main.scpp (see
# tests/httpserver_test.cpp) via the same single-file CLI form, entirely
# decoupled from scpp.toml/scpp build -- unaffected by the above.
set(SCPP_HTTPSERVER_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/httpserver.scpp")

# The old SCPP_HTTPSERVER_BUILD_STATIC CMake option (default OFF, never
# referenced by CI or docs) is gone too: scpp's manifest-driven
# `[profile.*]` system (which used to gate static linking behind an
# explicit, opt-in profile choice) was removed entirely as underdesigned.
# Static linking is now unconditional for every manifest-driven build
# (see link_executable in src/driver.cppm), so the httpserver binary
# scpp_workspace_artifacts already produces via the ordinary `scpp build`
# step below is a fully static binary with no special profile/flag
# needed -- there is nothing left to opt into.
#
# Convenience copy: land the workspace-built httpserver binary at a
# stable, easy-to-find path for manual runs, same path
# (${CMAKE_CURRENT_BINARY_DIR}/httpserver_app_bin) the old custom target
# used, so nothing that relied on that exact path breaks.
set(SCPP_HTTPSERVER_BINARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/httpserver_app_bin")
add_custom_target(httpserver_app_artifact ALL
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/.scpp/build/${LLVM_HOST_TRIPLE}/httpserver/httpserver"
        "${SCPP_HTTPSERVER_BINARY_PATH}"
    BYPRODUCTS "${SCPP_HTTPSERVER_BINARY_PATH}"
    DEPENDS scpp_workspace_artifacts
    VERBATIM)

add_executable(httpserver_test tests/httpserver_test.cpp)
target_compile_features(httpserver_test PRIVATE cxx_std_26)
target_compile_definitions(httpserver_test PRIVATE
    SCPP_BINARY_PATH="$<TARGET_FILE:scpp>"
    SCPP_HTTPSERVER_MODULE_PATH="${SCPP_HTTPSERVER_MODULE_PATH}"
    SCPP_HTTPSERVER_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    SCPP_HTTPSERVER_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-work")
add_test(NAME httpserver_test COMMAND httpserver_test)
