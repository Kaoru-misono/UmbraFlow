#pragma once

#include "ports.hpp"
#include "runtime-loader.hpp"
#include "trace.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <annotation/authorization.hpp>
#include <annotation/catalog.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>

#include <domain/detection.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <memory>
#include <optional>
#include <stop_token>

namespace uf::engine
{
    class EngineSession;

    // The read-only configuration a session captures once at construction. It is
    // a transport aggregate: build it with designated initializers. The live
    // fingerprint has no default state, so it must be supplied at every
    // construction site; every other field carries a safe in-class default.
    struct EngineSessionConfig final
    {
        annotation::ProjectFingerprint liveFingerprint;

        uint64                     maximumPixelComparisons{};
        MonotonicInstant::Duration recognitionTimeout{};
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};

        std::stop_token cancellation{};
    };

    // The result of a successful action-target search on one observation: the raw
    // anchor evidence, the authorization-ready detection bound to its recognizer
    // identity, and the single deterministic click pixel derived from the match.
    // None of its members has a default, so it is only ever built by findAction.
    class ActionFound final
    {
        annotation::AnchorEvidence  m_evidence;
        annotation::ActionDetection m_actionDetection;
        PixelPoint                  m_clickPixel;

    public:
        ActionFound(
            annotation::AnchorEvidence evidence,
            annotation::ActionDetection actionDetection,
            PixelPoint clickPixel
        ) noexcept;

        [[nodiscard]]
        auto evidence() const noexcept UF_LIFETIME_BOUND -> annotation::AnchorEvidence const&;

        [[nodiscard]]
        auto actionDetection() const noexcept UF_LIFETIME_BOUND
            -> annotation::ActionDetection const&;

        [[nodiscard]] auto clickPixel() const noexcept -> PixelPoint;
    };

    // A single-use, move-only handle over one captured frame, vended only by
    // EngineSession::observe. It carries the frame, its lease, and its identity,
    // plus a non-owning back-reference to the session that produced it.
    //
    // Lifetime contract: an Observation must never outlive its EngineSession. The
    // session outlives every observation it vends, so the raw back-pointer is a
    // pure optional observation of an object that provably outlives it, never an
    // owner. Every operation checks m_invalidated first and fails
    // StaleObservation before touching the session (D0/D1): consuming the handle
    // by value makes it typed single-use, and the flag fences any surviving alias
    // at runtime. The move operations copy the members into the destination and
    // invalidate the source, so a moved-from handle is dead exactly like a
    // consumed one and fails StaleObservation on any later use.
    class Observation final
    {
        friend class EngineSession;

        Frame                     m_frame;
        ObservationLease          m_lease;
        annotation::FrameIdentity m_frameIdentity;
        EngineSession*            m_session;
        bool                      m_invalidated{false};

        Observation(
            Frame frame,
            ObservationLease lease,
            annotation::FrameIdentity frameIdentity,
            EngineSession* p_session
        ) noexcept;

    public:
        Observation(Observation const&) = delete;
        Observation(Observation&& other) noexcept;
        auto operator=(Observation const&) -> Observation& = delete;
        auto operator=(Observation&& other) noexcept -> Observation&;

        ~Observation() = default;

        [[nodiscard]]
        auto resolvePage() -> Result<annotation::PageOutcome>;

        [[nodiscard]]
        auto findAction(
            annotation::RecognizerId recognizerId
        ) -> Result<std::optional<ActionFound>>;
    };

    // The record of one delivered click: the frame it was authorized against and
    // the client-space point posted to the sink.
    struct ActReceipt final
    {
        FrameId            frameId;
        Point<ClientSpace> clickPoint;
    };

    // The paired product of waitForPage: the observation that resolved the page
    // and the resolved page itself, so the caller never re-evaluates the frame.
    struct PageWait final
    {
        Observation              observation;
        annotation::ResolvedPage page;
    };

    class EngineSession final
    {
        friend class Observation;

        LoadedRuntime                m_loadedRuntime;
        std::unique_ptr<FrameSource> m_frameSource;
        std::unique_ptr<ActionSink>  m_actionSink;
        std::unique_ptr<TraceSink>   m_traceSink;
        EngineSessionConfig          m_config;

        EngineSession(
            LoadedRuntime loadedRuntime,
            std::unique_ptr<FrameSource> frameSource,
            std::unique_ptr<ActionSink> actionSink,
            std::unique_ptr<TraceSink> traceSink,
            EngineSessionConfig config
        ) noexcept;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND
            -> annotation::RecognitionCatalog const&;

        [[nodiscard]]
        auto makeRecognitionPolicy() const -> annotation::RecognitionPolicy;

        [[nodiscard]]
        auto emit(TraceEvent const& event) -> Status;

        [[nodiscard]]
        auto resolvePageFor(Frame const& frame) -> Result<annotation::PageOutcome>;

        [[nodiscard]]
        auto findActionFor(
            Frame const& frame,
            annotation::RecognizerId recognizerId
        ) -> Result<std::optional<ActionFound>>;

        // D6: the minimal known-popup sweep lands in P0-C; P1 replaces this no-op
        // with the bot:on registry. It runs once per waitForPage cycle so the loop
        // shape is already in place when the real sweep arrives.
        void sweepKnownPopups() noexcept;

    public:
        EngineSession(EngineSession const&) = delete;
        EngineSession(EngineSession&&) noexcept = default;
        auto operator=(EngineSession const&) -> EngineSession& = delete;
        auto operator=(EngineSession&&) noexcept -> EngineSession& = delete;

        ~EngineSession() = default;

        [[nodiscard]]
        static auto create(
            LoadedRuntime loadedRuntime,
            std::unique_ptr<FrameSource> frameSource,
            std::unique_ptr<ActionSink> actionSink,
            std::unique_ptr<TraceSink> traceSink,
            EngineSessionConfig config
        ) -> Result<EngineSession>;

        [[nodiscard]]
        auto observe() -> Result<Observation>;

        [[nodiscard]]
        auto act(
            Observation&& observation,
            annotation::ResolvedPage const& page,
            ActionFound const& action
        ) -> Result<ActReceipt>;

        [[nodiscard]]
        auto waitForPage(
            annotation::PageId pageId,
            MonotonicInstant::Duration timeout,
            MonotonicInstant::Duration pollInterval
        ) -> Result<PageWait>;
    };
}
