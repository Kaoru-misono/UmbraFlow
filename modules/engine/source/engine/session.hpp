#pragma once

#include "ports.hpp"
#include "runtime-loader.hpp"

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

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>

namespace uf::engine
{
    namespace detail
    {
        class EngineSessionIdentity;
    }

    class EngineSession;

    // The longest one capture may block before observe() gives up on it. It is a
    // conservative placeholder awaiting the first real daily and a real-machine
    // soak: the WGC adapter's own stall fuse is a second, and a compositor that
    // has produced nothing for twice that is not going to produce a frame this
    // cycle either. Calibrate it here -- it is the only place the bound is
    // spelled -- once measured capture latencies exist.
    inline constexpr auto k_defaultCaptureTimeout = MonotonicInstant::Duration{
        std::chrono::seconds{2}
    };

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
        MonotonicInstant::Duration captureTimeout{k_defaultCaptureTimeout};

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
    // EngineSession::observe. It carries the frame, its lease, its frame
    // identity, and a shared immutable token identifying the session that
    // produced it.
    //
    // The token owns no session state and is never dereferenced. It follows a
    // moved EngineSession and lets every operation reject a handle vended by a
    // different session without retaining a borrow into the session object.
    // Consuming the handle by value makes it typed single-use, and the invalidated
    // flag fences any surviving alias at runtime. The move operations copy the
    // members into the destination and invalidate the source, so a moved-from
    // handle is dead exactly like a consumed one and fails StaleObservation on
    // any later use.
    class Observation final
    {
        friend class EngineSession;

        Frame                                                m_frame;
        ObservationLease                                     m_lease;
        annotation::FrameIdentity                            m_frameIdentity;
        std::shared_ptr<detail::EngineSessionIdentity const> m_sessionIdentity;
        bool                                                 m_invalidated{false};

        Observation(
            Frame frame,
            ObservationLease lease,
            annotation::FrameIdentity frameIdentity,
            std::shared_ptr<detail::EngineSessionIdentity const> sessionIdentity
        ) noexcept;

    public:
        Observation(Observation const&) = delete;
        Observation(Observation&& other) noexcept;
        auto operator=(Observation const&) -> Observation& = delete;
        auto operator=(Observation&& other) noexcept -> Observation&;

        ~Observation() = default;

    };

    // The record of one delivered click: the frame it was authorized against and
    // the client-space point posted to the sink.
    struct ActReceipt final
    {
        FrameId            frameId;
        Point<ClientSpace> clickPoint;
    };

    // The recognition and action pipeline over one bound capture target.
    //
    // Trace lifetime contract: the session does NOT own its trace sink. It stores
    // a non-owning borrow of the run's trace::TraceRecorder, which is owned by
    // `task::TaskHost::startTask`: that function holds the recorder in a
    // std::unique_ptr local declared before the session it builds, so the session
    // is destroyed first on the normal path and on every early return, and the
    // recorder is non-movable so its address cannot drift while the session
    // borrows it. Any other owner MUST reproduce both properties -- construct the
    // recorder before the session, destroy it after, and never relocate it. The
    // borrow exists because the run has exactly one evidence stream and every
    // layer stamps its events through the same sequence counter, which a
    // per-session owned sink could not provide.
    class EngineSession final
    {
        friend class Observation;

        LoadedRuntime                                        m_loadedRuntime;
        std::shared_ptr<detail::EngineSessionIdentity const> m_identity;
        std::unique_ptr<IFrameSource>                        m_frameSource;
        std::unique_ptr<IActionSink>                         m_actionSink;
        trace::TraceRecorder&                                m_recorder;
        EngineSessionConfig                                  m_config;

        EngineSession(
            LoadedRuntime loadedRuntime,
            std::shared_ptr<detail::EngineSessionIdentity const> identity,
            std::unique_ptr<IFrameSource> frameSource,
            std::unique_ptr<IActionSink> actionSink,
            trace::TraceRecorder& recorder,
            EngineSessionConfig config
        ) noexcept;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND
            -> annotation::RecognitionCatalog const&;

        [[nodiscard]]
        auto makeRecognitionPolicy() const -> annotation::RecognitionPolicy;

        [[nodiscard]]
        auto emit(trace::TraceEvent const& event) -> Status;

    public:
        EngineSession(EngineSession const&) = delete;
        EngineSession(EngineSession&&) noexcept = default;
        auto operator=(EngineSession const&) -> EngineSession& = delete;
        auto operator=(EngineSession&&) noexcept -> EngineSession& = delete;

        ~EngineSession() = default;

        [[nodiscard]]
        static auto create(
            LoadedRuntime loadedRuntime,
            std::unique_ptr<IFrameSource> frameSource,
            std::unique_ptr<IActionSink> actionSink,
            trace::TraceRecorder& recorder,
            EngineSessionConfig config
        ) -> Result<EngineSession>;

        [[nodiscard]]
        auto observe() -> Result<Observation>;

        [[nodiscard]]
        auto resolvePage(
            Observation const& observation
        ) -> Result<annotation::PageOutcome>;

        [[nodiscard]]
        auto findAction(
            Observation const& observation,
            annotation::RecognizerId recognizerId
        ) -> Result<std::optional<ActionFound>>;

        [[nodiscard]]
        auto act(
            Observation&& observation,
            annotation::ResolvedPage const& page,
            ActionFound const& action
        ) -> Result<ActReceipt>;
    };
}
