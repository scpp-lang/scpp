set(SCPP_STDLIB_STD_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/std.scppm")
set(SCPP_STDLIB_STD_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libstd.scppa")
set(SCPP_STDLIB_SCPP_INTERFACE_PATH "${CMAKE_CURRENT_BINARY_DIR}/scpp.scppm")
set(SCPP_STDLIB_SCPP_ARCHIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/libscpp.scppa")

set(SCPP_STDLIB_STAGE_WORKSPACE_DIR "${CMAKE_BINARY_DIR}/scpp-libs-workspace")

file(GLOB_RECURSE SCPP_STDLIB_BUILD_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/scpp.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/*.scpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scpp/*.cpp")

add_custom_command(
    OUTPUT
        "${SCPP_STDLIB_STD_INTERFACE_PATH}"
        "${SCPP_STDLIB_STD_ARCHIVE_PATH}"
        "${SCPP_STDLIB_SCPP_INTERFACE_PATH}"
        "${SCPP_STDLIB_SCPP_ARCHIVE_PATH}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/scpp.toml" "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/scpp.toml"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_CURRENT_SOURCE_DIR}/std" "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/std"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_CURRENT_SOURCE_DIR}/scpp" "${SCPP_STDLIB_STAGE_WORKSPACE_DIR}/scpp"
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
