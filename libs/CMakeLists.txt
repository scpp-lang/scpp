set(SCPP_STDLIB_STD_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/std.scppm")
set(SCPP_STDLIB_STD_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libstd.scppa")
set(SCPP_STDLIB_SCPP_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/scpp.scppm")
set(SCPP_STDLIB_SCPP_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libscpp.scppa")

set(SCPP_STDLIB_STAGE_WORKSPACE_DIR "${CMAKE_BINARY_DIR}/scpp-libs-workspace")

file(GLOB_RECURSE SCPP_STDLIB_BUILD_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/*.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/*.cpp")

# The real workspace root (repo-root scpp.toml) declares three members --
# "libs/std", "libs/scpp", and "src" (see the design doc's ch6 "Workspace
# model") -- but this staged, CMake-driven build only ever wants the two
# stdlib packages: "src" (this compiler's own self-hosting-probe C++/scpp
# sources) is built directly by the bootstrap C++ compiler via the
# top-level CMakeLists.txt, not through the scpp package manager. Copying
# the real root manifest verbatim means its `members` list still names all
# three, and workspace loading eagerly validates every declared member has
# a real scpp.toml (see WorkspaceInfo::load_workspace in src/project.cppm)
# regardless of what actually gets built -- so "src" must still be staged
# here (symlinked, like "libs/std"/"libs/scpp" below) purely to satisfy
# that validation. It is never selected for compilation: the root
# manifest's `default-members` is pinned to exactly the two stdlib
# packages, and the plain `scpp build --lib` invoked below (no `-p`/
# `--workspace`) only ever builds default-members, so "src" is validated
# but not compiled here, keeping this stdlib-only artifact staging
# identical in scope to before.
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
