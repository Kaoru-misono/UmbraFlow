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
# store behaviour lives in the conformance suite, which registers the contract-
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
set(UF_REQUIRED_CONTRACT_GATES
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
    contract-agent-a03
    contract-agent-a04
    contract-agent-a05
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
set(UF_SCHEMA_ONLY_REQUIREMENTS)

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
        list(FIND UF_REQUIRED_CONTRACT_GATES "${CONTRACT_CASE}" REQUIRED_CASE_INDEX)
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
    # 2026-08-10. uf_add_conformance_run already registers the same gate for
    # each conformance run; this is the sibling that was missing it.
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

# The deployment's two validators, against both fixtures' real schema bytes.
# Those schema documents are C++ raw literals rather than files, which is what
# separates them from examples/: they are the bytes these cases judge a
# validator with, so the include root is tests/support and each is named by its
# own directory.
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
        ${PROJECT_NAME}_project
        ${PROJECT_NAME}_schema
)
# tests/ itself, so that the loader's cases reach tests/json/repository-path.hpp
# rather than becoming a seventh copy of it.
target_include_directories(test-deployment PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(test-deployment PRIVATE
    UF_STAGED_UMBRAFLOW_PROJECT="${UF_STAGED_UMBRAFLOW_PROJECT}"
    UF_STAGED_ARCANA_PROJECT="${UF_STAGED_ARCANA_PROJECT}"
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

# The project registration every Operator test builds on. It is the last thing
# that still builds a ProjectRegistration in C++: the conformance suite loads one
# out of examples/umbraflow instead, and Q5 of
# docs/archive/plans/2026-08-11-project-as-data.md moves these tests onto the same loader
# as its own change, after which this directory has nothing left in it. It lives
# in tests/support/ because tests/ is its only caller -- the suite includes
# nothing from it. Adding the directory here leaves every
# #include "project-fixture.hpp" spelled the same way.
set(UF_OPERATOR_FIXTURE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/support/umbraflow"
)
if(NOT EXISTS "${UF_OPERATOR_FIXTURE_DIR}/project-fixture.hpp")
    message(FATAL_ERROR
        "the Operator test project fixture is missing: "
        "${UF_OPERATOR_FIXTURE_DIR}/project-fixture.hpp"
    )
endif()

# A snapshot is composed from an observation only TaskHost can mint, so every
# Operator test drives a real Host over a real RuntimeArtifact. The fixture that
# builds one is the suite's own and encodes its template assets as PNG, which is
# the one library the Operator does not already link publicly.
#
# The include root rather than uf::conformance, deliberately: what tests/ takes
# from the module is three header-only fixtures, and the module's own library
# carries the suite's cases and a second doctest implementation beside them.
# The dependency runs from tests/ into the module and never back.
set(UF_OPERATOR_CONTRACT_INCLUDE_DIR
    "${PROJECT_SOURCE_DIR}/modules/conformance/source"
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
            operator/test-host-controller.cpp
            operator/test-ledger.cpp
            operator/test-lifecycle-facade.cpp
            operator/test-manifest.cpp
            operator/test-operation.cpp
            operator/test-tool-authority.cpp
        LIBS
            ${PROJECT_NAME}_operator
            ${PROJECT_NAME}_service
            ${PROJECT_NAME}_image
            # project-fixture.hpp builds its five authorities out of
            # modules/deployment, which is where the validators now live.
            ${PROJECT_NAME}_deployment
            ${PROJECT_NAME}_project
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
            workflow/test-step-kind.cpp
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
            ${PROJECT_NAME}_project
            # contract-state-s05 asks whether the session manifest binds the
            # plugin environment the framework actually runs. The Operator keeps
            # script private, so the gate that compares the manifest's ninth
            # value against the module that mints it names the module itself.
            ${PROJECT_NAME}_script
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
    cli/test-observe.cpp
    cli/test-ocr.cpp
    cli/test-open-project.cpp
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
        # cli links all three privately, so nothing propagates. test-observe.cpp
        # installs a release into a production Operator root and decodes the
        # project's capture through the suite's fixture, which is what the verb
        # under test is handed on a real desktop.
        ${PROJECT_NAME}_operator
        ${PROJECT_NAME}_image
        # test-observe.cpp starts ProductLifecycle directly to prove that its
        # Impl destructor is a last-resort lease owner, independently of the
        # CLI's explicit close path.
        ${PROJECT_NAME}_service
        # Whether observe left the Operator holding a control lease is ledger
        # state and reaches no verb result, so test-observe.cpp reads the
        # control transitions out of the database the Operator created. The
        # Operator keeps SQLite private, as it must, which is why the case that
        # reads its storage links the amalgamation itself -- the same reason
        # tests/operator/schema-binding.hpp does.
        ${PROJECT_NAME}_operator_sqlite3
)
target_include_directories(test-cli PRIVATE
    "${UF_OPERATOR_CONTRACT_INCLUDE_DIR}"
)
target_compile_definitions(test-cli PRIVATE
    UF_STAGED_UMBRAFLOW_PROJECT="${UF_STAGED_UMBRAFLOW_PROJECT}"
)
# cli links engine publicly, which links ocr publicly.
if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
    cpp_stage_runtime_libraries(test-cli ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
endif()

cpp_add_test(
    NAME test-project
    SOURCES
        project/test-project-kit.cpp
    LIBS
        ${PROJECT_NAME}_project
        ${PROJECT_NAME}_script
        ${PROJECT_NAME}_json
        ${PROJECT_NAME}_domain
        ${PROJECT_NAME}_image
)

# The two authoring paths meet only at the ProjectPlugin admission boundary, so
# this gate names the generator, the boundary, and the module that owns the real
# bidirectional document validation the boundary is handed. It is a target of
# its own rather than a file inside test-project, because test-project links
# neither the Operator nor deployment and a gate that only compiles the adapter
# bare is the gap this closes.
cpp_add_test(
    NAME test-authoring-path-parity
    SOURCES
        project/test-authoring-path-parity.cpp
    LIBS
        ${PROJECT_NAME}_project
        ${PROJECT_NAME}_deployment
        ${PROJECT_NAME}_operator
        ${PROJECT_NAME}_json
        ${PROJECT_NAME}_domain
        ${PROJECT_NAME}_image
)
if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
    cpp_stage_runtime_libraries(
        test-authoring-path-parity
        ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES}
    )
endif()

# umbraflow-project.json is judged by the offline kit and by the runtime loader,
# and those two live in modules that cannot link one another. This target is the
# only place both are linked at once, which is what lets one case hand one
# document to both and require the same sentence back. It cannot be a file
# inside test-project or test-deployment for exactly that reason.
cpp_add_test(
    NAME test-project-manifest-shape
    SOURCES
        project/test-project-manifest-shape.cpp
    LIBS
        ${PROJECT_NAME}_project
        ${PROJECT_NAME}_deployment
)
if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
    cpp_stage_runtime_libraries(
        test-project-manifest-shape
        ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES}
    )
endif()

add_executable(generate-umbraflow-example
    support/umbraflow/generate-example.cpp
)
cpp_apply_safety_profile(generate-umbraflow-example)
cpp_apply_utf8_encoding(generate-umbraflow-example)
target_link_libraries(generate-umbraflow-example PRIVATE
    ${PROJECT_NAME}_project
    ${PROJECT_NAME}_deployment
)
target_compile_features(generate-umbraflow-example PRIVATE cxx_std_23)

add_executable(generate-arcana-example
    support/arcana-expedition/generate-example.cpp
)
cpp_apply_safety_profile(generate-arcana-example)
cpp_apply_utf8_encoding(generate-arcana-example)
target_link_libraries(generate-arcana-example PRIVATE
    ${PROJECT_NAME}_project
    ${PROJECT_NAME}_deployment
)
target_compile_features(generate-arcana-example PRIVATE cxx_std_23)

add_custom_target(generate-example-projects ALL
    COMMAND ${Python3_EXECUTABLE}
        "${PROJECT_SOURCE_DIR}/scripts/build_runtime_corpus.py"
        --input "${PROJECT_SOURCE_DIR}/examples/arcana-expedition/database/map-regions.csv"
        --output "${UF_STAGED_ARCANA_PROJECT}/blob/map.blob"
    COMMAND $<TARGET_FILE:generate-umbraflow-example>
        "${PROJECT_SOURCE_DIR}/examples/umbraflow"
        "${UF_STAGED_UMBRAFLOW_PROJECT}"
    COMMAND $<TARGET_FILE:generate-arcana-example>
        "${PROJECT_SOURCE_DIR}/examples/arcana-expedition"
        "${UF_STAGED_ARCANA_PROJECT}"
    DEPENDS
        generate-umbraflow-example
        generate-arcana-example
        "${PROJECT_SOURCE_DIR}/scripts/build_runtime_corpus.py"
        "${PROJECT_SOURCE_DIR}/examples/arcana-expedition/database/map-regions.csv"
    COMMENT "[Project] Generating example Tool Catalogs"
    VERBATIM
)
add_dependencies(test-deployment generate-example-projects)
add_dependencies(test-cli generate-example-projects)
if(TARGET umbra-flow-conformance)
    add_dependencies(umbra-flow-conformance generate-example-projects)
endif()

add_test(
    NAME test-project-cli
    COMMAND ${CMAKE_COMMAND}
        -DUF_PROJECT_EXECUTABLE=$<TARGET_FILE:project>
        -DUF_PROJECT_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/project-cli-fixture
        # Two PNGs this repository already ships. The fixture copies each into
        # a corpus under the name its own sha256 gives it, so no hash is
        # written down anywhere and a template cut is declared exactly as a
        # project declares one.
        -DUF_PROJECT_TEST_FRAME=${PROJECT_SOURCE_DIR}/examples/umbraflow/runtime/probe-frame.png
        -DUF_PROJECT_TEST_OTHER_FRAME=${PROJECT_SOURCE_DIR}/examples/umbraflow/runtime/artifact/assets/fixture-anchor.png
        -P ${CMAKE_CURRENT_SOURCE_DIR}/project/test-project-cli.cmake
)
set_tests_properties(test-project-cli PROPERTIES
    TIMEOUT 30
    LABELS "CI"
)

# The reclamation pass, driven through the shipped binary rather than through a
# library call. That is the point of the gate: reclaimUnreferencedRuntimeArtifacts
# was implemented, tested and reachable from no production entry point at all
# until `umbra-flow reclaim` was added on 2026-08-17, and a test that calls the
# Coordinator directly is exactly the test that failed to notice.
#
# The driver is generated into the build tree rather than checked in beside
# test-project-cli.cmake, the same way the task Luau runner below is: it is
# produced by this file, and the alternative spelling is a second file that has
# to be kept in step with the registration by hand. @ONLY, so the driver's own
# ${} references survive configure and are read when CTest runs it.
set(CLI_RECLAIM_DRIVER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/test-reclaim-cli.cmake"
)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE
    OUTPUT "${CLI_RECLAIM_DRIVER}"
    @ONLY
    CONTENT [=[
cmake_minimum_required(VERSION 3.30)

if(NOT EXISTS "${UF_CLI_EXECUTABLE}")
    message(FATAL_ERROR
        "umbra-flow executable does not exist: ${UF_CLI_EXECUTABLE}"
    )
endif()

set(RECLAIM_ROOT "${UF_RECLAIM_TEST_ROOT}")
set(RECLAIM_LEDGER "${RECLAIM_ROOT}/operator-runtime.sqlite")
set(RECLAIM_STAGED
    "${RECLAIM_ROOT}/runtime-artifacts/.staging/interrupted-publication"
)

# What an interrupted publication leaves: bytes under a name in the staging
# tree, and no row anywhere naming them. The directory is planted before the
# root exists, because opening the root is the verb's job and a fixture that
# opened it first would be testing its own Coordinator.
file(REMOVE_RECURSE "${RECLAIM_ROOT}")
file(MAKE_DIRECTORY "${RECLAIM_STAGED}")
file(WRITE "${RECLAIM_STAGED}/blob.bin" "bytes a publication staged and left\n")

function(run_reclaim OUTPUT_VARIABLE)
    execute_process(
        COMMAND "${UF_CLI_EXECUTABLE}" reclaim --runtime "${RECLAIM_ROOT}"
        OUTPUT_VARIABLE SWEPT
        ERROR_VARIABLE SWEPT_ERROR
        RESULT_VARIABLE SWEPT_STATUS
    )
    if(NOT SWEPT_STATUS EQUAL 0)
        message(FATAL_ERROR
            "umbra-flow reclaim failed (${SWEPT_STATUS}):\n${SWEPT}${SWEPT_ERROR}"
        )
    endif()
    message("${SWEPT}")
    set(${OUTPUT_VARIABLE} "${SWEPT}" PARENT_SCOPE)
endfunction()

# The count is read back out of the report rather than matched loosely, so a
# report that prints a constant fails the second pass below.
function(staging_count REPORT OUTPUT_VARIABLE)
    if(NOT "${REPORT}" MATCHES "staging directories +([0-9]+)")
        message(FATAL_ERROR "reclaim reported no staging count:\n${REPORT}")
    endif()
    set(${OUTPUT_VARIABLE} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

run_reclaim(FIRST_REPORT)

# The verb reached the Operator rather than reporting over a root it never
# opened: the ledger the Coordinator creates is the evidence.
if(NOT EXISTS "${RECLAIM_LEDGER}")
    message(FATAL_ERROR
        "reclaim reported without opening an Operator ledger at ${RECLAIM_LEDGER}"
    )
endif()

if(EXISTS "${RECLAIM_STAGED}")
    message(FATAL_ERROR
        "reclaim left the staged tree behind: ${RECLAIM_STAGED}"
    )
endif()

staging_count("${FIRST_REPORT}" FIRST_STAGING)
if(NOT FIRST_STAGING STREQUAL "1")
    message(FATAL_ERROR
        "reclaim removed the staged tree and reported ${FIRST_STAGING} of them"
    )
endif()

# A second pass over the root it just swept. It reports nothing removed, which
# is what separates a measured count from a printed one.
run_reclaim(SECOND_REPORT)
staging_count("${SECOND_REPORT}" SECOND_STAGING)
if(NOT SECOND_STAGING STREQUAL "0")
    message(FATAL_ERROR
        "a second sweep of a swept root reported ${SECOND_STAGING} staging trees"
    )
endif()
]=]
    NEWLINE_STYLE LF
)
if(NOT EXISTS "${CLI_RECLAIM_DRIVER}")
    message(FATAL_ERROR "failed to generate the reclaim CLI test driver")
endif()

add_test(
    NAME test-reclaim-cli
    COMMAND ${CMAKE_COMMAND}
        -DUF_CLI_EXECUTABLE=$<TARGET_FILE:${PROJECT_NAME}>
        -DUF_RECLAIM_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/reclaim-cli-fixture
        -P "${CLI_RECLAIM_DRIVER}"
)
set_tests_properties(test-reclaim-cli PROPERTIES
    TIMEOUT 60
    LABELS "CI"
)

# The release upgrade and its capability approval, driven through the shipped
# binary rather than through a library call. That is the point of the gate:
# upgradeRuntimeArtifactAndPinSession and approveReleaseCapabilities were
# implemented, tested on a seven-row matrix and reachable from no production
# entry point at all until `umbra-flow upgrade` and `umbra-flow approve` were
# added on 2026-08-18, and a test that calls the Coordinator directly is
# exactly the test that failed to notice.
#
# The sequence is the whole U6 story through the real executable: a bootstrap
# upgrade on a fresh root, a second release whose expanded capability set is
# refused for want of an approval, the approval, and the third upgrade that
# lands it. The generation arithmetic is load-bearing: the refused upgrade
# rolls the root back at attempted+1, so the third upgrade can only land at
# generation 4 (1 installed, 1 attempted, 1 rollback, 1 final) and the driver
# asserts exactly that.
#
# The driver is generated into the build tree rather than checked in beside
# test-project-cli.cmake, the same way the reclaim driver above is: it is
# produced by this file, and the alternative spelling is a second file that has
# to be kept in step with the registration by hand. @ONLY, so the driver's own
# ${} references survive configure and are read when CTest runs it.
set(CLI_UPGRADE_DRIVER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/test-upgrade-cli.cmake"
)
file(CONFIGURE
    OUTPUT "${CLI_UPGRADE_DRIVER}"
    @ONLY
    CONTENT [=[
cmake_minimum_required(VERSION 3.30)

if(NOT EXISTS "${UF_CLI_EXECUTABLE}")
    message(FATAL_ERROR
        "umbra-flow executable does not exist: ${UF_CLI_EXECUTABLE}"
    )
endif()
if(NOT EXISTS "${UF_UPGRADE_PROJECT_FIXTURE}/umbraflow-project.json")
    message(FATAL_ERROR
        "upgrade project fixture does not exist: ${UF_UPGRADE_PROJECT_FIXTURE}"
    )
endif()

set(UPGRADE_FIXTURE "${UF_UPGRADE_TEST_ROOT}")
set(UPGRADE_PROJECT "${UPGRADE_FIXTURE}/project")
set(UPGRADE_RUNTIME "${UPGRADE_FIXTURE}/runtime")
set(UPGRADE_HANDOFF_1 "${UPGRADE_FIXTURE}/handoff-1")
set(UPGRADE_HANDOFF_2 "${UPGRADE_FIXTURE}/handoff-2")
set(UPGRADE_LEDGER "${UPGRADE_RUNTIME}/operator-runtime.sqlite")

# The project fixture is the shipped exemplar, copied so the Operator root the
# verb creates is a fixture file rather than bytes in the source tree.
file(REMOVE_RECURSE "${UPGRADE_FIXTURE}")
file(MAKE_DIRECTORY "${UPGRADE_FIXTURE}")
file(COPY "${UF_UPGRADE_PROJECT_FIXTURE}/" DESTINATION "${UPGRADE_PROJECT}")

# Handoff 1 is the exemplar's own artifact, byte for byte: its root hash is
# this binary's arithmetic over the manifest it carries, and the release
# manifest is written here with the exact canonical field order the trusted
# reader consumes.
file(COPY
    "${UPGRADE_PROJECT}/runtime/artifact/"
    DESTINATION "${UPGRADE_HANDOFF_1}/runtime-artifact"
)
file(SHA256
    "${UPGRADE_HANDOFF_1}/runtime-artifact/runtime-artifact.manifest.json"
    ARTIFACT_MANIFEST_HASH_1
)

function(write_release_manifest HANDOFF ROOT_HASH)
    file(WRITE "${HANDOFF}/release.manifest.json"
        "{\"annotation_workspace_format\":2,\"candidate_id\":\"cli-fixture\","
        "\"candidate_revision\":1,\"generation\":1,"
        "\"predecessor_publication_id\":null,"
        "\"replay_gate_hash\":\"${UF_UPGRADE_REPLAY_GATE_HASH}\","
        "\"runtime_artifact_root_hash\":\"${ROOT_HASH}\","
        "\"workspace_sqlite_revision\":2}")
endfunction()

write_release_manifest("${UPGRADE_HANDOFF_1}" "${ARTIFACT_MANIFEST_HASH_1}")
file(SHA256 "${UPGRADE_HANDOFF_1}/release.manifest.json" RELEASE_MANIFEST_HASH_1)

# Handoff 2 is a second release over a changed model: the model bytes differ,
# so the page_model entry and the artifact root hash differ. The rest of the
# artifact is copied unchanged, which is exactly what a compatible upgrade is.
file(COPY
    "${UPGRADE_HANDOFF_1}/runtime-artifact/"
    DESTINATION "${UPGRADE_HANDOFF_2}/runtime-artifact"
)
file(APPEND "${UPGRADE_HANDOFF_2}/runtime-artifact/runtime-model.toml"
    "# second release of the fixture model\n"
)
file(SHA256
    "${UPGRADE_HANDOFF_2}/runtime-artifact/runtime-model.toml"
    MODEL_HASH_2
)
file(SIZE
    "${UPGRADE_HANDOFF_2}/runtime-artifact/runtime-model.toml"
    MODEL_SIZE_2
)
file(READ
    "${UPGRADE_HANDOFF_2}/runtime-artifact/runtime-artifact.manifest.json"
    ARTIFACT_MANIFEST_2
)
string(REGEX REPLACE
    "\"path\":\"runtime-model.toml\",\"sha256\":\"[0-9a-f]+\",\"size\":[0-9]+"
    "\"path\":\"runtime-model.toml\",\"sha256\":\"${MODEL_HASH_2}\",\"size\":${MODEL_SIZE_2}"
    ARTIFACT_MANIFEST_2_REVISED "${ARTIFACT_MANIFEST_2}"
)
file(WRITE
    "${UPGRADE_HANDOFF_2}/runtime-artifact/runtime-artifact.manifest.json"
    "${ARTIFACT_MANIFEST_2_REVISED}"
)
file(SHA256
    "${UPGRADE_HANDOFF_2}/runtime-artifact/runtime-artifact.manifest.json"
    ARTIFACT_MANIFEST_HASH_2
)
write_release_manifest("${UPGRADE_HANDOFF_2}" "${ARTIFACT_MANIFEST_HASH_2}")
file(SHA256 "${UPGRADE_HANDOFF_2}/release.manifest.json" RELEASE_MANIFEST_HASH_2)

function(run_upgrade OUTPUT_VARIABLE HANDOFF ROOT_HASH RELEASE_HASH)
    execute_process(
        COMMAND "${UF_CLI_EXECUTABLE}" upgrade
            --project "${UPGRADE_PROJECT}" --runtime "${UPGRADE_RUNTIME}"
            --handoff "${HANDOFF}"
            --release-manifest-hash "sha256:${RELEASE_HASH}"
            --artifact-root-hash "sha256:${ROOT_HASH}"
            ${ARGN}
        OUTPUT_VARIABLE UPGRADED
        ERROR_VARIABLE UPGRADED_ERROR
        RESULT_VARIABLE UPGRADED_STATUS
    )
    if(NOT UPGRADED_STATUS EQUAL 0)
        message(FATAL_ERROR
            "umbra-flow upgrade failed (${UPGRADED_STATUS}):\n${UPGRADED}${UPGRADED_ERROR}"
        )
    endif()
    message("${UPGRADED}")
    set(${OUTPUT_VARIABLE} "${UPGRADED}" PARENT_SCOPE)
endfunction()

function(installed_generation REPORT OUTPUT_VARIABLE)
    if(NOT "${REPORT}" MATCHES "installed generation +([0-9]+)")
        message(FATAL_ERROR "upgrade reported no installed generation:\n${REPORT}")
    endif()
    set(${OUTPUT_VARIABLE} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# The bootstrap upgrade on a fresh root. The ledger the Coordinator creates is
# the evidence the verb reached the Operator rather than reporting over a root
# it never opened.
run_upgrade(
    FIRST_REPORT
    "${UPGRADE_HANDOFF_1}" "${ARTIFACT_MANIFEST_HASH_1}" "${RELEASE_MANIFEST_HASH_1}"
)
if(NOT EXISTS "${UPGRADE_LEDGER}")
    message(FATAL_ERROR
        "upgrade reported without opening an Operator ledger at ${UPGRADE_LEDGER}"
    )
endif()
installed_generation("${FIRST_REPORT}" FIRST_GENERATION)
if(NOT FIRST_GENERATION STREQUAL "1")
    message(FATAL_ERROR
        "the first upgrade on a fresh root installed generation ${FIRST_GENERATION}"
    )
endif()

# A second release whose session pins a wider capability set is refused until
# the expansion is approved, and the refusal rolls the root back.
execute_process(
    COMMAND "${UF_CLI_EXECUTABLE}" upgrade
        --project "${UPGRADE_PROJECT}" --runtime "${UPGRADE_RUNTIME}"
        --handoff "${UPGRADE_HANDOFF_2}"
        --release-manifest-hash "sha256:${RELEASE_MANIFEST_HASH_2}"
        --artifact-root-hash "sha256:${ARTIFACT_MANIFEST_HASH_2}"
        --capability operate
    OUTPUT_VARIABLE REFUSED_OUTPUT
    ERROR_VARIABLE REFUSED_ERROR
    RESULT_VARIABLE REFUSED_STATUS
)
if(REFUSED_STATUS EQUAL 0)
    message(FATAL_ERROR
        "an upgrade that expands capabilities was accepted without approval:\n${REFUSED_OUTPUT}"
    )
endif()
string(FIND "${REFUSED_ERROR}" "capability expansion" EXPANSION_REFUSAL)
if(EXPANSION_REFUSAL EQUAL -1)
    message(FATAL_ERROR
        "the refused upgrade did not name the capability expansion:\n${REFUSED_ERROR}"
    )
endif()

# The approval that makes the expansion legal, recorded on the release the
# refused pin targeted.
execute_process(
    COMMAND "${UF_CLI_EXECUTABLE}" approve
        --runtime "${UPGRADE_RUNTIME}"
        --artifact-root-hash "sha256:${ARTIFACT_MANIFEST_HASH_2}"
        --evidence-hash "sha256:${UF_UPGRADE_EVIDENCE_HASH}"
        --capability operate
    OUTPUT_VARIABLE APPROVED_OUTPUT
    ERROR_VARIABLE APPROVED_ERROR
    RESULT_VARIABLE APPROVED_STATUS
)
if(NOT APPROVED_STATUS EQUAL 0)
    message(FATAL_ERROR
        "umbra-flow approve failed (${APPROVED_STATUS}):\n${APPROVED_OUTPUT}${APPROVED_ERROR}"
    )
endif()
string(FIND "${APPROVED_OUTPUT}" "${ARTIFACT_MANIFEST_HASH_2}" APPROVED_ROOT_NAMED)
if(APPROVED_ROOT_NAMED EQUAL -1)
    message(FATAL_ERROR
        "the approval report did not name the approved artifact root:\n${APPROVED_OUTPUT}"
    )
endif()

# The refused upgrade rolled the root back at attempted+1, so the approved
# upgrade installs at generation 4: 1 bootstrap, 1 attempted, 1 rollback, and
# this one. A CAS that compared against a stale generation would red here.
run_upgrade(
    THIRD_REPORT
    "${UPGRADE_HANDOFF_2}" "${ARTIFACT_MANIFEST_HASH_2}" "${RELEASE_MANIFEST_HASH_2}"
    --capability operate
)
installed_generation("${THIRD_REPORT}" THIRD_GENERATION)
if(NOT THIRD_GENERATION STREQUAL "4")
    message(FATAL_ERROR
        "the approved upgrade installed generation ${THIRD_GENERATION}, expected 4"
    )
endif()
string(FIND "${THIRD_REPORT}" "${ARTIFACT_MANIFEST_HASH_2}" THIRD_ROOT_NAMED)
if(THIRD_ROOT_NAMED EQUAL -1)
    message(FATAL_ERROR
        "the approved upgrade did not report the installed artifact root:\n${THIRD_REPORT}"
    )
endif()
]=]
    NEWLINE_STYLE LF
)
if(NOT EXISTS "${CLI_UPGRADE_DRIVER}")
    message(FATAL_ERROR "failed to generate the upgrade CLI test driver")
endif()

add_test(
    NAME test-upgrade-cli
    COMMAND ${CMAKE_COMMAND}
        -DUF_CLI_EXECUTABLE=$<TARGET_FILE:${PROJECT_NAME}>
        -DUF_UPGRADE_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/upgrade-cli-fixture
        -DUF_UPGRADE_PROJECT_FIXTURE=${UF_STAGED_UMBRAFLOW_PROJECT}
        -DUF_UPGRADE_REPLAY_GATE_HASH=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
        -DUF_UPGRADE_EVIDENCE_HASH=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
        -P "${CLI_UPGRADE_DRIVER}"
)
set_tests_properties(test-upgrade-cli PROPERTIES
    TIMEOUT 120
    LABELS "CI"
)

if(TARGET ${PROJECT_NAME}_script)
    cpp_add_test(
        NAME test-script
        SOURCES
            script/test-script.cpp
            script/test-adversarial-substrate.cpp
            script/test-environments.cpp
            script/test-pure-data-boundary.cpp
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
            task/test-exploration-verbs.cpp
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
            task/test-runtime-v3-contract.cpp
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
    # hold one type of that name; it lives in modules/conformance because the
    # Operator's delivery contracts need the same privilege. The dependency runs
    # from tests/ into the module and never back.
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

#include <core/safety/annotations.hpp>

#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace
{
    // Reporting must not itself become the reason the process dies: std::cerr's
    // inserters are not noexcept, and a throw out of a catch handler in main
    // leaves nowhere to report it. The exit code carries the outcome alone.
    [[nodiscard]]
    auto report(std::string_view message, int code) noexcept -> int
    {
        try
        {
            std::cerr << message << '\n';
        }
        catch (...)
        {
        }
        return code;
    }
}

namespace
{
    [[nodiscard]]
    auto runContract(std::string_view contract) -> int
    {
        auto input = std::ifstream{std::string{contract}, std::ios::binary};
        if (!input)
        {
            return report("cannot open Luau contract", 2);
        }
        auto const source = std::string{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}
        };
        if (input.bad())
        {
            return report("cannot read Luau contract", 2);
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
            return report(engine.error().message(), 1);
        }

        auto result = engine->runNumber(source, contract);
        if (!result)
        {
            return report(result.error().message(), 1);
        }
        if (*result != 1.0)
        {
            return report("Luau contract did not return 1", 1);
        }
        return 0;
    }
}

auto main(int argumentCount, char const* const* p_arguments) -> int
{
    try
    {
        if (argumentCount != 2)
        {
            return report("usage: task-luau-test-runner <test.luau>", 2);
        }

        // SAFETY: a hosted entry point receives argumentCount argument pointers
        // followed by a null one ([basic.start.main]/2). That count arrives
        // beside the pointer rather than within it, so no expression can restate
        // the bound; the refusal above fixes it at two, and this is the single
        // place the C contract becomes a span.
        UF_UNSAFE_BUFFER_BEGIN
        auto const arguments = std::span<char const* const>{p_arguments, 2U};
        UF_UNSAFE_BUFFER_END

        return runContract(std::string_view{arguments[1]});
    }
    catch (std::exception const& error)
    {
        return report(error.what(), 1);
    }
    catch (...)
    {
        return report("unknown failure", 1);
    }
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

    # Concrete paths, never a glob: a contract that stops running must be a
    # visible deletion here rather than a file that quietly left the build. The
    # coverage check below is the other half of that -- a hand-written
    # list silently stops covering the directory the moment somebody adds a file
    # to it, which is how tests/task/test-jcs.luau ran in no gate on 2026-08-10.
    set(TASK_LUAU_TESTS
        "test-jcs-luau|task/test-jcs.luau"
        "test-runtime-model-v3-luau|task/test-runtime-model-v3.luau"
        "test-resolution-v3-luau|task/test-resolution-v3.luau"
        "test-receipt-request-v3-luau|task/test-receipt-request-v3.luau"
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

if(TARGET test-operator)
    add_test(
        NAME fault-matrix-crash
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/fault-injection/run-crash.py"
            $<TARGET_FILE:test-operator>
            "${CMAKE_CURRENT_BINARY_DIR}/fault-injection"
    )
    add_test(
        NAME fault-matrix-lease
        COMMAND test-operator
            "--test-case=fault matrix lease takeover reports in-flight dispatch and fences the displaced controller"
    )
    add_test(
        NAME fault-matrix-stale
        COMMAND test-operator
            "--test-case=fault matrix stale instance expires without emitting input"
    )
    add_test(
        NAME fault-matrix-timeout
        COMMAND test-operator
            "--test-case=fault matrix timeout never records an unreturned action as success"
    )
    add_test(
        NAME fault-matrix-tamper
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/fault-injection/run-tamper.py"
            $<TARGET_FILE:test-project>
            $<TARGET_FILE:test-operator>
    )
    add_test(
        NAME fault-matrix-migration
        COMMAND test-operator
            "--test-case=fault matrix migration failure rolls back the active release"
    )
    set_tests_properties(
        fault-matrix-crash
        fault-matrix-lease
        fault-matrix-stale
        fault-matrix-timeout
        fault-matrix-tamper
        fault-matrix-migration
        PROPERTIES
            LABELS "CI;FAULT-INJECTION"
            TIMEOUT 60
    )
endif()

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
# The release manifest publisher is the only writer of the document a consumer's
# downloader parses, so its shape, determinism and refusals are pinned like the
# other tree-level checks rather than left to the release day.
add_test(
    NAME test-publish-release
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/test-publish-release.py
)
# No gate registered here may REQUIRE another repository. check-spec-bundle did
# until 2026-08-16 and carried the CI label, so this repository's gate could not
# pass on a machine without that checkout. Ruling:
# docs/plans/2026-08-09-runtime-hardening-rewrite.md.
set_tests_properties(
    check-repository-surface
    check-modules
    check-safety
    test-check-safety
    test-publish-release
    PROPERTIES
    TIMEOUT 30
    LABELS "CI"
)

# The consumer's frozen interface lock, by absolute path, declared by whoever
# configured this build:
#
#   cmake --preset x64-debug \
#     -DUF_CONSUMER_INTERFACE_LOCK=<uf-chaos>/conformance/interface-lock/v1.18
#
# A cache entry and not an environment variable, for the reason CONTEXT.md gives
# for `project --frames-root`: nothing in this repository reads the environment,
# and a run whose outcome depends on ambient state has no record of what
# produced it. CMakeCache.txt holds this one, so a red parity gate can be traced
# to the checkout that reddened it. It is empty by default, and no default may
# ever be added -- a path guessed here is the absolute-path dependency the
# ruling above removed.
set(UF_CONSUMER_INTERFACE_LOCK "" CACHE PATH
    "Consumer interface-lock directory the parity gate reads; empty skips it"
)

# check-, not contract-: a script over two trees, owning no requirement id, the
# sibling of check-modules above.
#
# Registered in both states rather than only when the lock is declared. An
# unregistered gate is a name `ctest -N` never prints, and this capability sat
# unregistered and therefore unrun from the day it was written until 2026-08-17.
# Skipped is a third status beside passed and failed: a machine with no consumer
# checkout reports the gate by name as not run, and never as agreement.
#
# Declared, it carries the CI label deliberately. What it compares is this
# repository's producer schemas against the bytes the consumer froze, and the
# rewrite authority's own rule is that implementation stops when the contract
# disagrees -- a divergence report nobody is required to read is the defect a
# missing registration already was. Declaring the lock is therefore opting in to
# being stopped by it.
if(UF_CONSUMER_INTERFACE_LOCK STREQUAL "")
    add_test(
        NAME check-interface-lock-parity
        COMMAND ${CMAKE_COMMAND} -E echo
            "SKIPPED check-interface-lock-parity: no consumer interface lock declared; configure with -DUF_CONSUMER_INTERFACE_LOCK=<checkout>/conformance/interface-lock/<version>"
    )
    set_tests_properties(check-interface-lock-parity PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIPPED check-interface-lock-parity"
    )
else()
    # Declared and absent is a failure, never a skip: the reason to skip is that
    # nobody named a lock, and a named lock that is not there is a wrong name.
    if(NOT IS_DIRECTORY "${UF_CONSUMER_INTERFACE_LOCK}/vectors")
        message(FATAL_ERROR
            "UF_CONSUMER_INTERFACE_LOCK does not name an interface lock: "
            "${UF_CONSUMER_INTERFACE_LOCK}/vectors does not exist"
        )
    endif()
    add_test(
        NAME check-interface-lock-parity
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/contracts/test_interface_lock_parity.py"
            --lock-root "${UF_CONSUMER_INTERFACE_LOCK}"
    )
endif()
set_tests_properties(check-interface-lock-parity PROPERTIES
    TIMEOUT 60
    LABELS "CI;PARITY"
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

# The authoring backend owns the behavioural halves of a03 and a05. Register
# their focused unittest methods under the requirement IDs as well as retaining
# the aggregate above, so either behaviour can fail its own gate.
add_test(
    NAME contract-agent-a03
    COMMAND
        ${Python3_EXECUTABLE} -m unittest
        tools.annotate.tests.test_backend.ReplayBundleTests.test_bundle_is_the_five_part_closure_and_validates_against_the_checked_in_schema
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)
add_test(
    NAME contract-agent-a05
    COMMAND
        ${Python3_EXECUTABLE} -m unittest
        tools.annotate.tests.test_backend.PublicationGateTests.test_each_gate_is_required_and_neither_is_satisfied_by_the_others_evidence
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)
set_tests_properties(
    contract-agent-a03
    contract-agent-a05
    PROPERTIES
    TIMEOUT 300
    LABELS "CI;CONTRACT"
)
set_property(GLOBAL APPEND PROPERTY UF_REGISTERED_CONTRACT_CASES
    contract-agent-a03
    contract-agent-a05
)

get_property(REGISTERED_CONTRACT_CASES GLOBAL PROPERTY UF_REGISTERED_CONTRACT_CASES)
set(SORTED_REQUIRED_CONTRACT_CASES ${UF_REQUIRED_CONTRACT_GATES})
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
if(NOT "${SHAPE_ONLY_REQUIREMENTS}" STREQUAL "${SORTED_SCHEMA_ONLY_REQUIREMENTS}")
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
