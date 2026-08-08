set(APPLICATION_INFO_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(APPLICATION_INFO_HEADER "${APPLICATION_INFO_DIRECTORY}/application-info.hpp")
file(MAKE_DIRECTORY "${APPLICATION_INFO_DIRECTORY}")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/application-info.hpp.in"
    "${APPLICATION_INFO_HEADER}"
    @ONLY
)

set(CLI_LIBRARY ${PROJECT_NAME}_cli_support)

# Platform-free product logic: argument parsing and the error/exit-code
# boundary. Linked into the executable and tests. The run lifecycle itself lives
# in modules/task's TaskHost, and the trace file sink in modules/trace, which
# owns the single evidence stream.
add_library(${CLI_LIBRARY} STATIC
    cli/args.cpp
    cli/args.hpp
    cli/check.cpp
    cli/check.hpp
    cli/explore.cpp
    cli/explore.hpp
    cli/explore-protocol.cpp
    cli/explore-protocol.hpp
    cli/file-frame-source.cpp
    cli/file-frame-source.hpp
    # The OCR binding's DECLARATION is platform-free: `check` composes on every
    # host and needs an engine whenever a project claims what a region reads, so
    # it includes this header everywhere. Only the definition is gated below.
    cli/platform/ocr-engine-binding.hpp
    cli/project-skeleton.cpp
    cli/project-skeleton.hpp
    cli/replay.cpp
    cli/replay.hpp
    # The queue/results wire `explore` runs on: one framed reader, one results
    # writer, and the durable cursor that keeps a restart from re-delivering a
    # queue against a live target. It stays a pair of files of its own because
    # the shape outlived the second front-end it was factored out for.
    cli/queue-cursor.cpp
    cli/queue-cursor.hpp
    cli/queue-ipc.cpp
    cli/queue-ipc.hpp
    cli/run.cpp
    cli/run.hpp
    # The listing that ANSWERS --hwnd. Its rendering is here and its enumeration
    # is gated below, so the line a script parses is testable on a host with no
    # desktop.
    cli/targets.cpp
    cli/targets.hpp
)
cpp_apply_safety_profile(${CLI_LIBRARY})
target_include_directories(${CLI_LIBRARY}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/cli
)
# The exit-code boundary maps a task::TaskRunReport, so the task module is part
# of this library's interface on every host, not only where the run composition
# builds. The image module is private and is here for one consumer: the
# file-backed frame source `check` replays a project's own screens through
# decodes each screen's PNG.
target_link_libraries(${CLI_LIBRARY}
    PUBLIC
        ${PROJECT_NAME}_engine
        ${PROJECT_NAME}_task
    PRIVATE
        ${PROJECT_NAME}_image
)
target_compile_features(${CLI_LIBRARY} PUBLIC cxx_std_23)

if(WIN32 AND TARGET ${PROJECT_NAME}_controller)
    target_sources(${CLI_LIBRARY}
        PRIVATE
            cli/candidate-selection.cpp
            cli/candidate-selection.hpp
            cli/explore-windows.cpp
            cli/run-windows.cpp
            cli/targets-windows.cpp
            cli/platform/controller-action-sink.cpp
            cli/platform/controller-action-sink.hpp
            cli/platform/ocr-engine-binding.cpp
            cli/platform/target-binding.cpp
            cli/platform/target-binding.hpp
            cli/platform/wgc-frame-source.hpp
            cli/platform/windows-console-cancellation.cpp
            cli/platform/windows-console-cancellation.hpp
            cli/platform/windows-target-geometry.cpp
            cli/platform/windows-target-geometry.hpp
    )
    target_link_libraries(${CLI_LIBRARY}
        PUBLIC
            ${PROJECT_NAME}_controller
        PRIVATE
            user32
    )
else()
    target_sources(${CLI_LIBRARY}
        PRIVATE
            cli/explore-unsupported.cpp
            cli/run-unsupported.cpp
            cli/targets-unsupported.cpp
            cli/platform/ocr-engine-binding-unsupported.cpp
    )
endif()

add_executable(${PROJECT_NAME}
    cli/main.cpp
    "${APPLICATION_INFO_HEADER}"
    ${CPP_UTF8_RC}
)
cpp_apply_safety_profile(${PROJECT_NAME})
cpp_apply_utf8_manifest(${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "umbra-flow")
target_include_directories(${PROJECT_NAME} PRIVATE "${APPLICATION_INFO_DIRECTORY}")
target_link_libraries(${PROJECT_NAME} PRIVATE ${CLI_LIBRARY} ${PROJECT_NAME}_core)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

# cli_support links engine publicly, and engine links ocr publicly, so this
# executable resolves onnxruntime.dll from its own directory at process start;
# see cmake/runtime-payload.cmake for why that copy cannot be left to some
# other target's build order.
if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)
    cpp_stage_runtime_libraries(${PROJECT_NAME} ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})

    # What the text reads stand on at runtime, staged into the runtime output
    # directory beside the binary. It moved here from the input agent when that
    # binary was retired: this is now the only executable that reaches the OCR
    # port, so it is the only one that has to carry the payload, and a build
    # tree keeps answering `--ocr-models <bin>/models` as a release does.
    #
    # BOTH models are staged. The detection half used to be left out on the
    # grounds that nothing loaded it; `cycle_read_lines` loads it, and
    # platform::bindOcrEngine names it unconditionally, so a build tree that
    # staged only half the payload would answer `--ocr-models <bin>/models` with
    # a startup failure instead of a working engine.
    #
    # Named outputs rather than a bare custom target, so a 21 MB model is
    # compared once and then skipped instead of on every build.
    set(OCR_MODEL_STAGED "")
    set(OCR_MODEL_COMMANDS "")
    set(OCR_MODEL_ALL_SOURCES "")
    foreach(OCR_MODEL_NAME IN ITEMS ppocr-v6-small-rec ppocr-v6-small-det)
        set(OCR_PAYLOAD_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/models/${OCR_MODEL_NAME}"
        )
        set(OCR_MODEL_ROOT
            "${CMAKE_SOURCE_DIR}/modules/ocr/external/models/${OCR_MODEL_NAME}"
        )

        set(OCR_MODEL_SOURCES "")
        foreach(PAYLOAD_FILE IN ITEMS inference.onnx inference.yml)
            list(APPEND OCR_MODEL_SOURCES "${OCR_MODEL_ROOT}/${PAYLOAD_FILE}")
            list(APPEND OCR_MODEL_STAGED "${OCR_PAYLOAD_DIRECTORY}/${PAYLOAD_FILE}")
        endforeach()

        list(APPEND OCR_MODEL_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OCR_PAYLOAD_DIRECTORY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OCR_MODEL_SOURCES}
                "${OCR_PAYLOAD_DIRECTORY}"
        )
        list(APPEND OCR_MODEL_ALL_SOURCES ${OCR_MODEL_SOURCES})
    endforeach()

    add_custom_command(
        OUTPUT ${OCR_MODEL_STAGED}
        ${OCR_MODEL_COMMANDS}
        DEPENDS ${OCR_MODEL_ALL_SOURCES}
        COMMENT "[Payload] umbra-flow: staging the ocr detection and recognition models"
        VERBATIM
    )
    add_custom_target(${PROJECT_NAME}_ocr_payload DEPENDS ${OCR_MODEL_STAGED})
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_ocr_payload)
endif()
