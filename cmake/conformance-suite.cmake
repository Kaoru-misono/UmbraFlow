# The exported Operator conformance suite.
#
# Requirement P-05 is uf-chaos and a second game reaching the Operator through
# one conformance suite. That is only true if a repository other than this one can
# run the suite against its own ProjectRegistration and its own plugin, so the
# suite is parameterized by a project rather than by a fixture: the consumer
# writes one translation unit defining
# uf::operator_runtime::conformance::provideProject, and calls the function
# below.
#
# A consuming repository never includes this file. It adds this repository with
# add_subdirectory, which defines uf_add_conformance_suite for every
# directory in the build, and calls it. Nothing here is left in a directory
# scope for that reason: a variable set while this file is read reaches this
# repository's own directories and no consumer's, and include_guard(GLOBAL)
# would then make a consumer's own include of it a silent no-op with an empty
# source list. Every path and target the function needs is resolved inside it.
#
# The suite's translation units are compiled into the consumer's executable
# rather than shipped as a library, because this repository installs nothing and
# every module is type = static -- the Operator's private dependencies propagate
# as link-only, so a binary package would carry SQLite, stb, Luau and a vendored
# ONNX runtime payload before it carried a single contract. The binary is
# compiled with this repository's safety profile as the surrounding build
# configures it (CPP_WARNINGS_AS_ERRORS, CPP_ENABLE_HARDENING and the sanitizer
# options are cache entries of that build); the target belongs to the consumer,
# so anything beyond that is theirs to add to it afterwards.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/doctest-gate.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/platform.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/runtime-payload.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/safety-profile.cmake")

# One run of the suite against one project.
#
#   PROJECT  names the run and its CTest gate: conformance-<PROJECT>.
#   SOURCES  the consumer's provider translation units.
#   LIBS     anything the provider needs beyond what the suite already links.
#   CASES    contract IDs this run also registers one CTest each for, on top of
#            the aggregate gate. A run claims either every ID the suite declares
#            or none of them: at most one run may claim, because two projects
#            cannot both own the CTest name contract-control-c01, so requiring
#            the claiming run to claim all of them is what turns a new case in
#            conformance/source/ into a configure error rather than a case
#            that only ever runs inside an aggregate. This repository's own run
#            claims; every other run is the single gate a consumer gets.
function(uf_add_conformance_suite)
    cmake_parse_arguments(ARG "" "TARGET;PROJECT" "SOURCES;LIBS;CASES" ${ARGN})

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "uf_add_conformance_suite received unknown arguments: ${ARG_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT ARG_TARGET)
        message(FATAL_ERROR "uf_add_conformance_suite requires TARGET")
    endif()
    if(NOT ARG_PROJECT)
        message(FATAL_ERROR
            "uf_add_conformance_suite(${ARG_TARGET}) requires PROJECT"
        )
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR
            "uf_add_conformance_suite(${ARG_TARGET}) requires the provider SOURCES "
            "that define uf::operator_runtime::conformance::provideProject"
        )
    endif()
    if(NOT TARGET uf::operator)
        message(FATAL_ERROR
            "uf_add_conformance_suite(${ARG_TARGET}) needs uf::operator, which "
            "this build does not define. Add this repository with add_subdirectory "
            "before calling it."
        )
    endif()

    cmake_path(SET SUITE_ROOT NORMALIZE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../conformance"
    )
    set(SUITE_INCLUDE_DIR "${SUITE_ROOT}/include")
    set(SUITE_SOURCE_DIR "${SUITE_ROOT}/source")

    # Every case the suite runs. Concrete paths, never a glob: a case that stops
    # being compiled must be a visible deletion here rather than a file that
    # quietly left the build.
    set(SUITE_SOURCES
        "${SUITE_SOURCE_DIR}/suite-main.cpp"
        "${SUITE_SOURCE_DIR}/suite-support.cpp"
        "${SUITE_SOURCE_DIR}/suite-control-ledger.cpp"
        "${SUITE_SOURCE_DIR}/suite-project-authority.cpp"
    )
    foreach(SUITE_SOURCE IN LISTS SUITE_SOURCES)
        if(NOT EXISTS "${SUITE_SOURCE}")
            message(FATAL_ERROR
                "the Operator conformance suite is missing a source: ${SUITE_SOURCE}"
            )
        endif()
    endforeach()

    # Every contract ID the suite's own sources declare, read out of the source
    # text. That text is what a preprocessor has not seen yet, so this set
    # answers "is the name spelled the same in both places" and nothing about
    # whether the case is compiled; uf_require_executed_assertions below answers
    # the second question on the run.
    set(SUITE_DECLARED_CASES "")
    foreach(SUITE_SOURCE IN LISTS SUITE_SOURCES)
        file(READ "${SUITE_SOURCE}" SUITE_SOURCE_TEXT)
        string(REGEX MATCHALL
            "TEST_CASE[ \t\r\n]*\\([ \t\r\n]*\"(contract|schema)-[a-z0-9-]+\"[ \t\r\n]*\\)"
            SUITE_DECLARATIONS
            "${SUITE_SOURCE_TEXT}"
        )
        foreach(SUITE_DECLARATION IN LISTS SUITE_DECLARATIONS)
            string(REGEX MATCH
                "(contract|schema)-[a-z0-9-]+"
                SUITE_DECLARED_CASE
                "${SUITE_DECLARATION}"
            )
            list(APPEND SUITE_DECLARED_CASES "${SUITE_DECLARED_CASE}")
        endforeach()
    endforeach()
    set(UNIQUE_SUITE_DECLARED_CASES ${SUITE_DECLARED_CASES})
    list(REMOVE_DUPLICATES UNIQUE_SUITE_DECLARED_CASES)
    list(LENGTH SUITE_DECLARED_CASES SUITE_DECLARED_CASE_COUNT)
    list(LENGTH UNIQUE_SUITE_DECLARED_CASES UNIQUE_SUITE_DECLARED_CASE_COUNT)
    if(NOT SUITE_DECLARED_CASE_COUNT EQUAL UNIQUE_SUITE_DECLARED_CASE_COUNT)
        message(FATAL_ERROR
            "the Operator conformance suite declares duplicate contract TEST_CASE names: "
            "[${SUITE_DECLARED_CASES}]"
        )
    endif()

    set(UNIQUE_SUITE_CASES ${ARG_CASES})
    list(REMOVE_DUPLICATES UNIQUE_SUITE_CASES)
    list(LENGTH ARG_CASES SUITE_CASE_COUNT)
    list(LENGTH UNIQUE_SUITE_CASES UNIQUE_SUITE_CASE_COUNT)
    if(NOT SUITE_CASE_COUNT EQUAL UNIQUE_SUITE_CASE_COUNT)
        message(FATAL_ERROR
            "uf_add_conformance_suite(${ARG_TARGET}) contains duplicate CASES"
        )
    endif()

    if(ARG_CASES)
        set(SORTED_SUITE_CASES ${ARG_CASES})
        set(SORTED_SUITE_DECLARED_CASES ${SUITE_DECLARED_CASES})
        list(SORT SORTED_SUITE_CASES)
        list(SORT SORTED_SUITE_DECLARED_CASES)
        if(NOT SORTED_SUITE_CASES STREQUAL SORTED_SUITE_DECLARED_CASES)
            message(FATAL_ERROR
                "uf_add_conformance_suite(${ARG_TARGET}) CASES do not exactly match "
                "the suite's TEST_CASE declarations; "
                "requested=[${SORTED_SUITE_CASES}], declared=[${SORTED_SUITE_DECLARED_CASES}]"
            )
        endif()
    endif()

    get_property(REGISTERED_CONTRACT_CASES GLOBAL PROPERTY UF_REGISTERED_CONTRACT_CASES)
    foreach(SUITE_CASE IN LISTS ARG_CASES)
        if(NOT SUITE_CASE MATCHES "^(contract|schema)-")
            message(FATAL_ERROR
                "uf_add_conformance_suite(${ARG_TARGET}) rejects a gate outside "
                "the contract-/schema- vocabulary: ${SUITE_CASE}"
            )
        endif()
        list(FIND REGISTERED_CONTRACT_CASES "${SUITE_CASE}" REGISTERED_CASE_INDEX)
        if(NOT REGISTERED_CASE_INDEX EQUAL -1)
            message(FATAL_ERROR
                "uf_add_conformance_suite(${ARG_TARGET}) duplicates registered CTest: "
                "${SUITE_CASE}"
            )
        endif()
        if(TEST "${SUITE_CASE}")
            message(FATAL_ERROR
                "uf_add_conformance_suite(${ARG_TARGET}) collides with existing CTest: "
                "${SUITE_CASE}"
            )
        endif()
    endforeach()

    foreach(PROVIDER_SOURCE IN LISTS ARG_SOURCES)
        if(IS_ABSOLUTE "${PROVIDER_SOURCE}")
            set(PROVIDER_SOURCE_PATH "${PROVIDER_SOURCE}")
        else()
            set(PROVIDER_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${PROVIDER_SOURCE}")
        endif()
        if(NOT EXISTS "${PROVIDER_SOURCE_PATH}")
            message(FATAL_ERROR
                "uf_add_conformance_suite(${ARG_TARGET}) provider source does not exist: "
                "${PROVIDER_SOURCE}"
            )
        endif()
    endforeach()

    add_executable(${ARG_TARGET}
        ${SUITE_SOURCES}
        ${ARG_SOURCES}
    )
    cpp_apply_safety_profile(${ARG_TARGET})
    cpp_apply_utf8_encoding(${ARG_TARGET})

    # The compile definitions this repository's own directories carry. They do
    # not reach a target declared in a consumer's directory, and the suite's
    # translation units are this repository's sources wherever they compile.
    if(WIN32)
        target_compile_definitions(${ARG_TARGET} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE
        )
    endif()

    target_compile_definitions(${ARG_TARGET} PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
    target_include_directories(${ARG_TARGET} PRIVATE
        "${SUITE_INCLUDE_DIR}"
        "${SUITE_SOURCE_DIR}"
    )
    # uf::image and uf::deployment are the suite's own dependencies, not the
    # provider's: suite-support.cpp reaches observation-fixture.hpp, which
    # encodes the fixture's template assets with <image/png.hpp>, and
    # operator-protocol.hpp builds its plan authority out of the deployment's
    # two operator protocol readers. The Operator links neither, and must not
    # link the second.
    target_link_libraries(${ARG_TARGET} PRIVATE
        uf::operator
        uf::image
        uf::deployment
        uf::doctest
        ${ARG_LIBS}
    )
    target_compile_features(${ARG_TARGET} PRIVATE cxx_std_23)

    # The Operator links task publicly, which links engine and ocr publicly, and
    # onnxruntime resolves beside the executable rather than from PATH. The list
    # is empty exactly when the ocr module declared no payload, and staging then
    # does nothing -- so there is no condition here that could be false for the
    # caller the staging exists for.
    cpp_stage_runtime_libraries(${ARG_TARGET} ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})

    # The aggregate runs every case, including the ones named in prose that no
    # migration report ID covers, so it stays even when CASES names some of them
    # individually.
    #
    # CONFORMANCE shares no substring with CONTRACT or SCHEMA, and must not: -L
    # is a regex, so the former CONTRACT-SUITE label made `ctest -L CONTRACT`
    # report 44 where 40 is meant and a measurement was taken against the wrong
    # number before anyone noticed.
    add_test(NAME conformance-${ARG_PROJECT} COMMAND ${ARG_TARGET})
    set_tests_properties(conformance-${ARG_PROJECT} PROPERTIES
        TIMEOUT 120
        LABELS "CI;CONFORMANCE"
    )
    uf_require_executed_assertions(conformance-${ARG_PROJECT})

    foreach(SUITE_CASE IN LISTS ARG_CASES)
        add_test(
            NAME ${SUITE_CASE}
            COMMAND ${ARG_TARGET} "--test-case=${SUITE_CASE}"
        )
        # The label follows the name, so `ctest -L CONTRACT` selects the gates
        # that would go red without the behaviour and nothing else.
        if(SUITE_CASE MATCHES "^schema-")
            set(SUITE_CASE_LABELS "CI;SCHEMA")
        else()
            set(SUITE_CASE_LABELS "CI;CONTRACT")
        endif()
        set_tests_properties(${SUITE_CASE} PROPERTIES
            TIMEOUT 60
            LABELS "${SUITE_CASE_LABELS}"
        )
        uf_require_executed_assertions(${SUITE_CASE})
        set_property(GLOBAL APPEND PROPERTY UF_REGISTERED_CONTRACT_CASES "${SUITE_CASE}")
    endforeach()
endfunction()
