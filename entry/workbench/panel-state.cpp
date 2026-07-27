#include "panel-state.hpp"

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uf::workbench
{
    auto logSeverityWord(LogSeverity severity) -> std::string_view
    {
        switch (severity)
        {
        case LogSeverity::Info:
            return "INFO";
        case LogSeverity::Warning:
            return "WARN";
        case LogSeverity::Error:
            return "ERROR";
        }
        UF_UNREACHABLE_MSG("unknown LogSeverity value");
    }

    auto formatLogLine(
        LogSeverity severity,
        std::string_view timestamp,
        std::string_view message
    ) -> std::string
    {
        return std::format(
            "{}  {}  {}",
            timestamp,
            logSeverityWord(severity),
            message
        );
    }

    auto formatLogTimestamp(
        std::chrono::system_clock::time_point instant
    ) -> std::string
    {
        auto const seconds = std::chrono::floor<std::chrono::seconds>(instant);
        return std::format("{:%Y-%m-%dT%H:%M:%SZ}", seconds);
    }

    auto PanelUiState::report(LogSeverity severity, std::string message) -> void
    {
        statusSeverity = severity;
        statusLine     = std::move(message);
    }

    auto PanelUiState::captureLogEvent(
        std::string_view timestamp
    ) -> std::optional<LogEvent>
    {
        // Same collapse the disk mirror has always applied: an empty line and a
        // consecutive repeat of the last recorded message leave no new entry.
        if (statusLine.empty() || statusLine == lastLoggedStatus)
        {
            return std::nullopt;
        }

        auto event = LogEvent{
            .severity  = statusSeverity,
            .timestamp = std::string{timestamp},
            .message   = statusLine,
        };
        logEvents.emplace_back(event);
        if (logEvents.size() > k_logEventCapacity)
        {
            logEvents.pop_front();
        }
        lastLoggedStatus = statusLine;
        return event;
    }

    auto PanelUiState::clearLog() -> void
    {
        logEvents.clear();
    }
}
