include_guard(GLOBAL)

option(CPP_ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)
set(
    CPP_CLANG_TIDY_EXECUTABLE
    ""
    CACHE FILEPATH
    "Path to clang-tidy; discovered from PATH when empty"
)

if(CPP_ENABLE_CLANG_TIDY AND NOT CPP_CLANG_TIDY_EXECUTABLE)
    find_program(CPP_CLANG_TIDY_DISCOVERED NAMES clang-tidy REQUIRED)
    set(CPP_CLANG_TIDY_EXECUTABLE "${CPP_CLANG_TIDY_DISCOVERED}")
endif()

function(cpp_apply_static_analysis TARGET_NAME)
    if(NOT CPP_ENABLE_CLANG_TIDY)
        return()
    endif()

    # This repository's own configuration, resolved against this file rather
    # than CMAKE_SOURCE_DIR: under add_subdirectory the latter is the consuming
    # project's root, which carries its own .clang-tidy or none at all.
    cmake_path(SET CPP_CLANG_TIDY_CONFIG NORMALIZE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.clang-tidy"
    )
    set(CPP_CLANG_TIDY_COMMAND
        "${CPP_CLANG_TIDY_EXECUTABLE}"
        "--config-file=${CPP_CLANG_TIDY_CONFIG}"
    )

    # CMake invokes clang-tidy in clang-cl mode for MSVC compilation databases.
    # Some clang-tidy distributions do not translate /EHsc into Clang's feature
    # macros, causing exception-aware headers to be analyzed as if exceptions
    # were disabled. Keep the analysis frontend consistent with the real build.
    if(MSVC)
        list(APPEND CPP_CLANG_TIDY_COMMAND
            "--extra-arg=/EHsc"
            "--extra-arg=/D_CPPUNWIND=1"
            # Clang's default instantiation depth is 1024, and MSVC's own headers
            # exceed it while being parsed in clang-cl mode -- vision's frame
            # analysis stopped the whole analysis build there, which is why that
            # preset had never produced a clean result to read. The compiler that
            # actually builds this code has no such ceiling, so this raises the
            # ANALYSER to match rather than changing anything the build does.
            "--extra-arg=-ftemplate-depth=4096"
        )
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_CLANG_TIDY "${CPP_CLANG_TIDY_COMMAND}"
    )
endfunction()
