add_executable(${PROJECT_NAME}
    cli/main.cpp
    ${CPP_UTF8_RC}
)
cpp_apply_safety_profile(${PROJECT_NAME})
cpp_apply_utf8_manifest(${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "umbra-flow")
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_core)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
