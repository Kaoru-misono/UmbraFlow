# What makes a registered doctest CTest an executed gate rather than a name.
#
# doctest treats a --test-case filter that selects nothing as success: it runs
# no case, executes no assertion, and exits 0. A gate whose TEST_CASE is
# compiled out -- by an #if, by a platform guard, or because its translation
# unit left the target -- therefore stays green forever while proving nothing.
#
# The configure-time CASES check cannot close that. It matches CASES against
# TEST_CASE declarations found in source text, which the preprocessor has not
# touched yet, and the same source can compile out on one platform and not
# another. The property has to be checked on the run.
#
# doctest's run summary reports the number of assertions it executed, so the
# run itself carries the evidence. FAIL_REGULAR_EXPRESSION adds a failure
# condition and keeps the exit-code one; PASS_REGULAR_EXPRESSION would replace
# it, and a genuinely failing gate would then pass on the strength of its
# summary line.

include_guard(GLOBAL)

# doctest is vendored at external/doctest and belongs to no single module --
# tests/ and modules/conformance both compile against it. It is defined once,
# here, beside uf_require_executed_assertions below; a second CMakeLists.txt
# under external/doctest would only add a second place to keep in sync with this
# one. uf::doctest is the only name anything spells for it.
#
# The root CMakeLists.txt includes this file before the module autoloader runs,
# because modules/conformance names uf::doctest in its manifest and a manifest
# dependency has to be a target by the time the link pass resolves it. The two
# includes below that one are no-ops through the guard and stay, so neither
# tests/ nor a conformance run depends on that ordering.
cmake_path(SET UF_DOCTEST_INCLUDE_DIR NORMALIZE
    "${CMAKE_CURRENT_LIST_DIR}/../external/doctest"
)
if(NOT EXISTS "${UF_DOCTEST_INCLUDE_DIR}/doctest/doctest.h")
    message(FATAL_ERROR
        "doctest-gate: doctest headers are missing at ${UF_DOCTEST_INCLUDE_DIR}. "
        "Restore external/doctest before configuring."
    )
endif()

add_library(uf_doctest INTERFACE)
target_include_directories(uf_doctest SYSTEM INTERFACE "${UF_DOCTEST_INCLUDE_DIR}")
add_library(uf::doctest ALIAS uf_doctest)

# doctest writes the count immediately after "assertions: ", right-aligned in
# spaces, with no color escape between the two. The bracket expression spells
# the trailing pipe: an unescaped | is alternation, and CMake's argument parser
# eats the backslash of \| before the regex engine sees it, which turns the
# pattern into "anything".
set(UF_DOCTEST_NO_ASSERTIONS_PATTERN "assertions: +0 [|]")

# Fails each named CTest unless its doctest run executed at least one assertion.
function(uf_require_executed_assertions)
    foreach(DOCTEST_GATE IN LISTS ARGN)
        if(NOT TEST "${DOCTEST_GATE}")
            message(FATAL_ERROR
                "uf_require_executed_assertions names a CTest that is not "
                "registered: ${DOCTEST_GATE}"
            )
        endif()
        set_property(TEST "${DOCTEST_GATE}" PROPERTY
            FAIL_REGULAR_EXPRESSION "${UF_DOCTEST_NO_ASSERTIONS_PATTERN}"
        )
    endforeach()
endfunction()
