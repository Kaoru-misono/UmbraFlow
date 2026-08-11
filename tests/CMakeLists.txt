include("${PROJECT_SOURCE_DIR}/cmake/doctest-gate.cmake")

function(cpp_add_test)
    cmake_parse_arguments(ARG "NO_CTEST" "NAME" "SOURCES;LIBS" ${ARGN})

    foreach(TEST_SOURCE IN LISTS ARG_SOURCES)
        if(TEST_SOURCE MATCHES "\\$<")
            message(FATAL_ERROR
                "cpp_add_test(${ARG_NAME}) requires concrete source paths"
            )
        endif()
        if(IS_ABSOLUTE "${TEST_SOURCE}")
            set(TEST_SOURCE_PATH "${TEST_SOURCE}")
        else()
            set(TEST_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_SOURCE}")
        endif()
        if(NOT EXISTS "${TEST_SOURCE_PATH}")
            message(FATAL_ERROR
                "cpp_add_test(${ARG_NAME}) source does not exist: ${TEST_SOURCE}"
            )
        endif()
    endforeach()

    add_executable(${ARG_NAME}
        test-main.cpp
        ${ARG_SOURCES}
    )
    cpp_apply_safety_profile(${ARG_NAME})
    cpp_apply_utf8_encoding(${ARG_NAME})
    target_compile_definitions(${ARG_NAME} PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
    target_link_libraries(${ARG_NAME} PRIVATE uf::doctest ${ARG_LIBS})
    target_compile_features(${ARG_NAME} PRIVATE cxx_std_23)

    if(NOT ARG_NO_CTEST)
        add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME})
        set_tests_properties(${ARG_NAME} PROPERTIES
            TIMEOUT 60
            LABELS "CI"
        )
        uf_require_executed_assertions(${ARG_NAME})
    endif()
endfunction()

# Every gate a requirement owns, spelled exactly. Two prefixes, one vocabulary:
#
#   contract-<area>-<id>  exercises the code and goes red when the behaviour is
#                         removed.
#   schema-<area>-<id>    only reads schema/*.json and asserts that a definition
#                         exists with certain members. It passes whether or not
#                         the behaviour exists, which is why it may not wear the
#                         contract- name.
#
# One requirement may own one gate of each kind. C-09 through C-13 do: their
# store behaviour lives in the exported suite, which registers the contract-
# name, while their schema shape stayed in tests/operator and takes the schema-
# name.
#
# Which kind a case belongs to is not mechanically checked, and cannot be from
# here: neither CMake nor a doctest binary can read a case body and decide
# whether its assertions reach production code, and "goes red when the behaviour
# is removed" is a statement about a tree that does not exist yet. A contract-
# gate whose assertions only read a schema file is caught by review and by
# nothing else. What this list does enforce is that the registered CTest names
# are exactly these and are spelled in the vocabulary; what
# UF_SCHEMA_ONLY_REQUIREMENTS enforces is that the set of requirements holding
# no contract- gate at all cannot change without an edit here.
set(UF_REQUIRED_DOCTEST_CONTRACTS
    contract-product-p01
    contract-product-p02
    contract-product-p03
    contract-product-p04
    contract-product-p05-fixtures
    contract-product-p06
    schema-product-p01
    schema-product-p02
    schema-product-p03
    contract-runtime-u01
    contract-runtime-u02
    contract-runtime-u03
    contract-runtime-u04
    contract-runtime-u05
    contract-runtime-u06
    contract-runtime-u07
    contract-runtime-u08
    contract-state-s01
    contract-state-s02
    contract-state-s03
    contract-state-s05
    contract-state-s06
    contract-state-s04
    schema-state-s01
    schema-state-s02
    schema-state-s04
    contract-control-c01
    contract-control-c02
    contract-control-c03
    contract-control-c04
    contract-control-c05
    contract-control-c06
    contract-control-c07
    contract-control-c08
    contract-control-c09
    contract-control-c10
    contract-control-c11
    contract-control-c12
    contract-control-c13
    contract-control-c14
    schema-control-c03
    schema-control-c05
    schema-control-c08
    schema-control-c09
    schema-control-c10
    schema-control-c11
    schema-control-c12
    schema-control-c13
    contract-agent-a01
    contract-agent-a02
    contract-agent-a04
    contract-agent-a06
    contract-agent-a07
    contract-agent-a08
    schema-agent-a01
    schema-agent-a02
    schema-agent-a03
    schema-agent-a05
    schema-agent-a07
)

# The 42 REQUIRED_CORE requirements, prefix stripped. The list above says which
# kind of gate each one owns; this one says that none of them lost every gate.
# Deriving it from the list above would make it unfalsifiable, so it is written
# out: dropping both of a requirement's gates has to fail here.
set(UF_REQUIRED_CORE_REQUIREMENTS
    product-p01
    product-p02
    product-p03
    product-p04
    product-p05-fixtures
    product-p06
    runtime-u01
    runtime-u02
    runtime-u03
    runtime-u04
    runtime-u05
    runtime-u06
    runtime-u07
    runtime-u08
    state-s01
    state-s02
    state-s03
    state-s04
    state-s05
    state-s06
    control-c01
    control-c02
    control-c03
    control-c04
    control-c05
    control-c06
    control-c07
    control-c08
    control-c09
    control-c10
    control-c11
    control-c12
    control-c13
    control-c14
    agent-a01
    agent-a02
    agent-a03
    agent-a04
    agent-a05
    agent-a06
    agent-a07
    agent-a08
)
list(LENGTH UF_REQUIRED_CORE_REQUIREMENTS UF_REQUIRED_CORE_REQUIREMENT_COUNT)
if(NOT UF_REQUIRED_CORE_REQUIREMENT_COUNT EQUAL 42)
    message(FATAL_ERROR
        "the requirement matrix has 42 REQUIRED_CORE entries; this list has "
        "${UF_REQUIRED_CORE_REQUIREMENT_COUNT}"
    )
endif()

# The requirements that own no contract- gate: every assertion standing behind
# them reads a schema file, so they are covered in name and unenforced in fact.
# The check at the end of this file derives the same set from the registered
# gates and requires the two to agree, in both directions. That is as far as the
# prefix can be held to account from here -- it makes a reclassification, which
# is one keystroke on a name, an explicit edit to this list.
set(UF_SCHEMA_ONLY_REQUIREMENTS
    agent-a03
    agent-a05
)

# A suite is valid only when its requested IDs and concrete doctest declarations
# are the same set. The global registry also rejects placeholders, cross-suite
# duplicates, and migration-report omissions at configure time.
function(cpp_add_contract_suite)
    cmake_parse_arguments(ARG "" "TARGET" "CASES;SOURCES;LIBS" ${ARGN})

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "cpp_add_contract_suite received unknown arguments: ${ARG_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT ARG_TARGET)
        message(FATAL_ERROR
            "cpp_add_contract_suite requires TARGET"
        )
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR
            "cpp_add_contract_suite(${ARG_TARGET}) requires concrete SOURCES"
        )
    endif()
    if(NOT ARG_CASES)
        message(FATAL_ERROR
            "cpp_add_contract_suite(${ARG_TARGET}) requires exact CASES"
        )
    endif()

    set(UNIQUE_CONTRACT_SOURCES ${ARG_SOURCES})
    list(REMOVE_DUPLICATES UNIQUE_CONTRACT_SOURCES)
    list(LENGTH ARG_SOURCES CONTRACT_SOURCE_COUNT)
    list(LENGTH UNIQUE_CONTRACT_SOURCES UNIQUE_CONTRACT_SOURCE_COUNT)
    if(NOT CONTRACT_SOURCE_COUNT EQUAL UNIQUE_CONTRACT_SOURCE_COUNT)
        message(FATAL_ERROR
            "cpp_add_contract_suite(${ARG_TARGET}) contains duplicate SOURCES"
        )
    endif()

    set(CONTRACT_SOURCE_TEXT "")
    foreach(CONTRACT_SOURCE IN LISTS ARG_SOURCES)
        if(CONTRACT_SOURCE MATCHES "\\$<")
            message(FATAL_ERROR
                "cpp_add_contract_suite(${ARG_TARGET}) requires concrete source paths"
            )
        endif()
        if(IS_ABSOLUTE "${CONTRACT_SOURCE}")
            set(CONTRACT_SOURCE_PATH "${CONTRACT_SOURCE}")
        else()
            set(CONTRACT_SOURCE_PATH
                "${CMAKE_CURRENT_SOURCE_DIR}/${CONTRACT_SOURCE}"
            )
        endif()
        if(NOT EXISTS "${CONTRACT_SOURCE_PATH}")
            message(FATAL_ERROR
                "cpp_add_contract_suite(${ARG_TARGET}) source does not exist: ${CONTRACT_SOURCE}"
            )
        endif()
        file(READ "${CONTRACT_SOURCE_PATH}" CONTRACT_SOURCE_CONTENT)
        string(APPEND CONTRACT_SOURCE_TEXT "\n${CONTRACT_SOURCE_CONTENT}")
    endforeach()

    set(UNIQUE_CONTRACT_CASES ${ARG_CASES})
    list(REMOVE_DUPLICATES UNIQUE_CONTRACT_CASES)
    list(LENGTH ARG_CASES CONTRACT_CASE_COUNT)
    list(LENGTH UNIQUE_CONTRACT_CASES UNIQUE_CONTRACT_CASE_COUNT)
    if(NOT CONTRACT_CASE_COUNT EQUAL UNIQUE_CONTRACT_CASE_COUNT)
        message(FATAL_ERROR
            "cpp_add_contract_suite(${ARG_TARGET}) contains duplicate CASES"
        )
    endif()

    string(REGEX MATCHALL
        "TEST_CASE[ \t\r\n]*\\([ \t\r\n]*\"(contract|schema)-[a-z0-9-]+\"[ \t\r\n]*\\)"
        CONTRACT_CASE_DECLARATIONS
        "${CONTRACT_SOURCE_TEXT}"
    )
    set(DECLARED_CONTRACT_CASES "")
    foreach(CONTRACT_CASE_DECLARATION IN LISTS CONTRACT_CASE_DECLARATIONS)
        string(REGEX MATCH
            "(contract|schema)-[a-z0-9-]+"
            DECLARED_CONTRACT_CASE
            "${CONTRACT_CASE_DECLARATION}"
        )
        list(APPEND DECLARED_CONTRACT_CASES "${DECLARED_CONTRACT_CASE}")
    endforeach()

    set(UNIQUE_DECLARED_CONTRACT_CASES ${DECLARED_CONTRACT_CASES})
    list(REMOVE_DUPLICATES UNIQUE_DECLARED_CONTRACT_CASES)
    list(LENGTH DECLARED_CONTRACT_CASES DECLARED_CONTRACT_CASE_COUNT)
    list(LENGTH UNIQUE_DECLARED_CONTRACT_CASES UNIQUE_DECLARED_CONTRACT_CASE_COUNT)
    if(NOT DECLARED_CONTRACT_CASE_COUNT EQUAL UNIQUE_DECLARED_CONTRACT_CASE_COUNT)
        message(FATAL_ERROR
            "cpp_add_contract_suite(${ARG_TARGET}) sources declare duplicate contract TEST_CASE names"
        )
    endif()

    set(SORTED_REQUESTED_CONTRACT_CASES ${ARG_CASES})
    set(SORTED_DECLARED_CONTRACT_CASES ${DECLARED_CONTRACT_CASES})
    list(SORT SORTED_REQUESTED_CONTRACT_CASES)
    list(SORT SORTED_DECLARED_CONTRACT_CASES)
    if(NOT SORTED_REQUESTED_CONTRACT_CASES STREQUAL SORTED_DECLARED_CONTRACT_CASES)
        message(FATAL_ERROR
            "cpp_add_contract_suite(${ARG_TARGET}) CASES do not exactly match source TEST_CASE declarations; "
            "requested=[${SORTED_REQUESTED_CONTRACT_CASES}], declared=[${SORTED_DECLARED_CONTRACT_CASES}]"
        )
    endif()

    get_property(REGISTERED_CONTRACT_CASES GLOBAL PROPERTY UF_REGISTERED_CONTRACT_CASES)
    foreach(CONTRACT_CASE IN LISTS ARG_CASES)
        if(NOT CONTRACT_CASE MATCHES "^(contract|schema)-")
            message(FATAL_ERROR
                "cpp_add_contract_suite(${ARG_TARGET}) rejects a gate outside the "
                "contract-/schema- vocabulary: ${CONTRACT_CASE}"
            )
        endif()
        list(FIND UF_REQUIRED_DOCTEST_CONTRACTS "${CONTRACT_CASE}" REQUIRED_CASE_INDEX)
        if(REQUIRED_CASE_INDEX EQUAL -1)
            message(FATAL_ERROR
                "cpp_add_contract_suite(${ARG_TARGET}) rejects contract ID outside the migration report: ${CONTRACT_CASE}"
            )
        endif()
        list(FIND REGISTERED_CONTRACT_CASES "${CONTRACT_CASE}" REGISTERED_CASE_INDEX)
        if(NOT REGISTERED_CASE_INDEX EQUAL -1)
            message(FATAL_ERROR
                "cpp_add_contract_suite(${ARG_TARGET}) duplicates registered CTest: ${CONTRACT_CASE}"
            )
        endif()
        if(TEST "${CONTRACT_CASE}")
            message(FATAL_ERROR
                "cpp_add_contract_suite(${ARG_TARGET}) collides with existing CTest: ${CONTRACT_CASE}"
            )
        endif()
    endforeach()

    cpp_add_test(
        NO_CTEST
        NAME ${ARG_TARGET}
        SOURCES ${ARG_SOURCES}
        LIBS ${ARG_LIBS}
    )

    # The aggregate runs every compiled case, including the ones no migration
    # report ID names. Without it the per-CASE gates are the only execution, and
    # a TEST_CASE outside CASES is compiled, linked, and run by nothing --
    # test-contract-operator carried seven such cases and 416 assertions until
    # 2026-08-10. uf_add_conformance_suite already registers the same gate
    # for the exported suite; this is the sibling that was missing it.
    #
    # CONFORMANCE shares no substring with CONTRACT or SCHEMA, and must not: -L
    # is a regex, so the former CONTRACT-SUITE label made `ctest -L CONTRACT`
    # report 44 where 40 is meant.
    add_test(NAME ${ARG_TARGET} COMMAND ${ARG_TARGET})
    set_tests_properties(${ARG_TARGET} PROPERTIES
        TIMEOUT 120
        LABELS "CI;CONFORMANCE"
    )
    uf_require_executed_assertions(${ARG_TARGET})

    foreach(CONTRACT_CASE IN LISTS ARG_CASES)
        add_test(
            NAME ${CONTRACT_CASE}
            COMMAND ${ARG_TARGET} "--test-case=${CONTRACT_CASE}"
        )
        # The label follows the name, so `ctest -L CONTRACT` selects the gates
        # that would go red without the behaviour and nothing else.
        if(CONTRACT_CASE MATCHES "^schema-")
            set(CONTRACT_CASE_LABELS "CI;SCHEMA")
        else()
            set(CONTRACT_CASE_LABELS "CI;CONTRACT")
        endif()
        set_tests_properties(${CONTRACT_CASE} PROPERTIES
            TIMEOUT 60
            LABELS "${CONTRACT_CASE_LABELS}"
        )
        uf_require_executed_assertions(${CONTRACT_CASE})
        set_property(GLOBAL APPEND PROPERTY UF_REGISTERED_CONTRACT_CASES "${CONTRACT_CASE}")
    endforeach()
endfunction()
cpp_add_test(
    NAME test-core
    SOURCES
        core/test-capabilities.cpp
        core/test-checked-access.cpp
        core/test-checked-arithmetic.cpp
        core/test-error.cpp
        core/test-json-text.cpp
        core/test-monotonic-time.cpp
        core/test-strong-types.cpp
        core/test-utf8.cpp
    LIBS
        ${PROJECT_NAME}_core
)

cpp_add_test(
    NAME test-json
    SOURCES
        json/test-value.cpp
        json/test-schema.cpp
    LIBS
        ${PROJECT_NAME}_json
)

# The deployment's two validators, against both exemplars' real schema bytes.
# The exemplars' schema documents live beside their providers, so the include
# root is conformance/exemplars and each is named by its own directory.
#
# image is named here as well as inside the module because the loader decodes
# what probe_frame names (2.7 R9), so the fixture has to write a PNG the same
# decoder accepts rather than eight signature bytes.
cpp_add_test(
    NAME test-deployment
    SOURCES
        deployment/test-project-deployment.cpp
        deployment/test-project-directory.cpp
    LIBS
        ${PROJECT_NAME}_deployment
        ${PROJECT_NAME}_image
)
# tests/ itself, so that the loader's cases reach tests/json/repository-path.hpp
# rather than becoming a seventh copy of it.
target_include_directories(test-deployment PRIVATE
    "${PROJECT_SOURCE_DIR}/conformance/exemplars"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

cpp_add_test(
    NAME test-domain
    SOURCES
        domain/test-detection.cpp
        domain/test-error.cpp
        domain/test-frame.cpp
        domain/test-ids.cpp
        domain/test-space.cpp
    LIBS
        ${PROJECT_NAME}_domain
)

cpp_add_test(
    NAME test-vision
    SOURCES
        vision/test-sad.cpp
    LIBS
        ${PROJECT_NAME}_vision
)

cpp_add_test(
    NAME test-image
    SOURCES
        image/test-pixels.cpp
        image/test-png.cpp
    LIBS
        ${PROJECT_NAME}_image
        ${PROJECT_NAME}_vision
)

cpp_add_test(
    NAME test-trace
    SOURCES
        trace/test-trace.cpp
        trace/test-stream-validator.cpp
    LIBS
        ${PROJECT_NAME}_trace
)

# The project registration every Operator test builds on lives beside the
# conformance suite's umbraflow exemplar, whose provider adapts the same header.
# Adding its directory keeps the dependency running from tests/ into the exported
# suite and leaves every #include "project-fixture.hpp" spelled the same way.
set(UF_OPERATOR_FIXTURE_DIR
    "${PROJECT_SOURCE_DIR}/conformance/exemplars/umbraflow"
)
if(NOT EXISTS "${UF_OPERATOR_FIXTURE_DIR}/project-fixture.hpp")
    message(FATAL_ERROR
        "the Operator test project fixture is missing: "
        "${UF_OPERATOR_FIXTURE_DIR}/project-fixture.hpp"
    )
endif()

# A snapshot is composed from an observation only TaskHost can mint, so every
# Operator test drives a real Host over a real RuntimeArtifact. The fixture that
# builds one lives beside the exported suite's own headers and encodes its
# template assets as PNG, which is the one library the Operator does not already
# link publicly.
set(UF_OPERATOR_CONTRACT_INCLUDE_DIR
    "${PROJECT_SOURCE_DIR}/conformance/include"
)
if(NOT EXISTS "${UF_OPERATOR_CONTRACT_INCLUDE_DIR}/conformance/observation-fixture.hpp")
    message(FATAL_ERROR
        "the Operator observation fixture is missing: "
        "${UF_OPERATOR_CONTRACT_INCLUDE_DIR}/conformance/observation-fixture.hpp"
    )
endif()

if(TARGET ${PROJECT_NAME}_operator)
    cpp_add_test(
        NAME test-operator
        SOURCES
            operator/test-ledger.cpp
            operator/test-manifest.cpp
            operator/test-operation.cpp
        LIBS
            ${PROJECT_NAME}_operator
            ${PROJECT_NAME}_image
            # project-fixture.hpp builds its five authorities out of
            # modules/deployment, which is where the validators now live.
            ${PROJECT_NAME}_deployment
    )
    target_include_directories(test-operator PRIVATE
        "${UF_OPERATOR_FIXTURE_DIR}"
        "${UF_OPERATOR_CONTRACT_INCLUDE_DIR}"
    )
    if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
        cpp_stage_runtime_libraries(test-operator ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
    endif()

    cpp_add_contract_suite(
        TARGET test-contract-operator
        SOURCES
            operator/test-product-contract.cpp
            operator/test-project-plugin-contract.cpp
            operator/test-state-contract.cpp
            operator/test-control-contract.cpp
            operator/test-agent-audit-contract.cpp
        LIBS
            ${PROJECT_NAME}_operator
            ${PROJECT_NAME}_image
            # tests/operator/schema-binding.hpp opens the database the store
            # created and reads its columns back. The Operator keeps SQLite
            # private, as it must -- its public headers expose no sqlite3
            # handle -- so the gate that binds a stored row to the schema that
            # declares it links the amalgamation itself.
            ${PROJECT_NAME}_operator_sqlite3
            ${PROJECT_NAME}_deployment
        CASES
            contract-product-p01
            contract-product-p02
            contract-product-p03
            contract-product-p04
            contract-product-p05-fixtures
            contract-product-p06
            schema-product-p01
            schema-product-p02
            schema-product-p03
            contract-state-s01
            contract-state-s02
            contract-state-s03
            contract-state-s04
            contract-state-s05
            contract-state-s06
            schema-state-s01
            schema-state-s02
            schema-state-s04
            contract-control-c02
            contract-control-c03
            contract-control-c04
            contract-control-c05
            contract-control-c07
            contract-control-c08
            contract-control-c14
            schema-control-c03
            schema-control-c05
            schema-control-c08
            schema-control-c09
            schema-control-c10
            schema-control-c11
            schema-control-c12
            schema-control-c13
            contract-agent-a01
            contract-agent-a02
            contract-agent-a04
            contract-agent-a06
            contract-agent-a07
            contract-agent-a08
            schema-agent-a01
            schema-agent-a02
            schema-agent-a03
            schema-agent-a05
            schema-agent-a07
    )
    target_include_directories(test-contract-operator PRIVATE
        "${UF_OPERATOR_FIXTURE_DIR}"
        "${UF_OPERATOR_CONTRACT_INCLUDE_DIR}"
    )
    if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
        cpp_stage_runtime_libraries(
            test-contract-operator
            ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES}
        )
    endif()
endif()

cpp_add_test(
    NAME test-engine
    SOURCES
        engine/test-session.cpp
    LIBS
        ${PROJECT_NAME}_engine
        ${PROJECT_NAME}_image
)
# engine links ocr publicly, so this test resolves onnxruntime.dll from its
# own directory at process start; see cmake/runtime-payload.cmake.
if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
    cpp_stage_runtime_libraries(test-engine ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
endif()

set(CLI_TEST_SOURCES
    cli/test-args.cpp
    cli/test-explore-protocol.cpp
    cli/test-project-skeleton.cpp
    cli/test-targets.cpp
)
# selectCandidate operates on the controller's TargetCandidate, and
# modules/cli/manifest.txt puts candidate-selection.cpp in [sources.windows], so
# its test is registered only there.
if(WIN32)
    list(APPEND CLI_TEST_SOURCES cli/test-candidate-selection.cpp)
endif()
cpp_add_test(
    NAME test-cli
    SOURCES
        ${CLI_TEST_SOURCES}
    LIBS
        ${PROJECT_NAME}_cli
)
# cli links engine publicly, which links ocr publicly.
if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
    cpp_stage_runtime_libraries(test-cli ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
endif()

if(TARGET ${PROJECT_NAME}_script)
    cpp_add_test(
        NAME test-script
        SOURCES
            script/test-script.cpp
            script/test-adversarial-substrate.cpp
            script/test-environments.cpp
            script/test-veto-suite.cpp
        LIBS
            ${PROJECT_NAME}_script
    )
endif()

if(TARGET ${PROJECT_NAME}_task)
    cpp_add_test(
        NAME test-task
        SOURCES
            task/test-adversarial-surface.cpp
            task/test-confined-file.cpp
            task/test-task-binding.cpp
            task/test-framework-bundle.cpp
            task/test-script-validator.cpp
        LIBS
            ${PROJECT_NAME}_task
            ${PROJECT_NAME}_image
    )
    # task links engine publicly, which links ocr publicly.
    if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
        cpp_stage_runtime_libraries(test-task ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
    endif()

    cpp_add_contract_suite(
        TARGET test-contract-runtime
        SOURCES
            task/test-runtime-v2-contract.cpp
        LIBS
            ${PROJECT_NAME}_task
            ${PROJECT_NAME}_image
        CASES
            contract-runtime-u01
            contract-runtime-u02
            contract-runtime-u03
            contract-runtime-u04
            contract-runtime-u05
            contract-runtime-u06
            contract-runtime-u07
            contract-runtime-u08
    )
    # TaskHostTestAccess is declared a friend of TaskHost once, so uf::task may
    # hold one type of that name; it lives beside the exported suite because the
    # Operator's delivery contracts need the same privilege. The dependency runs
    # from tests/ into the suite and never back.
    target_include_directories(test-contract-runtime PRIVATE
        "${UF_OPERATOR_CONTRACT_INCLUDE_DIR}"
    )
    if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
        cpp_stage_runtime_libraries(
            test-contract-runtime
            ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES}
        )
    endif()

    set(TASK_LUAU_RUNNER_SOURCE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/task-luau-test-runner.cpp"
    )
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(CONFIGURE
        OUTPUT "${TASK_LUAU_RUNNER_SOURCE}"
        CONTENT [=[
#include <task/framework-bundle.hpp>

#include <script/engine.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

auto main(int argc, char** argv) -> int
{
    if (argc != 2)
    {
        std::cerr << "usage: task-luau-test-runner <test.luau>\n";
        return 2;
    }

    auto input = std::ifstream{argv[1], std::ios::binary};
    if (!input)
    {
        std::cerr << "cannot open Luau contract: " << argv[1] << '\n';
        return 2;
    }
    auto const source = std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    if (input.bad())
    {
        std::cerr << "cannot read Luau contract: " << argv[1] << '\n';
        return 2;
    }

    auto engine = uf::script::Engine::create(
        uf::script::EngineConfig{
            .frameworkModules = uf::task::frameworkScriptModules(),
            .frameworkProjectGlobals = {
                "evidence",
                "jcs",
                "model",
                "observe",
                "project",
                "resolution",
            },
        }
    );
    if (!engine)
    {
        std::cerr << engine.error().message() << '\n';
        return 1;
    }

    auto result = engine->runNumber(source, argv[1]);
    if (!result)
    {
        std::cerr << result.error().message() << '\n';
        return 1;
    }
    if (*result != 1.0)
    {
        std::cerr << "Luau contract did not return 1: " << argv[1] << '\n';
        return 1;
    }
    return 0;
}
]=]
        NEWLINE_STYLE LF
    )
    if(NOT EXISTS "${TASK_LUAU_RUNNER_SOURCE}")
        message(FATAL_ERROR "failed to generate the task Luau test runner")
    endif()

    add_executable(test-task-luau-runner
        "${TASK_LUAU_RUNNER_SOURCE}"
    )
    cpp_apply_safety_profile(test-task-luau-runner)
    cpp_apply_utf8_encoding(test-task-luau-runner)
    target_link_libraries(test-task-luau-runner PRIVATE ${PROJECT_NAME}_task)
    target_compile_features(test-task-luau-runner PRIVATE cxx_std_23)
    if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
        cpp_stage_runtime_libraries(
            test-task-luau-runner
            ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES}
        )
    endif()

    # Concrete paths, never a glob, for the same reason the conformance suite uses
    # concrete sources: a contract that stops running must be a visible deletion
    # here. The coverage check below is the other half of that -- a hand-written
    # list silently stops covering the directory the moment somebody adds a file
    # to it, which is how tests/task/test-jcs.luau ran in no gate on 2026-08-10.
    set(TASK_LUAU_TESTS
        "test-jcs-luau|task/test-jcs.luau"
        "test-runtime-model-v2-luau|task/test-runtime-model-v2.luau"
        "test-resolution-v2-luau|task/test-resolution-v2.luau"
        "test-receipt-request-v2-luau|task/test-receipt-request-v2.luau"
    )
    file(GLOB TASK_LUAU_PRESENT
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/task/*.luau"
    )
    set(TASK_LUAU_REGISTERED "")
    foreach(TASK_LUAU_TEST IN LISTS TASK_LUAU_TESTS)
        string(REPLACE "|" ";" TASK_LUAU_TEST_PARTS "${TASK_LUAU_TEST}")
        list(GET TASK_LUAU_TEST_PARTS 1 TASK_LUAU_REGISTERED_SOURCE)
        list(APPEND TASK_LUAU_REGISTERED "${TASK_LUAU_REGISTERED_SOURCE}")
    endforeach()
    foreach(TASK_LUAU_SOURCE IN LISTS TASK_LUAU_PRESENT)
        list(FIND TASK_LUAU_REGISTERED "${TASK_LUAU_SOURCE}" TASK_LUAU_INDEX)
        if(TASK_LUAU_INDEX EQUAL -1)
            message(FATAL_ERROR
                "a Luau contract under tests/task/ runs in no CTest: "
                "${TASK_LUAU_SOURCE}. Add it to TASK_LUAU_TESTS, or delete it."
            )
        endif()
    endforeach()
    foreach(TASK_LUAU_TEST IN LISTS TASK_LUAU_TESTS)
        string(REPLACE "|" ";" TASK_LUAU_TEST_PARTS "${TASK_LUAU_TEST}")
        list(GET TASK_LUAU_TEST_PARTS 0 TASK_LUAU_TEST_NAME)
        list(GET TASK_LUAU_TEST_PARTS 1 TASK_LUAU_TEST_SOURCE)
        set(TASK_LUAU_TEST_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/${TASK_LUAU_TEST_SOURCE}"
        )
        if(NOT EXISTS "${TASK_LUAU_TEST_PATH}")
            message(FATAL_ERROR
                "task Luau test source does not exist: ${TASK_LUAU_TEST_SOURCE}"
            )
        endif()
        add_test(
            NAME "${TASK_LUAU_TEST_NAME}"
            COMMAND test-task-luau-runner "${TASK_LUAU_TEST_PATH}"
        )
        set_tests_properties("${TASK_LUAU_TEST_NAME}" PROPERTIES
            TIMEOUT 60
            LABELS "CI;LUAU"
        )
    endforeach()
endif()

if(TARGET ${PROJECT_NAME}_controller)
    cpp_add_test(
        NAME test-controller
        SOURCES
            controller/test-capture-readback.cpp
            controller/test-capture-os-build.cpp
            controller/test-capture-stall.cpp
            controller/test-capture-wgc.cpp
            controller/test-discovery.cpp
            controller/test-dpi.cpp
            controller/test-audit-log.cpp
            controller/test-input-held.cpp
            controller/test-input-message.cpp
            controller/test-input-revalidation.cpp
            controller/test-input.cpp
            controller/test-target.cpp
        LIBS
            ${PROJECT_NAME}_controller
    )
endif()

# What the OCR adapter reads off a real frame. The fixture directory holds real
# game screenshots that are never committed, so the target is registered only
# when a developer has created it; rerun cmake configure after adding
# tests/assets/real-regression for it to appear. It sat inside the workbench
# block until the annotation front-ends were retired, which was only ever where
# the fixture directory happened to be first tested for.
if(EXISTS ${PROJECT_SOURCE_DIR}/tests/assets/real-regression)
    cpp_add_test(
        NAME test-ocr-real
        SOURCES
            ocr/test-ocr-real.cpp
        LIBS
            ${PROJECT_NAME}_ocr
            ${PROJECT_NAME}_image
            ${PROJECT_NAME}_vision
    )
    target_compile_definitions(test-ocr-real PRIVATE
        UF_REAL_REGRESSION_ROOT="${PROJECT_SOURCE_DIR}/tests/assets/real-regression"
        UF_OCR_MODEL_ROOT="${PROJECT_SOURCE_DIR}/modules/ocr/external/models"
    )
    # onnxruntime resolves beside the executable, so the runtime DLLs are copied
    # rather than assumed to be on PATH; see cmake/runtime-payload.cmake.
    cpp_stage_runtime_libraries(test-ocr-real ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
    # A distinct label keeps this out of the CI set; it must never run on CI
    # machines, which have only synthetic fixtures.
    set_tests_properties(test-ocr-real PROPERTIES
        LABELS "REAL"
        TIMEOUT 300
    )
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
# check-, not contract-: this runs a script over the tree, owns no requirement
# id and compiles no doctest, so it is a sibling of check-modules and
# check-safety rather than of the behavioural gates. It carried the contract-
# name only because a bare add_test bypasses uf_add_conformance_suite, which
# refuses a gate outside the contract-/schema- vocabulary.
add_test(
    NAME check-repository-surface
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/test-runtime-surface.py
        --root ${PROJECT_SOURCE_DIR}
)
add_test(
    NAME check-modules
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_modules.py
)
add_test(
    NAME check-safety
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_safety.py
)
add_test(
    NAME test-check-safety
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-check-safety.py
)
# Pins only. The full run reads the frozen bundle out of the consumer repository
# at an absolute path no CI runner has, and a form of the check that tolerated
# its absence would pass whether or not the bundle agreed. The name says what
# this gate delivers: agreement between the pinned hashes this repository states
# in prose and the ones its own script carries.
add_test(
    NAME check-spec-pins
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_spec_bundle.py --pins-only
)
set_tests_properties(
    check-repository-surface
    check-modules
    check-safety
    check-spec-pins
    test-check-safety
    PROPERTIES
    TIMEOUT 30
    LABELS "CI"
)

# The authoring authority's only coverage. It ran in no gate at all until
# 2026-08-10, so every property it proves -- capability minting, publication
# CAS, the cross-process lock, blob and document quotas, the loopback header
# checks -- was unenforced between runs somebody remembered to do by hand.
add_test(
    NAME test-annotate-backend
    COMMAND ${Python3_EXECUTABLE} -m unittest tools.annotate.tests.test_backend
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)
set_tests_properties(
    test-annotate-backend
    PROPERTIES
    TIMEOUT 300
    LABELS "CI"
)

get_property(REGISTERED_CONTRACT_CASES GLOBAL PROPERTY UF_REGISTERED_CONTRACT_CASES)
set(SORTED_REQUIRED_CONTRACT_CASES ${UF_REQUIRED_DOCTEST_CONTRACTS})
set(SORTED_REGISTERED_CONTRACT_CASES ${REGISTERED_CONTRACT_CASES})
list(SORT SORTED_REQUIRED_CONTRACT_CASES)
list(SORT SORTED_REGISTERED_CONTRACT_CASES)
if(NOT SORTED_REQUIRED_CONTRACT_CASES STREQUAL SORTED_REGISTERED_CONTRACT_CASES)
    message(FATAL_ERROR
        "Named contract CTest registration does not match the migration report; "
        "required=[${SORTED_REQUIRED_CONTRACT_CASES}], "
        "registered=[${SORTED_REGISTERED_CONTRACT_CASES}]"
    )
endif()

# The list above pins which gate each requirement owns. This pins that no
# requirement lost every gate it had, which a rename is exactly the way to do by
# accident.
set(COVERED_REQUIREMENTS "")
foreach(REGISTERED_GATE IN LISTS SORTED_REGISTERED_CONTRACT_CASES)
    string(REGEX REPLACE "^(contract|schema)-" "" COVERED_REQUIREMENT "${REGISTERED_GATE}")
    if(COVERED_REQUIREMENT STREQUAL REGISTERED_GATE)
        message(FATAL_ERROR
            "registered gate is outside the contract-/schema- vocabulary: ${REGISTERED_GATE}"
        )
    endif()
    list(APPEND COVERED_REQUIREMENTS "${COVERED_REQUIREMENT}")
endforeach()
list(REMOVE_DUPLICATES COVERED_REQUIREMENTS)
list(SORT COVERED_REQUIREMENTS)
set(SORTED_REQUIRED_CORE_REQUIREMENTS ${UF_REQUIRED_CORE_REQUIREMENTS})
list(SORT SORTED_REQUIRED_CORE_REQUIREMENTS)
if(NOT COVERED_REQUIREMENTS STREQUAL SORTED_REQUIRED_CORE_REQUIREMENTS)
    message(FATAL_ERROR
        "Gate registration does not cover the REQUIRED_CORE requirements exactly; "
        "required=[${SORTED_REQUIRED_CORE_REQUIREMENTS}], "
        "covered=[${COVERED_REQUIREMENTS}]"
    )
endif()

# Coverage above counts a schema- gate, which passes whether or not the
# behaviour exists. This separates the two so the requirements standing on a
# shape check alone are named rather than folded into a count of 42. Moving a
# gate between the prefixes changes the derived set and fails here.
set(BEHAVIOURAL_REQUIREMENTS "")
foreach(REGISTERED_GATE IN LISTS SORTED_REGISTERED_CONTRACT_CASES)
    if(REGISTERED_GATE MATCHES "^contract-")
        string(REGEX REPLACE "^contract-" "" BEHAVIOURAL_REQUIREMENT "${REGISTERED_GATE}")
        list(APPEND BEHAVIOURAL_REQUIREMENTS "${BEHAVIOURAL_REQUIREMENT}")
    endif()
endforeach()
list(REMOVE_DUPLICATES BEHAVIOURAL_REQUIREMENTS)
set(SHAPE_ONLY_REQUIREMENTS "")
foreach(COVERED_REQUIREMENT IN LISTS COVERED_REQUIREMENTS)
    list(FIND BEHAVIOURAL_REQUIREMENTS "${COVERED_REQUIREMENT}" BEHAVIOURAL_INDEX)
    if(BEHAVIOURAL_INDEX EQUAL -1)
        list(APPEND SHAPE_ONLY_REQUIREMENTS "${COVERED_REQUIREMENT}")
    endif()
endforeach()
list(SORT SHAPE_ONLY_REQUIREMENTS)
set(SORTED_SCHEMA_ONLY_REQUIREMENTS ${UF_SCHEMA_ONLY_REQUIREMENTS})
list(SORT SORTED_SCHEMA_ONLY_REQUIREMENTS)
if(NOT SHAPE_ONLY_REQUIREMENTS STREQUAL SORTED_SCHEMA_ONLY_REQUIREMENTS)
    message(FATAL_ERROR
        "The requirements that own no contract- gate are not the ones "
        "UF_SCHEMA_ONLY_REQUIREMENTS names; "
        "pinned=[${SORTED_SCHEMA_ONLY_REQUIREMENTS}], "
        "registered=[${SHAPE_ONLY_REQUIREMENTS}]"
    )
endif()
if(NOT TEST check-repository-surface)
    message(FATAL_ERROR "check-repository-surface was not registered")
endif()
