# Manifest-driven module autoloader.

include(cmake/tools.cmake)

# A module that declares [embed] needs an interpreter to generate its bundle
# translation unit at build time. Resolve it once here rather than per module.
find_package(Python3 COMPONENTS Interpreter QUIET)

# Compiles every .luau under LUAU_SOURCE_DIR into one generated translation unit
# and adds it to TARGET_NAME. The generated file lives only in the build tree, so
# the .luau sources stay the single source of truth for the embedded bytes and
# for the hashes recorded alongside them.
function(cpp_embed_luau_sources TARGET_NAME LUAU_SOURCE_DIR GENERATED_DIR VERSION LABEL)
    if(NOT IS_DIRECTORY "${LUAU_SOURCE_DIR}")
        message(FATAL_ERROR
            "[Embed] ${TARGET_NAME}: [embed].luau_directory does not exist: ${LUAU_SOURCE_DIR}"
        )
    endif()

    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR
            "[Embed] ${TARGET_NAME}: embedding .luau sources requires a Python 3 interpreter."
        )
    endif()

    # This repository's own script, resolved against this file rather than
    # CMAKE_SOURCE_DIR: under add_subdirectory the latter is the consuming
    # project's root, which cannot answer for it.
    cmake_path(SET EMBED_SCRIPT NORMALIZE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/embed_luau.py"
    )
    set(GENERATED_FILE "${GENERATED_DIR}/luau-bundle.generated.cpp")
    set(INPUT_LIST_FILE "${GENERATED_DIR}/luau-bundle.inputs.txt")

    # CONFIGURE_DEPENDS re-runs this glob at build time, so adding or deleting a
    # .luau source triggers a reconfigure before the build continues.
    file(GLOB_RECURSE LUAU_SOURCES CONFIGURE_DEPENDS "${LUAU_SOURCE_DIR}/*.luau")

    # Editing a source is caught by its own entry in DEPENDS. Adding or deleting
    # one is NOT: after the reconfigure above, the generated file is still newer
    # than every remaining input, so the build system would consider it current
    # and silently keep a stale bundle. Record the input set in a file that is
    # rewritten only when that set changes, and depend on it: its timestamp then
    # moves exactly when a source appears or disappears.
    list(JOIN LUAU_SOURCES "\n" INPUT_LIST)
    string(APPEND INPUT_LIST "\n")
    set(PREVIOUS_INPUT_LIST "")
    if(EXISTS "${INPUT_LIST_FILE}")
        file(READ "${INPUT_LIST_FILE}" PREVIOUS_INPUT_LIST)
    endif()
    if(NOT PREVIOUS_INPUT_LIST STREQUAL INPUT_LIST)
        file(WRITE "${INPUT_LIST_FILE}" "${INPUT_LIST}")
    endif()

    add_custom_command(
        OUTPUT "${GENERATED_FILE}"
        COMMAND ${Python3_EXECUTABLE} "${EMBED_SCRIPT}"
            --source-dir "${LUAU_SOURCE_DIR}"
            --output "${GENERATED_FILE}"
            --version "${VERSION}"
            --label "${LABEL}"
        DEPENDS "${EMBED_SCRIPT}" "${INPUT_LIST_FILE}" ${LUAU_SOURCES}
        COMMENT "[Embed] ${TARGET_NAME}: embedding Luau sources from ${LABEL}"
        VERBATIM
    )

    set_source_files_properties("${GENERATED_FILE}" PROPERTIES GENERATED TRUE)
    target_sources(${TARGET_NAME} PRIVATE "${GENERATED_FILE}")
endfunction()

# Applies the manifest's [sources.<platform>] sections to the module's globbed
# file set, in place: SRC_VAR and HDR_VAR name the caller's variables.
#
# A file named by any section is removed from the unconditional glob and added
# back only when its own section is the one this platform selects. The removal
# is the whole point: the glob has already found every file under source/, so
# without it a windows-only translation unit would compile on Linux too and the
# section would read as documentation rather than as a rule.
#
# Patterns are relative to source/<directory> -- the directory the module's own
# public include path points at, so a pattern is spelled the way an #include of
# the same file is. Each pattern is a plain file(GLOB): one path component per
# component written, no recursive descent, so `*.cpp` cannot silently swallow a
# subdirectory a later commit adds.
function(cpp_select_platform_sources
    MODULE_LABEL
    MODULE_PATH
    DIR_NAME
    DECLARED_PLATFORMS
    WINDOWS_PATTERNS
    LINUX_PATTERNS
    MACOS_PATTERNS
    OTHER_PATTERNS
    SRC_VAR
    HDR_VAR
)
    if(DECLARED_PLATFORMS STREQUAL "")
        return()
    endif()

    set(SOURCE_BASE "${MODULE_PATH}/source/${DIR_NAME}")
    if(NOT IS_DIRECTORY "${SOURCE_BASE}")
        message(FATAL_ERROR
            "[Manifest] ${MODULE_LABEL}: [sources.*] patterns are relative to "
            "${SOURCE_BASE}, which is not a directory."
        )
    endif()

    set(SRC_FILES "${${SRC_VAR}}")
    set(HDR_FILES "${${HDR_VAR}}")
    set(MODULE_FILES ${SRC_FILES} ${HDR_FILES})

    set(CLAIMED_FILES "")

    foreach(PLATFORM IN LISTS DECLARED_PLATFORMS)
        string(TOUPPER "${PLATFORM}" PLATFORM_UPPER)
        set(SELECTED_${PLATFORM} "")

        foreach(PATTERN IN LISTS ${PLATFORM_UPPER}_PATTERNS)
            file(GLOB PATTERN_MATCHES CONFIGURE_DEPENDS "${SOURCE_BASE}/${PATTERN}")

            # A pattern that matches nothing is how a source file quietly stops
            # being compiled: the file was renamed or moved, the section still
            # names the old path, and every platform loses it at once.
            if(NOT PATTERN_MATCHES)
                message(FATAL_ERROR
                    "[Manifest] ${MODULE_LABEL}: [sources.${PLATFORM}] pattern "
                    "'${PATTERN}' matches no file under ${SOURCE_BASE}."
                )
            endif()

            foreach(MATCHED_FILE IN LISTS PATTERN_MATCHES)
                if(NOT MATCHED_FILE IN_LIST MODULE_FILES)
                    message(FATAL_ERROR
                        "[Manifest] ${MODULE_LABEL}: [sources.${PLATFORM}] pattern "
                        "'${PATTERN}' matches '${MATCHED_FILE}', which is not one of "
                        "the module's own .cpp/.hpp files."
                    )
                endif()

                string(MAKE_C_IDENTIFIER "${MATCHED_FILE}" FILE_KEY)
                if(DEFINED CLAIMED_BY_${FILE_KEY})
                    if(CLAIMED_BY_${FILE_KEY} STREQUAL PLATFORM)
                        message(FATAL_ERROR
                            "[Manifest] ${MODULE_LABEL}: [sources.${PLATFORM}] names "
                            "'${MATCHED_FILE}' twice."
                        )
                    endif()
                    message(FATAL_ERROR
                        "[Manifest] ${MODULE_LABEL}: '${MATCHED_FILE}' is claimed by both "
                        "[sources.${CLAIMED_BY_${FILE_KEY}}] and [sources.${PLATFORM}]. "
                        "A file belongs to exactly one platform section."
                    )
                endif()

                set(CLAIMED_BY_${FILE_KEY} "${PLATFORM}")
                list(APPEND CLAIMED_FILES "${MATCHED_FILE}")
                list(APPEND SELECTED_${PLATFORM} "${MATCHED_FILE}")
            endforeach()
        endforeach()
    endforeach()

    # `other` is every platform with no section of its own, so a platform that
    # has one never also takes `other`'s files.
    cpp_get_platform_key(CURRENT_PLATFORM)
    if(CURRENT_PLATFORM IN_LIST DECLARED_PLATFORMS)
        set(ACTIVE_PLATFORM "${CURRENT_PLATFORM}")
    else()
        set(ACTIVE_PLATFORM "other")
    endif()

    set(ACTIVE_FILES "")
    if(DEFINED SELECTED_${ACTIVE_PLATFORM})
        set(ACTIVE_FILES "${SELECTED_${ACTIVE_PLATFORM}}")
    endif()

    list(REMOVE_ITEM SRC_FILES ${CLAIMED_FILES})
    list(REMOVE_ITEM HDR_FILES ${CLAIMED_FILES})

    foreach(ACTIVE_FILE IN LISTS ACTIVE_FILES)
        if(ACTIVE_FILE MATCHES "\\.cpp$")
            list(APPEND SRC_FILES "${ACTIVE_FILE}")
        else()
            list(APPEND HDR_FILES "${ACTIVE_FILE}")
        endif()
    endforeach()

    # file(GLOB) hands back a sorted list and the reinstated files were appended
    # to the end of one; sorting keeps the file set independent of the order the
    # sections happen to appear in.
    list(SORT SRC_FILES)
    list(SORT HDR_FILES)

    set(${SRC_VAR} "${SRC_FILES}" PARENT_SCOPE)
    set(${HDR_VAR} "${HDR_FILES}" PARENT_SCOPE)
endfunction()

function(cpp_define_module MODULE_ROOT_DIR DIR_NAME)
    set(MODULE_PATH "${MODULE_ROOT_DIR}/${DIR_NAME}")
    set(MANIFEST_FILE "${MODULE_PATH}/manifest.txt")

    if(NOT EXISTS "${MANIFEST_FILE}")
        return()
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MANIFEST_FILE}")

    cpp_parse_manifest("${MANIFEST_FILE}" MANIFEST)

    set(MODULE_SHORT_NAME "${MANIFEST_MODULE_NAME}")
    if(MODULE_SHORT_NAME STREQUAL "")
        set(MODULE_SHORT_NAME "${DIR_NAME}")
    endif()

    set(MODULE_NAME "${PROJECT_NAME}_${MODULE_SHORT_NAME}")
    set(MODULE_VERSION "${MANIFEST_VERSION}")
    set(MODULE_DISPLAY_NAME "${MODULE_SHORT_NAME}")
    if(MODULE_DISPLAY_NAME STREQUAL "")
        set(MODULE_DISPLAY_NAME "${MODULE_NAME}")
    endif()

    cpp_manifest_supports_current_platform(
        "${MANIFEST_MODULE_PLATFORMS}"
        MODULE_PLATFORM_SUPPORTED
    )
    if(NOT MODULE_PLATFORM_SUPPORTED)
        cpp_get_platform_key(CURRENT_PLATFORM)
        message(STATUS "[AutoLoader] Skipping: ${MODULE_DISPLAY_NAME} (unsupported on ${CURRENT_PLATFORM})")
        return()
    endif()

    cpp_manifest_normalize_module_type("${MANIFEST_MODULE_TYPE}" MODULE_KIND)

    message(STATUS "[AutoLoader] Configuring: ${MODULE_DISPLAY_NAME} (v${MODULE_VERSION})")

    # A module owns its vendored third-party under external/.
    # When that directory carries a CMakeLists.txt, build it here (Pass 1: Define)
    # so its targets exist before this module is linked (Pass 2). This keeps a
    # single-module library confined to the module — nothing leaks to the
    # top-level build; the module still declares the concrete targets it links
    # in its manifest [dependencies].
    if(EXISTS "${MODULE_PATH}/external/CMakeLists.txt")
        add_subdirectory(
            "${MODULE_PATH}/external"
            "${CMAKE_BINARY_DIR}/modules/${DIR_NAME}/external"
            EXCLUDE_FROM_ALL
        )
    endif()

    file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
        "${MODULE_PATH}/source/*.cpp"
    )
    file(GLOB_RECURSE HDR_FILES CONFIGURE_DEPENDS
        "${MODULE_PATH}/source/*.hpp"
    )

    cpp_select_platform_sources(
        "${MODULE_DISPLAY_NAME}"
        "${MODULE_PATH}"
        "${DIR_NAME}"
        "${MANIFEST_SOURCES_PLATFORMS}"
        "${MANIFEST_SOURCES_WINDOWS}"
        "${MANIFEST_SOURCES_LINUX}"
        "${MANIFEST_SOURCES_MACOS}"
        "${MANIFEST_SOURCES_OTHER}"
        SRC_FILES
        HDR_FILES
    )

    if(NOT SRC_FILES)
        set(DUMMY_FILE "${CMAKE_BINARY_DIR}/modules/${DIR_NAME}/AutoDummy.cpp")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/modules/${DIR_NAME}")

        if(NOT EXISTS "${DUMMY_FILE}")
            file(WRITE "${DUMMY_FILE}" "// Auto-generated by the module build system.\n")
        endif()

        list(APPEND SRC_FILES "${DUMMY_FILE}")

        message(STATUS "  -> [Info] No source files found. Generated dummy file to allow library creation.")
    endif()

    add_library(${MODULE_NAME} ${MODULE_KIND} ${SRC_FILES} ${HDR_FILES})

    # The name anything outside this repository spells. ${MODULE_NAME} carries
    # ${PROJECT_NAME}, which is this repository's only while this repository is
    # the top-level project; under add_subdirectory it is the consuming
    # project's, so the real target name is not something a consumer can write.
    add_library(uf::${MODULE_SHORT_NAME} ALIAS ${MODULE_NAME})

    cpp_apply_safety_profile(${MODULE_NAME})

    set_target_properties(${MODULE_NAME} PROPERTIES
        VERSION ${MODULE_VERSION}
        CPP_DEPS_PUBLIC "${MANIFEST_DEPS_PUBLIC}"
        CPP_DEPS_PRIVATE "${MANIFEST_DEPS_PRIVATE}"
        CPP_DEPS_PUBLIC_WINDOWS "${MANIFEST_DEPS_PUBLIC_WINDOWS}"
        CPP_DEPS_PRIVATE_WINDOWS "${MANIFEST_DEPS_PRIVATE_WINDOWS}"
        CPP_DEPS_PUBLIC_LINUX "${MANIFEST_DEPS_PUBLIC_LINUX}"
        CPP_DEPS_PRIVATE_LINUX "${MANIFEST_DEPS_PRIVATE_LINUX}"
        CPP_DEPS_PUBLIC_MACOS "${MANIFEST_DEPS_PUBLIC_MACOS}"
        CPP_DEPS_PRIVATE_MACOS "${MANIFEST_DEPS_PRIVATE_MACOS}"
    )

    string(TOUPPER ${MODULE_NAME} UPPER_NAME)
    target_compile_definitions(${MODULE_NAME} PRIVATE ${UPPER_NAME}_BUILD)

    target_compile_features(${MODULE_NAME} PUBLIC cxx_std_23)

    cpp_apply_platform_definitions(
        ${MODULE_NAME}
        "${MANIFEST_DEFINES_PUBLIC}"
        "${MANIFEST_DEFINES_PRIVATE}"
        "${MANIFEST_DEFINES_PUBLIC_WINDOWS}"
        "${MANIFEST_DEFINES_PRIVATE_WINDOWS}"
        "${MANIFEST_DEFINES_PUBLIC_LINUX}"
        "${MANIFEST_DEFINES_PRIVATE_LINUX}"
        "${MANIFEST_DEFINES_PUBLIC_MACOS}"
        "${MANIFEST_DEFINES_PRIVATE_MACOS}"
    )

    target_include_directories(${MODULE_NAME}
        PUBLIC
            $<BUILD_INTERFACE:${MODULE_PATH}/source>
            $<BUILD_INTERFACE:${MODULE_PATH}/source/${DIR_NAME}>
            $<INSTALL_INTERFACE:include>
    )

    if(NOT MANIFEST_EMBED_LUAU_DIRECTORY STREQUAL "")
        set(EMBED_LUAU_DIR "${MODULE_PATH}/${MANIFEST_EMBED_LUAU_DIRECTORY}")
        # The label is the only path that reaches the generated file's text.
        # Making it relative to this repository's root is what keeps the output
        # byte-identical across machines with different checkout locations --
        # and CMAKE_SOURCE_DIR is not that root under add_subdirectory, where
        # the same label would become an absolute or ../-prefixed path.
        cmake_path(SET EMBED_LUAU_BASE NORMALIZE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
        file(RELATIVE_PATH EMBED_LUAU_LABEL "${EMBED_LUAU_BASE}" "${EMBED_LUAU_DIR}")
        cpp_embed_luau_sources(
            "${MODULE_NAME}"
            "${EMBED_LUAU_DIR}"
            "${CMAKE_BINARY_DIR}/generated/modules/${DIR_NAME}"
            "${MANIFEST_EMBED_LUAU_VERSION}"
            "${EMBED_LUAU_LABEL}"
        )
    endif()

    if(MANIFEST_BUILD_PCH)
        set(PCH_HEADER "${MANIFEST_BUILD_PCH}")
        if(NOT IS_ABSOLUTE "${PCH_HEADER}")
            set(PCH_HEADER "${MODULE_PATH}/${PCH_HEADER}")
        endif()

        if(EXISTS "${PCH_HEADER}")
            target_precompile_headers(${MODULE_NAME} PRIVATE "${PCH_HEADER}")
        else()
            message(WARNING "[${MODULE_NAME}] PCH header not found: ${PCH_HEADER}")
        endif()
    endif()

    if(MANIFEST_BUILD_UNITY)
        set_target_properties(${MODULE_NAME} PROPERTIES UNITY_BUILD ON)
    endif()

endfunction()

function(cpp_link_module MODULE_ROOT_DIR DIR_NAME)
    set(MODULE_PATH "${MODULE_ROOT_DIR}/${DIR_NAME}")
    set(MANIFEST_FILE "${MODULE_PATH}/manifest.txt")

    # Resolve module name from manifest (same logic as cpp_define_module)
    set(MODULE_SHORT_NAME "${DIR_NAME}")
    if(EXISTS "${MANIFEST_FILE}")
        cpp_parse_manifest("${MANIFEST_FILE}" MANIFEST)
        if(NOT MANIFEST_MODULE_NAME STREQUAL "")
            set(MODULE_SHORT_NAME "${MANIFEST_MODULE_NAME}")
        endif()
    endif()

    set(MODULE_NAME "${PROJECT_NAME}_${MODULE_SHORT_NAME}")

    if(NOT TARGET ${MODULE_NAME})
        return()
    endif()

    get_target_property(DEPS_PUBLIC ${MODULE_NAME} CPP_DEPS_PUBLIC)
    get_target_property(DEPS_PRIVATE ${MODULE_NAME} CPP_DEPS_PRIVATE)
    get_target_property(DEPS_PUBLIC_WINDOWS ${MODULE_NAME} CPP_DEPS_PUBLIC_WINDOWS)
    get_target_property(DEPS_PRIVATE_WINDOWS ${MODULE_NAME} CPP_DEPS_PRIVATE_WINDOWS)
    get_target_property(DEPS_PUBLIC_LINUX ${MODULE_NAME} CPP_DEPS_PUBLIC_LINUX)
    get_target_property(DEPS_PRIVATE_LINUX ${MODULE_NAME} CPP_DEPS_PRIVATE_LINUX)
    get_target_property(DEPS_PUBLIC_MACOS ${MODULE_NAME} CPP_DEPS_PUBLIC_MACOS)
    get_target_property(DEPS_PRIVATE_MACOS ${MODULE_NAME} CPP_DEPS_PRIVATE_MACOS)

    cpp_link_platform_dependencies(
        ${MODULE_NAME}
        "${DEPS_PUBLIC}"
        "${DEPS_PRIVATE}"
        "${DEPS_PUBLIC_WINDOWS}"
        "${DEPS_PRIVATE_WINDOWS}"
        "${DEPS_PUBLIC_LINUX}"
        "${DEPS_PRIVATE_LINUX}"
        "${DEPS_PUBLIC_MACOS}"
        "${DEPS_PRIVATE_MACOS}"
    )

endfunction()


function(cpp_scan_and_load_modules)
    foreach(ROOT_PATH IN LISTS ARGN)
        if(ROOT_PATH STREQUAL "")
            continue()
        endif()

        if(NOT IS_DIRECTORY "${ROOT_PATH}")
            message(WARNING "[AutoLoader] Module root not found: ${ROOT_PATH}")
            continue()
        endif()

        file(GLOB CHILDREN RELATIVE "${ROOT_PATH}" "${ROOT_PATH}/*")

        set(MODULE_DIRS "")
        foreach(CHILD ${CHILDREN})
            if(IS_DIRECTORY "${ROOT_PATH}/${CHILD}" AND EXISTS "${ROOT_PATH}/${CHILD}/manifest.txt")
                list(APPEND MODULE_DIRS "${CHILD}")
            elseif(IS_DIRECTORY "${ROOT_PATH}/${CHILD}")
                if(CPP_VERBOSE_AUTOLOADER)
                    message(STATUS "[AutoLoader] Skipping '${ROOT_PATH}/${CHILD}' (no manifest.txt)")
                endif()
            endif()
        endforeach()

        # Pass 1: Define
        foreach(CHILD ${MODULE_DIRS})
            cpp_define_module("${ROOT_PATH}" "${CHILD}")
        endforeach()

        # Pass 2: Link
        foreach(CHILD ${MODULE_DIRS})
            cpp_link_module("${ROOT_PATH}" "${CHILD}")
        endforeach()
    endforeach()
endfunction()
