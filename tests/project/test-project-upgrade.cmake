cmake_minimum_required(VERSION 3.30)

# `project upgrade`, driven through the shipped binary against a release served
# off the local filesystem. There is no published release to fetch and there
# does not need to be: umbraflow-kit.json accepts a file:// host, so the whole
# transport, digest and swap path is exercised without a network.
#
# The `project` artifact of each staged release is THIS BINARY, copied. That is
# not a shortcut -- an upgrade ends by running the binary it installed, because
# the schema a declaration is judged against is compiled into an executable, and
# a fixture whose `project` artifact were text could never reach that step.

if(NOT EXISTS "${UF_PROJECT_EXECUTABLE}")
    message(FATAL_ERROR
        "project CLI executable does not exist: ${UF_PROJECT_EXECUTABLE}"
    )
endif()

if(WIN32)
    set(HOST_PLATFORM windows)
    set(EXECUTABLE_SUFFIX .exe)
elseif(APPLE)
    set(HOST_PLATFORM macos)
    set(EXECUTABLE_SUFFIX "")
elseif(UNIX)
    set(HOST_PLATFORM linux)
    set(EXECUTABLE_SUFFIX "")
else()
    message(FATAL_ERROR "unsupported release test platform")
endif()

cmake_host_system_information(RESULT HOST_PROCESSOR QUERY OS_PLATFORM)
string(TOLOWER "${HOST_PROCESSOR}" HOST_PROCESSOR)
if(HOST_PROCESSOR MATCHES "^(amd64|x86_64|x64)$")
    set(HOST_ARCH x64)
elseif(HOST_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(HOST_ARCH arm64)
else()
    message(FATAL_ERROR
        "unsupported release test architecture: ${HOST_PROCESSOR}"
    )
endif()

set(MANIFEST_NAME umbraflow-release-v1.json)
set(SERVER_ROOT "${UF_PROJECT_TEST_ROOT}/server")
set(SOURCE_DIRECTORY "${UF_PROJECT_TEST_ROOT}/source with spaces")
set(BUNDLE_ROOT "${SOURCE_DIRECTORY}/umbraflow-bin")
set(LATEST_DIRECTORY "${SERVER_ROOT}/releases/latest/download")

file(REMOVE_RECURSE "${UF_PROJECT_TEST_ROOT}")
file(MAKE_DIRECTORY "${SOURCE_DIRECTORY}" "${LATEST_DIRECTORY}")

# Stages one immutable release under releases/download/<name>, and mirrors its
# manifest under releases/latest/download when asked. Exact member order and
# compact spelling are the JCS bytes the consumer pins.
function(stage_release RELEASE_NAME PUBLISH_AS_LATEST)
    set(RELEASE_DIRECTORY "${SERVER_ROOT}/releases/download/${RELEASE_NAME}")
    file(MAKE_DIRECTORY "${RELEASE_DIRECTORY}")

    set(ARTIFACT_ROWS "")
    foreach(ARTIFACT_NAME IN ITEMS project umbra-flow umbra-flow-conformance)
        set(ASSET_NAME "${ARTIFACT_NAME}${EXECUTABLE_SUFFIX}")
        set(ASSET_PATH "${RELEASE_DIRECTORY}/${ASSET_NAME}")
        if(ARTIFACT_NAME STREQUAL "project")
            file(COPY_FILE "${UF_PROJECT_EXECUTABLE}" "${ASSET_PATH}")
        else()
            file(WRITE "${ASSET_PATH}"
                "fixture bytes for ${ARTIFACT_NAME} in ${RELEASE_NAME}\n"
            )
        endif()
        file(SHA256 "${ASSET_PATH}" ASSET_SHA256)
        if(NOT ARTIFACT_ROWS STREQUAL "")
            string(APPEND ARTIFACT_ROWS ",")
        endif()
        string(APPEND ARTIFACT_ROWS
            "{\"arch\":\"${HOST_ARCH}\",\"asset\":\"${ASSET_NAME}\","
            "\"name\":\"${ARTIFACT_NAME}\",\"path\":\"${ASSET_NAME}\","
            "\"platform\":\"${HOST_PLATFORM}\",\"sha256\":\"${ASSET_SHA256}\"}"
        )
    endforeach()

    string(CONCAT RELEASE_MANIFEST
        "{\"artifacts\":[${ARTIFACT_ROWS}],"
        "\"contract_versions\":[\"umbraflow-project/v3\"],"
        "\"release\":\"${RELEASE_NAME}\","
        "\"schema\":\"umbraflow-release/v1\"}"
    )
    file(WRITE "${RELEASE_DIRECTORY}/${MANIFEST_NAME}" "${RELEASE_MANIFEST}")
    if(PUBLISH_AS_LATEST)
        file(WRITE "${LATEST_DIRECTORY}/${MANIFEST_NAME}" "${RELEASE_MANIFEST}")
    endif()
endfunction()

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

stage_release(m0-local TRUE)
stage_release(m1-local FALSE)

file(TO_CMAKE_PATH "${SERVER_ROOT}" SERVER_URL_PATH)
if(WIN32)
    set(SERVER_URL "file:///${SERVER_URL_PATH}")
else()
    set(SERVER_URL "file://${SERVER_URL_PATH}")
endif()

# `release` is required and has no default: a project states whether it follows
# the newest release or pins one.
file(WRITE "${SOURCE_DIRECTORY}/umbraflow-kit.json"
    "{\"host\":\"${SERVER_URL}\",\"manifest\":\"${MANIFEST_NAME}\","
    "\"release\":\"latest\"}"
)

run_project(SCAFFOLD_RESULT SCAFFOLD_DIAGNOSTIC init
    --source "${SOURCE_DIRECTORY}"
    --plugin hand-written
    --plugin-id chaos.project
)
if(NOT SCAFFOLD_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project init must scaffold beside umbraflow-kit.json; "
        "diagnostic=[${SCAFFOLD_DIAGNOSTIC}]"
    )
endif()
if(EXISTS "${BUNDLE_ROOT}")
    message(FATAL_ERROR
        "project init must not install a release: acquiring binaries is "
        "upgrade's job and is destructive"
    )
endif()

# So that the check an upgrade runs afterwards has a complete tree to judge and
# its exit code says something about the release rather than about a project
# that was never built.
run_project(SETUP_BUILD_RESULT SETUP_BUILD_DIAGNOSTIC build
    --source "${SOURCE_DIRECTORY}"
)
if(NOT SETUP_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
        "the scaffolded project must build; diagnostic=[${SETUP_BUILD_DIAGNOSTIC}]"
    )
endif()

# ----------------------------------------------------------------------------
# A fresh install, following `latest`.
# ----------------------------------------------------------------------------
run_project(FRESH_RESULT FRESH_DIAGNOSTIC upgrade --source "${SOURCE_DIRECTORY}")
if(NOT FRESH_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project upgrade must install and then report; "
        "diagnostic=[${FRESH_DIAGNOSTIC}]"
    )
endif()
require_contains("the upgrade report" "${FRESH_DIAGNOSTIC}" "release=m0-local")
# The second half of the same command: the newly installed binary's own check,
# which is what tells the author what is left to do.
require_contains("the upgrade report" "${FRESH_DIAGNOSTIC}" "project check:")
if(NOT EXISTS "${BUNDLE_ROOT}/${MANIFEST_NAME}")
    message(FATAL_ERROR "project upgrade must pin the release manifest in the bundle")
endif()
foreach(ARTIFACT_NAME IN ITEMS project umbra-flow umbra-flow-conformance)
    if(NOT EXISTS "${BUNDLE_ROOT}/${ARTIFACT_NAME}${EXECUTABLE_SUFFIX}")
        message(FATAL_ERROR
            "project upgrade did not install ${ARTIFACT_NAME}${EXECUTABLE_SUFFIX}"
        )
    endif()
endforeach()
if(EXISTS "${SOURCE_DIRECTORY}/work/release-staging")
    message(FATAL_ERROR "a successful upgrade left its staging tree")
endif()

# ----------------------------------------------------------------------------
# An upgrade from one release to another, named on the command line.
#
# The swap is two renames and no delete, so the previous bundle is still there
# under its own release name until something later removes it.
# ----------------------------------------------------------------------------
run_project(PINNED_RESULT PINNED_DIAGNOSTIC upgrade
    --source "${SOURCE_DIRECTORY}"
    --release m1-local
)
if(NOT PINNED_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project upgrade --release must install the release it names; "
        "diagnostic=[${PINNED_DIAGNOSTIC}]"
    )
endif()
require_contains("the pinned upgrade report"
    "${PINNED_DIAGNOSTIC}" "release=m1-local")
file(READ "${BUNDLE_ROOT}/${MANIFEST_NAME}" INSTALLED_MANIFEST)
string(FIND "${INSTALLED_MANIFEST}" "\"release\":\"m1-local\"" INSTALLED_AT)
if(INSTALLED_AT EQUAL -1)
    message(FATAL_ERROR
        "the installed bundle must carry the manifest of the release it pinned"
    )
endif()

# The leftover is named for the release it holds, and every later `project`
# invocation removes it. It is not a rollback and nothing reads it.
run_project(SWEEP_RESULT SWEEP_DIAGNOSTIC check --source "${SOURCE_DIRECTORY}")
file(GLOB SUPERSEDED "${SOURCE_DIRECTORY}/umbraflow-bin.*")
if(NOT SUPERSEDED STREQUAL "")
    message(FATAL_ERROR
        "a later project command must remove the superseded bundle; "
        "left=[${SUPERSEDED}]"
    )
endif()

# ----------------------------------------------------------------------------
# A crash between the two renames leaves a state the next command can name and
# finish, rather than a project with no binaries and no explanation.
# ----------------------------------------------------------------------------
file(RENAME "${BUNDLE_ROOT}" "${SOURCE_DIRECTORY}/umbraflow-bin.m1-local")
run_project(RECOVERED_RESULT RECOVERED_DIAGNOSTIC check
    --source "${SOURCE_DIRECTORY}"
)
require_contains("the interrupted-upgrade report"
    "${RECOVERED_DIAGNOSTIC}" "interrupted between its two renames")
if(NOT EXISTS "${BUNDLE_ROOT}/${MANIFEST_NAME}")
    message(FATAL_ERROR
        "an interrupted upgrade must be finished by putting the bundle back"
    )
endif()

# Two set aside and none installed is a state nothing here can resolve, and it
# is refused by name rather than by picking one.
file(RENAME "${BUNDLE_ROOT}" "${SOURCE_DIRECTORY}/umbraflow-bin.m1-local")
file(MAKE_DIRECTORY "${SOURCE_DIRECTORY}/umbraflow-bin.m0-local")
run_project(AMBIGUOUS_RESULT AMBIGUOUS_DIAGNOSTIC check
    --source "${SOURCE_DIRECTORY}"
)
if(AMBIGUOUS_RESULT EQUAL 0)
    message(FATAL_ERROR
        "two superseded bundles and no installed one must be refused"
    )
endif()
require_contains("the ambiguous-bundle refusal"
    "${AMBIGUOUS_DIAGNOSTIC}" "umbraflow-bin.m0-local")
require_contains("the ambiguous-bundle refusal"
    "${AMBIGUOUS_DIAGNOSTIC}" "umbraflow-bin.m1-local")
file(REMOVE_RECURSE "${SOURCE_DIRECTORY}/umbraflow-bin.m0-local")
file(RENAME "${SOURCE_DIRECTORY}/umbraflow-bin.m1-local" "${BUNDLE_ROOT}")

# ----------------------------------------------------------------------------
# A host that answers a pin with another release's manifest. Without this the
# pin would silently stop pinning.
# ----------------------------------------------------------------------------
set(IMPOSTOR_DIRECTORY "${SERVER_ROOT}/releases/download/m2-local")
file(MAKE_DIRECTORY "${IMPOSTOR_DIRECTORY}")
file(COPY_FILE
    "${SERVER_ROOT}/releases/download/m0-local/${MANIFEST_NAME}"
    "${IMPOSTOR_DIRECTORY}/${MANIFEST_NAME}"
)
run_project(IMPOSTOR_RESULT IMPOSTOR_DIAGNOSTIC upgrade
    --source "${SOURCE_DIRECTORY}"
    --release m2-local
)
if(IMPOSTOR_RESULT EQUAL 0)
    message(FATAL_ERROR "a pin answered with another release must be refused")
endif()
require_contains("the mismatched-release refusal"
    "${IMPOSTOR_DIAGNOSTIC}" "release m2-local was requested")

# ----------------------------------------------------------------------------
# The byte ceiling is enforced by the transport, before an untrusted response
# can be fully materialized and parsed. A separate clean source, because a
# refused upgrade must leave the project it ran in untouched.
# ----------------------------------------------------------------------------
set(OVERSIZED_SOURCE "${UF_PROJECT_TEST_ROOT}/oversized source")
file(MAKE_DIRECTORY "${OVERSIZED_SOURCE}")
file(COPY_FILE
    "${SOURCE_DIRECTORY}/umbraflow-kit.json"
    "${OVERSIZED_SOURCE}/umbraflow-kit.json"
)
math(EXPR OVERSIZED_MANIFEST_SIZE "(1 << 20) + 1")
string(REPEAT x ${OVERSIZED_MANIFEST_SIZE} OVERSIZED_MANIFEST)
file(WRITE "${LATEST_DIRECTORY}/${MANIFEST_NAME}" "${OVERSIZED_MANIFEST}")
run_project(OVERSIZED_RESULT OVERSIZED_DIAGNOSTIC upgrade
    --source "${OVERSIZED_SOURCE}"
)
if(OVERSIZED_RESULT EQUAL 0)
    message(FATAL_ERROR "project upgrade must refuse an oversized release manifest")
endif()
require_contains("the manifest byte ceiling"
    "${OVERSIZED_DIAGNOSTIC}" "curl refused")
if(EXISTS "${OVERSIZED_SOURCE}/umbraflow-bin"
   OR EXISTS "${OVERSIZED_SOURCE}/work/release-staging")
    message(FATAL_ERROR "an oversized release manifest left installed bytes")
endif()

# ----------------------------------------------------------------------------
# A tampered artifact is refused by name and by hash. The digest gate is a
# content check rather than a size or existence check, so the replacement bytes
# are the same length as the ones they replace.
# ----------------------------------------------------------------------------
set(TAMPERED_ASSET
    "${SERVER_ROOT}/releases/download/m0-local/umbra-flow${EXECUTABLE_SUFFIX}"
)
file(READ "${TAMPERED_ASSET}" ORIGINAL_BYTES)
string(LENGTH "${ORIGINAL_BYTES}" ORIGINAL_SIZE)
string(REPEAT x ${ORIGINAL_SIZE} REPLACEMENT_BYTES)
file(WRITE "${TAMPERED_ASSET}" "${REPLACEMENT_BYTES}")
run_project(TAMPERED_RESULT TAMPERED_DIAGNOSTIC upgrade
    --source "${SOURCE_DIRECTORY}"
    --release m0-local
)
if(TAMPERED_RESULT EQUAL 0)
    message(FATAL_ERROR "project upgrade must refuse a tampered release artifact")
endif()
require_contains("the tampered artifact refusal"
    "${TAMPERED_DIAGNOSTIC}" "umbra-flow")
require_contains("the tampered artifact refusal"
    "${TAMPERED_DIAGNOSTIC}" "sha256")
if(NOT EXISTS "${BUNDLE_ROOT}/${MANIFEST_NAME}")
    message(FATAL_ERROR
        "a refused upgrade must leave the installed bundle in place"
    )
endif()

# ----------------------------------------------------------------------------
# A declaration the release does not match is INSTALLED anyway, and the work
# list follows.
#
# This is the whole reason upgrade does not gate on compatibility: the newly
# installed binary is the only thing that can tell an author whether an
# adaptation is right, so a refusal to install would leave them editing blind
# and re-downloading on every attempt. The exit code carries "there is work
# left" and the bundle is in place regardless.
# ----------------------------------------------------------------------------
file(READ "${SOURCE_DIRECTORY}/umbraflow-project.json" LIVE_DECLARATION)
string(REPLACE
    "\"tool_bindings\":[]"
    "\"plugin\":{},\"tool_bindings\":[]"
    BROKEN_DECLARATION
    "${LIVE_DECLARATION}"
)
if(BROKEN_DECLARATION STREQUAL LIVE_DECLARATION)
    message(FATAL_ERROR
        "the mismatch fixture must actually change the declaration"
    )
endif()
file(WRITE "${SOURCE_DIRECTORY}/umbraflow-project.json" "${BROKEN_DECLARATION}")

run_project(MISMATCH_RESULT MISMATCH_DIAGNOSTIC upgrade
    --source "${SOURCE_DIRECTORY}"
    --release m1-local
)
if(MISMATCH_RESULT EQUAL 0)
    message(FATAL_ERROR
        "an upgrade whose report finds work left must not report success"
    )
endif()
require_contains("the mismatched upgrade"
    "${MISMATCH_DIAGNOSTIC}" "release=m1-local")
require_contains("the adaptation report"
    "${MISMATCH_DIAGNOSTIC}" "does not declare")
file(READ "${BUNDLE_ROOT}/${MANIFEST_NAME}" MISMATCH_MANIFEST)
string(FIND "${MISMATCH_MANIFEST}" "\"release\":\"m1-local\"" MISMATCH_AT)
if(MISMATCH_AT EQUAL -1)
    message(FATAL_ERROR
        "a release must be installed even when the declaration does not match it"
    )
endif()
