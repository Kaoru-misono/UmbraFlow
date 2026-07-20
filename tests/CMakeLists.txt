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

if(TARGET ${PROJECT_NAME}_controller)
    cpp_add_test(
        NAME test-controller
        SOURCES
            controller/test-discovery.cpp
            controller/test-dpi.cpp
            controller/test-target.cpp
        LIBS
            ${PROJECT_NAME}_controller
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
    set_tests_properties(check-modules check-safety PROPERTIES
        TIMEOUT 30
        LABELS "CI"
    )
else()
    message(WARNING "[Safety] Python was not found; repository safety checks are not registered with CTest.")
endif()
