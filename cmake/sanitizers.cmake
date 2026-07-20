include_guard(GLOBAL)

option(CPP_ENABLE_ADDRESS_SANITIZER "Enable AddressSanitizer" OFF)
option(CPP_ENABLE_UNDEFINED_SANITIZER "Enable UndefinedBehaviorSanitizer" OFF)
option(CPP_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

function(cpp_apply_sanitizers TARGET_NAME)
    if(CPP_ENABLE_THREAD_SANITIZER AND CPP_ENABLE_ADDRESS_SANITIZER)
        message(FATAL_ERROR "[Safety] ThreadSanitizer and AddressSanitizer cannot be enabled together.")
    endif()

    if(NOT CPP_ENABLE_ADDRESS_SANITIZER
        AND NOT CPP_ENABLE_UNDEFINED_SANITIZER
        AND NOT CPP_ENABLE_THREAD_SANITIZER
    )
        return()
    endif()

    if(MSVC)
        if(CPP_ENABLE_UNDEFINED_SANITIZER OR CPP_ENABLE_THREAD_SANITIZER)
            message(FATAL_ERROR "[Safety] MSVC supports only the AddressSanitizer profile.")
        endif()

        target_compile_options(${TARGET_NAME} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address>"
        )
        target_link_options(${TARGET_NAME} PRIVATE /fsanitize=address)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "[Safety] Sanitizers are not configured for ${CMAKE_CXX_COMPILER_ID}.")
    endif()

    set(SANITIZERS "")
    if(CPP_ENABLE_ADDRESS_SANITIZER)
        list(APPEND SANITIZERS address)
    endif()
    if(CPP_ENABLE_UNDEFINED_SANITIZER)
        list(APPEND SANITIZERS undefined)
    endif()
    if(CPP_ENABLE_THREAD_SANITIZER)
        list(APPEND SANITIZERS thread)
    endif()
    list(JOIN SANITIZERS "," SANITIZER_LIST)

    target_compile_options(${TARGET_NAME} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-omit-frame-pointer;-fsanitize=${SANITIZER_LIST}>"
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fno-omit-frame-pointer
        "-fsanitize=${SANITIZER_LIST}"
    )
endfunction()
