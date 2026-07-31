#include "error-text.hpp"

#include <core/types/enum-reflection.hpp>
#include <domain/error.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

namespace uf::input_agent
{
    namespace
    {
        // A line stays one string, so the origin is reported as the source file
        // base name and line rather than the full build path.
        [[nodiscard]]
        auto originLabel(std::source_location location) -> std::string
        {
            return std::format(
                "{}:{}",
                std::filesystem::path{location.file_name()}.filename().string(),
                location.line()
            );
        }
    }

    auto formatAutomationError(Error const& error) -> std::string
    {
        auto name = std::optional<std::string_view>{};
        if (auto const kind = automationErrorKind(error); kind)
        {
            name = enumName(*kind);
        }
        auto formatted = std::string{name.value_or("UnknownAutomationErrorKind")}
            + ": "
            + std::string{error.message()};

        // Context frames record why a failure mattered to its caller, including
        // input compensation failures. Dropping them here would erase the only
        // record a completed run keeps of them.
        for (auto const& context : error.context())
        {
            formatted += " | ";
            formatted += context;
        }

        // The native code names its own category, so a reader can tell a Win32
        // code from an errno without knowing which subsystem failed.
        if (auto const nativeCode = error.nativeCode(); nativeCode)
        {
            formatted += " | ";
            formatted += nativeCode.category().name();
            formatted += ' ';
            formatted += std::to_string(nativeCode.value());
            formatted += ": ";
            formatted += nativeCode.message();
        }

        formatted += " | at ";
        formatted += originLabel(error.location());
        return formatted;
    }
}
