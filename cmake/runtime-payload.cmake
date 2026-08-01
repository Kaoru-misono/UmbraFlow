# Some executables load a dependency from beside their own binary rather than
# from PATH (onnxruntime.dll is the current example; see
# modules/ocr/external/CMakeLists.txt), so the build must place a copy there
# for every executable that transitively links the module owning it.
#
# CMAKE_RUNTIME_OUTPUT_DIRECTORY is one shared bin/ for every target in this
# build, so a target that never stages the file for itself can still *look*
# correct in a full build: some other target's copy step happens to land the
# file in that shared folder first, and this one finds it already there. That
# stops being true the moment the target is built alone -- a scoped release
# build of one executable, say -- and Windows then falls back to searching
# PATH, which is how a stale onnxruntime.dll once got loaded into a release
# umbra-flow.exe. Call this for each such target instead of relying on build
# order.

# Copies FILES beside TARGET_NAME's own build output after every build of that
# target. Uses $<TARGET_FILE_DIR:...> rather than CMAKE_RUNTIME_OUTPUT_DIRECTORY
# so this also lands in the right place under a multi-config generator, where
# CMake nests a per-config subdirectory this file never names directly.
function(cpp_stage_runtime_libraries TARGET_NAME)
    if(NOT ARGN)
        return()
    endif()

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ARGN}
            "$<TARGET_FILE_DIR:${TARGET_NAME}>"
        COMMENT "[Payload] ${TARGET_NAME}: staging runtime libraries beside the executable"
        VERBATIM
    )
endfunction()
