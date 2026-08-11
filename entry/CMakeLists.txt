set(APPLICATION_INFO_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(APPLICATION_INFO_HEADER "${APPLICATION_INFO_DIRECTORY}/application-info.hpp")
file(MAKE_DIRECTORY "${APPLICATION_INFO_DIRECTORY}")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/application-info.hpp.in"
    "${APPLICATION_INFO_HEADER}"
    @ONLY
)

# Only main.cpp lives here, for each of the two binaries. Everything the
# commands are made of is uf::cli and everything the suite is made of is
# uf::conformance, declared by their manifests: this directory is where an
# executable is declared, and an executable's build declaration is the one thing
# a module manifest cannot say.
add_executable(${PROJECT_NAME}
    cli/main.cpp
    "${APPLICATION_INFO_HEADER}"
)
cpp_apply_safety_profile(${PROJECT_NAME})
cpp_apply_utf8_encoding(${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "umbra-flow")
target_include_directories(${PROJECT_NAME} PRIVATE "${APPLICATION_INFO_DIRECTORY}")
target_link_libraries(${PROJECT_NAME} PRIVATE uf::cli uf::core)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

# cli links engine publicly, and engine links ocr publicly, so this executable
# resolves onnxruntime.dll from its own directory at process start; see
# cmake/runtime-payload.cmake for why that copy cannot be left to some other
# target's build order.
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
        # PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the payload is this
        # repository's, and under add_subdirectory the latter names a consumer's
        # root, where the build would ask for a file nothing can produce.
        set(OCR_MODEL_ROOT
            "${PROJECT_SOURCE_DIR}/modules/ocr/external/models/${OCR_MODEL_NAME}"
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

# The second shipped binary, and the same shape as the first: one main.cpp here,
# every case and fixture in uf::conformance. It is a binary rather than a
# subcommand of umbra-flow because every frame the suite observes and every
# action it posts declares TargetWorld::Recorded, so a run can never reach a live
# window while reaching one is the whole job of umbra-flow -- two binaries make
# that a link-time fact rather than a subcommand string. It takes no
# application-info.hpp: the version banner is umbra-flow's surface, and a run
# here is identified by the project directory it names.
add_executable(umbra-flow-conformance conformance/main.cpp)
cpp_apply_safety_profile(umbra-flow-conformance)
cpp_apply_utf8_encoding(umbra-flow-conformance)

# WHOLE_ARCHIVE, without which this binary runs no cases at all. A TEST_CASE
# registers itself from the dynamic initializer of a namespace-scope object that
# nothing refers to by name, and a linker pulls a static library's members in to
# resolve undefined symbols -- so the three translation units that hold nothing
# but cases are pulled in by nothing. The result is not a link error: the binary
# links, starts, reports "test cases: 0" and exits 0. This is what stops that;
# uf_require_executed_assertions on every gate is what would catch it if it ever
# stopped stopping it.
target_link_libraries(umbra-flow-conformance PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,uf::conformance>"
)
target_compile_features(umbra-flow-conformance PRIVATE cxx_std_23)

# conformance links task publicly, which links engine and ocr publicly, so this
# executable resolves onnxruntime.dll from its own directory at process start;
# see cmake/runtime-payload.cmake. It stages no OCR model payload: no case here
# reaches the text reader, and `--ocr-models` is umbra-flow's option.
cpp_stage_runtime_libraries(umbra-flow-conformance ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})
