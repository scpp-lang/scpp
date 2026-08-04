set(SCPP_STDLIB_STD_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/std.scppm")
set(SCPP_STDLIB_STD_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libstd.scppa")
set(SCPP_STDLIB_SCPP_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/scpp.scppm")
set(SCPP_STDLIB_SCPP_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libscpp.scppa")

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
# command to re-run (verified directly: touching src/ast.cppm alone
# triggers a rebuild of the `scpp` binary and therefore a re-run of this
# custom command). Only src/scpp.toml (the manifest describing *how*
# ast.cppm is packaged for the scpp-driven build below, e.g. its
# `sources`/`[dependencies]`) needs to be listed explicitly, since editing
# it alone (with no ast.cppm/compiler source change) wouldn't otherwise be
# noticed.

# This runs the just-built `scpp` compiler against its own real workspace
# root manifest (the repo-root scpp.toml, which declares members
# "libs/std", "libs/scpp", and "src" -- see the design doc's ch6
# "Workspace model" -- with no `default-members`, so a plain
# `scpp build --lib` builds all three). Earlier revisions of this command
# staged a throwaway copy of the workspace (via a private
# ${CMAKE_BINARY_DIR}/scpp-libs-workspace directory, symlinking each
# member in and copying scpp.toml over) purely to give `.scpp/build/`
# somewhere to land other than the real source tree; that indirection has
# been dropped in favor of running directly in ${CMAKE_SOURCE_DIR}, the
# real top-level source directory where the real scpp.toml already lives.
# Trade-off, explicitly accepted: `.scpp/build/` (already gitignored) is
# now written straight into the source tree and shared by every CMake
# binary directory built from this checkout -- e.g. a second, parallel
# build directory would reuse the same cache rather than getting its own
# isolated one -- in exchange for actually simplifying this command and
# matching the same day-to-day workflow a human contributor gets by
# running `scpp build --lib` from the repo root themselves.
add_custom_command(
    OUTPUT
        "${SCPP_STDLIB_STD_INTERFACE_PATH}"
        "${SCPP_STDLIB_STD_ARCHIVE_PATH}"
        "${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
        "${SCPP_STDLIB_SCPP_ARCHIVE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_SOURCE_DIR}" $<TARGET_FILE:scpp> build --lib
    COMMAND /bin/sh -c "set -eu; cp \"`find \"${CMAKE_SOURCE_DIR}/.scpp/build\" -path '*/dev/std/modules/std.scppm' | head -n 1`\" \"${SCPP_STDLIB_STD_INTERFACE_PATH}\"; cp \"`find \"${CMAKE_SOURCE_DIR}/.scpp/build\" -path '*/dev/std/archives/libstd.scppa' | head -n 1`\" \"${SCPP_STDLIB_STD_ARCHIVE_PATH}\"; cp \"`find \"${CMAKE_SOURCE_DIR}/.scpp/build\" -path '*/dev/scpp/modules/scpp.scppm' | head -n 1`\" \"${SCPP_STDLIB_SCPP_INTERFACE_PATH}\"; cp \"`find \"${CMAKE_SOURCE_DIR}/.scpp/build\" -path '*/dev/scpp/archives/libscpp.scppa' | head -n 1`\" \"${SCPP_STDLIB_SCPP_ARCHIVE_PATH}\""
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
