#include "session.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <core/utility/variant-match.hpp>

#include <domain/content-hash.hpp>
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
        [[nodiscard]] auto tracePayloadSchemaHash() -> ContentHash
        {
            auto const parsed = ContentHash::parse(
                std::format("sha256:{}", trace::k_traceSchemaHash)
            );
            UF_CHECK(parsed.has_value());
            return *parsed;
        }

        // Every engine event uses the recorder-owned stream identity. A frame
        // reference adds only the generic capture join keys; pixels and other
        // replay material never enter Audit Trace.
        [[nodiscard]]
        auto engineEvent(
            std::string_view eventType,
            std::optional<FrameIdentity> identity = std::nullopt,
            std::vector<trace::TraceField> fields = {}
        ) -> trace::TraceEventSpec
        {
            auto references = std::vector<trace::TraceReference>{};
            if (identity.has_value())
            {
                references = {
                    trace::TraceReference{
                        .type = "capture_session",
                        .id   = std::to_string(identity->sessionId().value()),
                    },
                    trace::TraceReference{
                        .type = "target_generation",
                        .id   = std::to_string(identity->targetGeneration().value()),
                    },
                    trace::TraceReference{
                        .type = "frame",
                        .id   = std::to_string(identity->frameId().value()),
                    },
                };
            }

            return trace::TraceEventSpec{
                .eventType = std::string{eventType},
                .audit     = trace::AuditMetadata{
                    .actor      = "engine",
                    .references = std::move(references),
                },
                .payload   = trace::TypedTracePayload{
                    .schemaHash = tracePayloadSchemaHash(),
                    .fields     = std::move(fields),
                },
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

    auto EngineSession::emit(trace::TraceEventSpec const& event) -> Status
    {
        return m_recorder.emit(event);
    }

    // Opens no trace line of its own. The owner opens the run bracket and gives
    // the recorder its immutable session and artifact identity before binding an
    // engine session to that stream.
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

            UF_TRY(
                emit(
                    engineEvent(
                        "engine.capture_retried",
                        std::nullopt,
                        {
                            trace::TraceField{
                                .name  = "capture_attempt",
                                .value = static_cast<uint64>(attempt + 1U),
                            },
                            trace::TraceField{
                                .name  = "error_kind",
                                .value = std::string{
                                    automationErrorWireName(
                                        AutomationErrorKind::CaptureStalled
                                    )
                                },
                            },
                        }
                    )
                )
            );
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

        UF_TRY(emit(engineEvent("engine.observed", identity)));

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
            auto event = engineEvent(
                "engine.action_found",
                identity,
                {
                    trace::TraceField{
                        .name  = "outcome",
                        .value = std::string{"failed"},
                    },
                    trace::TraceField{
                        .name  = "error_kind",
                        .value = std::string{
                            automationErrorWireName(
                                automationErrorKind(attempt.error()).value_or(
                                    AutomationErrorKind::InternalInvariant
                                )
                            )
                        },
                    },
                }
            );
            event.audit.references.emplace_back(
                trace::TraceReference{
                    .type = "template",
                    .id   = templateImage.identity,
                }
            );
            UF_TRY(emit(event));
            return std::unexpected{std::move(attempt).error()};
        }

        if (
            auto const* p_stop = std::get_if<SadSearchStopReason>(&attempt->result)
        )
        {
            auto event = engineEvent(
                "engine.action_found",
                identity,
                {
                    trace::TraceField{
                        .name  = "outcome",
                        .value = std::string{"stopped"},
                    },
                    trace::TraceField{
                        .name  = "stop_reason",
                        .value = std::string{
                            automationErrorWireName(searchStopKind(*p_stop))
                        },
                    },
                }
            );
            event.audit.references.emplace_back(
                trace::TraceReference{
                    .type = "template",
                    .id   = templateImage.identity,
                }
            );
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
            auto event = engineEvent(
                "engine.action_found",
                identity,
                {
                    trace::TraceField{
                        .name  = "outcome",
                        .value = std::string{"absent"},
                    },
                }
            );
            event.audit.references.emplace_back(
                trace::TraceReference{
                    .type = "template",
                    .id   = templateImage.identity,
                }
            );
            UF_TRY(emit(event));
            return std::optional<MatchFound>{std::nullopt};
        }

        // Truncating division makes the centre one reproducible pixel at either
        // parity. No click offset to add: an offset is authored on an element.
        auto const centerX = match->matchedRect.x() + match->matchedRect.width() / 2U;
        auto const centerY = match->matchedRect.y() + match->matchedRect.height() / 2U;

        auto event = engineEvent(
            "engine.action_found",
            identity,
            {
                trace::TraceField{
                    .name  = "outcome",
                    .value = std::string{"found"},
                },
                trace::TraceField{
                    .name  = "sad_score",
                    .value = match->sadScore,
                },
                trace::TraceField{
                    .name  = "maximum_sad",
                    .value = match->maximumSad,
                },
                trace::TraceField{
                    .name  = "matched_x",
                    .value = static_cast<uint64>(match->matchedRect.x()),
                },
                trace::TraceField{
                    .name  = "matched_y",
                    .value = static_cast<uint64>(match->matchedRect.y()),
                },
                trace::TraceField{
                    .name  = "matched_width",
                    .value = static_cast<uint64>(match->matchedRect.width()),
                },
                trace::TraceField{
                    .name  = "matched_height",
                    .value = static_cast<uint64>(match->matchedRect.height()),
                },
            }
        );
        event.audit.references.emplace_back(
            trace::TraceReference{
                .type = "template",
                .id   = templateImage.identity,
            }
        );
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
            UF_TRY(
                emit(
                    engineEvent(
                        "engine.text_read",
                        identity,
                        {
                            trace::TraceField{
                                .name  = "outcome",
                                .value = std::string{"failed"},
                            },
                            trace::TraceField{
                                .name  = "error_kind",
                                .value = std::string{
                                    automationErrorWireName(
                                        automationErrorKind(reading.error()).value_or(
                                            AutomationErrorKind::InternalInvariant
                                        )
                                    )
                                },
                            },
                        }
                    )
                )
            );
            return std::unexpected{std::move(reading).error()};
        }

        auto event = engineEvent(
            "engine.text_read",
            identity,
            {
                trace::TraceField{
                    .name  = "outcome",
                    .value = std::string{"completed"},
                },
                trace::TraceField{
                    .name  = "text_present",
                    .value = reading->line.has_value(),
                },
                trace::TraceField{
                    .name  = "confidence_bp",
                    .value = static_cast<uint64>(
                        reading->line
                            ? reading->line->confidenceBp
                            : uint32{0}
                    ),
                },
                trace::TraceField{
                    .name  = "read_x",
                    .value = static_cast<uint64>(rect.x()),
                },
                trace::TraceField{
                    .name  = "read_y",
                    .value = static_cast<uint64>(rect.y()),
                },
                trace::TraceField{
                    .name  = "read_width",
                    .value = static_cast<uint64>(rect.width()),
                },
                trace::TraceField{
                    .name  = "read_height",
                    .value = static_cast<uint64>(rect.height()),
                },
                trace::TraceField{
                    .name  = "read_micros",
                    .value = reading->durationMicros,
                },
            }
        );
        event.audit.references.emplace_back(
            trace::TraceReference{
                .type = "ocr_engine",
                .id   = reading->engineId,
            }
        );
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
            UF_TRY(
                emit(
                    engineEvent(
                        "engine.text_read",
                        identity,
                        {
                            trace::TraceField{
                                .name  = "outcome",
                                .value = std::string{"failed"},
                            },
                            trace::TraceField{
                                .name  = "error_kind",
                                .value = std::string{
                                    automationErrorWireName(
                                        automationErrorKind(reading.error()).value_or(
                                            AutomationErrorKind::InternalInvariant
                                        )
                                    )
                                },
                            },
                        }
                    )
                )
            );
            return std::unexpected{std::move(reading).error()};
        }

        // ONE event records the bounded read and its line count. Recognized text
        // remains in the caller-owned result; Audit Trace is not an OCR corpus.
        auto event = engineEvent(
            "engine.text_read",
            identity,
            {
                trace::TraceField{
                    .name  = "outcome",
                    .value = std::string{"completed"},
                },
                trace::TraceField{
                    .name  = "line_count",
                    .value = static_cast<uint64>(reading->lines.size()),
                },
                trace::TraceField{
                    .name  = "read_x",
                    .value = static_cast<uint64>(rect.x()),
                },
                trace::TraceField{
                    .name  = "read_y",
                    .value = static_cast<uint64>(rect.y()),
                },
                trace::TraceField{
                    .name  = "read_width",
                    .value = static_cast<uint64>(rect.width()),
                },
                trace::TraceField{
                    .name  = "read_height",
                    .value = static_cast<uint64>(rect.height()),
                },
                trace::TraceField{
                    .name  = "read_micros",
                    .value = reading->durationMicros,
                },
            }
        );
        event.audit.references.emplace_back(
            trace::TraceReference{
                .type = "ocr_engine",
                .id   = reading->engineId,
            }
        );
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
        trace::TraceEventSpec event,
        UnaimedInput input
    ) -> trace::TraceEventSpec
    {
        matchVariant(
            input,
            [&event](KeyName key)
            {
                event.payload.fields.emplace_back(
                    trace::TraceField{
                        .name  = "key",
                        .value = std::string{key.value()},
                    }
                );
            },
            [&event](int32 notches)
            {
                event.payload.fields.emplace_back(
                    trace::TraceField{
                        .name  = "wheel_notches",
                        .value = static_cast<int64>(notches),
                    }
                );
            }
        );
        return event;
    }

    auto EngineSession::rejectAction(
        FrameIdentity identity,
        Error const& error,
        std::optional<UnaimedInput> input
    ) -> Status
    {
        auto event = engineEvent(
            "engine.action_rejected",
            identity,
            {
                trace::TraceField{
                    .name  = "error_kind",
                    .value = std::string{
                        automationErrorWireName(
                            automationErrorKind(error).value_or(
                                AutomationErrorKind::InternalInvariant
                            )
                        )
                    },
                },
            }
        );
        if (input)
        {
            event = stampInput(std::move(event), *input);
        }
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

        UF_TRY(
            emit(
                engineEvent(
                    "engine.action_authorized",
                    identity,
                    {
                        trace::TraceField{
                            .name  = "pixel_x",
                            .value = static_cast<uint64>(point.x()),
                        },
                        trace::TraceField{
                            .name  = "pixel_y",
                            .value = static_cast<uint64>(point.y()),
                        },
                    }
                )
            )
        );

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
        std::string_view deliveredEventType,
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

        UF_TRY(
            emit(
                stampInput(
                    engineEvent(deliveredEventType, identity),
                    input
                )
            )
        );

        UF_TRY(
            emit(
                engineEvent("engine.observation_invalidated", identity)
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

        auto clickEvent = engineEvent(
            "engine.action_delivered",
            identity,
            {
                trace::TraceField{
                    .name  = "client_x",
                    .value = std::format("{}", clientPoint.x()),
                },
                trace::TraceField{
                    .name  = "client_y",
                    .value = std::format("{}", clientPoint.y()),
                },
            }
        );
        UF_TRY(emit(clickEvent));

        UF_TRY(
            emit(
                engineEvent("engine.observation_invalidated", identity)
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
                "engine.key_delivered",
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
                "engine.scroll_delivered",
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

        auto pressEvent = engineEvent(
            "engine.long_press_delivered",
            identity,
            {
                trace::TraceField{
                    .name  = "client_x",
                    .value = std::format("{}", clientPoint.x()),
                },
                trace::TraceField{
                    .name  = "client_y",
                    .value = std::format("{}", clientPoint.y()),
                },
                trace::TraceField{
                    .name  = "hold_millis",
                    .value = static_cast<uint64>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            hold
                        ).count()
                    ),
                },
            }
        );
        UF_TRY(emit(pressEvent));

        UF_TRY(
            emit(
                engineEvent("engine.observation_invalidated", identity)
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

        auto moveEvent = engineEvent(
            "engine.pointer_move_delivered",
            identity,
            {
                trace::TraceField{
                    .name  = "client_x",
                    .value = std::format("{}", clientPoint.x()),
                },
                trace::TraceField{
                    .name  = "client_y",
                    .value = std::format("{}", clientPoint.y()),
                },
            }
        );
        UF_TRY(emit(moveEvent));

        UF_TRY(
            emit(
                engineEvent("engine.observation_invalidated", identity)
            )
        );

        return PointerMoveReceipt{
            .frameId   = identity.frameId(),
            .movePoint = clientPoint,
        };
    }

    auto EngineSession::drag(
        Observation&& observation,
        PixelPoint start,
        PixelPoint end,
        MonotonicInstant::Duration travel
    ) -> Result<DragReceipt>
    {
        UF_TRY(
            beginDelivery(observation, "drag", "cancelled before drag delivery")
        );

        // longPress's clause, for its reason: a negative travel is a receipt and
        // a trace line describing an act nobody performed. Refused before the
        // observation is spent, so a caller with a sign error keeps its frame.
        if (travel < MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "a drag cannot travel backwards in time"
            );
        }

        // The far end converts FIRST and authorizes NOT AT ALL, and both halves
        // of that are deliberate.
        //
        // Converting first means a malformed end is refused before any
        // authorization is written, so the stream never carries an authorization
        // for a drag that could not be attempted.
        //
        // Not authorizing it is what keeps the ledger honest: authorizeCoordinate
        // emits engine.action_authorized and revalidates the target instance, and
        // both of those are facts about THIS FRAME rather than about a point on
        // it. Running it twice would write two authorizations for one delivered
        // drag, and a reader counting authorizations against deliveries would
        // find 2:1 for the one verb that names two points. What is genuinely
        // per-point is the conversion, and that runs for both.
        //
        // The client-area bound is neither of those and lives where it always
        // has: controller::drag checks BOTH endpoints against the live client
        // size before the button goes down.
        UF_TRY_VALUE(endFrame, pixelPointToFramePoint(end));

        UF_TRY_VALUE(startClient, authorizeCoordinate(observation, start));
        auto const endClient = (
            observation.m_frame.transform().frameToClient(endFrame)
        );

        auto const identity = observation.m_frameIdentity;

        auto delivered = m_actionSink->drag(
            startClient,
            endClient,
            travel,
            observation.m_lease
        );
        if (!delivered)
        {
            UF_TRY(rejectAction(identity, delivered.error(), std::nullopt));
            return std::unexpected{std::move(delivered).error()};
        }

        // The press has landed, travelled and been released; the handle dies for
        // clickPoint's reason, and here the frame is stale in the strongest sense
        // the engine has -- a drag exists to move what the frame was a picture of.
        observation.m_invalidated = true;

        auto dragEvent = engineEvent(
            "engine.drag_delivered",
            identity,
            {
                trace::TraceField{
                    .name  = "start_client_x",
                    .value = std::format("{}", startClient.x()),
                },
                trace::TraceField{
                    .name  = "start_client_y",
                    .value = std::format("{}", startClient.y()),
                },
                trace::TraceField{
                    .name  = "end_client_x",
                    .value = std::format("{}", endClient.x()),
                },
                trace::TraceField{
                    .name  = "end_client_y",
                    .value = std::format("{}", endClient.y()),
                },
                trace::TraceField{
                    .name  = "travel_millis",
                    .value = static_cast<uint64>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            travel
                        ).count()
                    ),
                },
            }
        );
        UF_TRY(emit(dragEvent));

        UF_TRY(
            emit(
                engineEvent("engine.observation_invalidated", identity)
            )
        );

        return DragReceipt{
            .frameId    = identity.frameId(),
            .startPoint = startClient,
            .endPoint   = endClient,
            .travel     = travel,
        };
    }
}
