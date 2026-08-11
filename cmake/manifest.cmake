include_guard(GLOBAL)

function(cpp_manifest_parse_bool VALUE OUT_VAR)
    string(TOLOWER "${VALUE}" VALUE_LOWER)

    if(VALUE_LOWER STREQUAL "1" OR VALUE_LOWER STREQUAL "true" OR VALUE_LOWER STREQUAL "yes" OR VALUE_LOWER STREQUAL "on")
        set(${OUT_VAR} ON PARENT_SCOPE)
    else()
        set(${OUT_VAR} OFF PARENT_SCOPE)
    endif()
endfunction()

# The module kinds, which are the CMake library types this build creates a
# target for. There is no kind for a module that produces no library: every
# module here compiles into one.
#
# An unknown word is fatal rather than STATIC. The warning this replaced never
# reached a reader, which is how `type = sources` sat in the conformance suite's
# manifest for a week naming a kind the build had no branch for; a manifest that
# says one thing while the build does another cannot be read out of the tree.
function(cpp_manifest_normalize_module_type VALUE OUT_VAR)
    set(TYPE "STATIC")

    if(NOT VALUE STREQUAL "")
        string(TOUPPER "${VALUE}" TYPE_UPPER)
        if(TYPE_UPPER STREQUAL "LIBRARY")
            set(TYPE "STATIC")
        elseif(TYPE_UPPER STREQUAL "STATIC" OR TYPE_UPPER STREQUAL "SHARED")
            set(TYPE "${TYPE_UPPER}")
        else()
            message(FATAL_ERROR
                "[Manifest] Unknown module type '${VALUE}'. "
                "Use one of: library, static, shared."
            )
        endif()
    endif()

    set(${OUT_VAR} "${TYPE}" PARENT_SCOPE)
endfunction()

# The platform vocabulary of [sources.<platform>]. It is the one from
# [dependencies.<platform>] plus `other`, which names every platform that has no
# section of its own.
function(cpp_manifest_normalize_source_platform VALUE OUT_VAR)
    string(TOLOWER "${VALUE}" VALUE_LOWER)

    if(VALUE_LOWER STREQUAL "other")
        set(${OUT_VAR} "other" PARENT_SCOPE)
        return()
    endif()

    cpp_manifest_normalize_platform("${VALUE}" NORMALIZED_PLATFORM)
    set(${OUT_VAR} "${NORMALIZED_PLATFORM}" PARENT_SCOPE)
endfunction()

function(cpp_manifest_append_list OUT_LIST_VAR VALUE)
    set(LIST_VALUES "${${OUT_LIST_VAR}}")

    string(REPLACE "," ";" VALUE_LIST "${VALUE}")
    string(REPLACE ";" ";" VALUE_LIST "${VALUE_LIST}")

    foreach(ITEM IN LISTS VALUE_LIST)
        string(STRIP "${ITEM}" ITEM)
        if(NOT ITEM STREQUAL "")
            list(APPEND LIST_VALUES "${ITEM}")
        endif()
    endforeach()

    set(${OUT_LIST_VAR} "${LIST_VALUES}" PARENT_SCOPE)
endfunction()

function(cpp_manifest_append_deps OUT_LIST_VAR VALUE)
    set(LIST_VALUES "${${OUT_LIST_VAR}}")

    string(REGEX REPLACE "[, \t]+" ";" VALUE_LIST "${VALUE}")

    foreach(ITEM IN LISTS VALUE_LIST)
        string(STRIP "${ITEM}" ITEM)
        if(NOT ITEM STREQUAL "")
            list(APPEND LIST_VALUES "${ITEM}")
        endif()
    endforeach()

    set(${OUT_LIST_VAR} "${LIST_VALUES}" PARENT_SCOPE)
endfunction()

# A key appears at most once per section. This parser would accumulate a repeated
# one, but scripts/check_modules.py reads the same file with Python's
# configparser, which raises DuplicateOptionError -- so a second `include =` or
# `public =` line is a manifest only half the tooling can read. List every value
# on the one line the key gets.
function(cpp_parse_manifest FILE_PATH OUT_PREFIX)
    if(NOT EXISTS "${FILE_PATH}")
        return()
    endif()

    file(STRINGS "${FILE_PATH}" LINES)

    set(VERSION "0.0.0")
    set(MODULE_NAME "")
    set(MODULE_TYPE "")
    set(MODULE_PLATFORMS "")
    set(BUILD_PCH "")
    set(BUILD_UNITY "")
    set(EMBED_LUAU_DIRECTORY "")
    set(EMBED_LUAU_VERSION "")
    set(DEPS_PUBLIC "")
    set(DEPS_PRIVATE "")
    set(DEPS_PUBLIC_WINDOWS "")
    set(DEPS_PRIVATE_WINDOWS "")
    set(DEPS_PUBLIC_LINUX "")
    set(DEPS_PRIVATE_LINUX "")
    set(DEPS_PUBLIC_MACOS "")
    set(DEPS_PRIVATE_MACOS "")
    set(DEFINES_PUBLIC "")
    set(DEFINES_PRIVATE "")
    set(DEFINES_PUBLIC_WINDOWS "")
    set(DEFINES_PRIVATE_WINDOWS "")
    set(DEFINES_PUBLIC_LINUX "")
    set(DEFINES_PRIVATE_LINUX "")
    set(DEFINES_PUBLIC_MACOS "")
    set(DEFINES_PRIVATE_MACOS "")
    set(SOURCES_PLATFORMS "")
    set(SOURCES_WINDOWS "")
    set(SOURCES_LINUX "")
    set(SOURCES_MACOS "")
    set(SOURCES_OTHER "")
    set(APPLICATION_NAME "")
    set(APPLICATION_VERSION "")
    set(CURRENT_SECTION "")

    foreach(LINE IN LISTS LINES)
        string(STRIP "${LINE}" LINE)

        if(LINE STREQUAL "" OR LINE MATCHES "^#")
            continue()
        endif()

        if(LINE MATCHES "^\\[(.+)\\]$")
            set(CURRENT_SECTION "${CMAKE_MATCH_1}")
            string(TOLOWER "${CURRENT_SECTION}" CURRENT_SECTION)

            # A [sources.*] section is registered where its header is read, not
            # where its keys are: which platforms have a section of their own is
            # what decides who `other` covers, and a section whose header is
            # present but whose include list is empty must be an error rather
            # than a platform silently falling back to `other`.
            if(CURRENT_SECTION STREQUAL "sources")
                message(FATAL_ERROR
                    "[Manifest] ${FILE_PATH}: [sources] must name a platform: "
                    "[sources.windows], [sources.linux], [sources.macos], or [sources.other]."
                )
            endif()

            if(CURRENT_SECTION MATCHES "^sources\\.(.+)$")
                cpp_manifest_normalize_source_platform("${CMAKE_MATCH_1}" DECLARED_PLATFORM)
                if(DECLARED_PLATFORM STREQUAL "")
                    message(FATAL_ERROR
                        "[Manifest] ${FILE_PATH}: unknown platform in section '[${CURRENT_SECTION}]'. "
                        "Use windows, linux, macos, or other."
                    )
                endif()
                if(NOT DECLARED_PLATFORM IN_LIST SOURCES_PLATFORMS)
                    list(APPEND SOURCES_PLATFORMS "${DECLARED_PLATFORM}")
                endif()
            endif()

            continue()
        endif()

        if(NOT LINE MATCHES "^([^=]+)=(.*)$")
            continue()
        endif()

        set(KEY "${CMAKE_MATCH_1}")
        set(VALUE "${CMAKE_MATCH_2}")
        string(STRIP "${KEY}" KEY)
        string(STRIP "${VALUE}" VALUE)
        string(TOLOWER "${KEY}" KEY_LOWER)

        set(SECTION_BASE "${CURRENT_SECTION}")
        set(SECTION_PLATFORM "")
        set(SECTION_HAS_PLATFORM FALSE)

        if(CURRENT_SECTION MATCHES "^(dependencies|defines)\\.(.+)$")
            set(SECTION_BASE "${CMAKE_MATCH_1}")
            set(SECTION_HAS_PLATFORM TRUE)
            cpp_manifest_normalize_platform("${CMAKE_MATCH_2}" SECTION_PLATFORM)
        elseif(CURRENT_SECTION MATCHES "^sources\\.(.+)$")
            set(SECTION_BASE "sources")
            set(SECTION_HAS_PLATFORM TRUE)
            # Already validated where the section header was read.
            cpp_manifest_normalize_source_platform("${CMAKE_MATCH_1}" SECTION_PLATFORM)
        endif()

        if(SECTION_BASE STREQUAL "application")
            if(KEY_LOWER STREQUAL "name")
                set(APPLICATION_NAME "${VALUE}")
            elseif(KEY_LOWER STREQUAL "version")
                set(APPLICATION_VERSION "${VALUE}")
            else()
                message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
            endif()
        elseif(SECTION_BASE STREQUAL "module")
            if(KEY_LOWER STREQUAL "name")
                set(MODULE_NAME "${VALUE}")
            elseif(KEY_LOWER STREQUAL "type")
                set(MODULE_TYPE "${VALUE}")
            elseif(KEY_LOWER STREQUAL "version")
                set(VERSION "${VALUE}")
            elseif(KEY_LOWER STREQUAL "platforms")
                cpp_manifest_append_list(MODULE_PLATFORMS "${VALUE}")
            else()
                message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
            endif()
        elseif(SECTION_BASE STREQUAL "build")
            if(KEY_LOWER STREQUAL "pch")
                set(BUILD_PCH "${VALUE}")
            elseif(KEY_LOWER STREQUAL "unity_build" OR KEY_LOWER STREQUAL "unity")
                cpp_manifest_parse_bool("${VALUE}" BUILD_UNITY)
            else()
                message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
            endif()
        # [embed] declares non-C++ sources that are compiled into the module.
        # luau_directory is module-relative and holds the .luau tree;
        # luau_version is the semantic version stamped into the generated bundle.
        elseif(SECTION_BASE STREQUAL "embed")
            if(KEY_LOWER STREQUAL "luau_directory")
                set(EMBED_LUAU_DIRECTORY "${VALUE}")
            elseif(KEY_LOWER STREQUAL "luau_version")
                set(EMBED_LUAU_VERSION "${VALUE}")
            else()
                message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
            endif()
        elseif(SECTION_BASE STREQUAL "dependencies")
            if(NOT SECTION_HAS_PLATFORM)
                if(KEY_LOWER STREQUAL "public")
                    cpp_manifest_append_deps(DEPS_PUBLIC "${VALUE}")
                elseif(KEY_LOWER STREQUAL "private")
                    cpp_manifest_append_deps(DEPS_PRIVATE "${VALUE}")
                else()
                    message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
                endif()
            elseif(SECTION_PLATFORM STREQUAL "windows")
                if(KEY_LOWER STREQUAL "public")
                    cpp_manifest_append_deps(DEPS_PUBLIC_WINDOWS "${VALUE}")
                elseif(KEY_LOWER STREQUAL "private")
                    cpp_manifest_append_deps(DEPS_PRIVATE_WINDOWS "${VALUE}")
                else()
                    message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
                endif()
            elseif(SECTION_PLATFORM STREQUAL "linux")
                if(KEY_LOWER STREQUAL "public")
                    cpp_manifest_append_deps(DEPS_PUBLIC_LINUX "${VALUE}")
                elseif(KEY_LOWER STREQUAL "private")
                    cpp_manifest_append_deps(DEPS_PRIVATE_LINUX "${VALUE}")
                else()
                    message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
                endif()
            elseif(SECTION_PLATFORM STREQUAL "macos")
                if(KEY_LOWER STREQUAL "public")
                    cpp_manifest_append_deps(DEPS_PUBLIC_MACOS "${VALUE}")
                elseif(KEY_LOWER STREQUAL "private")
                    cpp_manifest_append_deps(DEPS_PRIVATE_MACOS "${VALUE}")
                else()
                    message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
                endif()
            else()
                message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized platform in section '[${CURRENT_SECTION}]'.")
            endif()
        elseif(SECTION_BASE STREQUAL "defines")
            if(NOT SECTION_HAS_PLATFORM)
                if(KEY_LOWER STREQUAL "public")
                    cpp_manifest_append_list(DEFINES_PUBLIC "${VALUE}")
                elseif(KEY_LOWER STREQUAL "private")
                    cpp_manifest_append_list(DEFINES_PRIVATE "${VALUE}")
                else()
                    message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
                endif()
            elseif(SECTION_PLATFORM STREQUAL "windows")
                if(KEY_LOWER STREQUAL "public")
                    cpp_manifest_append_list(DEFINES_PUBLIC_WINDOWS "${VALUE}")
                elseif(KEY_LOWER STREQUAL "private")
                    cpp_manifest_append_list(DEFINES_PRIVATE_WINDOWS "${VALUE}")
                else()
                    message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
                endif()
            elseif(SECTION_PLATFORM STREQUAL "linux")
                if(KEY_LOWER STREQUAL "public")
                    cpp_manifest_append_list(DEFINES_PUBLIC_LINUX "${VALUE}")
                elseif(KEY_LOWER STREQUAL "private")
                    cpp_manifest_append_list(DEFINES_PRIVATE_LINUX "${VALUE}")
                else()
                    message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
                endif()
            elseif(SECTION_PLATFORM STREQUAL "macos")
                if(KEY_LOWER STREQUAL "public")
                    cpp_manifest_append_list(DEFINES_PUBLIC_MACOS "${VALUE}")
                elseif(KEY_LOWER STREQUAL "private")
                    cpp_manifest_append_list(DEFINES_PRIVATE_MACOS "${VALUE}")
                else()
                    message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}].")
                endif()
            else()
                message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized platform in section '[${CURRENT_SECTION}]'.")
            endif()
        # [sources.<platform>] names the files that compile on that platform
        # only. Patterns are relative to the module's source/<directory>/ -- the
        # same directory the module's own include path points at. A mistyped key
        # here would drop files out of the build without saying so, so it is
        # fatal rather than a warning.
        elseif(SECTION_BASE STREQUAL "sources")
            if(NOT KEY_LOWER STREQUAL "include")
                message(FATAL_ERROR
                    "[Manifest] ${FILE_PATH}: Unrecognized key '${KEY}' in [${CURRENT_SECTION}]. "
                    "The only key is 'include'."
                )
            endif()
            string(TOUPPER "${SECTION_PLATFORM}" SECTION_PLATFORM_UPPER)
            cpp_manifest_append_deps(SOURCES_${SECTION_PLATFORM_UPPER} "${VALUE}")
        else()
            message(WARNING "[Manifest] ${FILE_PATH}: Unrecognized section '[${CURRENT_SECTION}]'.")
        endif()
    endforeach()

    foreach(DECLARED_PLATFORM IN LISTS SOURCES_PLATFORMS)
        string(TOUPPER "${DECLARED_PLATFORM}" DECLARED_PLATFORM_UPPER)
        if(SOURCES_${DECLARED_PLATFORM_UPPER} STREQUAL "")
            message(FATAL_ERROR
                "[Manifest] ${FILE_PATH}: [sources.${DECLARED_PLATFORM}] declares no include patterns."
            )
        endif()
    endforeach()

    set(${OUT_PREFIX}_VERSION "${VERSION}" PARENT_SCOPE)
    set(${OUT_PREFIX}_MODULE_NAME "${MODULE_NAME}" PARENT_SCOPE)
    set(${OUT_PREFIX}_MODULE_TYPE "${MODULE_TYPE}" PARENT_SCOPE)
    set(${OUT_PREFIX}_MODULE_PLATFORMS "${MODULE_PLATFORMS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_BUILD_PCH "${BUILD_PCH}" PARENT_SCOPE)
    set(${OUT_PREFIX}_BUILD_UNITY "${BUILD_UNITY}" PARENT_SCOPE)
    set(${OUT_PREFIX}_EMBED_LUAU_DIRECTORY "${EMBED_LUAU_DIRECTORY}" PARENT_SCOPE)
    set(${OUT_PREFIX}_EMBED_LUAU_VERSION "${EMBED_LUAU_VERSION}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEPS_PUBLIC "${DEPS_PUBLIC}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEPS_PRIVATE "${DEPS_PRIVATE}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEPS_PUBLIC_WINDOWS "${DEPS_PUBLIC_WINDOWS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEPS_PRIVATE_WINDOWS "${DEPS_PRIVATE_WINDOWS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEPS_PUBLIC_LINUX "${DEPS_PUBLIC_LINUX}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEPS_PRIVATE_LINUX "${DEPS_PRIVATE_LINUX}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEPS_PUBLIC_MACOS "${DEPS_PUBLIC_MACOS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEPS_PRIVATE_MACOS "${DEPS_PRIVATE_MACOS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEFINES_PUBLIC "${DEFINES_PUBLIC}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEFINES_PRIVATE "${DEFINES_PRIVATE}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEFINES_PUBLIC_WINDOWS "${DEFINES_PUBLIC_WINDOWS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEFINES_PRIVATE_WINDOWS "${DEFINES_PRIVATE_WINDOWS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEFINES_PUBLIC_LINUX "${DEFINES_PUBLIC_LINUX}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEFINES_PRIVATE_LINUX "${DEFINES_PRIVATE_LINUX}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEFINES_PUBLIC_MACOS "${DEFINES_PUBLIC_MACOS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_DEFINES_PRIVATE_MACOS "${DEFINES_PRIVATE_MACOS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_SOURCES_PLATFORMS "${SOURCES_PLATFORMS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_SOURCES_WINDOWS "${SOURCES_WINDOWS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_SOURCES_LINUX "${SOURCES_LINUX}" PARENT_SCOPE)
    set(${OUT_PREFIX}_SOURCES_MACOS "${SOURCES_MACOS}" PARENT_SCOPE)
    set(${OUT_PREFIX}_SOURCES_OTHER "${SOURCES_OTHER}" PARENT_SCOPE)
    set(${OUT_PREFIX}_APPLICATION_NAME "${APPLICATION_NAME}" PARENT_SCOPE)
    set(${OUT_PREFIX}_APPLICATION_VERSION "${APPLICATION_VERSION}" PARENT_SCOPE)
endfunction()
