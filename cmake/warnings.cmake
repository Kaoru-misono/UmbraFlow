include_guard(GLOBAL)

option(CPP_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)

function(cpp_apply_warnings TARGET_NAME)
    if(MSVC)
        set(WARNING_OPTIONS
            /EHsc
            /MP
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:preprocessor
            /w14242
            /w14254
            /w14263
            /w14265
            /w14287
            /we4289
            /w14296
            /w14311
            /w44061
            /w44062
            /w14545
            /w14546
            /w14547
            /w14549
            /w14555
            /w14619
            /w14640
            /w14826
            /w14905
            /w14906
            /w14928
        )

        if(CPP_WARNINGS_AS_ERRORS)
            list(APPEND WARNING_OPTIONS /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        set(WARNING_OPTIONS
            -Wall
            -Wcast-align
            -Wconversion
            -Wdouble-promotion
            -Wextra
            -Wformat=2
            -Wimplicit-fallthrough
            -Wnon-virtual-dtor
            -Wnull-dereference
            -Wold-style-cast
            -Woverloaded-virtual
            -Wpedantic
            -Wshadow
            -Wsign-conversion
            -Wswitch-enum
            -Wundef
        )

        if(CPP_WARNINGS_AS_ERRORS)
            list(APPEND WARNING_OPTIONS -Werror)
        endif()
    else()
        message(WARNING "[Safety] No warning profile is defined for ${CMAKE_CXX_COMPILER_ID}.")
        return()
    endif()

    target_compile_options(${TARGET_NAME} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${WARNING_OPTIONS}>"
    )
endfunction()
