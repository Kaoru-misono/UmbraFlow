#include "session.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <annotation/authorization.hpp>
#include <annotation/catalog.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>

#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <format>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace uf::engine
{
    namespace
    {
        // Prefills a trace event with the frame identity every downstream event
        // shares, so each emit site sets only the fields unique to its kind.
        [[nodiscard]]
        auto identityEvent(
            TraceEventKind kind,
            annotation::FrameIdentity identity
        ) -> TraceEvent
        {
            return TraceEvent{
                .m_kind             = kind,
                .m_frameId          = identity.frameId(),
                .m_sessionId        = identity.sessionId(),
                .m_targetGeneration = identity.targetGeneration(),
            };
        }
    }

    ActionFound::ActionFound(
        annotation::AnchorEvidence evidence,
        annotation::ActionDetection actionDetection,
        PixelPoint clickPixel
    ) noexcept
        : m_evidence{std::move(evidence)}
        , m_actionDetection{std::move(actionDetection)}
        , m_clickPixel{clickPixel}
    {
    }

    auto ActionFound::evidence() const noexcept -> annotation::AnchorEvidence const&
    {
        return m_evidence;
    }

    auto ActionFound::actionDetection() const noexcept -> annotation::ActionDetection const&
    {
        return m_actionDetection;
    }

    auto ActionFound::clickPixel() const noexcept -> PixelPoint { return m_clickPixel; }

    Observation::Observation(
        Frame frame,
        ObservationLease lease,
        annotation::FrameIdentity frameIdentity,
        EngineSession* p_session
    ) noexcept
        : m_frame{std::move(frame)}
        , m_lease{lease}
        , m_frameIdentity{frameIdentity}
        , m_session{p_session}
    {
    }

    Observation::Observation(Observation&& other) noexcept
        : m_frame{other.m_frame}
        , m_lease{other.m_lease}
        , m_frameIdentity{other.m_frameIdentity}
        , m_session{other.m_session}
        , m_invalidated{other.m_invalidated}
    {
        // D0/D1: a moved-from handle must be as dead as a consumed one, so the
        // source fails StaleObservation on any later use.
        other.m_invalidated = true;
    }

    auto Observation::operator=(Observation&& other) noexcept -> Observation&
    {
        if (this == &other)
        {
            return *this;
        }

        m_frame         = other.m_frame;
        m_lease         = other.m_lease;
        m_frameIdentity = other.m_frameIdentity;
        m_session       = other.m_session;
        m_invalidated   = other.m_invalidated;

        // D0/D1: invalidate the source so the moved-from handle fails closed.
        other.m_invalidated = true;
        return *this;
    }

    auto Observation::resolvePage() -> Result<annotation::PageOutcome>
    {
        if (m_invalidated)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "resolvePage called on an invalidated observation"
            );
        }

        return m_session->resolvePageFor(m_frame);
    }

    auto Observation::findAction(
        annotation::RecognizerId recognizerId
    ) -> Result<std::optional<ActionFound>>
    {
        if (m_invalidated)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "findAction called on an invalidated observation"
            );
        }

        return m_session->findActionFor(m_frame, recognizerId);
    }

    EngineSession::EngineSession(
        LoadedRuntime loadedRuntime,
        std::unique_ptr<FrameSource> frameSource,
        std::unique_ptr<ActionSink> actionSink,
        std::unique_ptr<TraceSink> traceSink,
        EngineSessionConfig config
    ) noexcept
        : m_loadedRuntime{std::move(loadedRuntime)}
        , m_frameSource{std::move(frameSource)}
        , m_actionSink{std::move(actionSink)}
        , m_traceSink{std::move(traceSink)}
        , m_config{std::move(config)}
    {
    }

    auto EngineSession::catalog() const noexcept -> annotation::RecognitionCatalog const&
    {
        return m_loadedRuntime.m_runtime.manifest().catalog();
    }

    auto EngineSession::makeRecognitionPolicy() const -> annotation::RecognitionPolicy
    {
        return annotation::RecognitionPolicy{
            .m_maximumPixelComparisons = m_config.m_maximumPixelComparisons,
            .m_deadline                = MonotonicInstant::now().checkedAdd(m_config.m_recognitionTimeout),
            .m_cancellation            = m_config.m_cancellation,
        };
    }

    auto EngineSession::emit(TraceEvent const& event) -> Status
    {
        return m_traceSink->emit(event);
    }

    auto EngineSession::create(
        LoadedRuntime loadedRuntime,
        std::unique_ptr<FrameSource> frameSource,
        std::unique_ptr<ActionSink> actionSink,
        std::unique_ptr<TraceSink> traceSink,
        EngineSessionConfig config
    ) -> Result<EngineSession>
    {
        if (
            frameSource == nullptr
            || actionSink == nullptr
            || traceSink == nullptr
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "engine session requires a frame source, action sink, and trace sink"
            );
        }

        auto session = EngineSession{
            std::move(loadedRuntime),
            std::move(frameSource),
            std::move(actionSink),
            std::move(traceSink),
            std::move(config),
        };
        UF_TRY(session.emit(TraceEvent{.m_kind = TraceEventKind::SessionStarted}));
        return session;
    }

    auto EngineSession::observe() -> Result<Observation>
    {
        UF_TRY(m_frameSource->validateTargetInstance());
        UF_TRY_VALUE(frame, m_frameSource->capture());

        UF_TRY_VALUE(
            lease,
            ObservationLease::forFrame(frame, m_config.m_maxActionFrameAge)
        );
        auto const identity = annotation::FrameIdentity::fromFrame(frame);

        UF_TRY(emit(identityEvent(TraceEventKind::Observed, identity)));

        return Observation{std::move(frame), lease, identity, this};
    }

    auto EngineSession::resolvePageFor(
        Frame const& frame
    ) -> Result<annotation::PageOutcome>
    {
        auto const identity = annotation::FrameIdentity::fromFrame(frame);
        auto attempt = m_loadedRuntime.m_runtime.evaluatePage(
            frame,
            m_config.m_liveFingerprint,
            makeRecognitionPolicy()
        );
        if (!attempt)
        {
            auto event        = identityEvent(TraceEventKind::Failure, identity);
            event.m_errorKind = automationErrorKind(attempt.error());
            event.m_message   = std::string{attempt.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(attempt).error()};
        }

        if (
            auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                &attempt->m_result
            )
        )
        {
            auto event = identityEvent(TraceEventKind::RecognitionStopped, identity);
            event.m_recognizerId = p_stop->m_recognizerId;
            event.m_stopReason   = p_stop->m_reason;
            UF_TRY(emit(event));
            return fail(
                annotation::searchStopKind(p_stop->m_reason),
                std::format(
                    "page recognition stopped: {}",
                    annotation::searchStopDescription(p_stop->m_reason)
                )
            );
        }

        auto outcome = std::get<annotation::PageOutcome>(std::move(attempt->m_result));
        auto kind   = TraceEventKind::PageUnknown;
        auto pageId = std::optional<annotation::PageId>{};
        if (auto const* p_resolved = std::get_if<annotation::ResolvedPage>(&outcome))
        {
            kind   = TraceEventKind::PageResolved;
            pageId = p_resolved->pageId();
        }
        else if (std::holds_alternative<annotation::AmbiguousPages>(outcome))
        {
            kind = TraceEventKind::PageAmbiguous;
        }

        auto event     = identityEvent(kind, identity);
        event.m_pageId = pageId;
        UF_TRY(emit(event));
        return outcome;
    }

    auto EngineSession::findActionFor(
        Frame const& frame,
        annotation::RecognizerId recognizerId
    ) -> Result<std::optional<ActionFound>>
    {
        auto const identity = annotation::FrameIdentity::fromFrame(frame);
        auto attempt = m_loadedRuntime.m_runtime.evaluateActionTarget(
            frame,
            m_config.m_liveFingerprint,
            recognizerId,
            makeRecognitionPolicy()
        );
        if (!attempt)
        {
            auto event           = identityEvent(TraceEventKind::Failure, identity);
            event.m_errorKind    = automationErrorKind(attempt.error());
            event.m_recognizerId = recognizerId;
            event.m_message      = std::string{attempt.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(attempt).error()};
        }

        if (
            auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                &attempt->m_result
            )
        )
        {
            auto event = identityEvent(TraceEventKind::RecognitionStopped, identity);
            event.m_recognizerId = p_stop->m_recognizerId;
            event.m_stopReason   = p_stop->m_reason;
            UF_TRY(emit(event));
            return fail(
                annotation::searchStopKind(p_stop->m_reason),
                std::format(
                    "action target search stopped: {}",
                    annotation::searchStopDescription(p_stop->m_reason)
                )
            );
        }

        auto const& evidence = std::get<annotation::AnchorEvidence>(attempt->m_result);
        if (!evidence.hit())
        {
            auto event           = identityEvent(TraceEventKind::ActionAbsent, identity);
            event.m_recognizerId = recognizerId;
            event.m_sadScore     = evidence.sadScore();
            event.m_maximumSad   = evidence.maximumSad();
            UF_TRY(emit(event));
            return std::optional<ActionFound>{std::nullopt};
        }

        auto const* p_recognizer = catalog().findRecognizer(recognizerId);
        // evaluateActionTarget has already proven the recognizer is a catalog
        // action target, so its definition is necessarily present here.
        UF_CHECK(p_recognizer != nullptr);
        auto const matchedRect = evidence.matchedRect();
        UF_CHECK(matchedRect.has_value());

        UF_TRY_VALUE(
            clickPixel,
            annotation::resolveClickPixel(*p_recognizer, *matchedRect)
        );
        UF_TRY_VALUE(frameRect, pixelRectToFrameRect(*matchedRect));
        UF_TRY_VALUE(label, Label::create(std::string{p_recognizer->name().value()}));

        auto detection = Detection{
            frame.sessionId(),
            frame.targetGeneration(),
            frame.id(),
            std::move(label),
            frameRect,
            evidence.displayConfidence().value_or(0.0F),
        };
        UF_TRY_VALUE(
            actionDetection,
            annotation::ActionDetection::create(
                catalog(),
                recognizerId,
                std::move(detection)
            )
        );

        auto event           = identityEvent(TraceEventKind::ActionFound, identity);
        event.m_recognizerId = recognizerId;
        event.m_sadScore     = evidence.sadScore();
        event.m_maximumSad   = evidence.maximumSad();
        event.m_matchedRect  = *matchedRect;
        UF_TRY(emit(event));

        return std::optional<ActionFound>{
            ActionFound{evidence, std::move(actionDetection), clickPixel}
        };
    }

    auto EngineSession::act(
        Observation&& observation,
        annotation::ResolvedPage const& page,
        ActionFound const& action
    ) -> Result<ActReceipt>
    {
        // D0: an observation carries a back-reference to the session that vended
        // it. Acting on a handle from another session is a programming error, not
        // a recoverable runtime condition, so reject it as a broken invariant
        // before any other check touches the foreign observation.
        if (observation.m_session != this)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "observation belongs to a different session"
            );
        }

        if (observation.m_invalidated)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "act called on an invalidated observation"
            );
        }

        auto const identity = observation.m_frameIdentity;
        auto const delivery = annotation::ActionDeliveryState{
            .m_liveFingerprint  = m_config.m_liveFingerprint,
            .m_sessionId        = identity.sessionId(),
            .m_targetGeneration = identity.targetGeneration(),
            .m_frameId          = identity.frameId(),
            .m_now              = MonotonicInstant::now(),
        };
        auto authorized = annotation::authorizeCoordinateAction(
            catalog(),
            page,
            action.actionDetection(),
            observation.m_lease,
            delivery
        );
        if (!authorized)
        {
            auto event           = identityEvent(TraceEventKind::ActionRejected, identity);
            event.m_errorKind    = automationErrorKind(authorized.error());
            event.m_recognizerId = action.actionDetection().recognizerId();
            event.m_message      = std::string{authorized.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(authorized).error()};
        }

        UF_TRY(emit(identityEvent(TraceEventKind::ActionAuthorized, identity)));

        UF_TRY_VALUE(framePoint, pixelPointToFramePoint(action.clickPixel()));
        auto const clientPoint = observation.m_frame.transform().frameToClient(framePoint);

        // Forward the lease so the delivery layer re-runs the D0 injection-layer
        // fence (frameId, targetGeneration, and age) at post time as layer 2.
        UF_TRY(m_actionSink->click(clientPoint, observation.m_lease));

        // D0/D1: the click has landed, so consume the handle before any fallible
        // post-click trace emit. If a ClickDelivered or ObservationInvalidated
        // emit then fails, the error still propagates, but a retry with a
        // surviving alias finds the handle already dead and cannot double-deliver.
        observation.m_invalidated = true;

        auto clickEvent          = identityEvent(TraceEventKind::ClickDelivered, identity);
        clickEvent.m_clickClient = clientPoint;
        UF_TRY(emit(clickEvent));

        UF_TRY(emit(identityEvent(TraceEventKind::ObservationInvalidated, identity)));

        return ActReceipt{
            .m_frameId    = identity.frameId(),
            .m_clickPoint = clientPoint,
        };
    }

    auto EngineSession::waitForPage(
        annotation::PageId pageId,
        MonotonicInstant::Duration timeout,
        MonotonicInstant::Duration pollInterval
    ) -> Result<PageWait>
    {
        auto const deadline = MonotonicInstant::now().checkedAdd(timeout);
        if (!deadline)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "waitForPage timeout overflows the monotonic clock"
            );
        }

        for (;;)
        {
            sweepKnownPopups();

            UF_TRY_VALUE(observation, observe());
            UF_TRY_VALUE(outcome, observation.resolvePage());

            auto const* p_resolved = std::get_if<annotation::ResolvedPage>(&outcome);
            if (p_resolved != nullptr && p_resolved->pageId() == pageId)
            {
                return PageWait{
                    std::move(observation),
                    std::get<annotation::ResolvedPage>(std::move(outcome)),
                };
            }

            if (MonotonicInstant::now() >= *deadline)
            {
                return fail(
                    AutomationErrorKind::Timeout,
                    "waitForPage timed out before the target page resolved"
                );
            }

            if (m_config.m_cancellation.stop_requested())
            {
                return fail(
                    AutomationErrorKind::Cancelled,
                    "waitForPage cancelled before the target page resolved"
                );
            }

            std::this_thread::sleep_for(pollInterval);
        }
    }

    void EngineSession::sweepKnownPopups() noexcept
    {
        // D6: intentional no-op until P0-C. See the declaration for the roadmap.
    }
}
