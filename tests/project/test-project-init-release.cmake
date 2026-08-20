cmake_minimum_required(VERSION 3.30)

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

set(RELEASE_NAME m0-local)
set(MANIFEST_NAME umbraflow-release-v1.json)
set(SERVER_ROOT "${UF_PROJECT_TEST_ROOT}/server")
set(SOURCE_DIRECTORY "${UF_PROJECT_TEST_ROOT}/source with spaces")
set(ASSET_SOURCE "${UF_PROJECT_TEST_ROOT}/assets")
set(LATEST_DIRECTORY "${SERVER_ROOT}/releases/latest/download")
set(RELEASE_DIRECTORY
    "${SERVER_ROOT}/releases/download/${RELEASE_NAME}"
)
file(REMOVE_RECURSE "${UF_PROJECT_TEST_ROOT}")
file(MAKE_DIRECTORY
    "${SOURCE_DIRECTORY}"
    "${ASSET_SOURCE}"
    "${LATEST_DIRECTORY}"
    "${RELEASE_DIRECTORY}"
)

set(ARTIFACT_ROWS "")
foreach(ARTIFACT_NAME IN ITEMS project umbra-flow umbra-flow-conformance)
    set(ASSET_NAME "${ARTIFACT_NAME}${EXECUTABLE_SUFFIX}")
    set(ASSET_PATH "${ASSET_SOURCE}/${ASSET_NAME}")
    file(WRITE "${ASSET_PATH}" "fixture bytes for ${ARTIFACT_NAME}\n")
    file(SHA256 "${ASSET_PATH}" ASSET_SHA256)
    file(COPY_FILE
        "${ASSET_PATH}"
        "${RELEASE_DIRECTORY}/${ASSET_NAME}"
    )
    if(NOT ARTIFACT_ROWS STREQUAL "")
        string(APPEND ARTIFACT_ROWS ",")
    endif()
    string(APPEND ARTIFACT_ROWS
        "{\"arch\":\"${HOST_ARCH}\",\"asset\":\"${ASSET_NAME}\","
        "\"name\":\"${ARTIFACT_NAME}\",\"path\":\"${ASSET_NAME}\","
        "\"platform\":\"${HOST_PLATFORM}\",\"sha256\":\"${ASSET_SHA256}\"}"
    )
endforeach()

# Exact member order and compact spelling are the JCS bytes the consumer pins.
string(CONCAT RELEASE_MANIFEST
    "{\"artifacts\":[${ARTIFACT_ROWS}],"
    "\"contract_versions\":[\"umbraflow-project/v2\"],"
    "\"release\":\"${RELEASE_NAME}\","
    "\"schema\":\"umbraflow-release/v1\"}"
)
file(WRITE
    "${LATEST_DIRECTORY}/${MANIFEST_NAME}"
    "${RELEASE_MANIFEST}"
)

file(TO_CMAKE_PATH "${SERVER_ROOT}" SERVER_URL_PATH)
if(WIN32)
    set(SERVER_URL "file:///${SERVER_URL_PATH}")
else()
    set(SERVER_URL "file://${SERVER_URL_PATH}")
endif()
file(WRITE "${SOURCE_DIRECTORY}/umbraflow-kit.json"
    "{\"host\":\"${SERVER_URL}\",\"manifest\":\"${MANIFEST_NAME}\"}"
)

execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" init
        --source "${SOURCE_DIRECTORY}"
        --plugin hand-written
        --plugin-id chaos.project
    RESULT_VARIABLE INIT_RESULT
    OUTPUT_VARIABLE INIT_OUTPUT
    ERROR_VARIABLE INIT_ERROR
)
if(NOT INIT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project init must acquire and scaffold from the local release; "
        "exit=${INIT_RESULT}; stdout=[${INIT_OUTPUT}]; stderr=[${INIT_ERROR}]"
    )
endif()

set(BUNDLE_ROOT "${SOURCE_DIRECTORY}/umbraflow-bin")
if(NOT EXISTS "${BUNDLE_ROOT}/${MANIFEST_NAME}")
    message(FATAL_ERROR "project init must pin the release manifest in the bundle")
endif()
foreach(ARTIFACT_NAME IN ITEMS project umbra-flow umbra-flow-conformance)
    if(NOT EXISTS "${BUNDLE_ROOT}/${ARTIFACT_NAME}${EXECUTABLE_SUFFIX}")
        message(FATAL_ERROR
            "project init did not install ${ARTIFACT_NAME}${EXECUTABLE_SUFFIX}"
        )
    endif()
endforeach()
if(EXISTS "${SOURCE_DIRECTORY}/work/project-init-release")
    message(FATAL_ERROR "successful release installation left its staging tree")
endif()

# A separate clean source proves the manifest ceiling is enforced by curl,
# before an untrusted response can be fully materialized and parsed.
set(OVERSIZED_SOURCE "${UF_PROJECT_TEST_ROOT}/oversized source")
file(MAKE_DIRECTORY "${OVERSIZED_SOURCE}")
file(COPY_FILE
    "${SOURCE_DIRECTORY}/umbraflow-kit.json"
    "${OVERSIZED_SOURCE}/umbraflow-kit.json"
)
math(EXPR OVERSIZED_MANIFEST_SIZE "(1 << 20) + 1")
string(REPEAT x ${OVERSIZED_MANIFEST_SIZE} OVERSIZED_MANIFEST)
file(WRITE
    "${LATEST_DIRECTORY}/${MANIFEST_NAME}"
    "${OVERSIZED_MANIFEST}"
)
execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" init --source "${OVERSIZED_SOURCE}"
    RESULT_VARIABLE OVERSIZED_RESULT
    OUTPUT_VARIABLE OVERSIZED_OUTPUT
    ERROR_VARIABLE OVERSIZED_ERROR
)
set(OVERSIZED_DIAGNOSTIC "${OVERSIZED_OUTPUT}${OVERSIZED_ERROR}")
if(OVERSIZED_RESULT EQUAL 0)
    message(FATAL_ERROR "project init must refuse an oversized release manifest")
endif()
string(FIND "${OVERSIZED_DIAGNOSTIC}" "curl refused" OVERSIZED_CURL_REFUSAL)
if(OVERSIZED_CURL_REFUSAL EQUAL -1)
    message(FATAL_ERROR
        "the manifest byte ceiling must stop the transport; "
        "diagnostic=[${OVERSIZED_DIAGNOSTIC}]"
    )
endif()
if(EXISTS "${OVERSIZED_SOURCE}/umbraflow-bin"
   OR EXISTS "${OVERSIZED_SOURCE}/work/project-init-release")
    message(FATAL_ERROR "an oversized release manifest left installed bytes")
endif()

# With the server gone, a second init can only pass by verifying the immutable
# local manifest and artifacts instead of consulting a mutable latest URL.
file(REMOVE_RECURSE "${SERVER_ROOT}")
execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" init --source "${SOURCE_DIRECTORY}"
    RESULT_VARIABLE CACHED_RESULT
    OUTPUT_VARIABLE CACHED_OUTPUT
    ERROR_VARIABLE CACHED_ERROR
)
if(NOT CACHED_RESULT EQUAL 0)
    message(FATAL_ERROR
        "project init must use its verified cached bundle; "
        "stdout=[${CACHED_OUTPUT}]; stderr=[${CACHED_ERROR}]"
    )
endif()

# Same-size corruption proves the cache gate is a content-hash check, not a
# size or existence check. The server remains absent, so no repair is possible.
set(CORRUPTED_ARTIFACT
    "${BUNDLE_ROOT}/umbra-flow${EXECUTABLE_SUFFIX}"
)
file(READ "${CORRUPTED_ARTIFACT}" CORRUPTED_BYTES)
string(LENGTH "${CORRUPTED_BYTES}" CORRUPTED_SIZE)
string(REPEAT x ${CORRUPTED_SIZE} REPLACEMENT_BYTES)
file(WRITE "${CORRUPTED_ARTIFACT}" "${REPLACEMENT_BYTES}")
execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" init --source "${SOURCE_DIRECTORY}"
    RESULT_VARIABLE TAMPERED_RESULT
    OUTPUT_VARIABLE TAMPERED_OUTPUT
    ERROR_VARIABLE TAMPERED_ERROR
)
set(TAMPERED_DIAGNOSTIC "${TAMPERED_OUTPUT}${TAMPERED_ERROR}")
if(TAMPERED_RESULT EQUAL 0)
    message(FATAL_ERROR "project init must refuse a tampered cached artifact")
endif()
string(FIND "${TAMPERED_DIAGNOSTIC}" "umbra-flow" TAMPERED_NAME)
string(FIND "${TAMPERED_DIAGNOSTIC}" "sha256" TAMPERED_HASH)
if(TAMPERED_NAME EQUAL -1 OR TAMPERED_HASH EQUAL -1)
    message(FATAL_ERROR
        "the tampered cache refusal must name artifact and hash; "
        "diagnostic=[${TAMPERED_DIAGNOSTIC}]"
    )
endif()
