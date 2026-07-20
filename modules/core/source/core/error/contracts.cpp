#include "contracts.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace umbra_flow::detail
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
        std::abort();
    }
}
