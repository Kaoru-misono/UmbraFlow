set(CLI_LIBRARY ${PROJECT_NAME}_cli_support)

# Platform-free product logic: argument parsing, name resolution, the file trace
# sink, and the error/exit-code boundary. Linked into the executable and tests.
add_library(${CLI_LIBRARY} STATIC
    cli/args.cpp
    cli/args.hpp
    cli/file-trace-sink.cpp
    cli/file-trace-sink.hpp
    cli/name-resolution.cpp
    cli/name-resolution.hpp
    cli/run.cpp
    cli/run.hpp
)
cpp_apply_safety_profile(${CLI_LIBRARY})
target_include_directories(${CLI_LIBRARY}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/cli
)
target_link_libraries(${CLI_LIBRARY}
    PUBLIC
        ${PROJECT_NAME}_engine
)
target_compile_features(${CLI_LIBRARY} PUBLIC cxx_std_23)

if(WIN32 AND TARGET ${PROJECT_NAME}_controller)
    target_sources(${CLI_LIBRARY}
        PRIVATE
            cli/candidate-selection.cpp
            cli/candidate-selection.hpp
            cli/run-windows.cpp
            cli/platform/controller-action-sink.cpp
            cli/platform/controller-action-sink.hpp
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
            cli/run-unsupported.cpp
    )
endif()

add_executable(${PROJECT_NAME}
    cli/main.cpp
    ${CPP_UTF8_RC}
)
cpp_apply_safety_profile(${PROJECT_NAME})
cpp_apply_utf8_manifest(${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "umbra-flow")
target_link_libraries(${PROJECT_NAME} PRIVATE ${CLI_LIBRARY} ${PROJECT_NAME}_core)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

if(TARGET ${PROJECT_NAME}_controller)
    set(M0_DEMO_LIBRARY ${PROJECT_NAME}_m0_demo_support)
    set(M0_DEMO_EXECUTABLE ${PROJECT_NAME}_m0_demo)

    add_library(${M0_DEMO_LIBRARY} STATIC
        m0-demo/args.cpp
        m0-demo/args.hpp
        m0-demo/capture-mode.cpp
        m0-demo/capture-mode.hpp
        m0-demo/capture-output.cpp
        m0-demo/capture-output.hpp
        m0-demo/guard.cpp
        m0-demo/guard.hpp
        m0-demo/input-agent.cpp
        m0-demo/input-agent.hpp
        m0-demo/input-agent-protocol.cpp
        m0-demo/input-agent-protocol.hpp
        m0-demo/json-string.cpp
        m0-demo/json-string.hpp
        m0-demo/log-jsonl.cpp
        m0-demo/log-jsonl.hpp
        m0-demo/path-validation.cpp
        m0-demo/path-validation.hpp
        m0-demo/pacing.cpp
        m0-demo/pacing.hpp
        m0-demo/pipeline.cpp
        m0-demo/pipeline.hpp
        m0-demo/platform/windows-background-messages.cpp
        m0-demo/platform/windows-background-messages.hpp
        m0-demo/platform/windows-console-control.cpp
        m0-demo/platform/windows-console-control.hpp
        m0-demo/platform/windows-file-writer.cpp
        m0-demo/platform/windows-file-writer.hpp
        m0-demo/platform/windows-guard.cpp
        m0-demo/platform/windows-guard.hpp
        m0-demo/platform/windows-path.cpp
        m0-demo/platform/windows-path.hpp
        m0-demo/shutdown.cpp
        m0-demo/shutdown.hpp
        m0-demo/target-setup.cpp
        m0-demo/target-setup.hpp
    )
    cpp_apply_safety_profile(${M0_DEMO_LIBRARY})
    target_include_directories(${M0_DEMO_LIBRARY}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/m0-demo
    )
    target_link_libraries(${M0_DEMO_LIBRARY}
        PUBLIC
            ${PROJECT_NAME}_controller
            ${PROJECT_NAME}_vision
        PRIVATE
            ${PROJECT_NAME}_image
            advapi32
            ntdll
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
        workbench/app/canvas-math.cpp
        workbench/app/canvas-math.hpp
        workbench/app/workbench-app.cpp
        workbench/app/workbench-app.hpp
        workbench/authoring-edit.cpp
        workbench/authoring-edit.hpp
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

    # One-shot Windows Graphics Capture ingestion reuses the controller capture
    # session, so it is only compiled where the controller module is present.
    if(TARGET ${PROJECT_NAME}_controller)
        target_sources(${WORKBENCH_LIBRARY}
            PRIVATE
                workbench/platform/windows-capture-source.cpp
                workbench/platform/windows-capture-source.hpp
        )
        target_link_libraries(${WORKBENCH_LIBRARY}
            PRIVATE
                ${PROJECT_NAME}_controller
        )
    endif()

    # The GUI executable needs the engine (for backend logic) and the controller
    # (for WGC capture ingestion). Dear ImGui and the Direct3D 11 shell live only
    # behind this gate, so the platform-free workbench_support stays free of them.
    if(TARGET ${PROJECT_NAME}_controller AND TARGET ${PROJECT_NAME}_engine)
        set(IMGUI_LIBRARY imgui)

        # Dear ImGui ships no build system, so the core translation units and the
        # Win32 + Direct3D 11 backends are compiled here as a private static
        # library. Its headers are exported SYSTEM so the project's strict
        # warnings-as-errors profile never fires on third-party code, mirroring how
        # modules/script/external isolates Luau.
        add_library(${IMGUI_LIBRARY} STATIC
            workbench/external/imgui/imgui.cpp
            workbench/external/imgui/imgui_draw.cpp
            workbench/external/imgui/imgui_tables.cpp
            workbench/external/imgui/imgui_widgets.cpp
            workbench/external/imgui/backends/imgui_impl_win32.cpp
            workbench/external/imgui/backends/imgui_impl_dx11.cpp
        )
        target_include_directories(${IMGUI_LIBRARY} SYSTEM PUBLIC
            workbench/external/imgui
            workbench/external/imgui/backends
        )
        if(MSVC)
            target_compile_options(${IMGUI_LIBRARY} PRIVATE /W0)
        endif()
        target_link_libraries(${IMGUI_LIBRARY}
            PUBLIC
                d3d11
                dxgi
                dwmapi
                imm32
                user32
                gdi32
        )
        target_compile_features(${IMGUI_LIBRARY} PRIVATE cxx_std_17)

        set(WORKBENCH_EXECUTABLE ${PROJECT_NAME}_workbench)

        add_executable(${WORKBENCH_EXECUTABLE}
            workbench/app/main.cpp
            workbench/app/panels.cpp
            workbench/app/panels.hpp
            workbench/platform/windows-file-dialog.cpp
            workbench/platform/windows-file-dialog.hpp
            workbench/platform/windows-gui-shell.cpp
            workbench/platform/windows-gui-shell.hpp
            workbench/platform/windows-texture-cache.cpp
            workbench/platform/windows-texture-cache.hpp
            ${CPP_UTF8_RC}
        )
        cpp_apply_safety_profile(${WORKBENCH_EXECUTABLE})
        cpp_apply_utf8_manifest(${WORKBENCH_EXECUTABLE})
        set_target_properties(${WORKBENCH_EXECUTABLE} PROPERTIES
            OUTPUT_NAME "umbra-workbench"
        )
        target_link_libraries(${WORKBENCH_EXECUTABLE}
            PRIVATE
                ${WORKBENCH_LIBRARY}
                ${PROJECT_NAME}_engine
                ${PROJECT_NAME}_controller
                ${PROJECT_NAME}_image
                ${IMGUI_LIBRARY}
                comdlg32
                d3d11
                dxgi
                user32
        )
        target_compile_features(${WORKBENCH_EXECUTABLE} PRIVATE cxx_std_23)
    endif()
endif()
