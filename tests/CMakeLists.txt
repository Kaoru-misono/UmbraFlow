function(cpp_add_test)
    cmake_parse_arguments(ARG "" "NAME" "SOURCES;LIBS" ${ARGN})

    add_executable(${ARG_NAME}
        test-main.cpp
        ${ARG_SOURCES}
        ${CPP_UTF8_RC}
    )
    cpp_apply_safety_profile(${ARG_NAME})
    cpp_apply_utf8_manifest(${ARG_NAME})
    target_compile_definitions(${ARG_NAME} PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
    target_include_directories(${ARG_NAME} SYSTEM PRIVATE external/doctest)
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_LIBS})
    target_compile_features(${ARG_NAME} PRIVATE cxx_std_23)

    add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME})
    set_tests_properties(${ARG_NAME} PROPERTIES
        TIMEOUT 60
        LABELS "CI"
    )
endfunction()

cpp_add_test(
    NAME test-core
    SOURCES
        core/test-capabilities.cpp
        core/test-checked-access.cpp
        core/test-checked-arithmetic.cpp
        core/test-error.cpp
        core/test-monotonic-time.cpp
        core/test-strong-types.cpp
        core/test-utf8.cpp
    LIBS
        ${PROJECT_NAME}_core
)

cpp_add_test(
    NAME test-domain
    SOURCES
        domain/test-detection.cpp
        domain/test-error.cpp
        domain/test-frame.cpp
        domain/test-ids.cpp
        domain/test-space.cpp
        domain/test-time.cpp
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
    cli/test-check.cpp
    cli/test-drive-protocol.cpp
    cli/test-explore-protocol.cpp
    cli/test-project-skeleton.cpp
)
# selectCandidate operates on the controller's TargetCandidate and is compiled
# into cli_support only on Windows, so its test is registered only there.
if(WIN32 AND TARGET ${PROJECT_NAME}_controller)
    list(APPEND CLI_TEST_SOURCES cli/test-candidate-selection.cpp)
endif()
# The image module is private to cli_support, but the falsification-matrix cases
# have to write the screens and template marks a synthetic project is measured
# from, so the test links the PNG encoder itself.
cpp_add_test(
    NAME test-cli
    SOURCES
        ${CLI_TEST_SOURCES}
    LIBS
        ${PROJECT_NAME}_cli_support
        ${PROJECT_NAME}_image
)
# cli_support links engine publicly, which links ocr publicly.
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
            task/test-task.cpp
            task/test-adversarial-surface.cpp
            task/test-task-binding.cpp
            task/test-task-host.cpp
            task/test-determinism-harness.cpp
            task/test-exploration.cpp
            task/test-annotation-routines.cpp
            task/test-colour-key-crop.cpp
            task/test-framework-bundle.cpp
            task/test-framework-context.cpp
            task/test-framework-surface.cpp
            task/test-operator-front-end.cpp
            task/test-script-owned-primitives.cpp
            task/test-script-owned-model.cpp
            task/test-semantic-events.cpp
            task/test-veto-blocking.cpp
            task/test-task-loader.cpp
            task/test-script-validator.cpp
            task/test-deterministic.cpp
            task/test-time-primitives.cpp
        LIBS
            ${PROJECT_NAME}_task
            ${PROJECT_NAME}_image
    )
    # task links engine publicly, which links ocr publicly.
    if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
        cpp_stage_runtime_libraries(test-task ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
    endif()
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
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/assets/real-regression)
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
        UF_REAL_REGRESSION_ROOT="${CMAKE_SOURCE_DIR}/tests/assets/real-regression"
        UF_OCR_MODEL_ROOT="${CMAKE_SOURCE_DIR}/modules/ocr/external/models"
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

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME check-modules
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_modules.py
    )
    add_test(
        NAME check-safety
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_safety.py
    )
    add_test(
        NAME test-check-safety
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test-check-safety.py
    )
    set_tests_properties(check-modules check-safety test-check-safety PROPERTIES
        TIMEOUT 30
        LABELS "CI"
    )
else()
    message(WARNING "[Safety] Python was not found; repository safety checks are not registered with CTest.")
endif()
