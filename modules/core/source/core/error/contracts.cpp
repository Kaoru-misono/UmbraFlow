#include "contracts.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace uf::detail
{
    namespace
    {
        [[nodiscard]]
        auto contractKindName(ContractKind kind) noexcept -> std::string_view
        {
            switch (kind)
            {
            case ContractKind::Assertion: return "assertion";
            case ContractKind::Check: return "check";
            case ContractKind::Unreachable: return "unreachable";
            }

            return "unknown contract";
        }
    }

    [[noreturn]]
    auto contractViolation(
        ContractKind kind,
        std::string_view expression,
        std::string_view message,
        std::source_location location
    ) noexcept -> void
    {
        try
        {
            std::cerr << "[contract] " << contractKindName(kind) << " failed";

            if (!expression.empty())
            {
                std::cerr << ": " << expression;
            }

            if (!message.empty())
            {
                std::cerr << " - " << message;
            }

            std::cerr
                << " at "
                << location.file_name()
                << ':'
                << location.line()
                << " in "
                << location.function_name()
                << '\n';
            std::cerr.flush();
        }
        catch (...)
        {
            // Contract failure must terminate even if diagnostic output fails.
        }

#if defined(_WIN32)
        // The diagnostic above is the report. Windows would otherwise answer the
        // abort with a modal dialog that blocks the process until a human
        // dismisses it, which turns a violation into a hung run and takes over
        // the screen of whoever is at the machine -- and this suite drives
        // violations deliberately, both in cases that assert a refusal fires and
        // in the falsification loop that breaks a check to watch one go red.
        // Clearing both bits silences the dialog and the error-reporting handoff.
        // Nothing about when a contract fires, what it reports, or that the
        // process dies changes: abort still terminates and still leaves a
        // non-zero status for the runner to read.
        //
        // The cost, stated because it is real: no crash dump is handed to
        // Windows Error Reporting, so a violation that only reproduces here
        // cannot be post-mortem debugged from one. Restore _CALL_REPORTFAULT
        // temporarily when that is what you need.
        _set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

        std::abort();
    }
}
