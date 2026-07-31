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
    cli/drive.cpp
    cli/drive.hpp
    cli/drive-protocol.cpp
    cli/drive-protocol.hpp
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
# builds.
target_link_libraries(${CLI_LIBRARY}
    PUBLIC
        ${PROJECT_NAME}_engine
        ${PROJECT_NAME}_task
)
target_compile_features(${CLI_LIBRARY} PUBLIC cxx_std_23)

if(WIN32 AND TARGET ${PROJECT_NAME}_controller)
    target_sources(${CLI_LIBRARY}
        PRIVATE
            cli/candidate-selection.cpp
            cli/candidate-selection.hpp
            cli/drive-windows.cpp
            cli/run-windows.cpp
            cli/platform/controller-action-sink.cpp
            cli/platform/controller-action-sink.hpp
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

if(TARGET ${PROJECT_NAME}_controller)
    set(INPUT_AGENT_LIBRARY ${PROJECT_NAME}_input_agent_support)
    set(INPUT_AGENT_EXECUTABLE ${PROJECT_NAME}_input_agent)

    # The annotation front-end. It serves one authoring session's command queue
    # against one raw window: read a queue line, deliver the input, capture the
    # frames it was decided from, and answer on the results file. It grew out of
    # the M0 demo below and moved out of it on 2026-07-31, because it is product
    # and the demo is not.
    #
    # It also owns what the two still share -- the frame PNG writer, the path
    # confinement rules, the target selection and session setup, the JSON string
    # escape, the error text. The direction is deliberate: the frozen demo links
    # the product, never the reverse, so retiring the demo is a delete and not
    # another extraction.
    add_library(${INPUT_AGENT_LIBRARY} STATIC
        input-agent/agent.cpp
        input-agent/agent.hpp
        input-agent/annotation.cpp
        input-agent/annotation.hpp
        input-agent/arg-parsing.hpp
        input-agent/args.cpp
        input-agent/args.hpp
        input-agent/capture-output.cpp
        input-agent/capture-output.hpp
        input-agent/cursor.cpp
        input-agent/cursor.hpp
        input-agent/drive.cpp
        input-agent/drive.hpp
        input-agent/error-text.cpp
        input-agent/error-text.hpp
        input-agent/json-string.cpp
        input-agent/json-string.hpp
        input-agent/loop.cpp
        input-agent/loop.hpp
        input-agent/path-validation.cpp
        input-agent/path-validation.hpp
        input-agent/platform/windows-client-origin.cpp
        input-agent/platform/windows-client-origin.hpp
        input-agent/platform/windows-file-writer.cpp
        input-agent/platform/windows-file-writer.hpp
        input-agent/platform/windows-path.cpp
        input-agent/platform/windows-path.hpp
        input-agent/protocol.cpp
        input-agent/protocol.hpp
        input-agent/target-setup.cpp
        input-agent/target-setup.hpp
    )
    cpp_apply_safety_profile(${INPUT_AGENT_LIBRARY})
    target_include_directories(${INPUT_AGENT_LIBRARY}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/input-agent
    )
    # The trace module is here for one type: trace::FrontEnd. The input agent is
    # the annotation front-end, and which front-end drove a target is a fact the
    # whole system already has one definition of. Spelling it a second time here
    # is how the two would come to disagree, so the agent stamps its results
    # lines with trace's own value and trace's own wire name.
    target_link_libraries(${INPUT_AGENT_LIBRARY}
        PUBLIC
            ${PROJECT_NAME}_controller
            ${PROJECT_NAME}_trace
        PRIVATE
            ${PROJECT_NAME}_image
            ntdll
            user32
    )
    target_compile_features(${INPUT_AGENT_LIBRARY} PUBLIC cxx_std_23)

    add_executable(${INPUT_AGENT_EXECUTABLE}
        input-agent/main.cpp
        ${CPP_UTF8_RC}
    )
    cpp_apply_safety_profile(${INPUT_AGENT_EXECUTABLE})
    cpp_apply_utf8_manifest(${INPUT_AGENT_EXECUTABLE})
    set_target_properties(${INPUT_AGENT_EXECUTABLE} PROPERTIES
        OUTPUT_NAME "umbra-input-agent"
    )
    target_link_libraries(${INPUT_AGENT_EXECUTABLE} PRIVATE ${INPUT_AGENT_LIBRARY})
    target_compile_features(${INPUT_AGENT_EXECUTABLE} PRIVATE cxx_std_23)

    set(M0_DEMO_LIBRARY ${PROJECT_NAME}_m0_demo_support)
    set(M0_DEMO_EXECUTABLE ${PROJECT_NAME}_m0_demo)

    # The frozen M0 substrate demo: a fixed home -> result -> reset loop over
    # Windows Graphics Capture and strict-background input, plus the `capture`
    # subcommand the pitfalls notes still reach for. Nothing here is a product
    # path; `umbra-flow run` replaced it.
    add_library(${M0_DEMO_LIBRARY} STATIC
        m0-demo/args.cpp
        m0-demo/args.hpp
        m0-demo/capture-mode.cpp
        m0-demo/capture-mode.hpp
        m0-demo/guard.cpp
        m0-demo/guard.hpp
        m0-demo/log-jsonl.cpp
        m0-demo/log-jsonl.hpp
        m0-demo/pacing.cpp
        m0-demo/pacing.hpp
        m0-demo/pipeline.cpp
        m0-demo/pipeline.hpp
        m0-demo/platform/windows-background-messages.cpp
        m0-demo/platform/windows-background-messages.hpp
        m0-demo/platform/windows-console-control.cpp
        m0-demo/platform/windows-console-control.hpp
        m0-demo/platform/windows-guard.cpp
        m0-demo/platform/windows-guard.hpp
        m0-demo/shutdown.cpp
        m0-demo/shutdown.hpp
    )
    cpp_apply_safety_profile(${M0_DEMO_LIBRARY})
    target_include_directories(${M0_DEMO_LIBRARY}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/m0-demo
    )
    target_link_libraries(${M0_DEMO_LIBRARY}
        PUBLIC
            ${INPUT_AGENT_LIBRARY}
            ${PROJECT_NAME}_vision
        PRIVATE
            ${PROJECT_NAME}_image
            advapi32
            user32
    )
    target_compile_features(${M0_DEMO_LIBRARY} PUBLIC cxx_std_23)

    add_executable(${M0_DEMO_EXECUTABLE}
        m0-demo/main.cpp
        ${CPP_UTF8_RC}
    )
    cpp_apply_safety_profile(${M0_DEMO_EXECUTABLE})
    cpp_apply_utf8_manifest(${M0_DEMO_EXECUTABLE})
    set_target_properties(${M0_DEMO_EXECUTABLE} PROPERTIES OUTPUT_NAME "m0-demo")
    target_link_libraries(${M0_DEMO_EXECUTABLE} PRIVATE ${M0_DEMO_LIBRARY})
    target_compile_features(${M0_DEMO_EXECUTABLE} PRIVATE cxx_std_23)
endif()

if(WIN32 AND TARGET ${PROJECT_NAME}_annotation)
    set(WORKBENCH_LIBRARY ${PROJECT_NAME}_workbench_support)

    add_library(${WORKBENCH_LIBRARY} STATIC
        workbench/authoring-edit.cpp
        workbench/authoring-edit.hpp
        workbench/edit-page.cpp
        workbench/edit-page.hpp
        workbench/page-view.cpp
        workbench/page-view.hpp
        workbench/platform/windows-file-publication.cpp
        workbench/platform/windows-file-publication.hpp
        workbench/preview.cpp
        workbench/preview.hpp
        workbench/project-persistence.cpp
        workbench/project-persistence.hpp
        workbench/source-ingestion.cpp
        workbench/source-ingestion.hpp
    )
    cpp_apply_safety_profile(${WORKBENCH_LIBRARY})
    target_include_directories(${WORKBENCH_LIBRARY}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/workbench
    )
    target_link_libraries(${WORKBENCH_LIBRARY}
        PUBLIC
            ${PROJECT_NAME}_annotation
        PRIVATE
            ${PROJECT_NAME}_image
    )
    target_compile_features(${WORKBENCH_LIBRARY} PUBLIC cxx_std_23)

    # The Dear ImGui workbench GUI that once sat on top of this library was
    # archived; git history holds it. What remains here is the platform-free
    # authoring backend the CLI below drives, so the library carries no ImGui,
    # no Direct3D 11 shell, and no Windows Graphics Capture ingestion.

    # A development tool, and since the GUI was archived the only way to author a
    # project: it works from measured numbers rather than dragged rectangles. It
    # shares the persistence and edit layer rather than reimplementing either, so
    # every write it makes goes through AuthoringDocument's validation. It is a
    # separate binary precisely so nothing in the product can come to depend on
    # it.
    set(AUTHORING_LIBRARY ${PROJECT_NAME}_authoring_support)
    set(AUTHORING_EXECUTABLE ${PROJECT_NAME}_authoring)

    add_library(${AUTHORING_LIBRARY} STATIC
        authoring/command.cpp
        authoring/command.hpp
        authoring/command-runner.cpp
        authoring/command-runner.hpp
        authoring/json-writer.cpp
        authoring/json-writer.hpp
    )
    cpp_apply_safety_profile(${AUTHORING_LIBRARY})
    target_include_directories(${AUTHORING_LIBRARY}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/authoring
    )
    # workbench_support carries the authoring document, its edit layer, and the
    # atomic publication of a project. cli_support carries the exit-code
    # boundary this tool reuses, and the engine's runtime loader behind it is
    # what `match` runs a recognizer with. The image module decodes the PNG a
    # match is measured against.
    target_link_libraries(${AUTHORING_LIBRARY}
        PUBLIC
            ${WORKBENCH_LIBRARY}
            ${CLI_LIBRARY}
        PRIVATE
            ${PROJECT_NAME}_image
    )
    target_compile_features(${AUTHORING_LIBRARY} PUBLIC cxx_std_23)

    add_executable(${AUTHORING_EXECUTABLE}
        authoring/main.cpp
        ${CPP_UTF8_RC}
    )
    cpp_apply_safety_profile(${AUTHORING_EXECUTABLE})
    cpp_apply_utf8_manifest(${AUTHORING_EXECUTABLE})
    set_target_properties(${AUTHORING_EXECUTABLE} PROPERTIES
        OUTPUT_NAME "umbra-authoring"
    )
    target_link_libraries(${AUTHORING_EXECUTABLE} PRIVATE ${AUTHORING_LIBRARY})
    target_compile_features(${AUTHORING_EXECUTABLE} PRIVATE cxx_std_23)
endif()
