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

    set(CPP_CLANG_TIDY_COMMAND
        "${CPP_CLANG_TIDY_EXECUTABLE}"
        "--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy"
    )

    # CMake invokes clang-tidy in clang-cl mode for MSVC compilation databases.
    # Some clang-tidy distributions do not translate /EHsc into Clang's feature
    # macros, causing exception-aware headers to be analyzed as if exceptions
    # were disabled. Keep the analysis frontend consistent with the real build.
    if(MSVC)
        list(APPEND CPP_CLANG_TIDY_COMMAND
            "--extra-arg=/EHsc"
            "--extra-arg=/D_CPPUNWIND=1"
        )
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_CLANG_TIDY "${CPP_CLANG_TIDY_COMMAND}"
    )
endfunction()
