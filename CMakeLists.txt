cmake_minimum_required(VERSION 3.30)

set(APPLICATION_MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/manifest.txt")
include(cmake/manifest.cmake)
cpp_parse_manifest("${APPLICATION_MANIFEST_FILE}" ROOT_MANIFEST)

if("${ROOT_MANIFEST_APPLICATION_NAME}" STREQUAL "")
    message(FATAL_ERROR "[Application] manifest.txt must define [application].name.")
endif()
if("${ROOT_MANIFEST_APPLICATION_VERSION}" STREQUAL "")
    message(FATAL_ERROR "[Application] manifest.txt must define [application].version.")
endif()

project("${ROOT_MANIFEST_APPLICATION_NAME}"
    VERSION "${ROOT_MANIFEST_APPLICATION_VERSION}"
    DESCRIPTION "Native C++ visual automation framework for Windows: WGC capture, template matching, strict background PostMessage input"
    LANGUAGES CXX
)

set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${APPLICATION_MANIFEST_FILE}"
)

option(CPP_BUILD_TESTS "Build project tests" ON)
option(CPP_USE_CCACHE "Use ccache with Ninja when available" ON)
option(CPP_VERBOSE_AUTOLOADER "Enable verbose module loader logs" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    set(CPP_UTF8_MANIFEST "${CMAKE_SOURCE_DIR}/cmake/utf8.manifest")
    if(MSVC)
        set(CPP_UTF8_RC "")
    else()
        set(CPP_UTF8_RC "${CMAKE_SOURCE_DIR}/cmake/utf8.rc")
    endif()
else()
    set(CPP_UTF8_MANIFEST "")
    set(CPP_UTF8_RC "")
endif()

include(cmake/ccache.cmake)
include(cmake/safety-profile.cmake)
include(cmake/build.cmake)
include(cmake/runtime-payload.cmake)

set(CPP_MODULE_ROOTS
    "${CMAKE_SOURCE_DIR}/modules"
    CACHE STRING "Semicolon-separated module root directories"
)

cpp_scan_and_load_modules(${CPP_MODULE_ROOTS})

add_subdirectory(entry)

enable_testing()

# The exported Operator contract suite. It is not part of tests/: a consuming
# repository builds it from its own tree against its own project, and this
# repository is one of its runs. It is therefore registered here rather than
# under CPP_BUILD_TESTS, and before tests/, because tests/CMakeLists.txt ends by
# checking that every migration-report contract ID has a CTest and some of them
# are registered by the suite. See cmake/operator-contract-suite.cmake for what
# a consumer writes.
include(cmake/operator-contract-suite.cmake)
if(TARGET ${PROJECT_NAME}_operator)
    add_subdirectory(contract-suite)
endif()

if(CPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Refresh build/compile_commands.json on every build so clangd — which searches
# the build/ subdirectory by default — always sees the current build, whichever
# preset's binary directory produced it. Runs each build so it never goes stale.
if(CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_BINARY_DIR STREQUAL "${CMAKE_SOURCE_DIR}/build")
    add_custom_target(copy_compile_commands ALL
        ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/build/compile_commands.json"
        BYPRODUCTS "${CMAKE_SOURCE_DIR}/build/compile_commands.json"
        COMMENT "Refreshing build/compile_commands.json for clangd"
        VERBATIM
    )
endif()
