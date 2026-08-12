cmake_minimum_required(VERSION 3.30)

if(NOT EXISTS "${UF_PROJECT_EXECUTABLE}")
    message(FATAL_ERROR
        "project CLI executable does not exist: ${UF_PROJECT_EXECUTABLE}"
    )
endif()

set(SOURCE_DIRECTORY "${UF_PROJECT_TEST_ROOT}/source")
set(BUILD_DIRECTORY "${UF_PROJECT_TEST_ROOT}/build")
set(INPUT_PATH "${SOURCE_DIRECTORY}/declared.txt")

file(REMOVE_RECURSE "${UF_PROJECT_TEST_ROOT}")
file(MAKE_DIRECTORY "${SOURCE_DIRECTORY}")
file(WRITE "${INPUT_PATH}" "declared input\n")
file(SHA256 "${INPUT_PATH}" SOURCE_HASH_BEFORE)
file(GLOB_RECURSE SOURCE_FILES_BEFORE
    RELATIVE "${SOURCE_DIRECTORY}"
    "${SOURCE_DIRECTORY}/*"
)

execute_process(
    COMMAND "${UF_PROJECT_EXECUTABLE}" init
        --source "${SOURCE_DIRECTORY}"
        --build "${BUILD_DIRECTORY}"
        --input declared.txt
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
file(GLOB_RECURSE SOURCE_FILES_AFTER
    RELATIVE "${SOURCE_DIRECTORY}"
    "${SOURCE_DIRECTORY}/*"
)
if(
    NOT SOURCE_HASH_BEFORE STREQUAL SOURCE_HASH_AFTER
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
