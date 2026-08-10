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
