function(cpp_get_platform_key OUT_VAR)
    if(WIN32)
        set(PLATFORM_KEY "windows")
    elseif(APPLE)
        set(PLATFORM_KEY "macos")
    elseif(UNIX)
        set(PLATFORM_KEY "linux")
    else()
        set(PLATFORM_KEY "unknown")
    endif()

    set(${OUT_VAR} "${PLATFORM_KEY}" PARENT_SCOPE)
endfunction()

# Makes TARGET_NAME's executable run under the UTF-8 code page. Both files are
# this repository's own, so they resolve against this file's directory rather
# than against CMAKE_SOURCE_DIR, which is the consuming project's root whenever
# this repository is added with add_subdirectory.
function(cpp_apply_utf8_encoding TARGET_NAME)
    if(NOT WIN32)
        return()
    endif()

    if(MSVC)
        target_sources(${TARGET_NAME} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.manifest"
        )
    else()
        target_sources(${TARGET_NAME} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.rc"
        )
    endif()
endfunction()

function(cpp_platform_select_list OUT_VAR WINDOWS_LIST LINUX_LIST MACOS_LIST)
    cpp_get_platform_key(PLATFORM_KEY)

    if(PLATFORM_KEY STREQUAL "windows")
        set(${OUT_VAR} "${WINDOWS_LIST}" PARENT_SCOPE)
    elseif(PLATFORM_KEY STREQUAL "linux")
        set(${OUT_VAR} "${LINUX_LIST}" PARENT_SCOPE)
    elseif(PLATFORM_KEY STREQUAL "macos")
        set(${OUT_VAR} "${MACOS_LIST}" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "" PARENT_SCOPE)
        message(WARNING "[Platform] Unknown platform. Skipping platform-specific settings.")
    endif()
endfunction()

function(cpp_manifest_normalize_platform VALUE OUT_VAR)
    string(TOLOWER "${VALUE}" PLATFORM_LOWER)

    if(PLATFORM_LOWER STREQUAL "windows" OR PLATFORM_LOWER STREQUAL "win32")
        set(${OUT_VAR} "windows" PARENT_SCOPE)
    elseif(PLATFORM_LOWER STREQUAL "linux")
        set(${OUT_VAR} "linux" PARENT_SCOPE)
    elseif(PLATFORM_LOWER STREQUAL "mac" OR PLATFORM_LOWER STREQUAL "osx" OR PLATFORM_LOWER STREQUAL "macos" OR PLATFORM_LOWER STREQUAL "darwin")
        set(${OUT_VAR} "macos" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "" PARENT_SCOPE)
    endif()
endfunction()

function(cpp_manifest_supports_current_platform PLATFORMS OUT_VAR)
    if(PLATFORMS STREQUAL "")
        set(${OUT_VAR} TRUE PARENT_SCOPE)
        return()
    endif()

    cpp_get_platform_key(CURRENT_PLATFORM)
    set(IS_SUPPORTED FALSE)

    foreach(PLATFORM IN LISTS PLATFORMS)
        cpp_manifest_normalize_platform("${PLATFORM}" NORMALIZED_PLATFORM)
        if(NORMALIZED_PLATFORM STREQUAL "")
            message(WARNING "[Manifest] Unknown module platform '${PLATFORM}'.")
        elseif(NORMALIZED_PLATFORM STREQUAL CURRENT_PLATFORM)
            set(IS_SUPPORTED TRUE)
        endif()
    endforeach()

    set(${OUT_VAR} ${IS_SUPPORTED} PARENT_SCOPE)
endfunction()
