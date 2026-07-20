include(${CMAKE_CURRENT_LIST_DIR}/platform.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/manifest.cmake)

function(cpp_resolve_dependency RAW_DEP OUT_VAR)
    if(TARGET "${RAW_DEP}")
        set(${OUT_VAR} "${RAW_DEP}" PARENT_SCOPE)
        return()
    endif()

    set(PREFIXED "${PROJECT_NAME}_${RAW_DEP}")
    if(TARGET "${PREFIXED}")
        set(${OUT_VAR} "${PREFIXED}" PARENT_SCOPE)
        return()
    endif()

    set(${OUT_VAR} "${RAW_DEP}" PARENT_SCOPE)
endfunction()

function(cpp_collect_platform_lists OUT_PUBLIC OUT_PRIVATE WINDOWS_PUBLIC WINDOWS_PRIVATE LINUX_PUBLIC LINUX_PRIVATE MACOS_PUBLIC MACOS_PRIVATE)
    cpp_platform_select_list(PLATFORM_PUBLIC "${WINDOWS_PUBLIC}" "${LINUX_PUBLIC}" "${MACOS_PUBLIC}")
    cpp_platform_select_list(PLATFORM_PRIVATE "${WINDOWS_PRIVATE}" "${LINUX_PRIVATE}" "${MACOS_PRIVATE}")

    set(${OUT_PUBLIC} "${PLATFORM_PUBLIC}" PARENT_SCOPE)
    set(${OUT_PRIVATE} "${PLATFORM_PRIVATE}" PARENT_SCOPE)
endfunction()

function(cpp_link_target_list MODULE_NAME VISIBILITY DEP_LIST)
    if(NOT DEP_LIST OR DEP_LIST MATCHES "-NOTFOUND$")
        return()
    endif()

    set(LOCAL_DEPS ${DEP_LIST})
    list(REMOVE_DUPLICATES LOCAL_DEPS)

    foreach(DEP IN LISTS LOCAL_DEPS)
        string(STRIP "${DEP}" DEP)
        if(DEP STREQUAL "")
            continue()
        endif()

        cpp_resolve_dependency("${DEP}" RESOLVED_DEP)

        if(NOT TARGET "${RESOLVED_DEP}" AND NOT EXISTS "${RESOLVED_DEP}")
            message(WARNING "[Link] '${MODULE_NAME}' dependency '${DEP}' not found as target or path. Passing to linker.")
        endif()

        if(CPP_VERBOSE_AUTOLOADER)
            message(STATUS "[Link Debug] Linking '${MODULE_NAME}' with '${RESOLVED_DEP}'")
        endif()
        target_link_libraries(${MODULE_NAME} ${VISIBILITY} ${RESOLVED_DEP})
    endforeach()
endfunction()

function(cpp_apply_compile_definitions TARGET_NAME VISIBILITY DEFINES)
    if(NOT DEFINES OR DEFINES MATCHES "-NOTFOUND$")
        return()
    endif()

    target_compile_definitions(${TARGET_NAME} ${VISIBILITY} ${DEFINES})
endfunction()

function(cpp_apply_platform_definitions TARGET_NAME BASE_PUBLIC BASE_PRIVATE WINDOWS_PUBLIC WINDOWS_PRIVATE LINUX_PUBLIC LINUX_PRIVATE MACOS_PUBLIC MACOS_PRIVATE)
    cpp_apply_compile_definitions(${TARGET_NAME} PUBLIC "${BASE_PUBLIC}")
    cpp_apply_compile_definitions(${TARGET_NAME} PRIVATE "${BASE_PRIVATE}")

    cpp_collect_platform_lists(PLATFORM_PUBLIC PLATFORM_PRIVATE
        "${WINDOWS_PUBLIC}" "${WINDOWS_PRIVATE}"
        "${LINUX_PUBLIC}" "${LINUX_PRIVATE}"
        "${MACOS_PUBLIC}" "${MACOS_PRIVATE}")

    cpp_apply_compile_definitions(${TARGET_NAME} PUBLIC "${PLATFORM_PUBLIC}")
    cpp_apply_compile_definitions(${TARGET_NAME} PRIVATE "${PLATFORM_PRIVATE}")
endfunction()

function(cpp_link_platform_dependencies TARGET_NAME BASE_PUBLIC BASE_PRIVATE WINDOWS_PUBLIC WINDOWS_PRIVATE LINUX_PUBLIC LINUX_PRIVATE MACOS_PUBLIC MACOS_PRIVATE)
    cpp_link_target_list(${TARGET_NAME} PUBLIC "${BASE_PUBLIC}")
    cpp_link_target_list(${TARGET_NAME} PRIVATE "${BASE_PRIVATE}")

    cpp_collect_platform_lists(PLATFORM_PUBLIC PLATFORM_PRIVATE
        "${WINDOWS_PUBLIC}" "${WINDOWS_PRIVATE}"
        "${LINUX_PUBLIC}" "${LINUX_PRIVATE}"
        "${MACOS_PUBLIC}" "${MACOS_PRIVATE}")

    cpp_link_target_list(${TARGET_NAME} PUBLIC "${PLATFORM_PUBLIC}")
    cpp_link_target_list(${TARGET_NAME} PRIVATE "${PLATFORM_PRIVATE}")
endfunction()
