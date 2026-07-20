include_guard(GLOBAL)

option(CPP_ENABLE_HARDENING "Enable compiler and linker hardening" ON)

function(cpp_apply_hardening TARGET_NAME)
    if(NOT CPP_ENABLE_HARDENING)
        return()
    endif()

    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:/guard:cf;/sdl>"
        )
        target_link_options(${TARGET_NAME} PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${TARGET_NAME} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-fstack-protector-strong>"
        )
        target_link_options(${TARGET_NAME} PRIVATE -fstack-protector-strong)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_definitions(${TARGET_NAME} PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:_GLIBCXX_ASSERTIONS>"
            )
        endif()
    else()
        message(WARNING "[Safety] No hardening profile is defined for ${CMAKE_CXX_COMPILER_ID}.")
    endif()
endfunction()
