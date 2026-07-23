#pragma once

#include <core/types/integer.hpp>

#include <annotation/catalog.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace uf::engine
{
    // The trace schema is versioned so a downstream consumer can reject a line
    // it does not understand. serializeTraceEvent always emits it first.
    inline constexpr auto g_traceSchema = std::string_view{"engine-trace/v1"};

    enum class TraceEventKind : uint8
    {
        SessionStarted,
        Observed,
        PageResolved,
        PageUnknown,
        PageAmbiguous,
        ActionFound,
        ActionAbsent,
        ActionAuthorized,
        ActionRejected,
        ClickDelivered,
        ObservationInvalidated,
        RecognitionStopped,
        Failure,
    };

    // One flat trace record. Every field beyond the kind is optional so a single
    // struct describes every event in the pipeline; serializeTraceEvent omits an
    // absent field rather than emitting a null. This is a transport aggregate:
    // construct it with designated initializers and set only the fields the event
    // carries.
    struct TraceEvent final
    {
        TraceEventKind m_kind{};

        std::optional<FrameId>          m_frameId{};
        std::optional<SessionId>        m_sessionId{};
        std::optional<TargetGeneration> m_targetGeneration{};

        std::optional<annotation::PageId>       m_pageId{};
        std::optional<annotation::RecognizerId> m_recognizerId{};

        std::optional<uint64>    m_sadScore{};
        std::optional<uint64>    m_maximumSad{};
        std::optional<PixelRect> m_matchedRect{};

        std::optional<SadSearchStopReason> m_stopReason{};
        std::optional<AutomationErrorKind> m_errorKind{};
        std::optional<std::string>         m_message{};
        std::optional<Point<ClientSpace>>  m_clickClient{};
    };

    // Serializes one event to a single-line JSON object. Pure and I/O-free: the
    // field order is fixed and the schema version is emitted first, so the output
    // is a stable golden line. Emitted at the throw-instant by the engine (D4) so
    // a failed emit cannot be swallowed by a later caller.
    [[nodiscard]]
    auto serializeTraceEvent(TraceEvent const& event) -> std::string;
}
