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
    NAME test-annotation
    SOURCES
        annotation/test-authoring-compiler.cpp
        annotation/test-authoring-document.cpp
        annotation/test-authorization.cpp
        annotation/test-catalog.cpp
        annotation/test-colour-key-join.cpp
        annotation/test-content-hash.cpp
        annotation/test-recognition.cpp
        annotation/test-recognition-runtime.cpp
        annotation/test-regression-runner.cpp
        annotation/test-runtime-manifest.cpp
        annotation/test-template-asset.cpp
    LIBS
        ${PROJECT_NAME}_annotation
        ${PROJECT_NAME}_image
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
        engine/test-runtime-loader.cpp
        engine/test-session.cpp
    LIBS
        ${PROJECT_NAME}_engine
        ${PROJECT_NAME}_image
)

set(CLI_TEST_SOURCES
    cli/test-args.cpp
    cli/test-drive-protocol.cpp
)
# selectCandidate operates on the controller's TargetCandidate and is compiled
# into cli_support only on Windows, so its test is registered only there.
if(WIN32 AND TARGET ${PROJECT_NAME}_controller)
    list(APPEND CLI_TEST_SOURCES cli/test-candidate-selection.cpp)
endif()
cpp_add_test(
    NAME test-cli
    SOURCES
        ${CLI_TEST_SOURCES}
    LIBS
        ${PROJECT_NAME}_cli_support
)

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
            task/test-framework-bundle.cpp
            task/test-framework-context.cpp
            task/test-framework-surface.cpp
            task/test-operator-front-end.cpp
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

if(TARGET ${PROJECT_NAME}_m0_demo_support)
    cpp_add_test(
        NAME test-m0-demo
        SOURCES
            m0-demo/test-args.cpp
            m0-demo/test-capture-mode.cpp
            m0-demo/test-guard.cpp
            m0-demo/test-input-agent.cpp
            m0-demo/test-log-jsonl.cpp
            m0-demo/test-pacing.cpp
            m0-demo/test-pipeline.cpp
            m0-demo/test-shutdown.cpp
        LIBS
            ${PROJECT_NAME}_m0_demo_support
    )
endif()

if(TARGET ${PROJECT_NAME}_workbench_support)
    cpp_add_test(
        NAME test-workbench
        SOURCES
            workbench/test-authoring-actions.cpp
            workbench/test-authoring-edit.cpp
            workbench/test-canvas-math.cpp
            workbench/test-edit-page.cpp
            workbench/test-model-check-job.cpp
            workbench/test-model-check-view.cpp
            workbench/test-panel-state.cpp
            workbench/test-preview.cpp
            workbench/test-project-persistence.cpp
            workbench/test-project-tree.cpp
            workbench/test-source-ingestion.cpp
            workbench/test-workbench-app.cpp
        LIBS
            ${PROJECT_NAME}_workbench_support
            ${PROJECT_NAME}_image
    )

    # The AddressSanitizer smoke test drove the archived umbra-workbench GUI over
    # a generated project to exercise the panel draw path. Both the GUI and the
    # fixture generator went with it; git history holds them, and
    # docs/pitfalls/workbench-authoring-ui.md keeps the prescription.

    # The authoring CLI drives the same persistence and edit layer, so its test
    # is registered beside the workbench's and reaches across to the colour-key
    # crops that live there.
    if(TARGET ${PROJECT_NAME}_authoring_support)
        # The image module is private to authoring_support, but the budget cases
        # have to write source frames at a size no committed fixture has, so the
        # test links the PNG writer itself.
        cpp_add_test(
            NAME test-authoring
            SOURCES
                authoring/test-authoring-cli.cpp
            LIBS
                ${PROJECT_NAME}_authoring_support
                ${PROJECT_NAME}_image
        )
    endif()

    # Local-only real-screenshot regression. The fixture directory holds real
    # game screenshots that are never committed, so the target is registered
    # only when a developer has created it. Rerun cmake configure after adding
    # tests/assets/real-regression for this target to appear.
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/assets/real-regression)
        cpp_add_test(
            NAME test-real-regression
            SOURCES
                workbench/test-real-regression.cpp
            LIBS
                ${PROJECT_NAME}_workbench_support
                ${PROJECT_NAME}_image
        )
        target_compile_definitions(test-real-regression PRIVATE
            UF_REAL_REGRESSION_ROOT="${CMAKE_SOURCE_DIR}/tests/assets/real-regression"
        )
        # A distinct label keeps this out of the CI set; it must never run on CI
        # machines, which have only synthetic fixtures.
        set_tests_properties(test-real-regression PROPERTIES
            LABELS "REAL"
            TIMEOUT 600
        )

        # What the OCR adapter reads off a real frame. Local-only for the same
        # reason as the target above, and separate from it because it is the ocr
        # module under test rather than the workbench's regression runner.
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
        # onnxruntime resolves beside the executable, so the runtime DLLs are
        # copied rather than assumed to be on PATH.
        add_custom_command(TARGET test-ocr-real POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES}
                $<TARGET_FILE_DIR:test-ocr-real>
            VERBATIM
        )
        set_tests_properties(test-ocr-real PROPERTIES
            LABELS "REAL"
            TIMEOUT 300
        )
    endif()
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
