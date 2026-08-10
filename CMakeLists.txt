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
endif()

include(cmake/ccache.cmake)
include(cmake/safety-profile.cmake)
include(cmake/build.cmake)
include(cmake/runtime-payload.cmake)

# This repository's own modules. The default is spelled against this file's
# directory rather than CMAKE_SOURCE_DIR, which is the consuming project's root
# whenever this repository is added with add_subdirectory -- a consumer with a
# modules/ directory of its own would otherwise have it scanned in place of
# this one, define no module, and fail much later.
set(CPP_MODULE_ROOTS
    "${CMAKE_CURRENT_SOURCE_DIR}/modules"
    CACHE STRING "Semicolon-separated module root directories"
)

cpp_scan_and_load_modules(${CPP_MODULE_ROOTS})

# The imported ONNX runtime is declared by modules/ocr/external, which names it
# after this repository's project. Publish the same namespaced alias the module
# targets carry, so nothing outside this repository has to spell that name.
if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
    add_library(uf::ocr_onnxruntime ALIAS ${PROJECT_NAME}_ocr_onnxruntime)
endif()

add_subdirectory(entry)

# The exported Operator contract suite. It is not part of tests/: a consuming
# repository builds it from its own tree against its own project, and this
# repository is one of its runs. The function is defined for every build,
# top-level or not, because a consumer's add_subdirectory of this repository is
# the only way it reaches one.
include(cmake/operator-contract-suite.cmake)

# Everything below is this repository rehearsing its own contracts. A consumer
# adding this tree as a subdirectory gets the suite it calls and the modules it
# links; it does not get contract-suite-arcana in its CTest namespace, nor this
# repository's own tests, nor a compile_commands.json written into its tree.
if(PROJECT_IS_TOP_LEVEL)
    option(CPP_BUILD_TESTS "Build this repository's own tests" ON)

    enable_testing()

    # Before tests/, because tests/CMakeLists.txt ends by checking that every
    # migration-report contract ID has a CTest and some of them are registered
    # by the suite.
    if(TARGET ${PROJECT_NAME}_operator)
        add_subdirectory(contract-suite)
    endif()

    if(CPP_BUILD_TESTS)
        add_subdirectory(tests)
    endif()

    # Refresh build/compile_commands.json on every build so clangd -- which
    # searches the build/ subdirectory by default -- always sees the current
    # build, whichever preset's binary directory produced it. Runs each build so
    # it never goes stale.
    if(CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_BINARY_DIR STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/build")
        add_custom_target(copy_compile_commands ALL
            ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/compile_commands.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/build/compile_commands.json"
            BYPRODUCTS "${CMAKE_CURRENT_SOURCE_DIR}/build/compile_commands.json"
            COMMENT "Refreshing build/compile_commands.json for clangd"
            VERBATIM
        )
    endif()
endif()
