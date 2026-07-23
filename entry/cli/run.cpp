#include "run.hpp"

#include <core/types/enum-reflection.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace uf::cli
{
    auto formatRunError(Error const& error) -> std::string
    {
        auto kindName = std::optional<std::string_view>{};
        if (auto const kind = automationErrorKind(error); kind)
        {
            kindName = enumName(*kind);
        }

        auto formatted = std::string{kindName.value_or("UnknownAutomationErrorKind")};
        formatted += ": ";
        formatted += error.message();

        for (auto const& context : error.context())
        {
            formatted += " | ";
            formatted += context;
        }

        if (auto const nativeCode = error.nativeCode(); nativeCode)
        {
            formatted += " | ";
            formatted += nativeCode.category().name();
            formatted += ' ';
            formatted += std::to_string(nativeCode.value());
        }

        return formatted;
    }

    auto exitCodeForError(Error const& error, bool stopRequested) noexcept -> int32
    {
        // A Ctrl-C during a blocked step surfaces as that step's failure (a stalled
        // capture reports CaptureStalled, for example), not as Cancelled. The
        // operator's intent to stop takes precedence in reporting, so a requested
        // cancellation always maps to the documented cancellation code.
        if (stopRequested)
        {
            return 5;
        }

        auto const kind = automationErrorKind(error);
        if (!kind)
        {
            return 1;
        }
        switch (*kind)
        {
        case AutomationErrorKind::Cancelled:
            return 5;
        case AutomationErrorKind::Timeout:
            return 4;
        case AutomationErrorKind::TargetCompatibilityUnverified:
            return 2;
        case AutomationErrorKind::InvalidResource:
        case AutomationErrorKind::UnsupportedCapability:
        case AutomationErrorKind::TargetUnavailable:
        case AutomationErrorKind::CaptureUnavailable:
        case AutomationErrorKind::CaptureStalled:
        case AutomationErrorKind::RecognitionFailed:
        case AutomationErrorKind::StaleObservation:
        case AutomationErrorKind::ActionRejected:
        case AutomationErrorKind::ControllerDisconnected:
        case AutomationErrorKind::InternalInvariant:
        case AutomationErrorKind::IoFailure:
        case AutomationErrorKind::ExternalFailure:
            return 1;
        }
        return 1;
    }
}
