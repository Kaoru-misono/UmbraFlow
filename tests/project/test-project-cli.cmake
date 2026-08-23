cmake_minimum_required(VERSION 3.30)

if(NOT EXISTS "${UF_PROJECT_EXECUTABLE}")
    message(FATAL_ERROR
        "project CLI executable does not exist: ${UF_PROJECT_EXECUTABLE}"
    )
endif()

set(SOURCE_DIRECTORY "${UF_PROJECT_TEST_ROOT}/source")
set(BUILD_DIRECTORY "${UF_PROJECT_TEST_ROOT}/build")
# A declared input is a file the declaration names -- here the deployment's own
# resource. There is no flag that adds one beside the declaration, so the file
# whose removal a case below checks has to be one the document carries.
set(INPUT_PATH "${SOURCE_DIRECTORY}/content/facts.txt")
set(DECLARATIVE_DIRECTORY
    "${SOURCE_DIRECTORY}/declarative-tools/chaos.project"
)
set(DECLARATIVE_PATH
    "${DECLARATIVE_DIRECTORY}/dismiss-known-overlay.json"
)
set(ADAPTER_DIRECTORY
    "generated/adapters/chaos.project/dismiss-known-overlay"
)
set(GENERATED_ADAPTER
    "${BUILD_DIRECTORY}/${ADAPTER_DIRECTORY}/tool.luau"
)
set(GENERATED_ADAPTER_NAME
    "${ADAPTER_DIRECTORY}/tool.luau"
)
set(GENERATED_BLOB
    "${BUILD_DIRECTORY}/generated/resources/dream/facts.blob"
)
set(GENERATED_BLOB_NAME
    "generated/resources/dream/facts.blob"
)
set(GENERATED_MODULE
    "${BUILD_DIRECTORY}/generated/modules/dream/tool/main.luau"
)
set(GENERATED_MODULE_NAME
    "generated/modules/dream/tool/main.luau"
)
set(GENERATED_REGISTRATION
    "${BUILD_DIRECTORY}/generated/registrations/dream.json"
)
set(GENERATED_REGISTRATION_NAME
    "generated/registrations/dream.json"
)

file(REMOVE_RECURSE "${UF_PROJECT_TEST_ROOT}")
file(MAKE_DIRECTORY "${SOURCE_DIRECTORY}" "${DECLARATIVE_DIRECTORY}")

# A source tree is a project only when it holds umbraflow-project.json at its
# root, so the CLI's own rehearsal writes one. It is deliberately not declared
# as an input: build and check judge it either way.
file(WRITE "${SOURCE_DIRECTORY}/umbraflow-project.json" [=[{
  "schema": "umbraflow-project/v3",
  "runtime_artifact": "runtime/artifact",
  "primary_deployment": "dream",
  "template_cuts": [],
  "deployments": [
    {
      "name": "dream",
      "plugin_id": "chaos.dream",
      "tool_closure": {
        "entry": "main",
        "exported_entry_points": [],
        "modules": [
          {
            "name": "main",
            "path": "generated/adapters/chaos.project/dismiss-known-overlay/tool.luau"
          }
        ]
      },
      "plugin_authoring": "generated",
      "tools": [],
      "observed_instance_identity_schemas": [],
      "tool_bindings": [],
      "resources": [
        {"kind": "utf8", "name": "facts", "path": "content/facts.txt"}
      ]
    }
  ]
}]=])
file(MAKE_DIRECTORY "${SOURCE_DIRECTORY}/content")
file(WRITE "${SOURCE_DIRECTORY}/content/facts.txt" "declared facts\n")
file(WRITE "${DECLARATIVE_PATH}" [=[{
  "schema": "umbraflow-declarative-workflow-tool/v1",
  "tool_name": "chaos.project.dismiss_known_overlay",
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
if(NOT EXISTS "${GENERATED_BLOB}")
    message(FATAL_ERROR
        "project build must generate ${GENERATED_BLOB_NAME} from the "
        "deployment's declared resource"
    )
endif()
if(NOT EXISTS "${GENERATED_MODULE}")
    message(FATAL_ERROR
        "project build must generate ${GENERATED_MODULE_NAME} from the "
        "deployment's generated tool closure"
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
string(FIND "${MISSING_ERROR}" "content/facts.txt" MISSING_NAME_INDEX)
if(MISSING_NAME_INDEX EQUAL -1)
    message(FATAL_ERROR
        "missing-input diagnostic must name content/facts.txt; "
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
# The deployment declaration's execution closure, end to end.
#
# Every deployment names its module closure and its typed resources. build
# materializes the exact execution bytes and writes one
# generated/registrations/DEPLOYMENT.json identity record. A hand edit must be
# named like every other generated artifact.
#
# The last build above happened before the declared resource was removed, so the tree
# still holds every generated artifact; this section restores the declared
# input and then mutates one generated artifact at a time, checking that the
# command names the exact file or blob and that a build replaces it.
# ----------------------------------------------------------------------------
file(WRITE "${INPUT_PATH}" "declared facts\n")

# G2. A deleted generated resource is refused by name.
file(REMOVE "${GENERATED_BLOB}")
run_project(MISSING_BLOB_RESULT MISSING_BLOB_DIAGNOSTIC check
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(MISSING_BLOB_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must reject a deleted generated resource"
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
        "project build must restore a deleted generated resource; "
        "diagnostic=[${MISSING_BLOB_RESTORE_DIAGNOSTIC}]"
    )
endif()

# G3. A hand-edited execution-closure record is refused byte for byte.
file(WRITE "${GENERATED_REGISTRATION}" "hand edited\n")
run_project(EDITED_REGISTRATION_RESULT EDITED_REGISTRATION_DIAGNOSTIC check
    --source "${SOURCE_DIRECTORY}"
    --build "${BUILD_DIRECTORY}"
)
if(EDITED_REGISTRATION_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project check must reject an execution-closure record that "
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
        "project build must replace an edited execution-closure record; "
        "diagnostic=[${EDITED_REGISTRATION_RESTORE_DIAGNOSTIC}]"
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

file(MAKE_DIRECTORY
    "${CUT_SOURCE}"
    "${CUT_SOURCE}/plugin"
    "${CUT_CORPUS}"
    "${CUT_LYING_CORPUS}"
)

file(WRITE "${CUT_SOURCE}/plugin/dream.luau" [=[return {
    plugin_id = "chaos.dream",
}
]=])
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
  \"schema\": \"umbraflow-project/v3\",
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
      \"tool_closure\": {
        \"entry\": \"main\",
        \"exported_entry_points\": [],
        \"modules\": [
          {\"name\": \"main\", \"path\": \"plugin/dream.luau\"}
        ]
      },
      \"plugin_authoring\": \"hand-written\",
      \"plugin_justification\": \"A fixture plugin that answers from constants: umbraflow-declarative-workflow-tool/v1 has no member that decides what a handler returns.\",
      \"tools\": [],
      \"observed_instance_identity_schemas\": [],
      \"tool_bindings\": [],
      \"resources\": []
    }
  ]
}
")

foreach(CUT_BUILD_DIRECTORY
    "${CUT_BUILD}"
    "${CUT_NO_CORPUS_BUILD}"
    "${CUT_WRONG_BYTES_BUILD}"
)
    run_project(CUT_INIT_RESULT CUT_INIT_DIAGNOSTIC init
        --source "${CUT_SOURCE}"
        --build "${CUT_BUILD_DIRECTORY}"
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

# F5. An authored manifest is not a scaffold request. If its declared module
# closure is incomplete, init refuses by path and leaves the source unchanged.
set(BOOTSTRAP_ROOT "${UF_PROJECT_TEST_ROOT}/clean-bootstrap")
set(BOOTSTRAP_SOURCE "${BOOTSTRAP_ROOT}/source")
set(BOOTSTRAP_BUILD "${BOOTSTRAP_ROOT}/build")
file(MAKE_DIRECTORY "${BOOTSTRAP_SOURCE}")
file(COPY "${CUT_SOURCE}/" DESTINATION "${BOOTSTRAP_SOURCE}")
file(REMOVE_RECURSE "${BOOTSTRAP_SOURCE}/plugin")
file(READ "${BOOTSTRAP_SOURCE}/umbraflow-project.json" BOOTSTRAP_MANIFEST)
string(REPLACE
    "{\"name\": \"main\", \"path\": \"plugin/dream.luau\"}"
    "{\"name\": \"main\", \"path\": \"plugin/main.luau\"}, {\"name\": \"support\", \"path\": \"plugin/support.luau\"}"
    BOOTSTRAP_MANIFEST
    "${BOOTSTRAP_MANIFEST}"
)
file(WRITE "${BOOTSTRAP_SOURCE}/umbraflow-project.json" "${BOOTSTRAP_MANIFEST}")
file(GLOB_RECURSE BOOTSTRAP_BEFORE
    RELATIVE "${BOOTSTRAP_SOURCE}"
    "${BOOTSTRAP_SOURCE}/*"
)

run_project(BOOTSTRAP_INIT_RESULT BOOTSTRAP_INIT_DIAGNOSTIC init
    --source "${BOOTSTRAP_SOURCE}"
    --build "${BOOTSTRAP_BUILD}"
)
if(BOOTSTRAP_INIT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project init must refuse an incomplete authored module closure"
    )
endif()
require_contains("the incomplete authored closure refusal"
    "${BOOTSTRAP_INIT_DIAGNOSTIC}" "plugin/main.luau")
if(EXISTS "${BOOTSTRAP_SOURCE}/plugin/main.luau"
   OR EXISTS "${BOOTSTRAP_SOURCE}/plugin/support.luau")
    message(FATAL_ERROR "project init must not fill an authored module closure")
endif()
file(GLOB_RECURSE BOOTSTRAP_AFTER
    RELATIVE "${BOOTSTRAP_SOURCE}"
    "${BOOTSTRAP_SOURCE}/*"
)
if(NOT BOOTSTRAP_BEFORE STREQUAL BOOTSTRAP_AFTER)
    message(FATAL_ERROR "a refused authored init must not change the source tree")
endif()

# F6. An empty directory needs no copied manifest or Python scaffolder. init
# writes either authoring form, derives its inputs from that declaration, and
# the default work/build path is the path every later command consumes.
foreach(SCAFFOLD_FORM IN ITEMS generated hand-written)
    set(SCAFFOLD_SOURCE
        "${UF_PROJECT_TEST_ROOT}/scaffold-${SCAFFOLD_FORM}"
    )
    file(MAKE_DIRECTORY "${SCAFFOLD_SOURCE}")
    run_project(SCAFFOLD_INIT_RESULT SCAFFOLD_INIT_DIAGNOSTIC init
        --source "${SCAFFOLD_SOURCE}"
        --plugin "${SCAFFOLD_FORM}"
        --plugin-id chaos.project
    )
    if(NOT SCAFFOLD_INIT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "project init must scaffold ${SCAFFOLD_FORM}; "
            "diagnostic=[${SCAFFOLD_INIT_DIAGNOSTIC}]"
        )
    endif()

    foreach(SCAFFOLD_ACTION IN ITEMS build check)
        run_project(SCAFFOLD_RESULT SCAFFOLD_DIAGNOSTIC
            ${SCAFFOLD_ACTION} --source "${SCAFFOLD_SOURCE}"
        )
        if(NOT SCAFFOLD_RESULT EQUAL 0)
            message(FATAL_ERROR
                "scaffolded ${SCAFFOLD_FORM} project must ${SCAFFOLD_ACTION}; "
                "diagnostic=[${SCAFFOLD_DIAGNOSTIC}]"
            )
        endif()
    endforeach()
    if(NOT EXISTS "${SCAFFOLD_SOURCE}/work/build/project-kit.build")
        message(FATAL_ERROR
            "scaffolded ${SCAFFOLD_FORM} project must use source/work/build"
        )
    endif()

    file(GLOB_RECURSE SCAFFOLD_BEFORE
        RELATIVE "${SCAFFOLD_SOURCE}"
        "${SCAFFOLD_SOURCE}/*"
    )
    run_project(SCAFFOLD_REPEAT_RESULT SCAFFOLD_REPEAT_DIAGNOSTIC init
        --source "${SCAFFOLD_SOURCE}"
        --plugin "${SCAFFOLD_FORM}"
        --plugin-id chaos.project
    )
    if(SCAFFOLD_REPEAT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "project init must refuse to overwrite a scaffold; "
            "diagnostic=[${SCAFFOLD_REPEAT_DIAGNOSTIC}]"
        )
    endif()
    require_contains("the scaffold overwrite refusal"
        "${SCAFFOLD_REPEAT_DIAGNOSTIC}" "already exists")
    file(GLOB_RECURSE SCAFFOLD_AFTER
        RELATIVE "${SCAFFOLD_SOURCE}"
        "${SCAFFOLD_SOURCE}/*"
    )
    if(NOT SCAFFOLD_BEFORE STREQUAL SCAFFOLD_AFTER)
        message(FATAL_ERROR "a refused scaffold must not change the source tree")
    endif()
endforeach()
