add_executable(${PROJECT_NAME}
    cli/main.cpp
    ${CPP_UTF8_RC}
)
cpp_apply_safety_profile(${PROJECT_NAME})
cpp_apply_utf8_manifest(${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "umbra-flow")
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_core)
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
        m0-demo/ffi/external/stb_image.h
        m0-demo/ffi/external/stb_image_write.h
        m0-demo/ffi/png-decoder.cpp
        m0-demo/ffi/png-decoder.hpp
        m0-demo/ffi/png-encoder.cpp
        m0-demo/ffi/png-encoder.hpp
        m0-demo/ffi/png-limits.hpp
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
    target_include_directories(${M0_DEMO_LIBRARY}
        SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/m0-demo/ffi/external
    )
    target_link_libraries(${M0_DEMO_LIBRARY}
        PUBLIC
            ${PROJECT_NAME}_controller
            ${PROJECT_NAME}_vision
        PRIVATE
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
