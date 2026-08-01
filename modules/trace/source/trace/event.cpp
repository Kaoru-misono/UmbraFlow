#include "event.hpp"

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::trace
{
    namespace
    {
        // Minimal JSON string escaper: quote the value and escape the mandatory
        // control and structural bytes. trace is the only module that writes this
        // schema, so this is now the single copy -- engine/trace.cpp and
        // task/trace.cpp each carried one before the two streams merged.
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
        // The dotted spelling names the layer that authored the event.
        [[nodiscard]]
        auto traceEventKindName(TraceEventKind kind) noexcept -> std::string_view
        {
            switch (kind)
            {
            case TraceEventKind::RunStarted: return "run.started";
            case TraceEventKind::RunResourcesValidated: return "run.resources_validated";
            case TraceEventKind::RunFinished: return "run.finished";
            case TraceEventKind::EngineObserved: return "engine.observed";
            case TraceEventKind::EnginePageResolved: return "engine.page_resolved";
            case TraceEventKind::EngineActionFound: return "engine.action_found";
            case TraceEventKind::EngineTextRead: return "engine.text_read";
            case TraceEventKind::EngineActionAuthorized: return "engine.action_authorized";
            case TraceEventKind::EngineActionRejected: return "engine.action_rejected";
            case TraceEventKind::EngineActionDelivered: return "engine.action_delivered";
            case TraceEventKind::EngineKeyDelivered: return "engine.key_delivered";
            case TraceEventKind::EngineObservationInvalidated:
                return "engine.observation_invalidated";
            case TraceEventKind::TaskNativeCall: return "task.native_call";
            case TraceEventKind::FrameworkStepStarted: return "framework.step_started";
            case TraceEventKind::FrameworkStepFinished: return "framework.step_finished";
            case TraceEventKind::FrameworkRetryAttempt: return "framework.retry_attempt";
            case TraceEventKind::FrameworkRetryBackoff: return "framework.retry_backoff";
            case TraceEventKind::FrameworkInterruptMatched:
                return "framework.interrupt_matched";
            case TraceEventKind::FrameworkInterruptHandled:
                return "framework.interrupt_handled";
            case TraceEventKind::FrameworkInterruptExhausted:
                return "framework.interrupt_exhausted";
            case TraceEventKind::FrameworkSettled: return "framework.settled";
            case TraceEventKind::AnnotationClickDelivered:
                return "annotation.click_delivered";
            case TraceEventKind::AnnotationRegionSaved:
                return "annotation.region_saved";
            }

            UF_UNREACHABLE_MSG("Unknown TraceEventKind value");
        }

        [[nodiscard]]
        auto pageResolutionName(PageResolution outcome) noexcept -> std::string_view
        {
            switch (outcome)
            {
            case PageResolution::Resolved: return "Resolved";
            case PageResolution::Unknown: return "Unknown";
            case PageResolution::Ambiguous: return "Ambiguous";
            case PageResolution::Stopped: return "Stopped";
            case PageResolution::Failed: return "Failed";
            }

            UF_UNREACHABLE_MSG("Unknown PageResolution value");
        }

        [[nodiscard]]
        auto actionSearchName(ActionSearch outcome) noexcept -> std::string_view
        {
            switch (outcome)
            {
            case ActionSearch::Found: return "Found";
            case ActionSearch::Absent: return "Absent";
            case ActionSearch::Stopped: return "Stopped";
            case ActionSearch::Failed: return "Failed";
            }

            UF_UNREACHABLE_MSG("Unknown ActionSearch value");
        }

        [[nodiscard]]
        auto nativeCallOutcomeName(NativeCallOutcome outcome) noexcept -> std::string_view
        {
            switch (outcome)
            {
            case NativeCallOutcome::Succeeded: return "Succeeded";
            case NativeCallOutcome::Empty: return "Empty";
            case NativeCallOutcome::Failed: return "Failed";
            }

            UF_UNREACHABLE_MSG("Unknown NativeCallOutcome value");
        }

        [[nodiscard]]
        auto runOutcomeName(RunOutcome outcome) noexcept -> std::string_view
        {
            switch (outcome)
            {
            case RunOutcome::Completed: return "Completed";
            case RunOutcome::Failed: return "Failed";
            case RunOutcome::Cancelled: return "Cancelled";
            }

            UF_UNREACHABLE_MSG("Unknown RunOutcome value");
        }

        [[nodiscard]]
        auto stopReasonName(SadSearchStopReason reason) noexcept -> std::string_view
        {
            switch (reason)
            {
            case SadSearchStopReason::Cancelled: return "Cancelled";
            case SadSearchStopReason::TimedOut: return "TimedOut";
            case SadSearchStopReason::ComparisonBudgetExhausted:
                return "ComparisonBudgetExhausted";
            }

            UF_UNREACHABLE_MSG("Unknown SadSearchStopReason value");
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

            auto separate() -> void
            {
                if (m_needsComma)
                {
                    m_text += ',';
                }

                m_needsComma = true;
            }

        public:
            auto addLiteral(
                std::string_view name,
                std::string_view literalValue
            ) -> void
            {
                separate();
                m_text += escapeJsonString(name);
                m_text += ':';
                m_text += literalValue;
            }

            auto addString(
                std::string_view name,
                std::string_view value
            ) -> void
            {
                addLiteral(name, escapeJsonString(value));
            }

            // Emits a JSON array of the already-sorted `values`. Every element is
            // escaped, so a name that ever carries a quote or control byte stays
            // a valid line.
            auto addStringArray(
                std::string_view name,
                std::span<std::string const> values
            ) -> void
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
        // container's iteration order can never leak into the trace.
        [[nodiscard]]
        auto sortedCopy(std::vector<std::string> const& names) -> std::vector<std::string>
        {
            auto copy = names;
            std::ranges::sort(copy);
            return copy;
        }

        // The group writers below take the builder by mutable reference because
        // appending to the caller's half-built object is their whole operation,
        // exactly as for the builder's own methods; nothing is returned through
        // the parameter.
        auto addFrame(
            TraceLineBuilder& builder,
            FrameIdentity const& frame
        ) -> void
        {
            builder.addLiteral("frameId", std::format("{}", frame.frameId().value()));
            builder.addLiteral("sessionId", std::format("{}", frame.sessionId().value()));
            builder.addLiteral(
                "targetGeneration",
                std::format("{}", frame.targetGeneration().value())
            );
        }

        // The per-page evidence array of engine.page_resolved, built as one JSON
        // literal because the builder writes flat members and this is the schema's
        // only nested list of objects. The order is the resolver's own page order,
        // which is the catalog's, so it is stable across runs.
        [[nodiscard]]
        auto serializePageScores(
            std::span<TraceEvent::Page::Score const> scores
        ) -> std::string
        {
            auto text  = std::string{"["};
            auto first = true;
            for (auto const& score : scores)
            {
                if (!first)
                {
                    text += ',';
                }
                first = false;

                text += "{\"pageId\":";
                text += escapeJsonString(score.pageId.value().toString());
                text += score.candidate ? ",\"candidate\":true" : ",\"candidate\":false";
                if (score.worstAnchor.has_value())
                {
                    text += ",\"worstAnchor\":";
                    text += escapeJsonString(score.worstAnchor->value().toString());
                }
                if (score.worstAnchorSad.has_value())
                {
                    text += std::format(",\"worstAnchorSad\":{}", *score.worstAnchorSad);
                }
                if (score.worstAnchorMaximumSad.has_value())
                {
                    text += std::format(
                        ",\"worstAnchorMaximumSad\":{}",
                        *score.worstAnchorMaximumSad
                    );
                }
                text += '}';
            }

            text += ']';
            return text;
        }

        auto addPage(
            TraceLineBuilder& builder,
            TraceEvent::Page const& page
        ) -> void
        {
            builder.addString("pageOutcome", pageResolutionName(page.outcome));
            if (page.pageId.has_value())
            {
                builder.addString("pageId", page.pageId->value().toString());
            }
            if (!page.scores.empty())
            {
                builder.addLiteral("pageScores", serializePageScores(page.scores));
            }
        }

        auto addReading(
            TraceLineBuilder& builder,
            TraceEvent::Reading const& reading
        ) -> void
        {
            builder.addString("text", reading.text);
            builder.addLiteral("readRect", serializePixelRect(reading.rect));
            builder.addLiteral(
                "confidenceBp",
                std::format("{}", reading.confidenceBp)
            );
            builder.addString("ocrEngine", reading.engineId);
            builder.addLiteral(
                "readMicros",
                std::format("{}", reading.durationMicros)
            );
        }

        auto addAction(
            TraceLineBuilder& builder,
            TraceEvent::Action const& action
        ) -> void
        {
            builder.addString("actionOutcome", actionSearchName(action.outcome));
            if (action.sadScore.has_value())
            {
                builder.addLiteral("sadScore", std::format("{}", *action.sadScore));
            }
            if (action.maximumSad.has_value())
            {
                builder.addLiteral("maximumSad", std::format("{}", *action.maximumSad));
            }
            if (action.matchedRect.has_value())
            {
                builder.addLiteral("matchedRect", serializePixelRect(*action.matchedRect));
            }
        }

        auto addRun(
            TraceLineBuilder& builder,
            TraceEvent::Run const& run
        ) -> void
        {
            builder.addString("projectId", run.projectId);
            builder.addString("taskName", run.taskName);
            builder.addString("sourceHash", run.sourceHash);
            builder.addString("frameworkVersion", run.frameworkVersion);
            builder.addString("frameworkHash", run.frameworkHash);
            builder.addString("luauVersion", run.luauVersion);
            builder.addLiteral("seed", std::format("{}", run.seed));
        }

        // One past-the-end index pair naming a half-open range of `line`.
        struct MemberSpan final
        {
            std::size_t begin{};
            std::size_t end{};
        };

        // Advances past the JSON string starting at `line[start]`, which must be
        // its opening quote, and returns the index just past its closing quote.
        // Returns nullopt when the string is unterminated.
        [[nodiscard]]
        auto skipString(
            std::string_view line,
            std::size_t start
        ) noexcept -> std::optional<std::size_t>
        {
            auto index = start + 1U;
            while (index < line.size())
            {
                if (line[index] == '\\')
                {
                    index += 2U;
                    continue;
                }
                if (line[index] == '"')
                {
                    return index + 1U;
                }
                ++index;
            }
            return std::nullopt;
        }

        // Advances past one JSON value starting at `line[start]` and returns the
        // index just past it. Only the shapes this schema emits occur -- strings,
        // numbers, arrays and objects -- so the scan tracks nesting depth and
        // string state and stops at the first top-level ',' or '}'.
        [[nodiscard]]
        auto skipValue(
            std::string_view line,
            std::size_t start
        ) noexcept -> std::optional<std::size_t>
        {
            auto depth = 0U;
            auto index = start;
            while (index < line.size())
            {
                auto const character = line[index];
                if (character == '"')
                {
                    auto const next = skipString(line, index);
                    if (!next)
                    {
                        return std::nullopt;
                    }
                    index = *next;
                    continue;
                }
                if (character == '{' || character == '[')
                {
                    ++depth;
                }
                else if (character == '}' || character == ']')
                {
                    if (depth == 0U)
                    {
                        return index;
                    }
                    --depth;
                }
                else if (character == ',' && depth == 0U)
                {
                    return index;
                }
                ++index;
            }
            return std::nullopt;
        }

        // Locates the top-level member named `name` in the serialized object
        // `line`, returning the half-open range covering its key, colon and
        // value. Returns nullopt when the line is not an object or has no such
        // member, so a caller can leave the line untouched.
        [[nodiscard]]
        auto findTopLevelMember(
            std::string_view line,
            std::string_view name
        ) noexcept -> std::optional<MemberSpan>
        {
            if (line.size() < 2U || line.front() != '{')
            {
                return std::nullopt;
            }

            auto index = std::size_t{1};
            while (index < line.size() && line[index] != '}')
            {
                if (line[index] != '"')
                {
                    return std::nullopt;
                }

                auto const keyEnd = skipString(line, index);
                if (!keyEnd || *keyEnd >= line.size() || line[*keyEnd] != ':')
                {
                    return std::nullopt;
                }

                auto const valueEnd = skipValue(line, *keyEnd + 1U);
                if (!valueEnd)
                {
                    return std::nullopt;
                }

                // The key is stored escaped; every name this schema emits is
                // plain ASCII, so comparing the quoted spelling is exact.
                auto const key = line.substr(index, *keyEnd - index);
                if (key == escapeJsonString(name))
                {
                    return MemberSpan{.begin = index, .end = *valueEnd};
                }

                index = *valueEnd;
                if (index < line.size() && line[index] == ',')
                {
                    ++index;
                }
            }

            return std::nullopt;
        }
    }

    // Lower case like every other schema-owned enumerator spelling that names a
    // layer rather than an outcome, and independent of the enumerator name for
    // the reason above the kind table.
    auto frontEndWireName(FrontEnd frontEnd) noexcept -> std::string_view
    {
        switch (frontEnd)
        {
        case FrontEnd::Task: return "task";
        case FrontEnd::Operator: return "operator";
        case FrontEnd::Annotation: return "annotation";
        }

        UF_UNREACHABLE_MSG("Unknown FrontEnd value");
    }

    StampedTraceEvent::StampedTraceEvent(
        TraceEvent event,
        std::vector<std::string> openSteps,
        uint64 sequence,
        TaskRunId runId,
        GenerationId generationId,
        FrontEnd frontEnd,
        int64 wallClockUnixMillis
    )
        : m_event{std::move(event)}
        , m_openSteps{std::move(openSteps)}
        , m_sequence{sequence}
        , m_runId{runId}
        , m_generationId{generationId}
        , m_frontEnd{frontEnd}
        , m_wallClockUnixMillis{wallClockUnixMillis}
    {
    }

    auto StampedTraceEvent::event() const noexcept -> TraceEvent const& { return m_event; }

    auto StampedTraceEvent::openSteps() const noexcept -> std::span<std::string const>
    {
        return m_openSteps;
    }

    auto StampedTraceEvent::sequence() const noexcept -> uint64 { return m_sequence; }

    auto StampedTraceEvent::runId() const noexcept -> TaskRunId { return m_runId; }

    auto StampedTraceEvent::generationId() const noexcept -> GenerationId
    {
        return m_generationId;
    }

    auto StampedTraceEvent::frontEnd() const noexcept -> FrontEnd
    {
        return m_frontEnd;
    }

    auto StampedTraceEvent::wallClockUnixMillis() const noexcept -> int64
    {
        return m_wallClockUnixMillis;
    }

    auto serializeTraceEvent(StampedTraceEvent const& stamped) -> std::string
    {
        auto const& event = stamped.event();
        auto        builder = TraceLineBuilder{};

        builder.addString("schema", k_traceSchema);
        builder.addString("kind", traceEventKindName(event.kind));
        builder.addLiteral("seq", std::format("{}", stamped.sequence()));
        builder.addLiteral("runId", std::format("{}", stamped.runId().value()));
        builder.addLiteral(
            "generationId",
            std::format("{}", stamped.generationId().value())
        );
        builder.addString("frontEnd", frontEndWireName(stamped.frontEnd()));

        // Part of the stamp, so it sits with the identity triple rather than
        // among the event's own fields. Omitted when no step is open, which is
        // every line of a task that never called ctx:step.
        if (!stamped.openSteps().empty())
        {
            builder.addStringArray("steps", stamped.openSteps());
        }

        if (event.frame.has_value())
        {
            addFrame(builder, *event.frame);
        }

        if (event.page.has_value())
        {
            addPage(builder, *event.page);
        }

        if (event.action.has_value())
        {
            addAction(builder, *event.action);
        }

        if (event.reading.has_value())
        {
            addReading(builder, *event.reading);
        }

        if (event.run.has_value())
        {
            addRun(builder, *event.run);
        }

        if (event.resources.has_value())
        {
            builder.addStringArray(
                "elements",
                sortedCopy(event.resources->elements)
            );
            builder.addStringArray("pages", sortedCopy(event.resources->pages));
        }

        if (event.nativeCall.has_value())
        {
            builder.addString("verb", event.nativeCall->verb);
            if (event.nativeCall->cycleOrdinal.has_value())
            {
                builder.addLiteral(
                    "cycleOrdinal",
                    std::format("{}", *event.nativeCall->cycleOrdinal)
                );
            }
            if (event.nativeCall->hitCycleOrdinal.has_value())
            {
                builder.addLiteral(
                    "hitCycleOrdinal",
                    std::format("{}", *event.nativeCall->hitCycleOrdinal)
                );
            }
            if (event.nativeCall->durationMillis.has_value())
            {
                builder.addLiteral(
                    "durationMillis",
                    std::format("{}", *event.nativeCall->durationMillis)
                );
            }
            if (event.nativeCall->resourceName.has_value())
            {
                builder.addString("resourceName", *event.nativeCall->resourceName);
            }
            if (event.nativeCall->byteCount.has_value())
            {
                builder.addLiteral(
                    "byteCount",
                    std::format("{}", *event.nativeCall->byteCount)
                );
            }
            if (event.nativeCall->contentHash.has_value())
            {
                builder.addString("contentHash", *event.nativeCall->contentHash);
            }
            builder.addString("outcome", nativeCallOutcomeName(event.nativeCall->outcome));
        }

        if (event.framework.has_value())
        {
            if (!event.framework->label.empty())
            {
                builder.addString("label", event.framework->label);
            }
            if (event.framework->attempt.has_value())
            {
                builder.addLiteral("attempt", std::format("{}", *event.framework->attempt));
            }
            if (event.framework->attempts.has_value())
            {
                builder.addLiteral(
                    "attempts",
                    std::format("{}", *event.framework->attempts)
                );
            }
            if (event.framework->durationMillis.has_value())
            {
                builder.addLiteral(
                    "durationMillis",
                    std::format("{}", *event.framework->durationMillis)
                );
            }
        }

        if (event.annotation.has_value())
        {
            if (event.annotation->point.has_value())
            {
                builder.addLiteral(
                    "pointX",
                    std::format("{}", event.annotation->point->x())
                );
                builder.addLiteral(
                    "pointY",
                    std::format("{}", event.annotation->point->y())
                );
            }
            if (event.annotation->rect.has_value())
            {
                builder.addLiteral(
                    "regionRect",
                    serializePixelRect(*event.annotation->rect)
                );
            }
            if (event.annotation->byteCount.has_value())
            {
                builder.addLiteral(
                    "byteCount",
                    std::format("{}", *event.annotation->byteCount)
                );
            }
            if (event.annotation->contentHash.has_value())
            {
                builder.addString("contentHash", *event.annotation->contentHash);
            }
        }

        if (event.elementId.has_value())
        {
            builder.addString("elementId", event.elementId->value().toString());
        }

        if (event.templateHash.has_value())
        {
            builder.addString("templateHash", *event.templateHash);
        }

        if (event.stopReason.has_value())
        {
            builder.addString("stopReason", stopReasonName(*event.stopReason));
        }

        if (event.runOutcome.has_value())
        {
            builder.addString("runOutcome", runOutcomeName(*event.runOutcome));
        }

        if (event.errorKind.has_value())
        {
            // The domain's single wire spelling, which is also the `kind` a
            // script's Tier B error carries and the uf.errors constant it
            // compares against. That shared spelling -- not engine-trace/v1's
            // reflected PascalCase -- is why a trace line names a failure with
            // exactly the string the script saw.
            builder.addString("errorKind", automationErrorWireName(*event.errorKind));
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

        if (event.key.has_value())
        {
            builder.addString("key", event.key->value());
        }

        // The non-golden member goes last, so a reader scanning a line meets the
        // reproducible record first and the wall clock only at the end.
        builder.addLiteral(
            k_nonGoldenMember,
            std::format("{{\"wallClock\":{}}}", stamped.wallClockUnixMillis())
        );

        return builder.finish();
    }

    auto stripNonGoldenFields(std::string_view line) -> std::string
    {
        auto const span = findTopLevelMember(line, k_nonGoldenMember);
        if (!span)
        {
            return std::string{line};
        }

        // Drop the separating comma with the member: the one before it when the
        // member is not first, otherwise the one after it.
        auto begin = span->begin;
        auto end   = span->end;
        if (begin > 1U && line[begin - 1U] == ',')
        {
            --begin;
        }
        else if (end < line.size() && line[end] == ',')
        {
            ++end;
        }

        auto stripped = std::string{line.substr(0, begin)};
        stripped += line.substr(end);
        return stripped;
    }
}
