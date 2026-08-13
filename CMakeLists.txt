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
# Before the autoloader, not beside the test registrations that used to include
# it: modules/conformance names uf::doctest in its manifest, and a dependency
# resolved by cpp_link_module has to be a target by the time that pass runs.
include(cmake/doctest-gate.cmake)

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

cpp_embed_framework_schemas(
    ${PROJECT_NAME}_schema
    "${CMAKE_CURRENT_SOURCE_DIR}/schema"
    "${CMAKE_CURRENT_BINARY_DIR}/modules/schema"
    umbraflow-collection-fact-v1.schema.json
    umbraflow-declarative-workflow-tool-v1.schema.json
    umbraflow-fact-provenance-v1.schema.json
    umbraflow-fact-v1.schema.json
    umbraflow-journal-v1.schema.json
    umbraflow-operator-v1.schema.json
    umbraflow-policy-v1.schema.json
    umbraflow-project-registration-v1.schema.json
)

# The imported ONNX runtime is declared by modules/ocr/external, which names it
# after this repository's project. Publish the same namespaced alias the module
# targets carry, so nothing outside this repository has to spell that name.
if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
    add_library(uf::ocr_onnxruntime ALIAS ${PROJECT_NAME}_ocr_onnxruntime)
endif()

add_subdirectory(entry)

# Everything below is this repository rehearsing its own contracts. A consumer
# adding this tree as a subdirectory gets the modules it links; it does not get
# conformance-arcana in its CTest namespace, nor this repository's own tests, nor
# a compile_commands.json written into its tree.
if(PROJECT_IS_TOP_LEVEL)
    option(CPP_BUILD_TESTS "Build this repository's own tests" ON)

    enable_testing()

    set(UF_STAGED_EXAMPLE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/examples")
    set(UF_STAGED_UMBRAFLOW_PROJECT "${UF_STAGED_EXAMPLE_ROOT}/umbraflow")
    set(UF_STAGED_ARCANA_PROJECT "${UF_STAGED_EXAMPLE_ROOT}/arcana-expedition")
    foreach(EXAMPLE_PROJECT IN ITEMS umbraflow arcana-expedition)
        file(GLOB_RECURSE EXAMPLE_PROJECT_FILES CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/examples/${EXAMPLE_PROJECT}/*"
        )
        set(EXAMPLE_STAGE "${UF_STAGED_EXAMPLE_ROOT}/${EXAMPLE_PROJECT}")
        file(REMOVE_RECURSE "${EXAMPLE_STAGE}")
        file(MAKE_DIRECTORY "${EXAMPLE_STAGE}")
        file(COPY
            "${PROJECT_SOURCE_DIR}/examples/${EXAMPLE_PROJECT}/"
            DESTINATION "${EXAMPLE_STAGE}"
        )
    endforeach()

    # The Operator conformance suite's runs. The registration used to be included
    # outside this guard, so that a consumer's add_subdirectory of this
    # repository reached the function that compiled the suite into the consumer's
    # own executable. A project is now a directory of data and the suite is a
    # binary this repository ships, so no consumer add_subdirectory is left to
    # reach it: uf-chaos has had no CMake at all since its 4bb6e30. The binary
    # itself is declared in entry/ beside umbra-flow, because both are
    # executables; only these two runs are this repository rehearsing with it.
    include(cmake/conformance-run.cmake)

    # Before tests/, because tests/CMakeLists.txt ends by checking that every
    # migration-report contract ID has a CTest and some of them are registered
    # here.
    if(TARGET umbra-flow-conformance)
        # Every case the suite compiles, read off the target rather than listed:
        # modules/conformance is autoloaded, so its file set is the module's
        # glob and a second list here could only disagree with it. What a
        # concrete list used to buy -- a case cannot leave the build unnoticed --
        # is bought instead by the two checks below it: DECLARED is derived from
        # this same set, uf_add_conformance_run fails the configure when a run's
        # CASES and the declared TEST_CASEs disagree in either direction, and
        # uf_require_executed_assertions fails the run when a compiled-out case
        # leaves a gate with nothing to execute.
        get_target_property(UF_CONFORMANCE_SOURCES
            ${PROJECT_NAME}_conformance
            SOURCES
        )
        uf_declared_conformance_cases(
            UF_CONFORMANCE_DECLARED_CASES
            ${UF_CONFORMANCE_SOURCES}
        )

        # This repository's own run. It is the one that claims the migration
        # report's contract IDs as named CTests, because the IDs describe the
        # Operator this repository ships; every other run is a consumer's single
        # aggregate gate.
        #
        # C-09 through C-13 are here because their store behaviour is here. Only
        # their schema shape stayed in tests/operator, where it is registered
        # under the schema- name, so the contract- name belongs to the case that
        # goes red when the ledger stops enforcing them.
        uf_add_conformance_run(
            PROJECT   umbraflow
            DIRECTORY "${UF_STAGED_UMBRAFLOW_PROJECT}"
            DECLARED  ${UF_CONFORMANCE_DECLARED_CASES}
            CASES
                contract-control-c01
                contract-control-c06
                contract-control-c09
                contract-control-c10
                contract-control-c11
                contract-control-c12
                contract-control-c13
        )

        # Everything a consuming repository writes to run the suite, in full: one
        # call naming a directory. Both directories here are written the way a
        # consuming repository writes its own and are documentation a consumer
        # copies rather than scaffolding the suite needs -- nothing in
        # modules/conformance knows either of them by name.
        uf_add_conformance_run(
            PROJECT   arcana
            DIRECTORY "${UF_STAGED_ARCANA_PROJECT}"
        )
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
