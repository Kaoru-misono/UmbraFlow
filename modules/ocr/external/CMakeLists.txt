# ONNX Runtime is confined to the ocr module's FFI implementation, the way stb is
# confined to image's. Nothing above source/ocr/ffi/ may name an Ort type.
#
# The payload is committed under this directory; see manifest.toml for where it
# came from and how to re-verify it. A missing payload is reported here rather
# than as a link error three targets later.

set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime")

if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_c_api.h")
    message(FATAL_ERROR
        "[External] ocr: the onnxruntime payload is missing from ${ONNXRUNTIME_ROOT}.\n"
        "  Restore it with: python scripts/fetch_external.py --module ocr"
    )
endif()

add_library(${PROJECT_NAME}_ocr_onnxruntime SHARED IMPORTED GLOBAL)

set_target_properties(${PROJECT_NAME}_ocr_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
    IMPORTED_IMPLIB   "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
)

# SYSTEM so the vendor headers do not answer to this project's /W4 /WX profile,
# matching how image treats stb.
target_include_directories(${PROJECT_NAME}_ocr_onnxruntime SYSTEM INTERFACE
    "${ONNXRUNTIME_ROOT}/include"
)

# The runtime resolves onnxruntime.dll from the executable's own directory, so
# every consumer needs it beside the binary rather than on PATH. Exported as a
# variable because the copy belongs to whichever target produces an executable;
# doing it from here would guess at output directories this file cannot see.
set(UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES
    "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
    "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
    CACHE INTERNAL "Runtime DLLs the ocr module's consumers must ship beside their executable"
)
