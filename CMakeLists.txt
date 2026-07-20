cmake_minimum_required(VERSION 3.30)

project(UmbraFlow
    VERSION 0.1.0
    DESCRIPTION "Native C++ visual automation framework for Windows: WGC capture, template matching, strict background PostMessage input"
    LANGUAGES CXX
)

option(CPP_BUILD_TESTS "Build project tests" ON)
option(CPP_USE_CCACHE "Use ccache with Ninja when available" ON)
option(CPP_VERBOSE_AUTOLOADER "Enable verbose module loader logs" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    set(CPP_UTF8_MANIFEST "${CMAKE_SOURCE_DIR}/cmake/utf8.manifest")
    if(MSVC)
        set(CPP_UTF8_RC "")
    else()
        set(CPP_UTF8_RC "${CMAKE_SOURCE_DIR}/cmake/utf8.rc")
    endif()
else()
    set(CPP_UTF8_MANIFEST "")
    set(CPP_UTF8_RC "")
endif()

include(cmake/ccache.cmake)
include(cmake/safety-profile.cmake)
include(cmake/build.cmake)

set(CPP_MODULE_ROOTS
    "${CMAKE_SOURCE_DIR}/modules"
    CACHE STRING "Semicolon-separated module root directories"
)

cpp_scan_and_load_modules(${CPP_MODULE_ROOTS})

add_subdirectory(entry)

if(CPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
