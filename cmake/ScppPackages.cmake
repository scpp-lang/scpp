# Build rules for scpp-language packages, driven by an scpp compiler through
# its clang-like command line (`scpp build-module`, `scpp -c`, `scpp <objects>`).
#
# Nothing here is opaque: every module interface, every implementation
# partition, every native object, every package archive and every link is its
# own custom command with real OUTPUT/DEPENDS, so the generator can build them
# in parallel, print per-file progress and rebuild only what a change affects.
#
#   scpp_add_package(NAME <name> SOURCES <module sources...> [ALL]
#                    [DEPENDS <package>...] [NATIVE_OBJECTS <object-library>...]
#                    [LINK_LIBRARIES <link input>...]
#                    [COMPILER <target|path>] [OUTPUT_DIRECTORY <dir>])
#       Builds every module declared by SOURCES. Per primary module `M`:
#         <out>/M.scppm       the interface, from `scpp build-module` over the
#                             primary unit and the interface partitions it
#                             imports; rewritten only when its content changes,
#                             so what imports `M` is rebuilt only then
#         <obj>/M.P.scppo     one object per implementation partition `M:P`,
#                             from `scpp -c`
#         <out>/libM.scppa    the module archive: the object `build-module`
#                             emitted, plus the partition and native objects
#       `libM.scppa` sits next to `M.scppm`, which is where the compiler looks
#       for a prebuilt module's companion archive. Target scpp_pkg_<name> builds
#       all of it.
#       DEPENDS names the packages whose modules the sources may import: a
#       module of another package is visible only when that package is listed
#       directly, not merely reachable through a package that is. LINK_LIBRARIES
#       (`-lm`, or the path of an archive) go on the link line of every
#       executable that links the package, directly or through other packages.
#
#   scpp_add_executable(NAME <name> SOURCES <files...> OUTPUT <path> [ALL]
#                       [DEPENDS <package>...] [LINK_LIBRARIES <link input>...]
#                       [COMPILER <target|path>] [COMPILE_FLAGS <flags...>])
#       Compiles each plain source against the modules it imports and links
#       the objects with those modules' archives. DEPENDS works as above.
#
# What a source imports is read from its own `import`/`module` declarations
# when CMake configures, so re-run CMake after changing those lines. A module
# no package of the project provides must be named in
# SCPP_PACKAGES_PREBUILT_MODULES: it is then left to the compiler to find, as it
# does the stdlib it ships next to itself (link its archives via LINK_LIBRARIES).

include_guard(GLOBAL)

# The compiler used unless a call names one: an executable target of this
# project, or the path of a prebuilt scpp.
if(NOT DEFINED SCPP_PACKAGES_COMPILER)
    set(SCPP_PACKAGES_COMPILER "")
endif()
# Flags for every compile of a package.
if(NOT DEFINED SCPP_PACKAGES_COMPILE_FLAGS)
    set(SCPP_PACKAGES_COMPILE_FLAGS -O0)
endif()
# Where intermediates go, and package outputs unless a call says otherwise.
if(NOT DEFINED SCPP_PACKAGES_OUTPUT_ROOT)
    set(SCPP_PACKAGES_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/scpp-packages")
endif()
# Modules the compiler resolves itself (the prebuilt stdlib next to it) that
# no package of this project builds.
if(NOT DEFINED SCPP_PACKAGES_PREBUILT_MODULES)
    set(SCPP_PACKAGES_PREBUILT_MODULES "")
endif()
# Archives are merged with the same `ar` that CMake would use for a static
# library, which a project without any language enabled does not know.
if(NOT CMAKE_AR)
    find_program(SCPP_PACKAGES_AR ar REQUIRED)
    set(CMAKE_AR "${SCPP_PACKAGES_AR}")
endif()

# Reads one source file's module declaration and imports into the caller's
# scope: <prefix>_KIND (PRIMARY, INTERFACE_PARTITION, IMPLEMENTATION_PARTITION,
# IMPLEMENTATION_UNIT or PLAIN), <prefix>_MODULE, <prefix>_PARTITION,
# <prefix>_IMPORTS (imported modules) and <prefix>_PARTITION_IMPORTS (the
# partitions of its own module it imports, without the leading colon).
function(_scpp_scan_source file prefix)
    file(STRINGS "${file}" lines
        REGEX "^(export[ \t]+)?(module|import)[ \t]+[A-Za-z_:][A-Za-z0-9_.:]*[ \t]*;")
    set(kind PLAIN)
    set(module "")
    set(partition "")
    set(imports "")
    set(partition_imports "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^(export[ \t]+)?module[ \t]+([A-Za-z_][A-Za-z0-9_.]*)(:([A-Za-z_][A-Za-z0-9_]*))?[ \t]*;")
            if(NOT kind STREQUAL "PLAIN")
                continue()
            endif()
            set(module "${CMAKE_MATCH_2}")
            set(partition "${CMAKE_MATCH_4}")
            if(NOT "${CMAKE_MATCH_1}" STREQUAL "")
                if(NOT partition STREQUAL "")
                    set(kind INTERFACE_PARTITION)
                else()
                    set(kind PRIMARY)
                endif()
            elseif(NOT partition STREQUAL "")
                set(kind IMPLEMENTATION_PARTITION)
            else()
                set(kind IMPLEMENTATION_UNIT)
            endif()
        elseif(line MATCHES "^(export[ \t]+)?import[ \t]+([A-Za-z_:][A-Za-z0-9_.:]*)[ \t]*;")
            set(imported "${CMAKE_MATCH_2}")
            if(imported MATCHES "^:(.+)$")
                list(APPEND partition_imports "${CMAKE_MATCH_1}")
            else()
                list(APPEND imports "${imported}")
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES imports)
    list(REMOVE_DUPLICATES partition_imports)
    set(${prefix}_KIND "${kind}" PARENT_SCOPE)
    set(${prefix}_MODULE "${module}" PARENT_SCOPE)
    set(${prefix}_PARTITION "${partition}" PARENT_SCOPE)
    set(${prefix}_IMPORTS "${imports}" PARENT_SCOPE)
    set(${prefix}_PARTITION_IMPORTS "${partition_imports}" PARENT_SCOPE)
endfunction()

# The modules a source file imports.
function(scpp_source_imports file out_var)
    _scpp_scan_source("${file}" scan)
    set(${out_var} "${scan_IMPORTS}" PARENT_SCOPE)
endfunction()

function(_scpp_module_property module property out_var)
    get_property(value GLOBAL PROPERTY "SCPP_MODULE:${module}:${property}")
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

# Every module reachable from <modules...> through imports, importers before
# the modules they import -- the order a static link wants archives in.
function(scpp_module_closure out_var)
    set(visited "")
    set(post_order "")
    foreach(root IN LISTS ARGN)
        set(stack "visit:${root}")
        list(LENGTH stack depth)
        while(depth GREATER 0)
            list(POP_FRONT stack entry)
            if(entry MATCHES "^emit:(.*)$")
                list(APPEND post_order "${CMAKE_MATCH_1}")
            elseif(entry MATCHES "^visit:(.*)$")
                set(module "${CMAKE_MATCH_1}")
                if(NOT module IN_LIST visited AND NOT module IN_LIST SCPP_PACKAGES_PREBUILT_MODULES)
                    get_property(known GLOBAL PROPERTY "SCPP_MODULE:${module}:INTERFACE" SET)
                    if(NOT known)
                        message(FATAL_ERROR
                            "scpp: module '${module}' is imported but no scpp package provides it "
                            "(a package must be declared after the packages it imports from)")
                    endif()
                    list(APPEND visited "${module}")
                    _scpp_module_property("${module}" IMPORTS imports)
                    set(next "")
                    foreach(dependency IN LISTS imports)
                        list(APPEND next "visit:${dependency}")
                    endforeach()
                    list(PREPEND stack ${next} "emit:${module}")
                endif()
            endif()
            list(LENGTH stack depth)
        endwhile()
    endforeach()
    list(REVERSE post_order)
    set(${out_var} "${post_order}" PARENT_SCOPE)
endfunction()

# The `--import name=path` arguments for <modules...> as <args_var>, and the
# interface files they name as <files_var>.
function(scpp_module_import_arguments args_var files_var)
    set(args "")
    set(files "")
    foreach(module IN LISTS ARGN)
        _scpp_module_property("${module}" INTERFACE interface)
        list(APPEND args --import "${module}=${interface}")
        list(APPEND files "${interface}")
    endforeach()
    set(${args_var} "${args}" PARENT_SCOPE)
    set(${files_var} "${files}" PARENT_SCOPE)
endfunction()

# The archives of <modules...>, in the same order.
function(scpp_module_archives out_var)
    set(archives "")
    foreach(module IN LISTS ARGN)
        _scpp_module_property("${module}" ARCHIVE archive)
        list(APPEND archives "${archive}")
    endforeach()
    set(${out_var} "${archives}" PARENT_SCOPE)
endfunction()

# The link inputs the packages of <modules...> ask for, without repeats.
function(scpp_module_link_libraries out_var)
    set(libraries "")
    foreach(module IN LISTS ARGN)
        _scpp_module_property("${module}" LINK_LIBRARIES module_libraries)
        list(APPEND libraries ${module_libraries})
    endforeach()
    list(REMOVE_DUPLICATES libraries)
    set(${out_var} "${libraries}" PARENT_SCOPE)
endfunction()

# Rejects what `consumer` (a package or executable whose own modules are
# <own_modules>) imports from <file> unless it may see it: its own modules,
# the modules of <dependencies> (packages named directly) and prebuilt modules.
function(_scpp_check_imports consumer file own_modules dependencies imports)
    foreach(module IN LISTS imports)
        if(module IN_LIST own_modules OR module IN_LIST SCPP_PACKAGES_PREBUILT_MODULES)
            continue()
        endif()
        get_property(owner GLOBAL PROPERTY "SCPP_MODULE:${module}:PACKAGE")
        if(owner STREQUAL "")
            message(FATAL_ERROR
                "scpp: ${consumer}: ${file} imports module '${module}', which no scpp package provides "
                "(declare its package before this one, or list the module in SCPP_PACKAGES_PREBUILT_MODULES "
                "if the compiler provides it)")
        elseif(NOT owner IN_LIST dependencies)
            message(FATAL_ERROR
                "scpp: ${consumer}: ${file} imports module '${module}', which is exported only by package "
                "'${owner}', not a direct dependency; add it to DEPENDS to import it")
        endif()
    endforeach()
endfunction()

# Resolves <dependencies...> to declared packages.
function(_scpp_check_dependencies consumer)
    foreach(dependency IN LISTS ARGN)
        if(NOT TARGET scpp_pkg_${dependency})
            message(FATAL_ERROR
                "scpp: ${consumer}: DEPENDS names '${dependency}', which is not a declared scpp package "
                "(declare it first)")
        endif()
    endforeach()
endfunction()

# The partitions of `module` reachable from <partitions...> through partition
# imports. Reads the partition table that scpp_add_package keeps in its
# `part_partitions_<module>__<partition>` variables.
function(_scpp_partition_closure out_var module)
    set(pending ${ARGN})
    set(reached "")
    list(LENGTH pending remaining)
    while(remaining GREATER 0)
        list(POP_FRONT pending partition)
        if(NOT partition IN_LIST reached)
            if(NOT DEFINED part_file_${module}__${partition})
                message(FATAL_ERROR
                    "scpp: partition '${module}:${partition}' is imported but is not among the package's sources")
            endif()
            list(APPEND reached "${partition}")
            list(APPEND pending ${part_partitions_${module}__${partition}})
        endif()
        list(LENGTH pending remaining)
    endwhile()
    set(${out_var} "${reached}" PARENT_SCOPE)
endfunction()

function(scpp_add_package)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "ALL" "NAME;COMPILER;OUTPUT_DIRECTORY"
        "SOURCES;DEPENDS;NATIVE_OBJECTS;LINK_LIBRARIES")
    if(NOT ARG_NAME OR NOT ARG_SOURCES OR ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "scpp_add_package: expected NAME and SOURCES (unparsed: ${ARG_UNPARSED_ARGUMENTS})")
    endif()
    if(TARGET scpp_pkg_${ARG_NAME})
        message(FATAL_ERROR "scpp package ${ARG_NAME} is declared twice")
    endif()
    _scpp_check_dependencies("scpp package ${ARG_NAME}" ${ARG_DEPENDS})
    if(NOT ARG_COMPILER)
        set(ARG_COMPILER "${SCPP_PACKAGES_COMPILER}")
    endif()
    if(NOT ARG_COMPILER)
        message(FATAL_ERROR "scpp_add_package(${ARG_NAME}): no COMPILER and no SCPP_PACKAGES_COMPILER")
    endif()
    set(object_dir "${SCPP_PACKAGES_OUTPUT_ROOT}/${ARG_NAME}")
    if(NOT ARG_OUTPUT_DIRECTORY)
        set(ARG_OUTPUT_DIRECTORY "${object_dir}")
    endif()
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIRECTORY}" "${object_dir}")

    # Sort the sources into primary interface units and per-module partitions.
    set(primaries "")
    foreach(source IN LISTS ARG_SOURCES)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE
            OUTPUT_VARIABLE source_path)
        _scpp_scan_source("${source_path}" scan)
        if(scan_KIND STREQUAL "PLAIN")
            message(FATAL_ERROR "scpp package ${ARG_NAME}: ${source_path} declares no module")
        elseif(scan_KIND STREQUAL "IMPLEMENTATION_UNIT")
            message(FATAL_ERROR
                "scpp package ${ARG_NAME}: ${source_path}: module implementation units are not supported")
        elseif(scan_KIND STREQUAL "PRIMARY")
            if(DEFINED primary_file_${scan_MODULE})
                message(FATAL_ERROR "scpp package ${ARG_NAME}: module '${scan_MODULE}' has two primary interface units")
            endif()
            list(APPEND primaries "${scan_MODULE}")
            set(primary_file_${scan_MODULE} "${source_path}")
            set(primary_partitions_${scan_MODULE} "${scan_PARTITION_IMPORTS}")
            set(primary_imports_${scan_MODULE} "${scan_IMPORTS}")
        else()
            set(key "${scan_MODULE}__${scan_PARTITION}")
            if(DEFINED part_file_${key})
                message(FATAL_ERROR
                    "scpp package ${ARG_NAME}: partition '${scan_MODULE}:${scan_PARTITION}' is declared twice")
            endif()
            set(part_file_${key} "${source_path}")
            set(part_kind_${key} "${scan_KIND}")
            set(part_partitions_${key} "${scan_PARTITION_IMPORTS}")
            set(part_imports_${key} "${scan_IMPORTS}")
            list(APPEND partitions_of_${scan_MODULE} "${scan_PARTITION}")
        endif()
    endforeach()
    if(NOT primaries)
        message(FATAL_ERROR "scpp package ${ARG_NAME}: no primary module interface unit among the sources")
    endif()
    list(LENGTH primaries primary_count)
    if(ARG_NATIVE_OBJECTS AND NOT primary_count EQUAL 1)
        message(FATAL_ERROR
            "scpp package ${ARG_NAME}: NATIVE_OBJECTS need exactly one primary module to be merged into")
    endif()

    # Register every module before creating any rule: the modules of a
    # package import each other.
    foreach(module IN LISTS primaries)
        foreach(partition IN LISTS partitions_of_${module})
            if(NOT DEFINED primary_file_${module})
                message(FATAL_ERROR "scpp package ${ARG_NAME}: partition '${module}:${partition}' has no primary module")
            endif()
        endforeach()
        # The primary unit and the partitions it imports make up the interface.
        _scpp_partition_closure(interface_partitions "${module}" ${primary_partitions_${module}})
        set(interface_files "")
        set(imports ${primary_imports_${module}})
        foreach(partition IN LISTS interface_partitions)
            list(APPEND interface_files "${part_file_${module}__${partition}}")
            list(APPEND imports ${part_imports_${module}__${partition}})
        endforeach()
        list(REMOVE_DUPLICATES imports)
        set(interface_files_${module} "${interface_files}")
        set(interface_imports_${module} "${imports}")

        get_property(registered GLOBAL PROPERTY "SCPP_MODULE:${module}:INTERFACE" SET)
        if(registered)
            message(FATAL_ERROR "scpp package ${ARG_NAME}: module '${module}' is already provided by another package")
        endif()
        set_property(GLOBAL PROPERTY "SCPP_MODULE:${module}:PACKAGE" "${ARG_NAME}")
        set_property(GLOBAL PROPERTY "SCPP_MODULE:${module}:INTERFACE" "${ARG_OUTPUT_DIRECTORY}/${module}.scppm")
        set_property(GLOBAL PROPERTY "SCPP_MODULE:${module}:ARCHIVE" "${ARG_OUTPUT_DIRECTORY}/lib${module}.scppa")
        set_property(GLOBAL PROPERTY "SCPP_MODULE:${module}:IMPORTS" "${imports}")
        set_property(GLOBAL PROPERTY "SCPP_MODULE:${module}:LINK_LIBRARIES" "${ARG_LINK_LIBRARIES}")
    endforeach()

    # What the package's sources import has to be visible to it.
    foreach(module IN LISTS primaries)
        _scpp_check_imports("scpp package ${ARG_NAME}" "${primary_file_${module}}" "${primaries}"
            "${ARG_DEPENDS}" "${primary_imports_${module}}")
        foreach(partition IN LISTS partitions_of_${module})
            _scpp_check_imports("scpp package ${ARG_NAME}" "${part_file_${module}__${partition}}" "${primaries}"
                "${ARG_DEPENDS}" "${part_imports_${module}__${partition}}")
        endforeach()
    endforeach()

    set(outputs "")
    foreach(module IN LISTS primaries)
        set(interface "${ARG_OUTPUT_DIRECTORY}/${module}.scppm")
        set(archive "${ARG_OUTPUT_DIRECTORY}/lib${module}.scppa")
        list(APPEND outputs "${interface}" "${archive}")

        # One object per implementation partition, independent of the others.
        set(partition_objects "")
        foreach(partition IN LISTS partitions_of_${module})
            if(NOT part_kind_${module}__${partition} STREQUAL "IMPLEMENTATION_PARTITION")
                continue()
            endif()
            set(partition_file "${part_file_${module}__${partition}}")
            _scpp_partition_closure(imported_partitions "${module}" ${part_partitions_${module}__${partition}})
            set(partition_sources "")
            set(imports ${part_imports_${module}__${partition}})
            foreach(imported IN LISTS imported_partitions)
                list(APPEND partition_sources "${part_file_${module}__${imported}}")
                list(APPEND imports ${part_imports_${module}__${imported}})
            endforeach()
            list(REMOVE_DUPLICATES imports)
            scpp_module_closure(closure ${imports})
            scpp_module_import_arguments(import_args import_files ${closure})

            set(object "${object_dir}/${module}.${partition}.scppo")
            add_custom_command(
                OUTPUT "${object}"
                COMMAND "${ARG_COMPILER}" -c ${SCPP_PACKAGES_COMPILE_FLAGS}
                        "${partition_file}" ${partition_sources} -o "${object}" ${import_args}
                DEPENDS "${ARG_COMPILER}" "${partition_file}" ${partition_sources} ${import_files}
                COMMENT "Compiling scpp partition ${module}:${partition}"
                VERBATIM)
            list(APPEND partition_objects "${object}")
        endforeach()

        set(extra_objects ${partition_objects})
        foreach(native IN LISTS ARG_NATIVE_OBJECTS)
            list(APPEND extra_objects "$<TARGET_OBJECTS:${native}>")
        endforeach()

        # `build-module` writes the archive itself; it only needs merging into
        # a final one when there are further objects for it.
        if(extra_objects)
            set(module_archive "${object_dir}/${module}.primary.scppa")
        else()
            set(module_archive "${archive}")
        endif()
        scpp_module_closure(closure ${interface_imports_${module}})
        scpp_module_import_arguments(import_args import_files ${closure})
        # `build-module` writes the interface to a staging file that is copied
        # over the real one only when it differs, so an importer whose imported
        # interface did not change is not rebuilt (the generator re-stats
        # custom command outputs).
        set(staged_interface "${object_dir}/${module}.staged.scppm")
        add_custom_command(
            OUTPUT "${interface}" "${module_archive}"
            BYPRODUCTS "${staged_interface}"
            COMMAND "${ARG_COMPILER}" build-module ${SCPP_PACKAGES_COMPILE_FLAGS}
                    "${primary_file_${module}}" ${interface_files_${module}}
                    --interface-out "${staged_interface}" --archive-out "${module_archive}" ${import_args}
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${staged_interface}" "${interface}"
            DEPENDS "${ARG_COMPILER}" "${primary_file_${module}}" ${interface_files_${module}} ${import_files}
            COMMENT "Building scpp module ${module}"
            VERBATIM)
        if(extra_objects)
            add_custom_command(
                OUTPUT "${archive}"
                COMMAND "${CMAKE_COMMAND}" -E copy "${module_archive}" "${archive}"
                COMMAND "${CMAKE_AR}" rcs "${archive}" ${extra_objects}
                DEPENDS "${module_archive}" ${extra_objects}
                COMMENT "Archiving scpp module ${module}"
                COMMAND_EXPAND_LISTS
                VERBATIM)
        endif()
    endforeach()

    if(ARG_ALL)
        add_custom_target(scpp_pkg_${ARG_NAME} ALL DEPENDS ${outputs})
    else()
        add_custom_target(scpp_pkg_${ARG_NAME} DEPENDS ${outputs})
    endif()
    set_property(GLOBAL APPEND PROPERTY SCPP_PACKAGE_TARGETS scpp_pkg_${ARG_NAME})
endfunction()

function(scpp_add_executable)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "ALL" "NAME;COMPILER;OUTPUT"
        "SOURCES;DEPENDS;LINK_LIBRARIES;COMPILE_FLAGS")
    if(NOT ARG_NAME OR NOT ARG_SOURCES OR NOT ARG_OUTPUT OR ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "scpp_add_executable: expected NAME, SOURCES and OUTPUT (unparsed: ${ARG_UNPARSED_ARGUMENTS})")
    endif()
    _scpp_check_dependencies("scpp executable ${ARG_NAME}" ${ARG_DEPENDS})
    if(NOT ARG_COMPILER)
        set(ARG_COMPILER "${SCPP_PACKAGES_COMPILER}")
    endif()
    if(NOT ARG_COMPILE_FLAGS)
        set(ARG_COMPILE_FLAGS ${SCPP_PACKAGES_COMPILE_FLAGS})
    endif()
    cmake_path(ABSOLUTE_PATH ARG_OUTPUT BASE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" NORMALIZE)
    if(ARG_OUTPUT STREQUAL "${CMAKE_CURRENT_BINARY_DIR}/${ARG_NAME}")
        message(FATAL_ERROR
            "scpp executable ${ARG_NAME}: OUTPUT is the path the generator names the target after; "
            "put the executable somewhere else (e.g. bin/${ARG_NAME})")
    endif()

    # One object per source; what a source imports is compiled against.
    set(objects "")
    set(all_imports "")
    set(index 0)
    foreach(source IN LISTS ARG_SOURCES)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE
            OUTPUT_VARIABLE source_path)
        scpp_source_imports("${source_path}" imports)
        _scpp_check_imports("scpp executable ${ARG_NAME}" "${source_path}" "" "${ARG_DEPENDS}" "${imports}")
        list(APPEND all_imports ${imports})
        scpp_module_closure(closure ${imports})
        scpp_module_import_arguments(import_args import_files ${closure})

        set(object "${SCPP_PACKAGES_OUTPUT_ROOT}/${ARG_NAME}.${index}.o")
        cmake_path(GET source_path FILENAME source_name)
        add_custom_command(
            OUTPUT "${object}"
            COMMAND "${ARG_COMPILER}" -c ${ARG_COMPILE_FLAGS} "${source_path}" -o "${object}" ${import_args}
            DEPENDS "${ARG_COMPILER}" "${source_path}" ${import_files}
            COMMENT "Compiling scpp ${ARG_NAME}: ${source_name}"
            VERBATIM)
        list(APPEND objects "${object}")
        math(EXPR index "${index} + 1")
    endforeach()

    scpp_module_closure(link_closure ${all_imports})
    scpp_module_archives(archives ${link_closure})
    scpp_module_link_libraries(libraries ${link_closure})
    list(APPEND libraries ${ARG_LINK_LIBRARIES})
    list(REMOVE_DUPLICATES libraries)
    set(link_arguments "")
    foreach(link_input IN LISTS archives libraries)
        list(APPEND link_arguments --link "${link_input}")
    endforeach()

    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND "${ARG_COMPILER}" ${objects} ${link_arguments} -o "${ARG_OUTPUT}"
        DEPENDS "${ARG_COMPILER}" ${objects} ${archives}
        COMMENT "Linking scpp executable ${ARG_NAME}"
        VERBATIM)
    if(ARG_ALL)
        add_custom_target(${ARG_NAME} ALL DEPENDS "${ARG_OUTPUT}")
    else()
        add_custom_target(${ARG_NAME} DEPENDS "${ARG_OUTPUT}")
    endif()
endfunction()
