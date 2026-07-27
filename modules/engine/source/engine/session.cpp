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

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <stop_token>
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
                .kind             = kind,
                .frameId          = identity.frameId(),
                .sessionId        = identity.sessionId(),
                .targetGeneration = identity.targetGeneration(),
            };
        }

        // The longest a single poll sleep blocks before it re-checks cancellation
        // and the deadline. It bounds cancellation latency when the poll interval
        // is large, without changing the effective poll cadence.
        constexpr auto k_maxPollSleepSlice = std::chrono::milliseconds{100};

        // Sleeps for up to `interval`, in slices no longer than k_maxPollSleepSlice,
        // and stops early once cancellation is requested or the deadline has
        // passed. Sleeping is its only effect: the caller re-checks both conditions
        // and decides the outcome, so this only bounds latency.
        void pollSleep(
            MonotonicInstant::Duration interval,
            MonotonicInstant deadline,
            std::stop_token const& cancellation
        )
        {
            using Duration   = MonotonicInstant::Duration;
            auto const slice = std::chrono::duration_cast<Duration>(
                k_maxPollSleepSlice
            );
            auto remaining = interval;
            while (remaining > Duration::zero())
            {
                if (
                    cancellation.stop_requested()
                    || MonotonicInstant::now() >= deadline
                )
                {
                    return;
                }

                auto const step = std::min(remaining, slice);
                std::this_thread::sleep_for(step);
                remaining -= step;
            }
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
        std::unique_ptr<IFrameSource> frameSource,
        std::unique_ptr<IActionSink> actionSink,
        std::unique_ptr<ITraceSink> traceSink,
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
        return m_loadedRuntime.runtime.manifest().catalog();
    }

    auto EngineSession::makeRecognitionPolicy() const -> annotation::RecognitionPolicy
    {
        return annotation::RecognitionPolicy{
            .maximumPixelComparisons = m_config.maximumPixelComparisons,
            .deadline                = MonotonicInstant::now().checkedAdd(m_config.recognitionTimeout),
            .cancellation            = m_config.cancellation,
        };
    }

    auto EngineSession::emit(TraceEvent const& event) -> Status
    {
        return m_traceSink->emit(event);
    }

    auto EngineSession::create(
        LoadedRuntime loadedRuntime,
        std::unique_ptr<IFrameSource> frameSource,
        std::unique_ptr<IActionSink> actionSink,
        std::unique_ptr<ITraceSink> traceSink,
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
        UF_TRY(session.emit(TraceEvent{.kind = TraceEventKind::SessionStarted}));
        return session;
    }

    auto EngineSession::observe() -> Result<Observation>
    {
        // An external stop requested before observation aborts the capture before
        // any frame is taken, so a cancelled run stops promptly at the loop head.
        if (m_config.cancellation.stop_requested())
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "cancelled before observation"
            );
        }

        UF_TRY(m_frameSource->validateTargetInstance());
        UF_TRY_VALUE(frame, m_frameSource->capture());

        UF_TRY_VALUE(
            lease,
            ObservationLease::forFrame(frame, m_config.maxActionFrameAge)
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
        auto attempt = m_loadedRuntime.runtime.evaluatePage(
            frame,
            m_config.liveFingerprint,
            makeRecognitionPolicy()
        );
        if (!attempt)
        {
            auto event      = identityEvent(TraceEventKind::Failure, identity);
            event.errorKind = automationErrorKind(attempt.error());
            event.message   = std::string{attempt.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(attempt).error()};
        }

        if (
            auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                &attempt->result
            )
        )
        {
            auto event = identityEvent(TraceEventKind::RecognitionStopped, identity);
            event.recognizerId = p_stop->recognizerId;
            event.stopReason   = p_stop->reason;
            UF_TRY(emit(event));
            return fail(
                annotation::searchStopKind(p_stop->reason),
                std::format(
                    "page recognition stopped: {}",
                    annotation::searchStopDescription(p_stop->reason)
                )
            );
        }

        auto outcome = std::get<annotation::PageOutcome>(std::move(attempt->result));
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

        auto event   = identityEvent(kind, identity);
        event.pageId = pageId;
        UF_TRY(emit(event));
        return outcome;
    }

    auto EngineSession::findActionFor(
        Frame const& frame,
        annotation::RecognizerId recognizerId
    ) -> Result<std::optional<ActionFound>>
    {
        auto const identity = annotation::FrameIdentity::fromFrame(frame);
        auto attempt = m_loadedRuntime.runtime.evaluateActionTarget(
            frame,
            m_config.liveFingerprint,
            recognizerId,
            makeRecognitionPolicy()
        );
        if (!attempt)
        {
            auto event         = identityEvent(TraceEventKind::Failure, identity);
            event.errorKind    = automationErrorKind(attempt.error());
            event.recognizerId = recognizerId;
            event.message      = std::string{attempt.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(attempt).error()};
        }

        if (
            auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                &attempt->result
            )
        )
        {
            auto event = identityEvent(TraceEventKind::RecognitionStopped, identity);
            event.recognizerId = p_stop->recognizerId;
            event.stopReason   = p_stop->reason;
            UF_TRY(emit(event));
            return fail(
                annotation::searchStopKind(p_stop->reason),
                std::format(
                    "action target search stopped: {}",
                    annotation::searchStopDescription(p_stop->reason)
                )
            );
        }

        auto const& evidence = std::get<annotation::AnchorEvidence>(attempt->result);
        if (!evidence.hit())
        {
            auto event         = identityEvent(TraceEventKind::ActionAbsent, identity);
            event.recognizerId = recognizerId;
            event.sadScore     = evidence.sadScore();
            event.maximumSad   = evidence.maximumSad();
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

        auto event         = identityEvent(TraceEventKind::ActionFound, identity);
        event.recognizerId = recognizerId;
        event.sadScore     = evidence.sadScore();
        event.maximumSad   = evidence.maximumSad();
        event.matchedRect  = *matchedRect;
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
        // An external stop requested before delivery takes precedence over every
        // other outcome: fail closed before authorization and any sink call so a
        // cancelled run never posts input. This reads only session state, not the
        // observation, so it may run ahead of the foreign-observation guard below.
        if (m_config.cancellation.stop_requested())
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "cancelled before delivery"
            );
        }

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
            .liveFingerprint  = m_config.liveFingerprint,
            .sessionId        = identity.sessionId(),
            .targetGeneration = identity.targetGeneration(),
            .frameId          = identity.frameId(),
            .now              = MonotonicInstant::now(),
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
            auto event         = identityEvent(TraceEventKind::ActionRejected, identity);
            event.errorKind    = automationErrorKind(authorized.error());
            event.recognizerId = action.actionDetection().recognizerId();
            event.message      = std::string{authorized.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(authorized).error()};
        }

        UF_TRY(emit(identityEvent(TraceEventKind::ActionAuthorized, identity)));

        UF_TRY_VALUE(framePoint, pixelPointToFramePoint(action.clickPixel()));
        auto const clientPoint = observation.m_frame.transform().frameToClient(framePoint);

        // Revalidate the bound target instance at the delivery edge, immediately
        // before the sink call, to close the HWND-reuse window between observation
        // and delivery. This runs for every adapter, so a target replaced after
        // authorization is rejected here with zero sink calls.
        auto revalidation = m_frameSource->validateTargetInstance();
        if (!revalidation)
        {
            auto event         = identityEvent(TraceEventKind::ActionRejected, identity);
            event.errorKind    = automationErrorKind(revalidation.error());
            event.recognizerId = action.actionDetection().recognizerId();
            event.message      = std::string{revalidation.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(revalidation).error()};
        }

        // Forward the lease so the delivery layer re-runs the D0 injection-layer
        // fence (frameId, targetGeneration, and age) at post time as layer 2.
        UF_TRY(m_actionSink->click(clientPoint, observation.m_lease));

        // D0/D1: the click has landed, so consume the handle before any fallible
        // post-click trace emit. If a ClickDelivered or ObservationInvalidated
        // emit then fails, the error still propagates, but a retry with a
        // surviving alias finds the handle already dead and cannot double-deliver.
        observation.m_invalidated = true;

        auto clickEvent        = identityEvent(TraceEventKind::ClickDelivered, identity);
        clickEvent.clickClient = clientPoint;
        UF_TRY(emit(clickEvent));

        UF_TRY(emit(identityEvent(TraceEventKind::ObservationInvalidated, identity)));

        return ActReceipt{
            .frameId    = identity.frameId(),
            .clickPoint = clientPoint,
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

            if (m_config.cancellation.stop_requested())
            {
                return fail(
                    AutomationErrorKind::Cancelled,
                    "waitForPage cancelled before the target page resolved"
                );
            }

            pollSleep(pollInterval, *deadline, m_config.cancellation);
        }
    }

    void EngineSession::sweepKnownPopups() noexcept
    {
        // D6: intentional no-op until P0-C. See the declaration for the roadmap.
    }
}
