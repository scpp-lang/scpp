set(SCPP_STDLIB_STD_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/std.scppm")
set(SCPP_STDLIB_STD_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libstd.scppa")
set(SCPP_STDLIB_SCPP_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/scpp.scppm")
set(SCPP_STDLIB_SCPP_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libscpp.scppa")

set(SCPP_STDLIB_STAGE_WORKSPACE_DIR "${CMAKE_BINARY_DIR}/scpp-libs-workspace")

file(GLOB_RECURSE SCPP_STDLIB_BUILD_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/*.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/*.cpp")
# Note: src/ast.cppm itself is *not* globbed here -- it's already a real
# source of the scpp_ast CMake target (see the top-level CMakeLists.txt),
# so any edit to it already invalidates the `scpp` target this command's
# own DEPENDS below already lists, which in turn already forces this
# command to re-run. Only src/scpp.toml (the manifest describing *how*
# ast.cppm is packaged for the scpp-driven build below, e.g. its
# `sources`/`[dependencies]`) needs to be listed explicitly, since editing
# it alone (with no ast.cppm/compiler source change) wouldn't otherwise be
# noticed.

# The real workspace root (repo-root scpp.toml) declares three members --
# "libs/std", "libs/scpp", and "src" (see the design doc's ch6 "Workspace
# model"). The root manifest has no `default-members` (a virtual workspace
# with that key omitted defaults to *every* declared member -- see
# resolve_workspace in src/project.cppm), so the plain `scpp build --lib`
# invoked below (no `-p`/`--workspace`) builds all three packages,
# including "src"'s ast.cppm self-hosting target, every time this custom
# command runs. That means a self-hosting regression in ast.cppm now fails
# this custom command (and therefore the whole `cmake --build`), the same
# way a real std/scpp stdlib build failure always has -- this is
# intentional: it turns the self-hosting probe into a continuously
# enforced part of the ordinary CMake build rather than a separately-run
# check. "src" is symlinked into the staged workspace below (alongside
# "libs/std"/"libs/scpp") so its declared member path resolves and its
# ast.cppm sources are reachable from the staging tree; its build products
# (scpp.ast.scppm / libscpp-compiler.scppa) aren't copied out to a stable
# path below like std/scpp's are, since nothing else in this build
# consumes them yet -- only actually building (and thus validating) "src"
# is needed here, not exposing its artifacts elsewhere. Note this build
# still only produces the stdlib's own std/scpp artifacts as tracked
# CMake OUTPUTs (below); adding ast.cppm's own artifacts as tracked
# outputs is left for whenever something else in the build actually needs
# to consume them.
add_custom_command(
    OUTPUT
        "${SCPP_STDLIB_STD_INTERFACE_PATH}"
        "${SCPP_STDLIB_STD_ARCHIVE_PATH}"
        "${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
        "${SCPP_STDLIB_SCPP_ARCHIVE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/libs"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../scpp.toml" "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/scpp.toml"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_CURRENT_SOURCE_DIR}/std" "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/libs/std"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_CURRENT_SOURCE_DIR}/scpp" "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/libs/scpp"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/src"
    COMMAND ${CMAKE_COMMAND} -E chdir "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}" $<TARGET_FILE:scpp> build --lib
    COMMAND /bin/sh -c "set -eu; cp \"`find \"${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/.scpp/build\" -path '*/dev/std/modules/std.scppm' | head -n 1`\" \"${SCPP_STDLIB_STD_INTERFACE_PATH}\"; cp \"`find \"${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/.scpp/build\" -path '*/dev/std/archives/libstd.scppa' | head -n 1`\" \"${SCPP_STDLIB_STD_ARCHIVE_PATH}\"; cp \"`find \"${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/.scpp/build\" -path '*/dev/scpp/modules/scpp.scppm' | head -n 1`\" \"${SCPP_STDLIB_SCPP_INTERFACE_PATH}\"; cp \"`find \"${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/.scpp/build\" -path '*/dev/scpp/archives/libscpp.scppa' | head -n 1`\" \"${SCPP_STDLIB_SCPP_ARCHIVE_PATH}\""
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS
        scpp
        ${SCPP_STDLIB_BUILD_INPUTS}
    VERBATIM)

add_custom_target(scpp_std_artifacts ALL
    DEPENDS
        "${SCPP_STDLIB_STD_INTERFACE_PATH}"
        "${SCPP_STDLIB_STD_ARCHIVE_PATH}")

add_custom_target(scpp_scpp_artifacts ALL
    DEPENDS
        "${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
        "${SCPP_STDLIB_SCPP_ARCHIVE_PATH}")
