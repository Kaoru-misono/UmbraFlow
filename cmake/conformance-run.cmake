# Registering one run of the Operator conformance suite against one project
# directory.
#
# What replaced cmake/conformance-suite.cmake, and what did not survive with it.
# A project is now a directory of data, so a run compiles nothing: there is no
# consumer translation unit to add to a target, no safety profile to apply to
# somebody else's sources, no ONNX payload to stage beside a second executable
# and no provider file to check for existence. What is left is the CTest
# registration and the two checks that earn their keep -- the
# CASES-versus-declared-TEST_CASE cross-check, and uf_require_executed_assertions
# on every gate. See docs/plans/2026-08-11-project-as-data.md 3.
#
# There is no exported entry point here any more either. A consuming repository
# does not add this repository with add_subdirectory and does not build the
# suite: it runs the umbra-flow-conformance this repository ships, against its
# own directory. This file is therefore included from inside the
# PROJECT_IS_TOP_LEVEL guard, and nothing outside this repository reaches it.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/doctest-gate.cmake")

# Every contract ID the suite's own sources declare, read out of the source
# text. That text is what a preprocessor has not seen yet, so this set answers
# "is the name spelled the same in both places" and nothing about whether the
# case is compiled; uf_require_executed_assertions below answers the second
# question on the run.
function(uf_declared_conformance_cases OUTPUT_VARIABLE)
    set(DECLARED_CASES "")
    foreach(SUITE_SOURCE IN LISTS ARGN)
        if(NOT EXISTS "${SUITE_SOURCE}")
            message(FATAL_ERROR
                "the Operator conformance suite is missing a source: ${SUITE_SOURCE}"
            )
        endif()
        file(READ "${SUITE_SOURCE}" SUITE_SOURCE_TEXT)
        string(REGEX MATCHALL
            "TEST_CASE[ \t\r\n]*\\([ \t\r\n]*\"(contract|schema)-[a-z0-9-]+\"[ \t\r\n]*\\)"
            SUITE_DECLARATIONS
            "${SUITE_SOURCE_TEXT}"
        )
        foreach(SUITE_DECLARATION IN LISTS SUITE_DECLARATIONS)
            string(REGEX MATCH
                "(contract|schema)-[a-z0-9-]+"
                DECLARED_CASE
                "${SUITE_DECLARATION}"
            )
            list(APPEND DECLARED_CASES "${DECLARED_CASE}")
        endforeach()
    endforeach()

    set(UNIQUE_DECLARED_CASES ${DECLARED_CASES})
    list(REMOVE_DUPLICATES UNIQUE_DECLARED_CASES)
    list(LENGTH DECLARED_CASES DECLARED_CASE_COUNT)
    list(LENGTH UNIQUE_DECLARED_CASES UNIQUE_DECLARED_CASE_COUNT)
    if(NOT DECLARED_CASE_COUNT EQUAL UNIQUE_DECLARED_CASE_COUNT)
        message(FATAL_ERROR
            "the Operator conformance suite declares duplicate contract TEST_CASE "
            "names: [${DECLARED_CASES}]"
        )
    endif()

    set(${OUTPUT_VARIABLE} ${DECLARED_CASES} PARENT_SCOPE)
endfunction()

# One run of the suite against one project directory.
#
#   PROJECT    names the run and its CTest gate: conformance-<PROJECT>.
#   DIRECTORY  the project directory the run is judged against.
#   DECLARED   every contract ID the suite's sources declare, as
#              uf_declared_conformance_cases read them.
#   CASES      contract IDs this run also registers one CTest each for, on top
#              of the aggregate gate. A run claims either every declared ID or
#              none of them: at most one run may claim, because two projects
#              cannot both own the CTest name contract-control-c01, so requiring
#              the claiming run to claim all of them is what turns a new case in
#              conformance/source/ into a configure error rather than a case that
#              only ever runs inside an aggregate.
function(uf_add_conformance_run)
    cmake_parse_arguments(ARG "" "PROJECT;DIRECTORY" "CASES;DECLARED" ${ARGN})

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "uf_add_conformance_run received unknown arguments: ${ARG_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT ARG_PROJECT)
        message(FATAL_ERROR "uf_add_conformance_run requires PROJECT")
    endif()
    if(NOT ARG_DIRECTORY)
        message(FATAL_ERROR
            "uf_add_conformance_run(${ARG_PROJECT}) requires DIRECTORY"
        )
    endif()
    if(NOT TARGET umbra-flow-conformance)
        message(FATAL_ERROR
            "uf_add_conformance_run(${ARG_PROJECT}) needs the "
            "umbra-flow-conformance target, which this build does not define."
        )
    endif()

    # The two root documents, by the names the loader opens them under. A run
    # against a directory holding neither would otherwise fail at run time with
    # a message about a file, where the mistake is in this call.
    foreach(PROJECT_DOCUMENT IN ITEMS umbraflow-project.json umbraflow-conformance.json)
        if(NOT EXISTS "${ARG_DIRECTORY}/${PROJECT_DOCUMENT}")
            message(FATAL_ERROR
                "uf_add_conformance_run(${ARG_PROJECT}) names a directory with no "
                "${PROJECT_DOCUMENT}: ${ARG_DIRECTORY}"
            )
        endif()
    endforeach()

    set(UNIQUE_RUN_CASES ${ARG_CASES})
    list(REMOVE_DUPLICATES UNIQUE_RUN_CASES)
    list(LENGTH ARG_CASES RUN_CASE_COUNT)
    list(LENGTH UNIQUE_RUN_CASES UNIQUE_RUN_CASE_COUNT)
    if(NOT RUN_CASE_COUNT EQUAL UNIQUE_RUN_CASE_COUNT)
        message(FATAL_ERROR
            "uf_add_conformance_run(${ARG_PROJECT}) contains duplicate CASES"
        )
    endif()

    if(ARG_CASES)
        set(SORTED_RUN_CASES ${ARG_CASES})
        set(SORTED_DECLARED_CASES ${ARG_DECLARED})
        list(SORT SORTED_RUN_CASES)
        list(SORT SORTED_DECLARED_CASES)
        if(NOT SORTED_RUN_CASES STREQUAL SORTED_DECLARED_CASES)
            message(FATAL_ERROR
                "uf_add_conformance_run(${ARG_PROJECT}) CASES do not exactly match "
                "the suite's TEST_CASE declarations; "
                "requested=[${SORTED_RUN_CASES}], declared=[${SORTED_DECLARED_CASES}]"
            )
        endif()
    endif()

    get_property(REGISTERED_CONTRACT_CASES GLOBAL PROPERTY UF_REGISTERED_CONTRACT_CASES)
    foreach(RUN_CASE IN LISTS ARG_CASES)
        if(NOT RUN_CASE MATCHES "^(contract|schema)-")
            message(FATAL_ERROR
                "uf_add_conformance_run(${ARG_PROJECT}) rejects a gate outside "
                "the contract-/schema- vocabulary: ${RUN_CASE}"
            )
        endif()
        list(FIND REGISTERED_CONTRACT_CASES "${RUN_CASE}" REGISTERED_CASE_INDEX)
        if(NOT REGISTERED_CASE_INDEX EQUAL -1)
            message(FATAL_ERROR
                "uf_add_conformance_run(${ARG_PROJECT}) duplicates registered CTest: "
                "${RUN_CASE}"
            )
        endif()
        if(TEST "${RUN_CASE}")
            message(FATAL_ERROR
                "uf_add_conformance_run(${ARG_PROJECT}) collides with existing CTest: "
                "${RUN_CASE}"
            )
        endif()
    endforeach()

    # The aggregate runs every case, including the ones named in prose that no
    # migration report ID covers, so it stays even when CASES names some of them
    # individually.
    #
    # CONFORMANCE shares no substring with CONTRACT or SCHEMA, and must not: -L
    # is a regex, so the former CONTRACT-SUITE label made `ctest -L CONTRACT`
    # report 44 where 40 is meant and a measurement was taken against the wrong
    # number before anyone noticed.
    add_test(
        NAME conformance-${ARG_PROJECT}
        COMMAND umbra-flow-conformance --project "${ARG_DIRECTORY}"
    )
    set_tests_properties(conformance-${ARG_PROJECT} PROPERTIES
        TIMEOUT 120
        LABELS "CI;CONFORMANCE"
    )
    uf_require_executed_assertions(conformance-${ARG_PROJECT})

    foreach(RUN_CASE IN LISTS ARG_CASES)
        add_test(
            NAME ${RUN_CASE}
            COMMAND umbra-flow-conformance
                --project "${ARG_DIRECTORY}"
                "--test-case=${RUN_CASE}"
        )
        # The label follows the name, so `ctest -L CONTRACT` selects the gates
        # that would go red without the behaviour and nothing else.
        if(RUN_CASE MATCHES "^schema-")
            set(RUN_CASE_LABELS "CI;SCHEMA")
        else()
            set(RUN_CASE_LABELS "CI;CONTRACT")
        endif()
        set_tests_properties(${RUN_CASE} PROPERTIES
            TIMEOUT 60
            LABELS "${RUN_CASE_LABELS}"
        )
        uf_require_executed_assertions(${RUN_CASE})
        set_property(GLOBAL APPEND PROPERTY UF_REGISTERED_CONTRACT_CASES "${RUN_CASE}")
    endforeach()
endfunction()
