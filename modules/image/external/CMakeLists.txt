# stb is confined to the image module's FFI implementation.
add_library(${PROJECT_NAME}_image_stb INTERFACE)
target_include_directories(${PROJECT_NAME}_image_stb SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/stb
)
