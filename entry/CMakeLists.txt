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
    cli/drive.cpp
    cli/drive.hpp
    cli/drive-protocol.cpp
    cli/drive-protocol.hpp
    cli/explore.cpp
    cli/explore.hpp
    cli/explore-cursor.cpp
    cli/explore-cursor.hpp
    cli/explore-protocol.cpp
    cli/explore-protocol.hpp
    cli/file-frame-source.cpp
    cli/file-frame-source.hpp
    cli/json-text.cpp
    cli/json-text.hpp
    cli/run.cpp
    cli/run.hpp
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
            cli/drive-windows.cpp
            cli/explore-windows.cpp
            cli/run-windows.cpp
            cli/platform/controller-action-sink.cpp
            cli/platform/controller-action-sink.hpp
            cli/platform/ocr-engine-binding.cpp
            cli/platform/ocr-engine-binding.hpp
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
            cli/drive-unsupported.cpp
            cli/explore-unsupported.cpp
            cli/run-unsupported.cpp
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

    # What `cycle_read` stands on at runtime, staged into the runtime output
    # directory beside the binary. It moved here from the input agent when that
    # binary was retired: this is now the only executable that reaches the OCR
    # port, so it is the only one that has to carry the payload, and a build
    # tree keeps answering `--ocr-models <bin>/models` as a release does.
    #
    # The detection model is deliberately NOT staged. The adapter recognises an
    # already-located line and refuses TextLayout::Block, so shipping 9.8 MB of
    # weights nothing loads would only make the payload look complete.
    #
    # Named outputs rather than a bare custom target, so a 21 MB model is
    # compared once and then skipped instead of on every build.
    set(OCR_PAYLOAD_DIRECTORY
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/models/ppocr-v6-small-rec"
    )
    set(OCR_MODEL_ROOT
        "${CMAKE_SOURCE_DIR}/modules/ocr/external/models/ppocr-v6-small-rec"
    )

    set(OCR_MODEL_SOURCES "")
    set(OCR_MODEL_STAGED "")
    foreach(PAYLOAD_FILE IN ITEMS inference.onnx inference.yml)
        list(APPEND OCR_MODEL_SOURCES "${OCR_MODEL_ROOT}/${PAYLOAD_FILE}")
        list(APPEND OCR_MODEL_STAGED "${OCR_PAYLOAD_DIRECTORY}/${PAYLOAD_FILE}")
    endforeach()

    add_custom_command(
        OUTPUT ${OCR_MODEL_STAGED}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OCR_PAYLOAD_DIRECTORY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OCR_MODEL_SOURCES}
            "${OCR_PAYLOAD_DIRECTORY}"
        DEPENDS ${OCR_MODEL_SOURCES}
        COMMENT "[Payload] umbra-flow: staging the ocr recognition model"
        VERBATIM
    )
    add_custom_target(${PROJECT_NAME}_ocr_payload DEPENDS ${OCR_MODEL_STAGED})
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_ocr_payload)
endif()
