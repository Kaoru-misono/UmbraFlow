#include "trace.hpp"

#include <core/error/contracts.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace uf::engine
{
    namespace
    {
        // Minimal JSON string escaper. trace is a module and must not link the
        // frozen m0-demo escaper, so this is a local copy of the same approach:
        // quote the value and escape the mandatory control and structural bytes.
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
        auto traceEventKindName(TraceEventKind kind) noexcept -> std::string_view
        {
            switch (kind)
            {
            case TraceEventKind::SessionStarted: return "SessionStarted";
            case TraceEventKind::Observed: return "Observed";
            case TraceEventKind::PageResolved: return "PageResolved";
            case TraceEventKind::PageUnknown: return "PageUnknown";
            case TraceEventKind::PageAmbiguous: return "PageAmbiguous";
            case TraceEventKind::ActionFound: return "ActionFound";
            case TraceEventKind::ActionAbsent: return "ActionAbsent";
            case TraceEventKind::ActionAuthorized: return "ActionAuthorized";
            case TraceEventKind::ActionRejected: return "ActionRejected";
            case TraceEventKind::ClickDelivered: return "ClickDelivered";
            case TraceEventKind::ObservationInvalidated: return "ObservationInvalidated";
            case TraceEventKind::RecognitionStopped: return "RecognitionStopped";
            case TraceEventKind::Failure: return "Failure";
            }

            UF_UNREACHABLE_MSG("Unknown TraceEventKind value");
        }

        [[nodiscard]]
        auto stopReasonName(SadSearchStopReason reason) noexcept -> std::string_view
        {
            switch (reason)
            {
            case SadSearchStopReason::Cancelled: return "Cancelled";
            case SadSearchStopReason::TimedOut: return "TimedOut";
            case SadSearchStopReason::ComparisonBudgetExhausted: return "ComparisonBudgetExhausted";
            }

            UF_UNREACHABLE_MSG("Unknown SadSearchStopReason value");
        }

        [[nodiscard]]
        auto errorKindName(AutomationErrorKind kind) -> std::string_view
        {
            auto const name = enumName(kind);
            UF_CHECK_MSG(name.has_value(), "AutomationErrorKind must be reflected");
            return *name;
        }

        [[nodiscard]]
        auto serializePixelRect(PixelRect const& rect) -> std::string
        {
            return std::format(
                "{{\"x\":{},\"y\":{},\"width\":{},\"height\":{}}}",
                rect.x(),
                rect.y(),
                rect.width(),
                rect.height()
            );
        }

        // Accumulates a JSON object, tracking whether the leading comma is due.
        // Mutating its own buffer is the builder's whole purpose, so its methods
        // do not use output parameters.
        class TraceLineBuilder final
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

            [[nodiscard]]
            auto finish() -> std::string
            {
                m_text += '}';
                return std::move(m_text);
            }
        };
    }

    auto serializeTraceEvent(TraceEvent const& event) -> std::string
    {
        auto builder = TraceLineBuilder{};

        builder.addString("schema", k_traceSchema);
        builder.addString("kind", traceEventKindName(event.kind));

        if (event.frameId.has_value())
        {
            builder.addLiteral("frameId", std::format("{}", event.frameId->value()));
        }

        if (event.sessionId.has_value())
        {
            builder.addLiteral("sessionId", std::format("{}", event.sessionId->value()));
        }

        if (event.targetGeneration.has_value())
        {
            builder.addLiteral(
                "targetGeneration",
                std::format("{}", event.targetGeneration->value())
            );
        }

        if (event.pageId.has_value())
        {
            builder.addString("pageId", event.pageId->value().toString());
        }

        if (event.recognizerId.has_value())
        {
            builder.addString("recognizerId", event.recognizerId->value().toString());
        }

        if (event.sadScore.has_value())
        {
            builder.addLiteral("sadScore", std::format("{}", *event.sadScore));
        }

        if (event.maximumSad.has_value())
        {
            builder.addLiteral("maximumSad", std::format("{}", *event.maximumSad));
        }

        if (event.matchedRect.has_value())
        {
            builder.addLiteral("matchedRect", serializePixelRect(*event.matchedRect));
        }

        if (event.stopReason.has_value())
        {
            builder.addString("stopReason", stopReasonName(*event.stopReason));
        }

        if (event.errorKind.has_value())
        {
            builder.addString("errorKind", errorKindName(*event.errorKind));
        }

        if (event.message.has_value())
        {
            builder.addString("message", *event.message);
        }

        if (event.clickClient.has_value())
        {
            builder.addLiteral("clickClientX", std::format("{}", event.clickClient->x()));
            builder.addLiteral("clickClientY", std::format("{}", event.clickClient->y()));
        }

        return builder.finish();
    }
}
