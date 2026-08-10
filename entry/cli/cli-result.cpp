#include "cli-result.hpp"

#include <core/types/enum-reflection.hpp>

#include <domain/error.hpp>

#include <task/task-host.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uf::cli
{
    auto formatError(Error const& error) -> std::string
    {
        auto kindName = std::optional<std::string_view>{};
        if (auto const kind = automationErrorKind(error); kind)
        {
            kindName = enumName(*kind);
        }

        auto formatted = std::string{
            kindName.value_or("UnknownAutomationErrorKind")
        };
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

        auto const location = error.location();
        formatted += " | at ";
        formatted += std::filesystem::path{
            location.file_name()
        }.filename().string();
        formatted += ':';
        formatted += std::to_string(location.line());

        return formatted;
    }

    auto exitCodeForError(
        Error const& error,
        bool stopRequested
    ) noexcept -> ExitCode
    {
        if (stopRequested)
        {
            return ExitCode::Cancelled;
        }

        auto const kind = automationErrorKind(error);
        if (!kind)
        {
            return ExitCode::Failure;
        }

        switch (*kind)
        {
        case AutomationErrorKind::Cancelled:
            return ExitCode::Cancelled;
        case AutomationErrorKind::Timeout:
            return ExitCode::Timeout;
        case AutomationErrorKind::TargetCompatibilityUnverified:
            return ExitCode::TargetCompatibilityUnverified;
        case AutomationErrorKind::InvalidResource:
        case AutomationErrorKind::UnsupportedCapability:
        case AutomationErrorKind::TargetUnavailable:
        case AutomationErrorKind::CaptureUnavailable:
        case AutomationErrorKind::CaptureStalled:
        case AutomationErrorKind::RecognitionIncomplete:
        case AutomationErrorKind::StaleObservation:
        case AutomationErrorKind::PageUnresolved:
        case AutomationErrorKind::ActionRejected:
        case AutomationErrorKind::ControllerDisconnected:
        case AutomationErrorKind::InternalInvariant:
        case AutomationErrorKind::IoFailure:
        case AutomationErrorKind::ExternalFailure:
            return ExitCode::Failure;
        }
        return ExitCode::Failure;
    }

    auto exitCodeForTaskReport(
        task::TaskRunReport const& report,
        bool stopRequested
    ) noexcept -> ExitCode
    {
        if (!report.failure)
        {
            return ExitCode::Success;
        }
        return exitCodeForError(*report.failure, stopRequested);
    }
}
