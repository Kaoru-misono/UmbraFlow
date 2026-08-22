#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#if defined(_WIN32)
#    include <crtdbg.h>
#    include <cstdlib>
#    include <windows.h>
#endif

// A contract violation aborts, and this suite deliberately drives violations:
// several cases assert that a refusal fires, and falsifying a check means
// breaking it on purpose and watching a case go red. On Windows the C runtime
// answers an abort with a modal dialog that blocks the process until a human
// dismisses it, which turns a failing assertion into a hung test run and takes
// over the screen of whoever is working on the machine. The report goes to
// stderr instead and the abort becomes an exit code, which is what a test runner
// reads anyway.
//
// This only silences the dialog. Nothing about when a contract fires, what it
// reports, or whether the process dies changes.
int main(int argc, char** argv)
{
#if defined(_WIN32)
    _set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (auto const report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT})
    {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
#endif

    return doctest::Context{argc, argv}.run();
}
