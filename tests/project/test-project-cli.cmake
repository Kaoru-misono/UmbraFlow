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
set(DECLARED_CATALOG
    "${SOURCE_DIRECTORY}/generated/tool-catalogs/chaos.project/tool-catalog-v1.json"
)
set(GENERATED_CATALOG
    "${BUILD_DIRECTORY}/generated/tool-catalogs/chaos.project/tool-catalog-v1.json"
)
set(GENERATED_CATALOG_NAME
    "generated/tool-catalogs/chaos.project/tool-catalog-v1.json"
)
set(GENERATED_BLOB
    "${BUILD_DIRECTORY}/generated/artifact-blobs/facts.blob"
)
set(GENERATED_BLOB_NAME
    "generated/artifact-blobs/facts.blob"
)
set(GENERATED_REGISTRATION
    "${BUILD_DIRECTORY}/generated/registrations/artifact-roots-v1.json"
)
set(GENERATED_REGISTRATION_NAME
    "generated/registrations/artifact-roots-v1.json"
)

file(REMOVE_RECURSE "${UF_PROJECT_TEST_ROOT}")
file(MAKE_DIRECTORY "${SOURCE_DIRECTORY}" "${DECLARATIVE_DIRECTORY}")
file(WRITE "${INPUT_PATH}" "declared input\n")

# A source tree is a project only when it holds umbraflow-project.json at its
# root, so the CLI's own rehearsal writes one. It is deliberately not declared
# as an input: build and check judge it either way.
file(WRITE "${SOURCE_DIRECTORY}/umbraflow-project.json" [=[{
  "schema": "umbraflow-project/v1",
  "runtime_artifact": "runtime/artifact",
  "primary_deployment": "dream",
  "template_cuts": [],
  "deployments": [
    {
      "name": "dream",
      "plugin_id": "chaos.dream",
      "baseline_event_type": "project.baseline_created",
      "plugin": "generated/adapters/chaos.project/dismiss-known-overlay.luau",
      "plugin_authoring": "generated",
      "project_state_schema": "schema/state.json",
      "project_observation_schema": "schema/observation.json",
      "tool_precondition_schema": "schema/precondition.json",
      "reconcile_schema": "schema/reconcile.json",
      "tool_catalog": "generated/tool-catalogs/chaos.project/tool-catalog-v1.json",
      "journal_event_schema_manifest": "schema/journal-manifest.json",
      "reconcile_manifest": "schema/reconcile-manifest.json",
      "journal_payload_schemas": ["schema/journal-0.json"],
      "effect_payload_schemas": [],
      "observed_instance_identity_schemas": [],
      "artifact_blobs": [
        {"name": "facts", "path": "content/facts.txt"}
      ]
    }
  ]
}]=])
file(MAKE_DIRECTORY
    "${SOURCE_DIRECTORY}/generated/tool-catalogs/chaos.project"
    "${SOURCE_DIRECTORY}/content"
)
file(WRITE "${DECLARED_CATALOG}" [=[{
  "schema": "umbraflow-tool-catalog/v1",
  "plugin_id": "chaos.project",
  "tool_precondition_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "effect_payload_sha256s": [],
  "tools": [
    {
      "name": "chaos.dismiss_known_overlay",
      "argument_schema": "observed_instance_id",
      "version": "1.0.0",
      "mutability": "read_only",
      "surface": "semantic",
      "idempotency": "delivery_safe",
      "required_capabilities": [],
      "ui_action_bounds": [],
      "effect_bounds": [],
      "timeout_policy": {
        "maximum_elapsed_ms": 3000,
        "on_timeout": "stop"
      },
      "workflow_limits": {
        "maximum_steps": 2,
        "maximum_dispatches": 1,
        "maximum_observations": 2,
        "maximum_waits": 1,
        "maximum_elapsed_ms": 3000
      }
    }
  ]
}]=])
file(WRITE "${SOURCE_DIRECTORY}/content/facts.txt" "declared facts\n")
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
        --input content/facts.txt
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
if(NOT EXISTS "${GENERATED_CATALOG}")
    message(FATAL_ERROR
        "project build must generate ${GENERATED_CATALOG_NAME} from the "
        "deployment's declared tool catalog source"
    )
endif()
if(NOT EXISTS "${GENERATED_BLOB}")
    message(FATAL_ERROR
        "project build must generate ${GENERATED_BLOB_NAME} from the "
        "deployment's declared artifact blob"
    )
endif()
if(NOT EXISTS "${GENERATED_REGISTRATION}")
    message(FATAL_ERROR
        "project build must generate ${GENERATED_REGISTRATION_NAME} from the "
        "declared artifact closure"
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

# ----------------------------------------------------------------------------
# Template cuts, end to end through the command line.
#
# The capability landed as a C++ API in 2b378df and the command line never
# reached it: build, check and freeze each passed an empty resolver, so a
# project could not declare a cut at all and the first consumer that wanted one
# went on cropping images in its own Python. Everything below is that gap,
# stated as cases.
#
# Nothing here writes a hash down. The corpus is a PNG this repository already
# ships, copied under the name its own sha256 gives it, and the declaration
# interpolates that same computed value -- so a fixture that stopped hashing
# what it copied could not go on agreeing with itself.
# ----------------------------------------------------------------------------
function(run_project OUT_RESULT OUT_DIAGNOSTIC)
    execute_process(
        COMMAND "${UF_PROJECT_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE COMMAND_RESULT
        OUTPUT_VARIABLE COMMAND_OUTPUT
        ERROR_VARIABLE COMMAND_ERROR
    )
    set(${OUT_RESULT} "${COMMAND_RESULT}" PARENT_SCOPE)
    set(${OUT_DIAGNOSTIC} "${COMMAND_OUTPUT}${COMMAND_ERROR}" PARENT_SCOPE)
endfunction()

function(require_contains LABEL HAYSTACK NEEDLE)
    string(FIND "${HAYSTACK}" "${NEEDLE}" FOUND_AT)
    if(FOUND_AT EQUAL -1)
        message(FATAL_ERROR
            "${LABEL} must name [${NEEDLE}]; diagnostic=[${HAYSTACK}]"
        )
    endif()
endfunction()

# ----------------------------------------------------------------------------
# The deployment declaration's tool catalog and artifact closure, end to end.
#
# Every deployment names its declared tool catalog source and its artifact
# blobs. build reads the first back into generated/tool-catalogs/ and the
# second back into generated/artifact-blobs/, with
# generated/registrations/artifact-roots-v1.json stating exactly the closure
# those blobs declare. Nothing states a registration separately, so the only
# way a registration can disagree with the closure is a hand edit -- and
# check must name it like every other generated artifact.
#
# The last build above happened before declared.txt was removed, so the tree
# still holds every generated artifact; this section restores the declared
# input and then mutates one generated artifact at a time, checking that the
# command names the exact file or blob and that a build replaces it.
# ----------------------------------------------------------------------------
file(WRITE "${INPUT_PATH}" "declared input\n")

# G1. A hand-edited generated Tool Catalog is refused by name, and a rebuild
# replaces it.
file(WRITE "${GENERATED_CATALOG}" "hand edited\n")
run_project(EDITED_CATALOG_RESULT EDITED_CATALOG_DIAGNOSTIC check
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(EDITED_CATALOG_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must reject a hand-edited generated Tool Catalog"
    )
endif()
require_contains("the edited-catalog refusal"
    "${EDITED_CATALOG_DIAGNOSTIC}"
    "generated project artifact \"${GENERATED_CATALOG_NAME}\" does not match its declared source")
run_project(EDITED_CATALOG_RESTORE_RESULT EDITED_CATALOG_RESTORE_DIAGNOSTIC build
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(NOT EDITED_CATALOG_RESTORE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must replace a hand-edited generated Tool Catalog; "
        "diagnostic=[${EDITED_CATALOG_RESTORE_DIAGNOSTIC}]"
    )
endif()

# G2. A deleted generated artifact blob is refused by name.
file(REMOVE "${GENERATED_BLOB}")
run_project(MISSING_BLOB_RESULT MISSING_BLOB_DIAGNOSTIC check
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(MISSING_BLOB_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must reject a deleted generated artifact blob"
    )
endif()
require_contains("the missing-blob refusal" "${MISSING_BLOB_DIAGNOSTIC}"
    "generated project artifact \"${GENERATED_BLOB_NAME}\" is missing")
run_project(MISSING_BLOB_RESTORE_RESULT MISSING_BLOB_RESTORE_DIAGNOSTIC build
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(NOT MISSING_BLOB_RESTORE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must restore a deleted generated artifact blob; "
        "diagnostic=[${MISSING_BLOB_RESTORE_DIAGNOSTIC}]"
    )
endif()

# G3. The artifact-roots registration names a blob the closure does not
# declare; check refuses the registration file byte for byte.
file(WRITE "${GENERATED_REGISTRATION}" [=[{
  "artifact_roots": [
    {
      "name": "other",
      "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
    }
  ],
  "schema": "umbraflow-project-kit-artifact-registration/v1"
}]=])
run_project(EDITED_REGISTRATION_RESULT EDITED_REGISTRATION_DIAGNOSTIC check
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(EDITED_REGISTRATION_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must reject an artifact-roots registration that "
        "disagrees with the declared closure"
    )
endif()
require_contains("the edited-registration refusal"
    "${EDITED_REGISTRATION_DIAGNOSTIC}"
    "generated project artifact \"${GENERATED_REGISTRATION_NAME}\" does not match its declared source")
run_project(EDITED_REGISTRATION_RESTORE_RESULT EDITED_REGISTRATION_RESTORE_DIAGNOSTIC build
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(NOT EDITED_REGISTRATION_RESTORE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must replace an edited artifact-roots registration; "
        "diagnostic=[${EDITED_REGISTRATION_RESTORE_DIAGNOSTIC}]"
    )
endif()

# G4. A declared tool catalog source the tree does not hold is refused by
# name at build time, like a declared cut's missing corpus.
file(REMOVE "${DECLARED_CATALOG}")
run_project(MISSING_CATALOG_SOURCE_RESULT MISSING_CATALOG_SOURCE_DIAGNOSTIC build
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(MISSING_CATALOG_SOURCE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must refuse a declared tool catalog source the tree "
        "does not hold"
    )
endif()
require_contains("the missing-catalog-source refusal"
    "${MISSING_CATALOG_SOURCE_DIAGNOSTIC}" "tool catalog source")
require_contains("the missing-catalog-source refusal"
    "${MISSING_CATALOG_SOURCE_DIAGNOSTIC}" "${GENERATED_CATALOG_NAME}")
file(WRITE "${DECLARED_CATALOG}" [=[{
  "schema": "umbraflow-tool-catalog/v1",
  "plugin_id": "chaos.project",
  "tool_precondition_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "effect_payload_sha256s": [],
  "tools": [
    {
      "name": "chaos.dismiss_known_overlay",
      "argument_schema": "observed_instance_id",
      "version": "1.0.0",
      "mutability": "read_only",
      "surface": "semantic",
      "idempotency": "delivery_safe",
      "required_capabilities": [],
      "ui_action_bounds": [],
      "effect_bounds": [],
      "timeout_policy": {
        "maximum_elapsed_ms": 3000,
        "on_timeout": "stop"
      },
      "workflow_limits": {
        "maximum_steps": 2,
        "maximum_dispatches": 1,
        "maximum_observations": 2,
        "maximum_waits": 1,
        "maximum_elapsed_ms": 3000
      }
    }
  ]
}]=])
run_project(CATALOG_SOURCE_RESTORE_RESULT CATALOG_SOURCE_RESTORE_DIAGNOSTIC build
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(NOT CATALOG_SOURCE_RESTORE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must accept a restored declared tool catalog source; "
        "diagnostic=[${CATALOG_SOURCE_RESTORE_DIAGNOSTIC}]"
    )
endif()

set(CUT_ROOT "${UF_PROJECT_TEST_ROOT}/template-cut")
set(CUT_SOURCE "${CUT_ROOT}/source")
set(CUT_BUILD "${CUT_ROOT}/build")
set(CUT_NO_CORPUS_BUILD "${CUT_ROOT}/build-no-corpus")
set(CUT_WRONG_BYTES_BUILD "${CUT_ROOT}/build-wrong-bytes")
set(CUT_RELEASE "${CUT_ROOT}/release")
set(CUT_CORPUS "${CUT_ROOT}/corpus")
set(CUT_LYING_CORPUS "${CUT_ROOT}/lying-corpus")
set(CUT_TEMPLATE_NAME "generated/templates/locator/mark.png")

if(NOT EXISTS "${UF_PROJECT_TEST_FRAME}")
    message(FATAL_ERROR
        "the template-cut fixture source is missing: ${UF_PROJECT_TEST_FRAME}"
    )
endif()
if(NOT EXISTS "${UF_PROJECT_TEST_OTHER_FRAME}")
    message(FATAL_ERROR
        "the mismatched-bytes fixture source is missing: "
        "${UF_PROJECT_TEST_OTHER_FRAME}"
    )
endif()

file(MAKE_DIRECTORY "${CUT_SOURCE}" "${CUT_CORPUS}" "${CUT_LYING_CORPUS}")
file(WRITE "${CUT_SOURCE}/declared.txt" "declared input\n")
file(SHA256 "${UF_PROJECT_TEST_FRAME}" CUT_SOURCE_HASH)
file(SHA256 "${UF_PROJECT_TEST_OTHER_FRAME}" CUT_OTHER_HASH)
file(COPY_FILE
    "${UF_PROJECT_TEST_FRAME}"
    "${CUT_CORPUS}/${CUT_SOURCE_HASH}.png"
)
# A corpus whose file names lie: the name the declaration asks for, over
# another image's bytes. The kit re-hashes what a resolver answers, and this is
# what proves the command line reaches that check instead of going around it.
file(COPY_FILE
    "${UF_PROJECT_TEST_OTHER_FRAME}"
    "${CUT_LYING_CORPUS}/${CUT_SOURCE_HASH}.png"
)

file(WRITE "${CUT_SOURCE}/umbraflow-project.json" "{
  \"schema\": \"umbraflow-project/v1\",
  \"runtime_artifact\": \"runtime/artifact\",
  \"primary_deployment\": \"dream\",
  \"template_cuts\": [
    {
      \"template\": \"locator/mark.png\",
      \"source_sha256s\": [\"${CUT_SOURCE_HASH}\"],
      \"rect\": {\"x\": 0, \"y\": 0, \"width\": 2, \"height\": 1}
    }
  ],
  \"deployments\": [
    {
      \"name\": \"dream\",
      \"plugin_id\": \"chaos.dream\",
      \"baseline_event_type\": \"project.baseline_created\",
      \"plugin\": \"plugin/dream.luau\",
      \"plugin_authoring\": \"hand-written\",
      \"plugin_justification\": \"A fixture plugin that answers from constants: umbraflow-declarative-workflow-tool/v1 has no member that decides what a Reduce returns.\",
      \"project_state_schema\": \"schema/state.json\",
      \"project_observation_schema\": \"schema/observation.json\",
      \"tool_precondition_schema\": \"schema/precondition.json\",
      \"reconcile_schema\": \"schema/reconcile.json\",
      \"tool_catalog\": \"schema/catalog.json\",
      \"journal_event_schema_manifest\": \"schema/journal-manifest.json\",
      \"reconcile_manifest\": \"schema/reconcile-manifest.json\",
      \"journal_payload_schemas\": [\"schema/journal-0.json\"],
      \"effect_payload_schemas\": [],
      \"observed_instance_identity_schemas\": [],
      \"artifact_blobs\": []
    }
  ]
}
")
file(WRITE "${CUT_SOURCE}/schema/catalog.json" [=[{
  "schema": "umbraflow-tool-catalog/v1",
  "plugin_id": "chaos.dream",
  "tool_precondition_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "effect_payload_sha256s": [],
  "tools": [
    {
      "name": "chaos.dismiss_known_overlay",
      "argument_schema": "observed_instance_id",
      "version": "1.0.0",
      "mutability": "read_only",
      "surface": "semantic",
      "idempotency": "delivery_safe",
      "required_capabilities": [],
      "ui_action_bounds": [],
      "effect_bounds": [],
      "timeout_policy": {
        "maximum_elapsed_ms": 3000,
        "on_timeout": "stop"
      },
      "workflow_limits": {
        "maximum_steps": 2,
        "maximum_dispatches": 1,
        "maximum_observations": 2,
        "maximum_waits": 1,
        "maximum_elapsed_ms": 3000
      }
    }
  ]
}]=])

foreach(CUT_BUILD_DIRECTORY
    "${CUT_BUILD}"
    "${CUT_NO_CORPUS_BUILD}"
    "${CUT_WRONG_BYTES_BUILD}"
)
    run_project(CUT_INIT_RESULT CUT_INIT_DIAGNOSTIC init
        --source "${CUT_SOURCE}"
        --build "${CUT_BUILD_DIRECTORY}"
        --input declared.txt
    )
    if(NOT CUT_INIT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "project init must exit 0 for ${CUT_BUILD_DIRECTORY}; "
            "exit=${CUT_INIT_RESULT}; diagnostic=[${CUT_INIT_DIAGNOSTIC}]"
        )
    endif()
endforeach()

# F1, the positive control. Without it every refusal below is satisfied by a
# command that refuses every cut there is.
run_project(CUT_BUILD_RESULT CUT_BUILD_DIAGNOSTIC build
    --source "${CUT_SOURCE}"
    --build "${CUT_BUILD}"
    --frames-root "${CUT_CORPUS}"
)
if(NOT CUT_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must cut a declared template when its source resolves; "
        "exit=${CUT_BUILD_RESULT}; diagnostic=[${CUT_BUILD_DIAGNOSTIC}]"
    )
endif()
if(NOT EXISTS "${CUT_BUILD}/${CUT_TEMPLATE_NAME}")
    message(FATAL_ERROR
        "project build must produce ${CUT_TEMPLATE_NAME} from the declaration"
    )
endif()

# F2. A source no corpus can answer for is refused by name, and the refusal
# says how to supply one. The build directory is a fresh one, so the absent
# artifact below is evidence that nothing was produced rather than evidence
# that something earlier produced it.
run_project(CUT_UNRESOLVED_RESULT CUT_UNRESOLVED_DIAGNOSTIC build
    --source "${CUT_SOURCE}"
    --build "${CUT_NO_CORPUS_BUILD}"
)
if(CUT_UNRESOLVED_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must refuse a declared cut it cannot resolve rather "
        "than skipping it; diagnostic=[${CUT_UNRESOLVED_DIAGNOSTIC}]"
    )
endif()
require_contains("the unresolved-source refusal"
    "${CUT_UNRESOLVED_DIAGNOSTIC}" "${CUT_SOURCE_HASH}")
require_contains("the unresolved-source refusal"
    "${CUT_UNRESOLVED_DIAGNOSTIC}" "--frames-root")
if(EXISTS "${CUT_NO_CORPUS_BUILD}/${CUT_TEMPLATE_NAME}")
    message(FATAL_ERROR
        "a cut that could not be resolved must leave no template behind"
    )
endif()

# F3. Bytes that do not hash to what was asked for are refused, and the refusal
# names both hashes. The command line supplies bytes and never verifies them,
# so this only passes if it reaches the kit's check.
run_project(CUT_MISMATCH_RESULT CUT_MISMATCH_DIAGNOSTIC build
    --source "${CUT_SOURCE}"
    --build "${CUT_WRONG_BYTES_BUILD}"
    --frames-root "${CUT_LYING_CORPUS}"
)
if(CUT_MISMATCH_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project build must refuse resolved bytes whose hash is not the one "
        "declared; diagnostic=[${CUT_MISMATCH_DIAGNOSTIC}]"
    )
endif()
require_contains("the mismatched-source refusal"
    "${CUT_MISMATCH_DIAGNOSTIC}" "${CUT_SOURCE_HASH}")
require_contains("the mismatched-source refusal"
    "${CUT_MISMATCH_DIAGNOSTIC}" "${CUT_OTHER_HASH}")
if(EXISTS "${CUT_WRONG_BYTES_BUILD}/${CUT_TEMPLATE_NAME}")
    message(FATAL_ERROR
        "a cut whose source bytes were wrong must leave no template behind"
    )
endif()

# F4. check and freeze answer exactly as build does. The build directory below
# already holds the template from F1, so a check that judged the tree instead
# of the declaration would pass here -- which is the whole point of asking.
run_project(CUT_CHECK_RESULT CUT_CHECK_DIAGNOSTIC check
    --source "${CUT_SOURCE}"
    --build "${CUT_BUILD}"
)
if(CUT_CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must refuse a declared cut it cannot resolve, even "
        "with the template already built; "
        "diagnostic=[${CUT_CHECK_DIAGNOSTIC}]"
    )
endif()
require_contains("the check refusal"
    "${CUT_CHECK_DIAGNOSTIC}" "${CUT_SOURCE_HASH}")
require_contains("the check refusal"
    "${CUT_CHECK_DIAGNOSTIC}" "--frames-root")

run_project(CUT_FREEZE_REFUSAL_RESULT CUT_FREEZE_REFUSAL_DIAGNOSTIC freeze
    --source "${CUT_SOURCE}"
    --build "${CUT_BUILD}"
    --release "${CUT_RELEASE}"
)
if(CUT_FREEZE_REFUSAL_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project freeze must refuse a declared cut it cannot resolve; "
        "diagnostic=[${CUT_FREEZE_REFUSAL_DIAGNOSTIC}]"
    )
endif()
require_contains("the freeze refusal"
    "${CUT_FREEZE_REFUSAL_DIAGNOSTIC}" "${CUT_SOURCE_HASH}")
if(EXISTS "${CUT_RELEASE}")
    message(FATAL_ERROR
        "a freeze that refused a declared cut must publish no release"
    )
endif()

run_project(CUT_CHECKED_RESULT CUT_CHECKED_DIAGNOSTIC check
    --source "${CUT_SOURCE}"
    --build "${CUT_BUILD}"
    --frames-root "${CUT_CORPUS}"
)
if(NOT CUT_CHECKED_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must accept the build it just produced when the same "
        "corpus is given; exit=${CUT_CHECKED_RESULT}; "
        "diagnostic=[${CUT_CHECKED_DIAGNOSTIC}]"
    )
endif()

run_project(CUT_FROZEN_RESULT CUT_FROZEN_DIAGNOSTIC freeze
    --source "${CUT_SOURCE}"
    --build "${CUT_BUILD}"
    --release "${CUT_RELEASE}"
    --frames-root "${CUT_CORPUS}"
)
if(NOT CUT_FROZEN_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project freeze must publish a release whose declared cut resolves; "
        "exit=${CUT_FROZEN_RESULT}; diagnostic=[${CUT_FROZEN_DIAGNOSTIC}]"
    )
endif()
file(GLOB_RECURSE FROZEN_TEMPLATES
    RELATIVE "${CUT_RELEASE}"
    "${CUT_RELEASE}/*/${CUT_TEMPLATE_NAME}"
)
if(FROZEN_TEMPLATES STREQUAL "")
    message(FATAL_ERROR
        "the frozen release must carry the template the declaration cut"
    )
endif()
