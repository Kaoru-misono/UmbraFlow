include_guard(GLOBAL)

include(CheckCXXCompilerFlag)

option(
    CPP_ENABLE_COMPILER_SAFETY_ANALYSIS
    "Enable supported compiler lifetime, bounds, and thread-safety diagnostics"
    ON
)
option(
    CPP_REQUIRE_CLANG_LIFETIME_SAFETY
    "Fail configuration unless Clang lifetime-safety diagnostics are available"
    OFF
)

set(CPP_COMPILER_SAFETY_OPTIONS "")

if(CPP_ENABLE_COMPILER_SAFETY_ANALYSIS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    check_cxx_compiler_flag(
        "-Wunsafe-buffer-usage -Werror=unknown-warning-option"
        CPP_HAS_UNSAFE_BUFFER_USAGE
    )
    check_cxx_compiler_flag(
        "-Wthread-safety -Werror=unknown-warning-option"
        CPP_HAS_THREAD_SAFETY
    )
    check_cxx_compiler_flag(
        "-Wlifetime-safety-permissive -Werror=unknown-warning-option"
        CPP_HAS_LIFETIME_SAFETY
    )

    if(CPP_HAS_UNSAFE_BUFFER_USAGE)
        list(APPEND CPP_COMPILER_SAFETY_OPTIONS -Wunsafe-buffer-usage)
    endif()
    if(CPP_HAS_THREAD_SAFETY)
        list(APPEND CPP_COMPILER_SAFETY_OPTIONS -Wthread-safety)
    endif()
    if(CPP_HAS_LIFETIME_SAFETY)
        list(APPEND CPP_COMPILER_SAFETY_OPTIONS -Wlifetime-safety-permissive)
    endif()
endif()

if(CPP_REQUIRE_CLANG_LIFETIME_SAFETY AND NOT CPP_HAS_LIFETIME_SAFETY)
    message(FATAL_ERROR
        "[Safety] The selected compiler does not support -Wlifetime-safety-permissive. "
        "Use the pinned Clang analysis toolchain or disable the required analysis explicitly."
    )
endif()

function(cpp_apply_compiler_safety_analysis TARGET_NAME)
    if(NOT CPP_COMPILER_SAFETY_OPTIONS)
        return()
    endif()

    target_compile_options(${TARGET_NAME} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CPP_COMPILER_SAFETY_OPTIONS}>"
    )
endfunction()
