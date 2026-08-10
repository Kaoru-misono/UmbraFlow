# The exported Operator contract suite.
#
# Requirement P-05 is uf-chaos and a second game reaching the Operator through
# one contract suite. That is only true if a repository other than this one can
# run the suite against its own ProjectRegistration and its own plugin, so the
# suite is parameterized by a project rather than by a fixture: the consumer
# writes one translation unit defining
# uf::operator_runtime::contract::projectUnderTest, and calls the function
# below.
#
# The suite's translation units are compiled into the consumer's executable
# rather than shipped as a library. A test binary must be built with the
# consumer's own safety profile and sanitizers to mean anything, and this
# repository installs nothing today -- the Operator's public headers reach
# task, engine and ocr, so a binary package would have to carry a vendored
# ONNX runtime payload before it carried a single contract.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/doctest-gate.cmake")

set(UF_CONTRACT_SUITE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../contract-suite")
set(UF_CONTRACT_SUITE_INCLUDE_DIR "${UF_CONTRACT_SUITE_ROOT}/include")
set(UF_CONTRACT_SUITE_SOURCE_DIR "${UF_CONTRACT_SUITE_ROOT}/source")
set(UF_CONTRACT_SUITE_DOCTEST_DIR "${CMAKE_CURRENT_LIST_DIR}/../tests/external/doctest")
set(UF_CONTRACT_SUITE_OPERATOR_TARGET "${PROJECT_NAME}_operator")

# Every case the suite runs. Concrete paths, never a glob: a case that stops
# being compiled must be a visible deletion here rather than a file that
# quietly left the build.
set(UF_CONTRACT_SUITE_SOURCES
    "${UF_CONTRACT_SUITE_SOURCE_DIR}/suite-main.cpp"
    "${UF_CONTRACT_SUITE_SOURCE_DIR}/harness.cpp"
    "${UF_CONTRACT_SUITE_SOURCE_DIR}/suite-control-ledger.cpp"
    "${UF_CONTRACT_SUITE_SOURCE_DIR}/suite-project-authority.cpp"
)
foreach(UF_CONTRACT_SUITE_SOURCE IN LISTS UF_CONTRACT_SUITE_SOURCES)
    if(NOT EXISTS "${UF_CONTRACT_SUITE_SOURCE}")
        message(FATAL_ERROR
            "the Operator contract suite is missing a source: ${UF_CONTRACT_SUITE_SOURCE}"
        )
    endif()
endforeach()

# Every contract ID the suite's own sources declare, read out of the source
# text. That text is what a preprocessor has not seen yet, so this set answers
# "is the name spelled the same in both places" and nothing about whether the
# case is compiled; uf_require_executed_assertions below answers the second
# question on the run.
set(UF_CONTRACT_SUITE_DECLARED_CASES "")
foreach(UF_CONTRACT_SUITE_SOURCE IN LISTS UF_CONTRACT_SUITE_SOURCES)
    file(READ "${UF_CONTRACT_SUITE_SOURCE}" UF_CONTRACT_SUITE_TEXT)
    string(REGEX MATCHALL
        "TEST_CASE[ \t\r\n]*\\([ \t\r\n]*\"(contract|schema)-[a-z0-9-]+\"[ \t\r\n]*\\)"
        UF_CONTRACT_SUITE_DECLARATIONS
        "${UF_CONTRACT_SUITE_TEXT}"
    )
    foreach(UF_CONTRACT_SUITE_DECLARATION IN LISTS UF_CONTRACT_SUITE_DECLARATIONS)
        string(REGEX MATCH
            "(contract|schema)-[a-z0-9-]+"
            UF_CONTRACT_SUITE_DECLARED_CASE
            "${UF_CONTRACT_SUITE_DECLARATION}"
        )
        list(APPEND UF_CONTRACT_SUITE_DECLARED_CASES "${UF_CONTRACT_SUITE_DECLARED_CASE}")
    endforeach()
endforeach()
set(UF_CONTRACT_SUITE_UNIQUE_DECLARED_CASES ${UF_CONTRACT_SUITE_DECLARED_CASES})
list(REMOVE_DUPLICATES UF_CONTRACT_SUITE_UNIQUE_DECLARED_CASES)
list(LENGTH UF_CONTRACT_SUITE_DECLARED_CASES UF_CONTRACT_SUITE_DECLARED_CASE_COUNT)
list(LENGTH UF_CONTRACT_SUITE_UNIQUE_DECLARED_CASES UF_CONTRACT_SUITE_UNIQUE_DECLARED_CASE_COUNT)
if(NOT UF_CONTRACT_SUITE_DECLARED_CASE_COUNT EQUAL UF_CONTRACT_SUITE_UNIQUE_DECLARED_CASE_COUNT)
    message(FATAL_ERROR
        "the Operator contract suite declares duplicate contract TEST_CASE names: "
        "[${UF_CONTRACT_SUITE_DECLARED_CASES}]"
    )
endif()

# One run of the suite against one project.
#
#   PROJECT  names the run and its CTest gate: contract-suite-<PROJECT>.
#   SOURCES  the consumer's provider translation units.
#   LIBS     anything the provider needs beyond the Operator.
#   CASES    contract IDs this run also registers one CTest each for, on top of
#            the aggregate gate. A run claims either every ID the suite declares
#            or none of them: at most one run may claim, because two projects
#            cannot both own the CTest name contract-control-c01, so requiring
#            the claiming run to claim all of them is what turns a new case in
#            contract-suite/source/ into a configure error rather than a case
#            that only ever runs inside an aggregate. This repository's own run
#            claims; every other run is the single gate a consumer gets.
function(uf_add_operator_contract_suite)
    cmake_parse_arguments(ARG "" "TARGET;PROJECT" "SOURCES;LIBS;CASES" ${ARGN})

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "uf_add_operator_contract_suite received unknown arguments: ${ARG_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT ARG_TARGET)
        message(FATAL_ERROR "uf_add_operator_contract_suite requires TARGET")
    endif()
    if(NOT ARG_PROJECT)
        message(FATAL_ERROR
            "uf_add_operator_contract_suite(${ARG_TARGET}) requires PROJECT"
        )
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR
            "uf_add_operator_contract_suite(${ARG_TARGET}) requires the provider SOURCES "
            "that define uf::operator_runtime::contract::projectUnderTest"
        )
    endif()
    if(NOT TARGET ${UF_CONTRACT_SUITE_OPERATOR_TARGET})
        message(FATAL_ERROR
            "uf_add_operator_contract_suite(${ARG_TARGET}) needs "
            "${UF_CONTRACT_SUITE_OPERATOR_TARGET}, which this build does not define"
        )
    endif()

    set(UNIQUE_SUITE_CASES ${ARG_CASES})
    list(REMOVE_DUPLICATES UNIQUE_SUITE_CASES)
    list(LENGTH ARG_CASES SUITE_CASE_COUNT)
    list(LENGTH UNIQUE_SUITE_CASES UNIQUE_SUITE_CASE_COUNT)
    if(NOT SUITE_CASE_COUNT EQUAL UNIQUE_SUITE_CASE_COUNT)
        message(FATAL_ERROR
            "uf_add_operator_contract_suite(${ARG_TARGET}) contains duplicate CASES"
        )
    endif()

    if(ARG_CASES)
        set(SORTED_SUITE_CASES ${ARG_CASES})
        set(SORTED_SUITE_DECLARED_CASES ${UF_CONTRACT_SUITE_DECLARED_CASES})
        list(SORT SORTED_SUITE_CASES)
        list(SORT SORTED_SUITE_DECLARED_CASES)
        if(NOT SORTED_SUITE_CASES STREQUAL SORTED_SUITE_DECLARED_CASES)
            message(FATAL_ERROR
                "uf_add_operator_contract_suite(${ARG_TARGET}) CASES do not exactly match "
                "the suite's TEST_CASE declarations; "
                "requested=[${SORTED_SUITE_CASES}], declared=[${SORTED_SUITE_DECLARED_CASES}]"
            )
        endif()
    endif()

    get_property(REGISTERED_CONTRACT_CASES GLOBAL PROPERTY UF_REGISTERED_CONTRACT_CASES)
    foreach(SUITE_CASE IN LISTS ARG_CASES)
        if(NOT SUITE_CASE MATCHES "^(contract|schema)-")
            message(FATAL_ERROR
                "uf_add_operator_contract_suite(${ARG_TARGET}) rejects a gate outside "
                "the contract-/schema- vocabulary: ${SUITE_CASE}"
            )
        endif()
        list(FIND REGISTERED_CONTRACT_CASES "${SUITE_CASE}" REGISTERED_CASE_INDEX)
        if(NOT REGISTERED_CASE_INDEX EQUAL -1)
            message(FATAL_ERROR
                "uf_add_operator_contract_suite(${ARG_TARGET}) duplicates registered CTest: "
                "${SUITE_CASE}"
            )
        endif()
        if(TEST "${SUITE_CASE}")
            message(FATAL_ERROR
                "uf_add_operator_contract_suite(${ARG_TARGET}) collides with existing CTest: "
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
                "uf_add_operator_contract_suite(${ARG_TARGET}) provider source does not exist: "
                "${PROVIDER_SOURCE}"
            )
        endif()
    endforeach()

    add_executable(${ARG_TARGET}
        ${UF_CONTRACT_SUITE_SOURCES}
        ${ARG_SOURCES}
        ${CPP_UTF8_RC}
    )
    cpp_apply_safety_profile(${ARG_TARGET})
    cpp_apply_utf8_manifest(${ARG_TARGET})
    target_compile_definitions(${ARG_TARGET} PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
    target_include_directories(${ARG_TARGET} PRIVATE
        "${UF_CONTRACT_SUITE_INCLUDE_DIR}"
        "${UF_CONTRACT_SUITE_SOURCE_DIR}"
    )
    target_include_directories(${ARG_TARGET} SYSTEM PRIVATE
        "${UF_CONTRACT_SUITE_DOCTEST_DIR}"
    )
    target_link_libraries(${ARG_TARGET} PRIVATE
        ${UF_CONTRACT_SUITE_OPERATOR_TARGET}
        ${ARG_LIBS}
    )
    target_compile_features(${ARG_TARGET} PRIVATE cxx_std_23)

    # The Operator links task publicly, which links engine and ocr publicly, and
    # onnxruntime resolves beside the executable rather than from PATH.
    if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
        cpp_stage_runtime_libraries(${ARG_TARGET} ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
    endif()

    # The aggregate runs every case, including the ones named in prose that no
    # migration report ID covers, so it stays even when CASES names some of them
    # individually.
    add_test(NAME contract-suite-${ARG_PROJECT} COMMAND ${ARG_TARGET})
    set_tests_properties(contract-suite-${ARG_PROJECT} PROPERTIES
        TIMEOUT 120
        LABELS "CI;CONTRACT-SUITE"
    )
    uf_require_executed_assertions(contract-suite-${ARG_PROJECT})

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
