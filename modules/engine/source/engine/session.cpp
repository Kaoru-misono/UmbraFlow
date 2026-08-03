#include "session.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <core/utility/variant-match.hpp>

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
#include <vision/template-match.hpp>

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
        // Prefills the frame identity every engine event shares -- the join key
        // tying an event to its capture -- so each emit site sets only its own
        // fields.
        [[nodiscard]]
        auto identityEvent(
            trace::TraceEventKind kind,
            FrameIdentity identity
        ) -> trace::TraceEvent
        {
            return trace::TraceEvent{
                .kind  = kind,
                .frame = identity,
            };
        }

        // Hands `continuation` the frame's pixels as a BGRA8 view, widening a
        // Gray8 frame into a buffer this stack frame owns for the whole
        // synchronous call. A continuation, as in vision's withGrayFrame,
        // because the view must not outlive that storage.
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
    }

    Observation::Observation(
        Frame frame,
        ObservationLease lease,
        FrameIdentity frameIdentity,
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
        // A moved-from handle must be as dead as a consumed one; see the header.
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

        other.m_invalidated = true;
        return *this;
    }

    EngineSession::EngineSession(
        std::shared_ptr<detail::EngineSessionIdentity const> identity,
        std::unique_ptr<IFrameSource> frameSource,
        std::unique_ptr<IActionSink> actionSink,
        std::unique_ptr<ocr::IOcrEngine> ocrEngine,
        trace::TraceRecorder& recorder,
        EngineSessionConfig config
    ) noexcept
        : m_identity{std::move(identity)}
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

    auto EngineSession::runRead(
        Frame const& frame,
        ocr::ReadSpec const& spec,
        uint64 ceilingPixels,
        std::string_view ceilingName
    ) const -> Result<TimedReadout>
    {
        if (m_ocrEngine == nullptr)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "this engine session was built without an OCR adapter, so it "
                "cannot read text"
            );
        }

        // Both reads name the region they charge against a ceiling; a
        // whole-image spec has no area for one to bound.
        UF_CHECK(spec.rect.has_value());
        auto const area = checkedMultiply(
            static_cast<uint64>(spec.rect->width()),
            static_cast<uint64>(spec.rect->height())
        );
        if (!area || *area == 0U || *area > ceilingPixels)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "a read region of {}x{} is empty or beyond the host's {} "
                    "ceiling of {} pixels",
                    spec.rect->width(),
                    spec.rect->height(),
                    ceilingName,
                    ceilingPixels
                )
            );
        }

        auto const started = MonotonicInstant::now();
        auto       readout = withBgraFrame(
            frame,
            [this, &spec](BgraImage const& image) -> Result<ocr::Readout>
            {
                return m_ocrEngine->read(image, spec);
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

        return TimedReadout{
            .readout        = *std::move(readout),
            .engineId       = std::string{m_ocrEngine->identity()},
            .durationMicros = static_cast<uint64>(std::max(micros, int64{0})),
        };
    }

    auto EngineSession::readTextOnFrame(
        Frame const& frame,
        PixelRect rect
    ) const -> Result<ReadAttempt>
    {
        // SingleLine and never Block: the caller asserted the region holds one
        // line, so running detection here would work around that contract
        // rather than honour it.
        UF_TRY_VALUE(
            timed,
            runRead(
                frame,
                ocr::ReadSpec{
                    .rect   = rect,
                    .layout = ocr::TextLayout::SingleLine,
                },
                k_maximumReadPixels,
                "single-line read"
            )
        );

        auto attempt = ReadAttempt{
            .engineId       = std::move(timed.engineId),
            .durationMicros = timed.durationMicros,
        };
        if (!timed.readout.lines.empty())
        {
            // Under SingleLine there is at most one line, and ocr's ordering is
            // a contract, so "the first" is the same line on every run.
            auto& line   = timed.readout.lines.front();
            attempt.line = TextReading{
                .text         = std::move(line.text),
                .rect         = rect,
                .confidenceBp = line.confidenceBp,
            };
        }
        return attempt;
    }

    auto EngineSession::readTextLinesOnFrame(
        Frame const& frame,
        PixelRect rect,
        uint32 maximumLines
    ) const -> Result<BlockReadAttempt>
    {
        UF_TRY_VALUE(
            timed,
            runRead(
                frame,
                ocr::ReadSpec{
                    .rect         = rect,
                    .layout       = ocr::TextLayout::Block,
                    .maximumLines = maximumLines,
                },
                k_maximumBlockReadPixels,
                "block read"
            )
        );

        auto attempt = BlockReadAttempt{
            .engineId       = std::move(timed.engineId),
            .durationMicros = timed.durationMicros,
        };
        attempt.lines.reserve(timed.readout.lines.size());
        for (auto& line : timed.readout.lines)
        {
            // The line's OWN rectangle -- where the frame held the text, not
            // where the caller looked -- in frame pixels per ocr::TextLine.
            attempt.lines.emplace_back(
                TextReading{
                    .text         = std::move(line.text),
                    .rect         = line.bounds,
                    .confidenceBp = line.confidenceBp,
                }
            );
        }
        return attempt;
    }

    auto EngineSession::makeRecognitionPolicy() const -> RecognitionPolicy
    {
        return RecognitionPolicy{
            .maximumPixelComparisons = m_config.maximumPixelComparisons,
            .deadline                = MonotonicInstant::now().checkedAdd(m_config.recognitionTimeout),
            .cancellation            = m_config.cancellation,
        };
    }

    auto EngineSession::emit(trace::TraceEvent const& event) -> Status
    {
        return m_recorder.emit(event);
    }

    // Opens no trace line of its own: a run binds exactly one engine session, and
    // task::TaskHost's `run.started` already records that instant with the
    // project, task, hashes, seed and ids a bare session event never carried.
    auto EngineSession::create(
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
            std::make_shared<detail::EngineSessionIdentity>(),
            std::move(frameSource),
            std::move(actionSink),
            std::move(ocrEngine),
            recorder,
            std::move(config),
        };
    }

    auto EngineSession::captureRidingOutStalls() -> Result<Frame>
    {
        for (auto attempt = 0U; ; ++attempt)
        {
            // The deadline is minted per attempt so no adapter decides for
            // itself how long an observation may block, and the cancel source
            // travels with it so an empty frame pool cannot swallow a stop for a
            // whole capture. A fresh one each time because a retry sharing the
            // deadline the last attempt exhausted is not an attempt at all. An
            // overflowing timeout is a configuration error, not a licence to
            // wait.
            auto const deadline = MonotonicInstant::now().checkedAdd(
                m_config.captureTimeout
            );
            if (!deadline)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "capture timeout overflows the monotonic clock"
                );
            }

            auto captured = m_frameSource->capture(
                IFrameSource::CaptureBudget{
                    .deadline     = *deadline,
                    .cancellation = m_config.cancellation,
                }
            );
            if (captured)
            {
                return *std::move(captured);
            }

            auto error = std::move(captured).error();
            auto const stalled =
                automationErrorKind(error) == AutomationErrorKind::CaptureStalled;
            if (!stalled || attempt >= k_maximumCaptureStallRetries)
            {
                return std::unexpected{std::move(error)};
            }

            // Asked between attempts rather than only at the top of observe(): a
            // Ctrl-C during the retries would otherwise wait out every remaining
            // capture timeout before anyone looked.
            if (m_config.cancellation.stop_requested())
            {
                return fail(
                    AutomationErrorKind::Cancelled,
                    "cancelled while retrying a stalled capture"
                );
            }

            auto event           = trace::TraceEvent{};
            event.kind           = trace::TraceEventKind::EngineCaptureRetried;
            event.captureAttempt = attempt + 1U;
            event.errorKind      = AutomationErrorKind::CaptureStalled;
            event.message        = std::string{error.message()};
            UF_TRY(emit(event));
        }
    }

    auto EngineSession::observe() -> Result<Observation>
    {
        // Refusing here stops a cancelled run at the loop head, before a capture.
        if (m_config.cancellation.stop_requested())
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "cancelled before observation"
            );
        }

        UF_TRY(m_frameSource->validateTargetInstance());

        UF_TRY_VALUE(frame, captureRidingOutStalls());

        UF_TRY_VALUE(
            lease,
            ObservationLease::forFrame(frame, m_config.maxActionFrameAge)
        );
        auto const identity = FrameIdentity::fromFrame(frame);

        UF_TRY(emit(identityEvent(trace::TraceEventKind::EngineObserved, identity)));

        return Observation{std::move(frame), lease, identity, m_identity};
    }

    auto EngineSession::matchTemplate(
        Observation const& observation,
        GrayTemplateImage const& templateImage,
        PixelRect searchRoi
    ) -> Result<std::optional<MatchFound>>
    {
        UF_TRY(ensureUsable(observation, "matchTemplate"));

        auto const& frame    = observation.m_frame;
        auto const  identity = FrameIdentity::fromFrame(frame);

        // A search on a frame of the wrong geometry still returns a number, so
        // skipping this gate would be invisible.
        auto compatible = ensureCompatibleFrame(
            frame,
            m_config.liveFingerprint,
            m_config.projectFingerprint
        );
        auto attempt = compatible
            ? matchTemplateOnFrame(
                  frame,
                  templateImage,
                  searchRoi,
                  makeRecognitionPolicy()
              )
            : Result<TemplateMatchAttempt>{
                  std::unexpected{std::move(compatible).error()}
              };

        // Every exit writes one engine.action_found naming how the search ended;
        // the template identity stands in for the element id it does not have.
        if (!attempt)
        {
            auto event   = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
            event.action = trace::TraceEvent::Action{
                .outcome = trace::ActionSearch::Failed,
            };
            event.templateHash = templateImage.identity;
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
            event.templateHash = templateImage.identity;
            event.stopReason   = *p_stop;
            UF_TRY(emit(event));
            return fail(
                searchStopKind(*p_stop),
                std::format(
                    "template match stopped: {}",
                    searchStopDescription(*p_stop)
                )
            );
        }

        auto const& match = std::get<std::optional<TemplateMatch>>(
            attempt->result
        );
        if (!match)
        {
            auto event   = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
            event.action = trace::TraceEvent::Action{
                .outcome = trace::ActionSearch::Absent,
            };
            event.templateHash = templateImage.identity;
            UF_TRY(emit(event));
            return std::optional<MatchFound>{std::nullopt};
        }

        // Truncating division makes the centre one reproducible pixel at either
        // parity. No click offset to add: an offset is authored on an element.
        auto const centerX = match->matchedRect.x() + match->matchedRect.width() / 2U;
        auto const centerY = match->matchedRect.y() + match->matchedRect.height() / 2U;

        auto event   = identityEvent(trace::TraceEventKind::EngineActionFound, identity);
        event.action = trace::TraceEvent::Action{
            .outcome     = trace::ActionSearch::Found,
            .sadScore    = match->sadScore,
            .maximumSad  = match->maximumSad,
            .matchedRect = match->matchedRect,
        };
        event.templateHash = templateImage.identity;
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
        auto const  identity = FrameIdentity::fromFrame(frame);
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

    auto EngineSession::readTextLines(
        Observation const& observation,
        PixelRect rect,
        uint32 maximumLines
    ) -> Result<std::vector<TextReading>>
    {
        UF_TRY(ensureUsable(observation, "readTextLines"));

        auto const& frame    = observation.m_frame;
        auto const  identity = FrameIdentity::fromFrame(frame);
        auto        reading  = readTextLinesOnFrame(frame, rect, maximumLines);
        if (!reading)
        {
            auto event      = identityEvent(trace::TraceEventKind::EngineTextRead, identity);
            event.errorKind = automationErrorKind(reading.error());
            event.message   = std::string{reading.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(reading).error()};
        }

        // ONE event for the call, carrying the region, the whole cost and every
        // located line. Per-line events would let a reader summing durations
        // count the same milliseconds twice; a lineless event would leave a click
        // at a line with nothing in the stream to check it against.
        auto lines = std::vector<trace::TraceEvent::Reading::Line>{};
        lines.reserve(reading->lines.size());
        for (auto const& line : reading->lines)
        {
            lines.emplace_back(
                trace::TraceEvent::Reading::Line{
                    .text         = line.text,
                    .rect         = line.rect,
                    .confidenceBp = line.confidenceBp,
                }
            );
        }

        auto event    = identityEvent(trace::TraceEventKind::EngineTextRead, identity);
        event.reading = trace::TraceEvent::Reading{
            .text         = std::string{},
            .rect         = rect,
            .confidenceBp = 0,
            .engineId       = std::string{reading->engineId},
            .durationMicros = reading->durationMicros,
            .lines          = std::move(lines),
        };
        UF_TRY(emit(event));

        return std::move(reading->lines);
    }

    auto EngineSession::cropRegion(
        Observation const& observation,
        PixelRect rect
    ) -> Result<CroppedRegion>
    {
        UF_TRY(ensureUsable(observation, "cropRegion"));

        auto const& frame    = observation.m_frame;
        auto const  identity = FrameIdentity::fromFrame(frame);

        return withBgraFrame(
            frame,
            [rect, identity](BgraImage const& image) -> Result<CroppedRegion>
            {
                if (
                    rect.right() > image.width()
                    || rect.bottom() > image.height()
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "the crop region {}x{}+{}+{} does not fit inside the "
                            "{}x{} frame",
                            rect.width(),
                            rect.height(),
                            rect.x(),
                            rect.y(),
                            image.width(),
                            image.height()
                        )
                    );
                }

                auto const rowBytes = checkedMultiply(
                    checkedCast<std::size_t>(rect.width()).value_or(0U),
                    std::size_t{4}
                );
                auto const total = rowBytes.has_value()
                    ? checkedMultiply(
                          *rowBytes,
                          checkedCast<std::size_t>(rect.height()).value_or(0U)
                      )
                    : std::nullopt;
                if (!total.has_value())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "the crop region's byte size overflows"
                    );
                }

                auto pixels = std::vector<std::byte>(*total, std::byte{0});
                for (auto row = uint32{0}; row < rect.height(); ++row)
                {
                    for (auto column = uint32{0}; column < rect.width(); ++column)
                    {
                        auto const pixel = image.pixelAt(
                            rect.x() + column,
                            rect.y() + row
                        );
                        auto const base = (std::size_t{row} * *rowBytes)
                            + (std::size_t{column} * 4U);
                        checkedAt(pixels, base)      = std::byte{pixel.blue};
                        checkedAt(pixels, base + 1U) = std::byte{pixel.green};
                        checkedAt(pixels, base + 2U) = std::byte{pixel.red};
                        checkedAt(pixels, base + 3U) = std::byte{pixel.alpha};
                    }
                }

                return CroppedRegion{
                    .frame  = identity,
                    .width  = rect.width(),
                    .height = rect.height(),
                    .pixels = std::move(pixels),
                };
            }
        );
    }

    auto EngineSession::beginDelivery(
        Observation const& observation,
        std::string_view verb,
        std::string_view cancelMessage
    ) const -> Status
    {
        // Fail closed before the sink is touched: a requested stop outranks every
        // other outcome, and the handle must be this session's and unspent.
        if (m_config.cancellation.stop_requested())
        {
            return fail(AutomationErrorKind::Cancelled, std::string{cancelMessage});
        }
        return ensureUsable(observation, verb);
    }

    auto EngineSession::stampInput(
        trace::TraceEvent event,
        UnaimedInput input
    ) -> trace::TraceEvent
    {
        matchVariant(
            input,
            [&event](KeyName key) { event.key = key; },
            [&event](int32 notches) { event.wheelNotches = notches; }
        );
        return event;
    }

    auto EngineSession::rejectAction(
        FrameIdentity identity,
        Error const& error,
        std::optional<UnaimedInput> input
    ) -> Status
    {
        auto event = identityEvent(
            trace::TraceEventKind::EngineActionRejected,
            identity
        );
        if (input)
        {
            event = stampInput(std::move(event), *input);
        }
        event.errorKind = automationErrorKind(error);
        event.message   = std::string{error.message()};
        return emit(event);
    }

    auto EngineSession::authorizeCoordinate(
        Observation const& observation,
        PixelPoint point
    ) -> Result<Point<ClientSpace>>
    {
        auto const identity = observation.m_frameIdentity;

        // What survives of the coordinate authorization now the element and page
        // are layer two's: the geometry must still be what the page model was
        // authored against, the frame still within its lease. Both refuse here,
        // before any sink call.
        if (m_config.liveFingerprint != m_config.projectFingerprint)
        {
            auto mismatch = fail(
                AutomationErrorKind::TargetCompatibilityUnverified,
                "live size or DPI does not match the page model's fingerprint"
            );
            UF_TRY(rejectAction(identity, mismatch.error(), std::nullopt));
            return std::move(mismatch);
        }

        auto lease = observation.m_lease.validate(
            identity.sessionId(),
            identity.targetGeneration(),
            identity.frameId(),
            MonotonicInstant::now()
        );
        if (!lease)
        {
            UF_TRY(rejectAction(identity, lease.error(), std::nullopt));
            return std::unexpected{std::move(lease).error()};
        }

        UF_TRY(emit(identityEvent(trace::TraceEventKind::EngineActionAuthorized, identity)));

        UF_TRY_VALUE(framePoint, pixelPointToFramePoint(point));
        auto const clientPoint = observation.m_frame.transform().frameToClient(framePoint);

        auto revalidation = m_frameSource->validateTargetInstance();
        if (!revalidation)
        {
            UF_TRY(rejectAction(identity, revalidation.error(), std::nullopt));
            return std::unexpected{std::move(revalidation).error()};
        }

        return clientPoint;
    }

    auto EngineSession::deliverUnaimed(
        Observation&& observation,
        std::string_view verb,
        std::string_view cancelMessage,
        trace::TraceEventKind deliveredKind,
        UnaimedInput input
    ) -> Result<FrameIdentity>
    {
        UF_TRY(beginDelivery(observation, verb, cancelMessage));

        auto const identity = observation.m_frameIdentity;

        // The whole of what replaces the coordinate authorization: the input must
        // reach the target instance the observation came from, so a window
        // replaced since is refused here, before any sink call.
        auto revalidation = m_frameSource->validateTargetInstance();
        if (!revalidation)
        {
            UF_TRY(rejectAction(identity, revalidation.error(), input));
            return std::unexpected{std::move(revalidation).error()};
        }

        auto delivered = matchVariant(
            input,
            [this, identity](KeyName key) -> Status
            {
                return m_actionSink->pressKey(key, identity.targetGeneration());
            },
            [this, &observation](int32 notches) -> Status
            {
                return m_actionSink->scroll(notches, observation.m_lease);
            }
        );
        if (!delivered)
        {
            UF_TRY(rejectAction(identity, delivered.error(), input));
            return std::unexpected{std::move(delivered).error()};
        }

        // The input has landed, so the handle dies before any fallible emit: a
        // retry over a surviving alias must find it dead rather than deliver
        // twice.
        observation.m_invalidated = true;

        UF_TRY(emit(stampInput(identityEvent(deliveredKind, identity), input)));

        UF_TRY(
            emit(
                identityEvent(
                    trace::TraceEventKind::EngineObservationInvalidated,
                    identity
                )
            )
        );

        return identity;
    }

    auto EngineSession::clickPoint(
        Observation&& observation,
        PixelPoint point
    ) -> Result<ActReceipt>
    {
        UF_TRY(
            beginDelivery(observation, "clickPoint", "cancelled before delivery")
        );
        UF_TRY_VALUE(clientPoint, authorizeCoordinate(observation, point));

        auto const identity = observation.m_frameIdentity;

        auto delivered = m_actionSink->click(clientPoint, observation.m_lease);
        if (!delivered)
        {
            UF_TRY(rejectAction(identity, delivered.error(), std::nullopt));
            return std::unexpected{std::move(delivered).error()};
        }

        // The click has landed, so the handle dies before any fallible emit: a
        // retry over a surviving alias must find it dead rather than deliver
        // twice.
        observation.m_invalidated = true;

        // The annotation line carries the FRAME point the caller named as well as
        // the client point the desktop received -- on that stream the first is
        // what the agent believed it was doing. Why the spelling differs: header.
        auto clickEvent = identityEvent(
            m_recorder.frontEnd() == trace::FrontEnd::Annotation
                ? trace::TraceEventKind::AnnotationClickDelivered
                : trace::TraceEventKind::EngineActionDelivered,
            identity
        );
        clickEvent.clickClient = clientPoint;
        if (m_recorder.frontEnd() == trace::FrontEnd::Annotation)
        {
            clickEvent.annotation = trace::TraceEvent::Annotation{
                .point = point,
            };
        }
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
        UF_TRY_VALUE(
            identity,
            deliverUnaimed(
                std::move(observation),
                "pressKey",
                "cancelled before key delivery",
                trace::TraceEventKind::EngineKeyDelivered,
                UnaimedInput{key}
            )
        );

        return KeyReceipt{
            .frameId = identity.frameId(),
            .key     = key,
        };
    }

    auto EngineSession::scroll(
        Observation&& observation,
        int32 notches
    ) -> Result<ScrollReceipt>
    {
        UF_TRY_VALUE(
            identity,
            deliverUnaimed(
                std::move(observation),
                "scroll",
                "cancelled before scroll delivery",
                trace::TraceEventKind::EngineScrollDelivered,
                UnaimedInput{notches}
            )
        );

        return ScrollReceipt{
            .frameId = identity.frameId(),
            .notches = notches,
        };
    }

    auto EngineSession::longPress(
        Observation&& observation,
        PixelPoint point,
        MonotonicInstant::Duration hold
    ) -> Result<LongPressReceipt>
    {
        UF_TRY(
            beginDelivery(
                observation,
                "longPress",
                "cancelled before long press delivery"
            )
        );

        // Not the ceiling -- that is the host surface's -- but the one thing about
        // a hold this layer cannot pass on: a negative duration is a receipt and a
        // trace line describing an act nobody performed. Refused before the
        // observation is spent, so a caller with a sign error keeps its frame.
        if (hold < MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "a long press hold cannot run backwards"
            );
        }

        UF_TRY_VALUE(clientPoint, authorizeCoordinate(observation, point));

        auto const identity = observation.m_frameIdentity;

        auto delivered = m_actionSink->longPress(
            clientPoint,
            hold,
            observation.m_lease
        );
        if (!delivered)
        {
            UF_TRY(rejectAction(identity, delivered.error(), std::nullopt));
            return std::unexpected{std::move(delivered).error()};
        }

        // The press has landed and been released; the handle dies for
        // clickPoint's reason.
        observation.m_invalidated = true;

        auto pressEvent        = identityEvent(
            trace::TraceEventKind::EngineLongPressDelivered,
            identity
        );
        pressEvent.clickClient = clientPoint;
        pressEvent.holdMillis  = static_cast<uint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(hold).count()
        );
        UF_TRY(emit(pressEvent));

        UF_TRY(
            emit(
                identityEvent(
                    trace::TraceEventKind::EngineObservationInvalidated,
                    identity
                )
            )
        );

        return LongPressReceipt{
            .frameId    = identity.frameId(),
            .pressPoint = clientPoint,
            .hold       = hold,
        };
    }

    auto EngineSession::movePointer(
        Observation&& observation,
        PixelPoint point
    ) -> Result<PointerMoveReceipt>
    {
        UF_TRY(
            beginDelivery(
                observation,
                "movePointer",
                "cancelled before pointer move delivery"
            )
        );
        UF_TRY_VALUE(clientPoint, authorizeCoordinate(observation, point));

        auto const identity = observation.m_frameIdentity;

        auto delivered = m_actionSink->movePointer(clientPoint, observation.m_lease);
        if (!delivered)
        {
            UF_TRY(rejectAction(identity, delivered.error(), std::nullopt));
            return std::unexpected{std::move(delivered).error()};
        }

        // The move has landed; the handle dies for clickPoint's reason.
        observation.m_invalidated = true;

        // Written under the engine spelling on every stream including the
        // exploration one. There is no annotation.* spelling to prefer: the two
        // exist because a bare CLICK would otherwise claim a recognition, and a
        // move claims none on any stream.
        auto moveEvent        = identityEvent(
            trace::TraceEventKind::EnginePointerMoveDelivered,
            identity
        );
        moveEvent.clickClient = clientPoint;
        UF_TRY(emit(moveEvent));

        UF_TRY(
            emit(
                identityEvent(
                    trace::TraceEventKind::EngineObservationInvalidated,
                    identity
                )
            )
        );

        return PointerMoveReceipt{
            .frameId   = identity.frameId(),
            .movePoint = clientPoint,
        };
    }
}
