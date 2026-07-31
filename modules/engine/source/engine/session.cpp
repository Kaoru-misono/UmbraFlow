#include "session.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
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
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <ocr/engine.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <vision/bgra-image.hpp>
#include <vision/sad.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uf::engine::detail
{
    class EngineSessionIdentity final
    {
    };
}

namespace uf::engine
{
    namespace
    {
        // Prefills a trace event with the frame identity every downstream event
        // shares, so each emit site sets only the fields unique to its kind. The
        // frame group is the join key that ties an engine event to the capture it
        // came from.
        [[nodiscard]]
        auto identityEvent(
            trace::TraceEventKind kind,
            annotation::FrameIdentity identity
        ) -> trace::TraceEvent
        {
            return trace::TraceEvent{
                .kind  = kind,
                .frame = identity,
            };
        }

        // Hands `continuation` the frame's pixels as a BGRA8 view, widening a
        // Gray8 frame into a buffer that lives on this stack frame for the whole
        // synchronous call. It mirrors annotation's withGrayFrame, and for the
        // same reason: the view must not outlive the storage behind it, and a
        // continuation is the only shape that says so structurally.
        //
        // A Gray8 frame is widened rather than refused because the recognition
        // path already accepts one, and an OCR read that worked on a colour
        // capture but not on a grey one would be a difference nothing in the
        // model explains.
        template <typename Continuation>
        [[nodiscard]]
        auto withBgraFrame(
            Frame const& frame,
            Continuation const& continuation
        ) -> std::invoke_result_t<Continuation const&, BgraImage const&>
        {
            auto const p_pixels = frame.pixels();
            UF_CHECK(p_pixels != nullptr);
            switch (frame.pixelFormat())
            {
            case PixelFormat::Bgra8:
            {
                UF_TRY_VALUE(
                    image,
                    BgraImage::create(
                        p_pixels->bytes(),
                        frame.width(),
                        frame.height(),
                        frame.stride()
                    )
                );
                return std::invoke(continuation, image);
            }
            case PixelFormat::Gray8:
            {
                auto const source     = p_pixels->bytes();
                auto const widthSize  = checkedCast<std::size_t>(frame.width());
                auto const heightSize = checkedCast<std::size_t>(frame.height());
                UF_CHECK(widthSize.has_value() && heightSize.has_value());
                auto const rowBytes = checkedMultiply(*widthSize, std::size_t{4});
                UF_CHECK(rowBytes.has_value());
                auto const total = checkedMultiply(*rowBytes, *heightSize);
                UF_CHECK(total.has_value());

                auto widened = std::vector<std::byte>(*total, std::byte{0});
                for (auto row = std::size_t{0}; row < *heightSize; ++row)
                {
                    auto const sourceRow = checkedMultiply(row, frame.stride());
                    UF_CHECK(sourceRow.has_value());
                    for (auto column = std::size_t{0}; column < *widthSize; ++column)
                    {
                        auto const* p_level = tryAt(source, *sourceRow + column);
                        if (p_level == nullptr)
                        {
                            return fail(
                                AutomationErrorKind::InvalidResource,
                                "the frame's Gray8 buffer is shorter than its "
                                "own geometry"
                            );
                        }
                        auto const base = (row * *rowBytes) + (column * 4U);
                        checkedAt(widened, base)      = *p_level;
                        checkedAt(widened, base + 1U) = *p_level;
                        checkedAt(widened, base + 2U) = *p_level;
                        checkedAt(widened, base + 3U) = std::byte{255};
                    }
                }

                UF_TRY_VALUE(
                    image,
                    BgraImage::create(
                        widened,
                        frame.width(),
                        frame.height(),
                        *rowBytes
                    )
                );
                return std::invoke(continuation, image);
            }
            }

            UF_UNREACHABLE_MSG("Unknown PixelFormat value");
        }

        // The required anchor of `evaluation` that scored worst against its own
        // ceiling, which is the one a non-resolution turns on. An anchor whose
        // search produced no comparable position at all carries no score and is
        // worse than any scored one, so it wins immediately. Returns nullopt when
        // the page declares no required anchor.
        [[nodiscard]]
        auto worstRequiredAnchor(
            annotation::PageEvaluation const& evaluation
        ) noexcept -> std::optional<annotation::AnchorEvidence>
        {
            auto worst = std::optional<annotation::AnchorEvidence>{};
            for (auto const& anchor : evaluation.required())
            {
                if (!worst)
                {
                    worst = anchor;
                    continue;
                }
                if (!worst->sadScore().has_value())
                {
                    break;
                }
                if (
                    !anchor.sadScore().has_value()
                    || *anchor.sadScore() > *worst->sadScore()
                )
                {
                    worst = anchor;
                }
            }
            return worst;
        }

        // The per-page evidence behind one completed page-resolution attempt, in
        // the resolver's own page order. It answers the operator's first question
        // on a non-resolution -- which page was close and by how much -- which the
        // outcome name alone cannot.
        [[nodiscard]]
        auto pageScores(
            annotation::PageResolutionEvidence const& evidence
        ) -> std::vector<trace::TraceEvent::Page::Score>
        {
            auto scores = std::vector<trace::TraceEvent::Page::Score>{};
            scores.reserve(evidence.pages().size());
            for (auto const& evaluation : evidence.pages())
            {
                auto score = trace::TraceEvent::Page::Score{
                    .pageId    = evaluation.pageId(),
                    .candidate = evaluation.candidate(),
                };
                if (auto const worst = worstRequiredAnchor(evaluation))
                {
                    score.worstAnchor           = worst->elementId();
                    score.worstAnchorSad        = worst->sadScore();
                    score.worstAnchorMaximumSad = worst->maximumSad();
                }
                scores.emplace_back(std::move(score));
            }
            return scores;
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
        std::shared_ptr<detail::EngineSessionIdentity const> sessionIdentity
    ) noexcept
        : m_frame{std::move(frame)}
        , m_lease{lease}
        , m_frameIdentity{frameIdentity}
        , m_sessionIdentity{std::move(sessionIdentity)}
    {
    }

    Observation::Observation(Observation&& other) noexcept
        : m_frame{other.m_frame}
        , m_lease{other.m_lease}
        , m_frameIdentity{other.m_frameIdentity}
        , m_sessionIdentity{other.m_sessionIdentity}
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

        m_frame           = other.m_frame;
        m_lease           = other.m_lease;
        m_frameIdentity   = other.m_frameIdentity;
        m_sessionIdentity = other.m_sessionIdentity;
        m_invalidated     = other.m_invalidated;

        // D0/D1: invalidate the source so the moved-from handle fails closed.
        other.m_invalidated = true;
        return *this;
    }

    EngineSession::EngineSession(
        LoadedRuntime loadedRuntime,
        std::shared_ptr<detail::EngineSessionIdentity const> identity,
        std::unique_ptr<IFrameSource> frameSource,
        std::unique_ptr<IActionSink> actionSink,
        std::unique_ptr<ocr::IOcrEngine> ocrEngine,
        trace::TraceRecorder& recorder,
        EngineSessionConfig config
    ) noexcept
        : m_loadedRuntime{std::move(loadedRuntime)}
        , m_identity{std::move(identity)}
        , m_frameSource{std::move(frameSource)}
        , m_actionSink{std::move(actionSink)}
        , m_ocrEngine{std::move(ocrEngine)}
        , m_recorder{recorder}
        , m_config{std::move(config)}
    {
    }

    auto EngineSession::ensureUsable(
        Observation const& observation,
        std::string_view verb
    ) const -> Status
    {
        if (observation.m_sessionIdentity != m_identity)
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
                std::format("{} called on an invalidated observation", verb)
            );
        }
        return ok();
    }

    auto EngineSession::readTextOnFrame(
        Frame const& frame,
        PixelRect rect
    ) const -> Result<ReadAttempt>
    {
        if (m_ocrEngine == nullptr)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "this engine session was built without an OCR adapter, so it "
                "cannot read text"
            );
        }

        auto const area = checkedMultiply(
            static_cast<uint64>(rect.width()),
            static_cast<uint64>(rect.height())
        );
        if (!area || *area == 0U || *area > k_maximumReadPixels)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "a read region of {}x{} is empty or beyond the host's "
                    "single-line read ceiling of {} pixels",
                    rect.width(),
                    rect.height(),
                    k_maximumReadPixels
                )
            );
        }

        auto const started = MonotonicInstant::now();
        auto       readout = withBgraFrame(
            frame,
            [this, rect](BgraImage const& image) -> Result<ocr::Readout>
            {
                // SingleLine and never Block. The adapter this project ships
                // refuses Block because it runs no detector, and the refusal is
                // the honest answer: the caller asserted the region holds one
                // line, so asking for detection here would be working around a
                // contract rather than honouring it.
                return m_ocrEngine->read(
                    image,
                    ocr::ReadSpec{
                        .rect   = rect,
                        .layout = ocr::TextLayout::SingleLine,
                    }
                );
            }
        );
        auto const elapsed = MonotonicInstant::now().saturatingDurationSince(started);
        auto const micros  = std::chrono::duration_cast<std::chrono::microseconds>(
            elapsed
        ).count();
        if (!readout)
        {
            return std::unexpected{std::move(readout).error()};
        }

        auto attempt = ReadAttempt{
            .engineId       = std::string{m_ocrEngine->identity()},
            .durationMicros = static_cast<uint64>(std::max(micros, int64{0})),
        };
        if (!readout->lines.empty())
        {
            // The first line, and under SingleLine there is at most one: the
            // ordering is a contract, so "the first" is the same line on every
            // run over the same pixels.
            auto& line   = readout->lines.front();
            attempt.line = TextReading{
                .text         = std::move(line.text),
                .rect         = rect,
                .confidenceBp = line.confidenceBp,
            };
        }
        return attempt;
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

    auto EngineSession::emit(trace::TraceEvent const& event) -> Status
    {
        return m_recorder.emit(event);
    }

    // The former engine-trace/v1 SessionStarted event has no successor here. It
    // carried no fields at all, and the run's own `run.started` -- written by
    // task::TaskHost with the project, task, source hash, framework version and
    // hash, seed, run id and generation id -- records strictly more about the same
    // instant, since a run binds exactly one engine session. Every session a
    // product run builds now comes from TaskHost, so there is no path left that
    // opens a trace on the first engine.observed.
    auto EngineSession::create(
        LoadedRuntime loadedRuntime,
        std::unique_ptr<IFrameSource> frameSource,
        std::unique_ptr<IActionSink> actionSink,
        trace::TraceRecorder& recorder,
        EngineSessionConfig config,
        std::unique_ptr<ocr::IOcrEngine> ocrEngine
    ) -> Result<EngineSession>
    {
        if (frameSource == nullptr || actionSink == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "engine session requires a frame source and an action sink"
            );
        }

        return EngineSession{
            std::move(loadedRuntime),
            std::make_shared<detail::EngineSessionIdentity>(),
            std::move(frameSource),
            std::move(actionSink),
            std::move(ocrEngine),
            recorder,
            std::move(config),
        };
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

        // Every capture is bounded. The deadline is minted here, from the one
        // configured capture timeout, so no adapter decides for itself how long
        // an observation may block, and the session's own cancel source travels
        // with it so a stop requested while a frame pool is empty ends the wait
        // instead of being noticed one whole capture later. A timeout that
        // overflows the monotonic clock is a configuration error rather than a
        // licence to wait forever, so it fails closed here.
        auto const captureDeadline = MonotonicInstant::now().checkedAdd(
            m_config.captureTimeout
        );
        if (!captureDeadline)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "capture timeout overflows the monotonic clock"
            );
        }
        UF_TRY_VALUE(
            frame,
            m_frameSource->capture(
                IFrameSource::CaptureBudget{
                    .deadline     = *captureDeadline,
                    .cancellation = m_config.cancellation,
                }
            )
        );

        UF_TRY_VALUE(
            lease,
            ObservationLease::forFrame(frame, m_config.maxActionFrameAge)
        );
        auto const identity = annotation::FrameIdentity::fromFrame(frame);

        UF_TRY(emit(identityEvent(trace::TraceEventKind::EngineObserved, identity)));

        return Observation{std::move(frame), lease, identity, m_identity};
    }

    auto EngineSession::resolvePage(
        Observation const& observation
    ) -> Result<annotation::PageOutcome>
    {
        UF_TRY(ensureUsable(observation, "resolvePage"));

        auto const& frame    = observation.m_frame;
        auto const identity = annotation::FrameIdentity::fromFrame(frame);
        auto attempt = m_loadedRuntime.runtime.evaluatePage(
            frame,
            m_config.liveFingerprint,
            makeRecognitionPolicy()
        );
        // Every exit of a page-resolution attempt writes one engine.page_resolved
        // event whose outcome names how it ended, so a reader never has to infer
        // the stage a stop or failure came from.
        if (!attempt)
        {
            auto event      = identityEvent(trace::TraceEventKind::EnginePageResolved, identity);
            event.page      = trace::TraceEvent::Page{
                .outcome = trace::PageResolution::Failed,
            };
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
            auto event         = identityEvent(trace::TraceEventKind::EnginePageResolved, identity);
            event.page         = trace::TraceEvent::Page{
                .outcome = trace::PageResolution::Stopped,
            };
            event.elementId = p_stop->elementId;
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

        auto outcome    = std::get<annotation::PageOutcome>(std::move(attempt->result));
        auto resolution = trace::PageResolution::Unknown;
        auto pageId     = std::optional<annotation::PageId>{};
        if (auto const* p_resolved = std::get_if<annotation::ResolvedPage>(&outcome))
        {
            resolution = trace::PageResolution::Resolved;
            pageId     = p_resolved->pageId();
        }
        else if (std::holds_alternative<annotation::AmbiguousPages>(outcome))
        {
            resolution = trace::PageResolution::Ambiguous;
        }

        // Every alternative of a completed attempt carries the same evidence, so
        // the scores reach the line whether the page resolved, stayed unknown, or
        // came out ambiguous -- the two non-resolutions being exactly the cases an
        // operator has to explain.
        auto const& evidence = std::visit(
            [](auto const& alternative) noexcept
                -> annotation::PageResolutionEvidence const&
            {
                return alternative.evidence();
            },
            outcome
        );

        auto event = identityEvent(trace::TraceEventKind::EnginePageResolved, identity);
        event.page = trace::TraceEvent::Page{
            .outcome = resolution,
            .pageId  = pageId,
            .scores  = pageScores(evidence),
        };
        UF_TRY(emit(event));
        return outcome;
    }

    auto EngineSession::findAction(
        Observation const& observation,
        annotation::PageId pageId,
        annotation::ElementId elementId
    ) -> Result<std::optional<ActionFound>>
    {
        UF_TRY(ensureUsable(observation, "findAction"));

        auto const& frame    = observation.m_frame;
        auto const identity = annotation::FrameIdentity::fromFrame(frame);
        auto attempt = m_loadedRuntime.runtime.evaluateActionTarget(
            frame,
            m_config.liveFingerprint,
            pageId,
            elementId,
            makeRecognitionPolicy()
        );
        // As with resolvePage, every exit writes one engine.action_found event
        // whose outcome names how the search ended.
        if (!attempt)
        {
            auto event         = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
            event.action       = trace::TraceEvent::Action{
                .outcome = trace::ActionSearch::Failed,
            };
            event.elementId = elementId;
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
            auto event         = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
            event.action       = trace::TraceEvent::Action{
                .outcome = trace::ActionSearch::Stopped,
            };
            event.elementId = p_stop->elementId;
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
            auto event         = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
            event.action       = trace::TraceEvent::Action{
                .outcome    = trace::ActionSearch::Absent,
                .sadScore   = evidence.sadScore(),
                .maximumSad = evidence.maximumSad(),
            };
            event.elementId = elementId;
            UF_TRY(emit(event));
            return std::optional<ActionFound>{std::nullopt};
        }

        auto const* p_element = catalog().findElement(elementId);
        // evaluateActionTarget has already proven that this page references the
        // element and exercises interact on it, so its definition is
        // necessarily present here.
        UF_CHECK(p_element != nullptr);
        auto const matchedRect = evidence.matchedRect();
        UF_CHECK(matchedRect.has_value());

        UF_TRY_VALUE(
            clickPixel,
            annotation::resolveClickPixel(*p_element, *matchedRect)
        );
        UF_TRY_VALUE(frameRect, pixelRectToFrameRect(*matchedRect));
        UF_TRY_VALUE(label, Label::create(std::string{p_element->name().value()}));

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
                elementId,
                std::move(detection)
            )
        );

        auto event         = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
        event.action       = trace::TraceEvent::Action{
            .outcome     = trace::ActionSearch::Found,
            .sadScore    = evidence.sadScore(),
            .maximumSad  = evidence.maximumSad(),
            .matchedRect = *matchedRect,
        };
        event.elementId = elementId;
        UF_TRY(emit(event));

        return std::optional<ActionFound>{
            ActionFound{evidence, std::move(actionDetection), clickPixel}
        };
    }

    auto EngineSession::matchTemplate(
        Observation const& observation,
        annotation::GrayTemplateImage const& templateImage,
        PixelRect searchRoi
    ) -> Result<std::optional<MatchFound>>
    {
        UF_TRY(ensureUsable(observation, "matchTemplate"));

        auto const& frame    = observation.m_frame;
        auto const  identity = annotation::FrameIdentity::fromFrame(frame);
        auto const  hashText = templateImage.hash.toString();

        // The compatibility gate a catalog-driven search gets from inside
        // evaluateActionTarget. A raw match reaches no catalog entry point, so it
        // asks for itself rather than being the one search that runs on a frame
        // the project may not be compared against.
        auto compatible = m_loadedRuntime.runtime.ensureCompatibleFrame(
            frame,
            m_config.liveFingerprint
        );
        auto attempt = compatible
            ? annotation::matchTemplateOnFrame(
                  frame,
                  templateImage,
                  searchRoi,
                  makeRecognitionPolicy()
              )
            : Result<annotation::TemplateMatchAttempt>{
                  std::unexpected{std::move(compatible).error()}
              };

        // Every exit writes one engine.action_found whose outcome names how the
        // search ended, exactly as findAction does; the template hash stands in
        // for the element id a raw template does not have.
        if (!attempt)
        {
            auto event   = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
            event.action = trace::TraceEvent::Action{
                .outcome = trace::ActionSearch::Failed,
            };
            event.templateHash = hashText;
            event.errorKind    = automationErrorKind(attempt.error());
            event.message      = std::string{attempt.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(attempt).error()};
        }

        if (
            auto const* p_stop = std::get_if<SadSearchStopReason>(&attempt->result)
        )
        {
            auto event   = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
            event.action = trace::TraceEvent::Action{
                .outcome = trace::ActionSearch::Stopped,
            };
            event.templateHash = hashText;
            event.stopReason   = *p_stop;
            UF_TRY(emit(event));
            return fail(
                annotation::searchStopKind(*p_stop),
                std::format(
                    "template match stopped: {}",
                    annotation::searchStopDescription(*p_stop)
                )
            );
        }

        auto const& match = std::get<std::optional<annotation::TemplateMatch>>(
            attempt->result
        );
        if (!match)
        {
            auto event   = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
            event.action = trace::TraceEvent::Action{
                .outcome = trace::ActionSearch::Absent,
            };
            event.templateHash = hashText;
            UF_TRY(emit(event));
            return std::optional<MatchFound>{std::nullopt};
        }

        // Integer division truncates, so the centre is one reproducible pixel for
        // both even and odd extents. There is no click offset to add: an offset
        // is authored on a catalog element, and this template has none.
        auto const centerX = match->matchedRect.x() + match->matchedRect.width() / 2U;
        auto const centerY = match->matchedRect.y() + match->matchedRect.height() / 2U;

        auto event   = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
        event.action = trace::TraceEvent::Action{
            .outcome     = trace::ActionSearch::Found,
            .sadScore    = match->sadScore,
            .maximumSad  = match->maximumSad,
            .matchedRect = match->matchedRect,
        };
        event.templateHash = hashText;
        UF_TRY(emit(event));

        return std::optional<MatchFound>{
            MatchFound{
                .matchedRect = match->matchedRect,
                .clickPixel  = PixelPoint{centerX, centerY},
                .sadScore    = match->sadScore,
                .maximumSad  = match->maximumSad,
            }
        };
    }

    auto EngineSession::readText(
        Observation const& observation,
        PixelRect rect
    ) -> Result<std::optional<TextReading>>
    {
        UF_TRY(ensureUsable(observation, "readText"));

        auto const& frame    = observation.m_frame;
        auto const  identity = annotation::FrameIdentity::fromFrame(frame);
        auto        reading  = readTextOnFrame(frame, rect);
        if (!reading)
        {
            auto event      = identityEvent(trace::TraceEventKind::EngineTextRead, identity);
            event.errorKind = automationErrorKind(reading.error());
            event.message   = std::string{reading.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(reading).error()};
        }

        auto event     = identityEvent(trace::TraceEventKind::EngineTextRead, identity);
        event.reading  = trace::TraceEvent::Reading{
            .text           = reading->line ? reading->line->text : std::string{},
            .rect           = rect,
            .confidenceBp   = reading->line ? reading->line->confidenceBp : uint32{0},
            .engineId       = std::string{reading->engineId},
            .durationMicros = reading->durationMicros,
        };
        UF_TRY(emit(event));

        if (!reading->line)
        {
            return std::optional<TextReading>{std::nullopt};
        }
        return std::optional<TextReading>{*std::move(reading->line)};
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

        // D0: an observation carries the stable identity token of the session
        // that vended it. Acting on a handle from another session is a programming
        // error, so reject it before any other check touches the foreign handle.
        // A consumed or moved-from handle is dead on the same terms.
        UF_TRY(ensureUsable(observation, "act"));

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
            auto event      = identityEvent(trace::TraceEventKind::EngineActionRejected, identity);
            event.elementId = action.actionDetection().elementId();
            event.errorKind = automationErrorKind(authorized.error());
            event.message   = std::string{authorized.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(authorized).error()};
        }

        UF_TRY(emit(identityEvent(trace::TraceEventKind::EngineActionAuthorized, identity)));

        UF_TRY_VALUE(framePoint, pixelPointToFramePoint(action.clickPixel()));
        auto const clientPoint = observation.m_frame.transform().frameToClient(framePoint);

        // Revalidate the bound target instance at the delivery edge, immediately
        // before the sink call, to close the HWND-reuse window between observation
        // and delivery. This runs for every adapter, so a target replaced after
        // authorization is rejected here with zero sink calls.
        auto revalidation = m_frameSource->validateTargetInstance();
        if (!revalidation)
        {
            auto event      = identityEvent(trace::TraceEventKind::EngineActionRejected, identity);
            event.elementId = action.actionDetection().elementId();
            event.errorKind = automationErrorKind(revalidation.error());
            event.message   = std::string{revalidation.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(revalidation).error()};
        }

        // Forward the lease so the delivery layer re-runs the D0 injection-layer
        // fence (frameId, targetGeneration, and age) at post time as layer 2.
        UF_TRY(m_actionSink->click(clientPoint, observation.m_lease));

        // D0/D1: the click has landed, so consume the handle before any fallible
        // post-click trace emit. If an engine.action_delivered or
        // engine.observation_invalidated emit then fails, the error still
        // propagates, but a retry with a surviving alias finds the handle already
        // dead and cannot double-deliver.
        observation.m_invalidated = true;

        auto clickEvent = identityEvent(
            trace::TraceEventKind::EngineActionDelivered,
            identity
        );
        clickEvent.clickClient = clientPoint;
        UF_TRY(emit(clickEvent));

        UF_TRY(
            emit(
                identityEvent(
                    trace::TraceEventKind::EngineObservationInvalidated,
                    identity
                )
            )
        );

        return ActReceipt{
            .frameId    = identity.frameId(),
            .clickPoint = clientPoint,
        };
    }

    auto EngineSession::clickPoint(
        Observation&& observation,
        PixelPoint point
    ) -> Result<ActReceipt>
    {
        // The same three fail-closed gates act() opens with, in the same order
        // and for the same reasons.
        if (m_config.cancellation.stop_requested())
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "cancelled before delivery"
            );
        }
        UF_TRY(ensureUsable(observation, "clickPoint"));

        auto const identity = observation.m_frameIdentity;

        // What survives of the coordinate authorization once the element and the
        // page have moved up a layer: the target must still be the geometry this
        // project was authored against, and the frame the coordinate was derived
        // from must still be within its lease. Both are refused here, with zero
        // sink calls, exactly as authorizeCoordinateAction refuses them.
        if (m_config.liveFingerprint != catalog().fingerprint())
        {
            auto event      = identityEvent(trace::TraceEventKind::EngineActionRejected, identity);
            event.errorKind = AutomationErrorKind::TargetCompatibilityUnverified;
            event.message   = std::string{
                "live size or DPI does not match the annotation project fingerprint"
            };
            UF_TRY(emit(event));
            return fail(
                AutomationErrorKind::TargetCompatibilityUnverified,
                "live size or DPI does not match the annotation project fingerprint"
            );
        }

        auto lease = observation.m_lease.validate(
            identity.sessionId(),
            identity.targetGeneration(),
            identity.frameId(),
            MonotonicInstant::now()
        );
        if (!lease)
        {
            auto event      = identityEvent(trace::TraceEventKind::EngineActionRejected, identity);
            event.errorKind = automationErrorKind(lease.error());
            event.message   = std::string{lease.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(lease).error()};
        }

        UF_TRY(emit(identityEvent(trace::TraceEventKind::EngineActionAuthorized, identity)));

        UF_TRY_VALUE(framePoint, pixelPointToFramePoint(point));
        auto const clientPoint = observation.m_frame.transform().frameToClient(framePoint);

        auto revalidation = m_frameSource->validateTargetInstance();
        if (!revalidation)
        {
            auto event      = identityEvent(trace::TraceEventKind::EngineActionRejected, identity);
            event.errorKind = automationErrorKind(revalidation.error());
            event.message   = std::string{revalidation.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(revalidation).error()};
        }

        UF_TRY(m_actionSink->click(clientPoint, observation.m_lease));

        // D0/D1: the click has landed, so the handle dies before any fallible
        // post-click emit, exactly as in act().
        observation.m_invalidated = true;

        auto clickEvent = identityEvent(
            trace::TraceEventKind::EngineActionDelivered,
            identity
        );
        clickEvent.clickClient = clientPoint;
        UF_TRY(emit(clickEvent));

        UF_TRY(
            emit(
                identityEvent(
                    trace::TraceEventKind::EngineObservationInvalidated,
                    identity
                )
            )
        );

        return ActReceipt{
            .frameId    = identity.frameId(),
            .clickPoint = clientPoint,
        };
    }

    auto EngineSession::pressKey(
        Observation&& observation,
        KeyName key
    ) -> Result<KeyReceipt>
    {
        // The same three fail-closed gates act() opens with, in the same order and
        // for the same reasons: a requested stop outranks every other outcome, a
        // foreign handle is rejected before anything else touches it, and a
        // consumed or moved-from handle is dead.
        if (m_config.cancellation.stop_requested())
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "cancelled before key delivery"
            );
        }
        UF_TRY(ensureUsable(observation, "pressKey"));

        auto const identity = observation.m_frameIdentity;

        // Revalidate the bound target instance immediately before the post, exactly
        // as act() does. This is the whole of what replaces the coordinate
        // authorization: the keystroke must reach the target instance the
        // observation came from, and a window replaced since then is refused here
        // with zero sink calls.
        auto revalidation = m_frameSource->validateTargetInstance();
        if (!revalidation)
        {
            auto event      = identityEvent(
                trace::TraceEventKind::EngineActionRejected,
                identity
            );
            event.key       = key;
            event.errorKind = automationErrorKind(revalidation.error());
            event.message   = std::string{revalidation.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(revalidation).error()};
        }

        auto delivered = m_actionSink->pressKey(key, identity.targetGeneration());
        if (!delivered)
        {
            auto event      = identityEvent(
                trace::TraceEventKind::EngineActionRejected,
                identity
            );
            event.key       = key;
            event.errorKind = automationErrorKind(delivered.error());
            event.message   = std::string{delivered.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(delivered).error()};
        }

        // The keystroke has landed, so the handle dies before any fallible
        // post-delivery emit -- the same ordering act() uses, and for the same
        // reason: a retry over a surviving alias must find the handle already dead
        // rather than deliver twice.
        observation.m_invalidated = true;

        auto keyEvent = identityEvent(
            trace::TraceEventKind::EngineKeyDelivered,
            identity
        );
        keyEvent.key = key;
        UF_TRY(emit(keyEvent));

        UF_TRY(
            emit(
                identityEvent(
                    trace::TraceEventKind::EngineObservationInvalidated,
                    identity
                )
            )
        );

        return KeyReceipt{
            .frameId = identity.frameId(),
            .key     = key,
        };
    }
}
