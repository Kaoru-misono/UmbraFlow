cmake_minimum_required(VERSION 3.30)

if(NOT EXISTS "${UF_PROJECT_EXECUTABLE}")
    message(FATAL_ERROR
        "project CLI executable does not exist: ${UF_PROJECT_EXECUTABLE}"
    )
endif()

set(SOURCE_DIRECTORY "${UF_PROJECT_TEST_ROOT}/source")
set(BUILD_DIRECTORY "${UF_PROJECT_TEST_ROOT}/build")
set(INPUT_PATH "${SOURCE_DIRECTORY}/declared.txt")
set(DECLARATIVE_DIRECTORY
    "${SOURCE_DIRECTORY}/declarative-tools/chaos.project"
)
set(DECLARATIVE_PATH
    "${DECLARATIVE_DIRECTORY}/dismiss-known-overlay.json"
)
set(GENERATED_ADAPTER
    "${BUILD_DIRECTORY}/generated/adapters/chaos.project/dismiss-known-overlay.luau"
)
set(GENERATED_ADAPTER_NAME
    "generated/adapters/chaos.project/dismiss-known-overlay.luau"
)

file(REMOVE_RECURSE "${UF_PROJECT_TEST_ROOT}")
file(MAKE_DIRECTORY "${SOURCE_DIRECTORY}" "${DECLARATIVE_DIRECTORY}")
file(WRITE "${INPUT_PATH}" "declared input\n")
file(WRITE "${DECLARATIVE_PATH}" [=[{
  "schema": "umbraflow-declarative-workflow-tool/v1",
  "tool_name": "chaos.dismiss_known_overlay",
  "target_argument": "observed_instance_id",
  "allowed_instance_kinds": ["chaos.overlay"],
  "fresh_observation": {
    "required_surface": "chaos.overlay_layer",
    "require_unambiguous": true
  },
  "ui_finding": {"kind": "observed_instance_absent"},
  "states": [
    {
      "state_key": "await-overlay",
      "kind": "wait",
      "observation_budget": 1,
      "timeout_ms": 1000
    },
    {
      "state_key": "dismiss-overlay",
      "kind": "ui_action",
      "ui_action": "chaos.ui.dismiss_overlay",
      "timeout_ms": 2000
    }
  ],
  "steps": ["await-overlay", "dismiss-overlay"],
  "bounds": {
    "maximum_states": 2,
    "maximum_steps": 2,
    "maximum_dispatches": 1,
    "maximum_observations": 2,
    "maximum_waits": 1,
    "maximum_elapsed_ms": 3000
  }
}]=])
file(SHA256 "${INPUT_PATH}" SOURCE_HASH_BEFORE)
file(SHA256 "${DECLARATIVE_PATH}" DECLARATIVE_HASH_BEFORE)
file(GLOB_RECURSE SOURCE_FILES_BEFORE
    RELATIVE "${SOURCE_DIRECTORY}"
    "${SOURCE_DIRECTORY}/*"
)

execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" init
        --source "${SOURCE_DIRECTORY}"
        --build "${BUILD_DIRECTORY}"
        --input declared.txt
        --input declarative-tools/chaos.project/dismiss-known-overlay.json
    RESULT_VARIABLE INIT_RESULT
    OUTPUT_VARIABLE INIT_OUTPUT
    ERROR_VARIABLE INIT_ERROR
)
if(NOT INIT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project init must exit 0; exit=${INIT_RESULT}; "
        "stdout=[${INIT_OUTPUT}]; stderr=[${INIT_ERROR}]"
    )
endif()

execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" build
        --source "${SOURCE_DIRECTORY}"
        --build "${BUILD_DIRECTORY}"
    RESULT_VARIABLE BUILD_RESULT
    OUTPUT_VARIABLE BUILD_OUTPUT
    ERROR_VARIABLE BUILD_ERROR
)
if(NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must exit 0; exit=${BUILD_RESULT}; "
        "stdout=[${BUILD_OUTPUT}]; stderr=[${BUILD_ERROR}]"
    )
endif()

file(SHA256 "${INPUT_PATH}" SOURCE_HASH_AFTER)
file(SHA256 "${DECLARATIVE_PATH}" DECLARATIVE_HASH_AFTER)
file(GLOB_RECURSE SOURCE_FILES_AFTER
    RELATIVE "${SOURCE_DIRECTORY}"
    "${SOURCE_DIRECTORY}/*"
)
if(
    NOT SOURCE_HASH_BEFORE STREQUAL SOURCE_HASH_AFTER
    OR NOT DECLARATIVE_HASH_BEFORE STREQUAL DECLARATIVE_HASH_AFTER
    OR NOT SOURCE_FILES_BEFORE STREQUAL SOURCE_FILES_AFTER
)
    message(FATAL_ERROR
        "project build must leave the source tree unchanged"
    )
endif()

execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" check
        --source "${SOURCE_DIRECTORY}"
        --build "${BUILD_DIRECTORY}"
    RESULT_VARIABLE CHECK_RESULT
    OUTPUT_VARIABLE CHECK_OUTPUT
    ERROR_VARIABLE CHECK_ERROR
)
if(NOT CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must exit 0; exit=${CHECK_RESULT}; "
        "stdout=[${CHECK_OUTPUT}]; stderr=[${CHECK_ERROR}]"
    )
endif()

if(NOT EXISTS "${GENERATED_ADAPTER}")
    message(FATAL_ERROR
        "project build must generate ${GENERATED_ADAPTER_NAME}"
    )
endif()
file(WRITE "${GENERATED_ADAPTER}" "hand edited\n")
execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" check
        --source "${SOURCE_DIRECTORY}"
        --build "${BUILD_DIRECTORY}"
    RESULT_VARIABLE EDITED_RESULT
    OUTPUT_VARIABLE EDITED_OUTPUT
    ERROR_VARIABLE EDITED_ERROR
)
if(EDITED_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must reject a hand-edited generated adapter"
    )
endif()
set(EXPECTED_EDITED_ERROR
    "generated project artifact \"${GENERATED_ADAPTER_NAME}\" does not match its declared source"
)
string(FIND "${EDITED_ERROR}" "${EXPECTED_EDITED_ERROR}" EDITED_NAME_INDEX)
if(EDITED_NAME_INDEX EQUAL -1)
    message(FATAL_ERROR
        "edited-artifact diagnostic must be [${EXPECTED_EDITED_ERROR}]; "
        "stderr=[${EDITED_ERROR}]"
    )
endif()

execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" build
        --source "${SOURCE_DIRECTORY}"
        --build "${BUILD_DIRECTORY}"
    RESULT_VARIABLE REBUILD_RESULT
    OUTPUT_VARIABLE REBUILD_OUTPUT
    ERROR_VARIABLE REBUILD_ERROR
)
if(NOT REBUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must replace a hand-edited generated adapter; "
        "exit=${REBUILD_RESULT}; stdout=[${REBUILD_OUTPUT}]; "
        "stderr=[${REBUILD_ERROR}]"
    )
endif()

file(REMOVE "${INPUT_PATH}")
execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" check
        --source "${SOURCE_DIRECTORY}"
        --build "${BUILD_DIRECTORY}"
    RESULT_VARIABLE MISSING_RESULT
    OUTPUT_VARIABLE MISSING_OUTPUT
    ERROR_VARIABLE MISSING_ERROR
)
if(MISSING_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must reject a removed declared input"
    )
endif()
string(FIND "${MISSING_ERROR}" "declared.txt" MISSING_NAME_INDEX)
if(MISSING_NAME_INDEX EQUAL -1)
    message(FATAL_ERROR
        "missing-input diagnostic must name declared.txt; "
        "stderr=[${MISSING_ERROR}]"
    )
endif()
