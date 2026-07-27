#include <task/trace.hpp>

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        // Minimal JSON string escaper. task is a module and must not link another
        // subsystem's escaper, so this is a local copy of engine/trace.cpp's
        // approach: quote the value and escape the mandatory control and
        // structural bytes.
        [[nodiscard]]
        auto escapeJsonString(std::string_view value) -> std::string
        {
            auto constexpr hex = std::string_view{"0123456789abcdef"};

            auto output = std::string{"\""};
            output.reserve(value.size() + 2U);
            for (auto const character : value)
            {
                auto const byte = static_cast<unsigned char>(character);
                switch (byte)
                {
                case '"': output += "\\\""; break;
                case '\\': output += "\\\\"; break;
                case '\b': output += "\\b"; break;
                case '\f': output += "\\f"; break;
                case '\n': output += "\\n"; break;
                case '\r': output += "\\r"; break;
                case '\t': output += "\\t"; break;
                default:
                    if (byte < 0x20U)
                    {
                        output += "\\u00";
                        output += hex[byte >> 4U];
                        output += hex[byte & 0x0FU];
                    }
                    else
                    {
                        output += static_cast<char>(byte);
                    }
                    break;
                }
            }

            output += '"';
            return output;
        }

        // Schema-owned names, kept independent of enum reflection so the wire
        // format cannot silently follow a rename of the underlying enumerator.
        [[nodiscard]]
        auto taskTraceEventKindName(TaskTraceEventKind kind) noexcept
            -> std::string_view
        {
            switch (kind)
            {
            case TaskTraceEventKind::TaskStarted: return "TaskStarted";
            case TaskTraceEventKind::ResourcesValidated: return "ResourcesValidated";
            case TaskTraceEventKind::HostCall: return "HostCall";
            case TaskTraceEventKind::TaskFinished: return "TaskFinished";
            }

            UF_UNREACHABLE_MSG("Unknown TaskTraceEventKind value");
        }

        [[nodiscard]]
        auto hostCallOutcomeName(HostCallOutcome outcome) noexcept -> std::string_view
        {
            switch (outcome)
            {
            case HostCallOutcome::Succeeded: return "Succeeded";
            case HostCallOutcome::Empty: return "Empty";
            case HostCallOutcome::Failed: return "Failed";
            }

            UF_UNREACHABLE_MSG("Unknown HostCallOutcome value");
        }

        [[nodiscard]]
        auto taskExitReasonName(TaskExitReason reason) noexcept -> std::string_view
        {
            switch (reason)
            {
            case TaskExitReason::Completed: return "Completed";
            case TaskExitReason::Failed: return "Failed";
            case TaskExitReason::Cancelled: return "Cancelled";
            }

            UF_UNREACHABLE_MSG("Unknown TaskExitReason value");
        }

        // The snake_case wire spelling of an automation error kind, owned by this
        // schema and kept independent of enum reflection so the wire format cannot
        // silently follow a rename. It deliberately matches the `kind` a script's
        // Tier B error table reports (umbra-tables.cpp), so a trace line names a
        // failure with exactly the spelling the script saw. The trailing
        // UF_UNREACHABLE keeps the switch total under /WX without a default that
        // would hide a newly added kind.
        [[nodiscard]]
        auto errorKindWireName(AutomationErrorKind kind) noexcept -> std::string_view
        {
            switch (kind)
            {
            case AutomationErrorKind::Cancelled:                     return "cancelled";
            case AutomationErrorKind::Timeout:                       return "timeout";
            case AutomationErrorKind::InvalidResource:               return "invalid_resource";
            case AutomationErrorKind::UnsupportedCapability:         return "unsupported_capability";
            case AutomationErrorKind::TargetCompatibilityUnverified: return "target_compatibility_unverified";
            case AutomationErrorKind::TargetUnavailable:             return "target_unavailable";
            case AutomationErrorKind::CaptureUnavailable:            return "capture_unavailable";
            case AutomationErrorKind::CaptureStalled:                return "capture_stalled";
            case AutomationErrorKind::RecognitionFailed:             return "recognition_failed";
            case AutomationErrorKind::StaleObservation:              return "stale_observation";
            case AutomationErrorKind::ActionRejected:                return "action_rejected";
            case AutomationErrorKind::ControllerDisconnected:        return "controller_disconnected";
            case AutomationErrorKind::InternalInvariant:             return "internal_invariant";
            case AutomationErrorKind::IoFailure:                     return "io_failure";
            case AutomationErrorKind::ExternalFailure:               return "external_failure";
            }

            UF_UNREACHABLE_MSG("Unknown AutomationErrorKind value");
        }

        // Accumulates a JSON object, tracking whether the leading comma is due.
        // Mutating its own buffer is the builder's whole purpose, so its methods
        // do not use output parameters.
        class TaskTraceLineBuilder final
        {
            std::string m_text{"{"};
            bool        m_needsComma{false};

            void separate()
            {
                if (m_needsComma)
                {
                    m_text += ',';
                }

                m_needsComma = true;
            }

        public:
            void addLiteral(std::string_view name, std::string_view literalValue)
            {
                separate();
                m_text += escapeJsonString(name);
                m_text += ':';
                m_text += literalValue;
            }

            void addString(std::string_view name, std::string_view value)
            {
                addLiteral(name, escapeJsonString(value));
            }

            // Emits a JSON array of the already-sorted `values`. Every element is
            // escaped, so a name that ever carries a quote or control byte stays a
            // valid line.
            void addStringArray(
                std::string_view name,
                std::span<std::string const> values
            )
            {
                separate();
                m_text += escapeJsonString(name);
                m_text += ":[";
                auto first = true;
                for (auto const& value : values)
                {
                    if (!first)
                    {
                        m_text += ',';
                    }
                    first = false;
                    m_text += escapeJsonString(value);
                }
                m_text += ']';
            }

            [[nodiscard]]
            auto finish() -> std::string
            {
                m_text += '}';
                return std::move(m_text);
            }
        };

        // Sorts a copy of `names` before it reaches the wire, so an unordered
        // container's iteration order can never leak into the trace (a determinism
        // constraint the ledger draws explicitly to trace/state serialization).
        [[nodiscard]]
        auto sortedCopy(std::vector<std::string> const& names)
            -> std::vector<std::string>
        {
            auto copy = names;
            std::ranges::sort(copy);
            return copy;
        }
    }

    auto serializeTaskTraceEvent(TaskTraceEvent const& event) -> std::string
    {
        auto builder = TaskTraceLineBuilder{};

        builder.addString("schema", k_taskTraceSchema);
        builder.addString("kind", taskTraceEventKindName(event.kind));

        if (event.taskName.has_value())
        {
            builder.addString("taskName", *event.taskName);
        }

        if (event.scriptHash.has_value())
        {
            builder.addString("scriptHash", *event.scriptHash);
        }

        if (event.luauVersion.has_value())
        {
            builder.addString("luauVersion", *event.luauVersion);
        }

        if (event.projectId.has_value())
        {
            builder.addString("projectId", *event.projectId);
        }

        if (event.seed.has_value())
        {
            builder.addLiteral("seed", std::format("{}", *event.seed));
        }

        if (event.recognizers.has_value())
        {
            builder.addStringArray("recognizers", sortedCopy(*event.recognizers));
        }

        if (event.pages.has_value())
        {
            builder.addStringArray("pages", sortedCopy(*event.pages));
        }

        if (event.verb.has_value())
        {
            builder.addString("verb", *event.verb);
        }

        if (event.outcome.has_value())
        {
            builder.addString("outcome", hostCallOutcomeName(*event.outcome));
        }

        if (event.errorKind.has_value())
        {
            builder.addString("errorKind", errorKindWireName(*event.errorKind));
        }

        if (event.exitReason.has_value())
        {
            builder.addString("exitReason", taskExitReasonName(*event.exitReason));
        }

        return builder.finish();
    }
}
