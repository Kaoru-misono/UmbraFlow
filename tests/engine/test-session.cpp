#include <session.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/time/poll-sleep.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <vision/template-match.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// The engine session's whole surface after the script-owned page model's first
// retirement wave: observe, matchTemplate, clickPoint and pressKey. It names no
// annotation type -- resolvePage, findAction and act went with the C++ page
// model (docs/plans/2026-07-31-script-owned-page-model.md 9) -- and what the
// cases below still prove is every guarantee that never mentioned a page: the
// frame identity fence, the lease, the fingerprint, single consumption, the
// delivery-edge revalidation, the cancel gates and the capture deadline.
namespace uf::engine
{
    namespace
    {
        // The identity the session's recorder stamps. The engine never authors
        // it -- task::TaskHost does -- so any fixed pair proves the same thing.
        constexpr auto k_runId        = TaskRunId{11};
        constexpr auto k_generationId = GenerationId{2};

        // The grey levels the fixture paints with. Distinct, so a template cut
        // from one scores zero where it was painted and nonzero everywhere else.
        constexpr auto k_presentGray = uint8{5};
        constexpr auto k_absentGray  = uint8{9};

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        [[nodiscard]]
        auto requireErrorKind(Error const& error, AutomationErrorKind expected) -> void
        {
            auto const kind = automationErrorKind(error);
            REQUIRE(kind.has_value());
            CHECK(*kind == expected);
        }

        [[nodiscard]]
        auto instantAt(MonotonicInstant::Duration duration) -> MonotonicInstant
        {
            return MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{duration});
        }

        [[nodiscard]]
        auto fingerprintOf(uint32 width, uint32 height, uint32 dpi) -> ProjectFingerprint
        {
            auto const result = ProjectFingerprint::create(width, height, dpi, dpi);
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]]
        auto pixelRectOf(uint32 x, uint32 y, uint32 width, uint32 height) -> PixelRect
        {
            auto const result = PixelRect::create(x, y, width, height);
            REQUIRE(result.has_value());
            return *result;
        }

        // Decoded exactly as the script layer's template_load decodes one, so a
        // match here is a match there.
        [[nodiscard]]
        auto grayTemplate(uint8 gray, uint32 extent = 1) -> GrayTemplateImage
        {
            auto rgba = std::vector<std::byte>{};
            for (auto index = uint32{0}; index < extent * extent; ++index)
            {
                rgba.emplace_back(asByte(gray));
                rgba.emplace_back(asByte(gray));
                rgba.emplace_back(asByte(gray));
                rgba.emplace_back(asByte(255));
            }
            auto encoded = image::encodeRgbaPng(
                "engine-session-template.png",
                extent,
                extent,
                rgba
            );
            REQUIRE(encoded.has_value());
            auto decoded = decodeTemplateImage(
                *encoded,
                "gray-" + std::to_string(static_cast<unsigned>(gray))
            );
            REQUIRE(decoded.has_value());
            return *std::move(decoded);
        }

        [[nodiscard]]
        auto grayFrame(
            ProjectFingerprint fingerprint,
            std::vector<std::byte> pixels,
            FrameId frameId,
            MonotonicInstant capturedAt
        ) -> Frame
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(fingerprint.width()),
                static_cast<float>(fingerprint.height()),
                fingerprint.width(),
                fingerprint.height()
            );
            REQUIRE(transform.has_value());
            auto const width = checkedCast<std::size_t>(fingerprint.width());
            REQUIRE(width.has_value());
            auto const stride = checkedMultiply(
                width.value_or(std::size_t{0}),
                bytesPerPixel(PixelFormat::Gray8)
            );
            REQUIRE(stride.has_value());
            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(pixels))
            };
            auto frame = Frame::create(
                frameId,
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                capturedAt,
                fingerprint.width(),
                fingerprint.height(),
                stride.value_or(std::size_t{0}),
                PixelFormat::Gray8,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        // A frame carrying the searched-for grey at x = 1, so a match lands at a
        // known pixel.
        [[nodiscard]]
        auto matchingPixels() -> std::vector<std::byte>
        {
            return std::vector<std::byte>{asByte(0), asByte(k_presentGray), asByte(0)};
        }

        // A frame carrying a different grey, so a search for the template above
        // completes with a nonzero distance at its best position.
        [[nodiscard]]
        auto missingPixels() -> std::vector<std::byte>
        {
            return std::vector<std::byte>{asByte(0), asByte(k_absentGray), asByte(0)};
        }

        // Replays a fixed sequence of frames. Once the sequence is exhausted the
        // last frame repeats, so a poll loop never observes a spurious capture
        // failure while a deadline it is waiting on elapses.
        class FakeFrameSource final : public IFrameSource
        {
            std::vector<Frame> m_frames;
            std::size_t        m_index{0};
            bool               m_targetValid{true};

        public:
            explicit FakeFrameSource(std::vector<Frame> frames) noexcept
                : m_frames{std::move(frames)}
            {
            }

            // Models the HWND-reuse window the delivery-edge guard closes: valid
            // during observe, invalid by the time the click revalidates it.
            void invalidateTargetInstance() noexcept
            {
                m_targetValid = false;
            }

            [[nodiscard]]
            auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
            {
                if (m_frames.empty())
                {
                    return fail(
                        AutomationErrorKind::CaptureUnavailable,
                        "fake frame source has no frames to replay"
                    );
                }

                auto const index = std::min(m_index, m_frames.size() - 1U);
                ++m_index;
                return m_frames[index];
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                if (!m_targetValid)
                {
                    return fail(
                        AutomationErrorKind::TargetUnavailable,
                        "bound target instance is no longer valid"
                    );
                }
                return ok();
            }
        };

        // Blocks until the deadline its budget carries, then reports the expiry
        // and keeps what it was handed. The one frame source here that HONOURS
        // its budget: every other fake returns at once, which would say nothing
        // about a deadline, and only a source that keeps the budget can show the
        // session minted a real one rather than an instant an adapter may ignore.
        class DeadlineHonouringFrameSource final : public IFrameSource
        {
            std::optional<MonotonicInstant> m_deadline{};
            bool                            m_cancellable{false};

        public:
            [[nodiscard]]
            auto capture(CaptureBudget const& budget) -> Result<Frame> override
            {
                m_deadline    = budget.deadline;
                m_cancellable = budget.cancellation.stop_possible();

                // An hour of requested sleep, so the deadline -- and nothing
                // else -- is what ends the wait.
                pollSleep(
                    std::chrono::duration_cast<MonotonicInstant::Duration>(
                        std::chrono::hours{1}
                    ),
                    budget.deadline,
                    budget.cancellation
                );

                return fail(
                    AutomationErrorKind::Timeout,
                    "no frame arrived before the capture deadline"
                );
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }

            [[nodiscard]]
            auto deadline() const noexcept -> std::optional<MonotonicInstant>
            {
                return m_deadline;
            }

            // A default std::stop_token can never request a stop, so this tells a
            // session that forwarded its own cancel source from one that did not.
            [[nodiscard]] auto cancellable() const noexcept -> bool
            {
                return m_cancellable;
            }
        };

        // Counts delivered clicks so fail-closed cases can assert none escaped,
        // and keeps the last client point and lease for the happy path.
        class CountingActionSink final : public IActionSink
        {
            uint32                            m_clickCount{0};
            std::optional<Point<ClientSpace>> m_lastClick{};
            std::optional<ObservationLease>   m_lastLease{};
            bool                              m_refuseClick{false};

            uint32                          m_keyCount{0};
            std::optional<KeyName>          m_lastKey{};
            std::optional<TargetGeneration> m_lastKeyGeneration{};

            uint32                          m_scrollCount{0};
            std::optional<int32>            m_lastNotches{};
            std::optional<ObservationLease> m_lastScrollLease{};

            uint32                                    m_longPressCount{0};
            std::optional<Point<ClientSpace>>         m_lastLongPress{};
            std::optional<MonotonicInstant::Duration> m_lastHold{};
            std::optional<ObservationLease>           m_lastLongPressLease{};

            uint32                            m_moveCount{0};
            std::optional<Point<ClientSpace>> m_lastMove{};
            std::optional<ObservationLease>   m_lastMoveLease{};

        public:
            // Models the window the engine's own gates cannot close: the sink
            // accepts the post as authorized and the injection layer refuses it
            // at delivery, where the lease is revalidated a second time.
            void refuseClicks() noexcept
            {
                m_refuseClick = true;
            }

            [[nodiscard]]
            auto click(
                Point<ClientSpace> point,
                ObservationLease const& lease
            ) -> Status override
            {
                if (m_refuseClick)
                {
                    return fail(
                        AutomationErrorKind::StaleObservation,
                        "the observation aged out at the injection layer"
                    );
                }

                ++m_clickCount;
                m_lastClick = point;
                m_lastLease = lease;
                return ok();
            }

            // A keystroke carries no lease, so this records the generation it was
            // fenced on -- the whole of what pressKey forwards.
            [[nodiscard]]
            auto pressKey(
                KeyName key,
                TargetGeneration actionGeneration
            ) -> Status override
            {
                ++m_keyCount;
                m_lastKey           = key;
                m_lastKeyGeneration = actionGeneration;
                return ok();
            }

            // A scroll carries the lease and no coordinate: this records that a
            // wheel was asked for, and that the lease reaching delivery is intact.
            [[nodiscard]]
            auto scroll(
                int32 notches,
                ObservationLease const& lease
            ) -> Status override
            {
                ++m_scrollCount;
                m_lastNotches     = notches;
                m_lastScrollLease = lease;
                return ok();
            }

            // Coordinate, hold and lease are all recorded because all three are
            // separately droppable: a case asserting only that a press happened
            // would pass against a sink handed a hold it threw away.
            [[nodiscard]]
            auto longPress(
                Point<ClientSpace> point,
                MonotonicInstant::Duration hold,
                ObservationLease const& lease
            ) -> Status override
            {
                ++m_longPressCount;
                m_lastLongPress      = point;
                m_lastHold           = hold;
                m_lastLongPressLease = lease;
                return ok();
            }

            // Counted separately from the click, which is the whole point of the
            // verb: a case proving a move pressed nothing reads both counters,
            // and a sink that folded them together could not say so.
            [[nodiscard]]
            auto movePointer(
                Point<ClientSpace> point,
                ObservationLease const& lease
            ) -> Status override
            {
                ++m_moveCount;
                m_lastMove      = point;
                m_lastMoveLease = lease;
                return ok();
            }

            [[nodiscard]] auto clickCount() const noexcept -> uint32
            {
                return m_clickCount;
            }

            [[nodiscard]]
            auto lastClick() const noexcept -> std::optional<Point<ClientSpace>>
            {
                return m_lastClick;
            }

            [[nodiscard]]
            auto lastLease() const noexcept -> std::optional<ObservationLease>
            {
                return m_lastLease;
            }

            [[nodiscard]] auto keyCount() const noexcept -> uint32
            {
                return m_keyCount;
            }

            [[nodiscard]] auto lastKey() const noexcept -> std::optional<KeyName>
            {
                return m_lastKey;
            }

            [[nodiscard]]
            auto lastKeyGeneration() const noexcept -> std::optional<TargetGeneration>
            {
                return m_lastKeyGeneration;
            }

            [[nodiscard]] auto scrollCount() const noexcept -> uint32
            {
                return m_scrollCount;
            }

            [[nodiscard]] auto lastNotches() const noexcept -> std::optional<int32>
            {
                return m_lastNotches;
            }

            [[nodiscard]]
            auto lastScrollLease() const noexcept -> std::optional<ObservationLease>
            {
                return m_lastScrollLease;
            }

            [[nodiscard]] auto longPressCount() const noexcept -> uint32
            {
                return m_longPressCount;
            }

            [[nodiscard]]
            auto lastLongPress() const noexcept -> std::optional<Point<ClientSpace>>
            {
                return m_lastLongPress;
            }

            [[nodiscard]]
            auto lastHold() const noexcept
                -> std::optional<MonotonicInstant::Duration>
            {
                return m_lastHold;
            }

            [[nodiscard]]
            auto lastLongPressLease() const noexcept
                -> std::optional<ObservationLease>
            {
                return m_lastLongPressLease;
            }

            [[nodiscard]] auto moveCount() const noexcept -> uint32
            {
                return m_moveCount;
            }

            [[nodiscard]]
            auto lastMove() const noexcept -> std::optional<Point<ClientSpace>>
            {
                return m_lastMove;
            }

            [[nodiscard]]
            auto lastMoveLease() const noexcept -> std::optional<ObservationLease>
            {
                return m_lastMoveLease;
            }
        };

        // Records the payload of every event the recorder stamps, so a test can
        // assert the sequence of kinds and the outcome each one carries.
        class CollectingTraceSink final : public trace::ITraceSink
        {
            std::vector<trace::TraceEvent> m_events{};

        public:
            [[nodiscard]]
            auto emit(trace::StampedTraceEvent const& event) -> Status override
            {
                m_events.emplace_back(event.event());
                return ok();
            }

            [[nodiscard]]
            auto events() const noexcept UF_LIFETIME_BOUND
                -> std::span<trace::TraceEvent const>
            {
                return m_events;
            }
        };

        // Fails the emit whose kind matches a nominated trigger, so a fallible
        // post-click trace can be exercised deterministically.
        class FailOnKindTraceSink final : public trace::ITraceSink
        {
            trace::TraceEventKind m_failKind;

        public:
            explicit FailOnKindTraceSink(trace::TraceEventKind failKind) noexcept
                : m_failKind{failKind}
            {
            }

            [[nodiscard]]
            auto emit(trace::StampedTraceEvent const& event) -> Status override
            {
                if (event.event().kind == m_failKind)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "trace sink failed to emit"
                    );
                }
                return ok();
            }
        };

        // Declaration order is the lifetime order the trace contract requires:
        // the recorder outlives the session that borrows it, and the unique_ptr
        // keeps its address stable when this struct is moved out of makeSession.
        struct SessionUnderTest final
        {
            std::unique_ptr<trace::TraceRecorder> recorder{};
            Result<EngineSession>                 session;
            FakeFrameSource*                      source{};
            CountingActionSink*                   clicks{};
            CollectingTraceSink*                  traces{};
        };

        [[nodiscard]]
        auto baseConfig(ProjectFingerprint fingerprint) -> EngineSessionConfig
        {
            return EngineSessionConfig{
                .liveFingerprint    = fingerprint,
                .projectFingerprint = fingerprint,
                .maximumPixelComparisons = 1'000,
                .recognitionTimeout      = std::chrono::duration_cast<
                    MonotonicInstant::Duration
                >(std::chrono::seconds{5}),
            };
        }

        [[nodiscard]]
        auto makeSession(
            std::vector<Frame> frames,
            EngineSessionConfig config
        ) -> SessionUnderTest
        {
            auto frameSource = std::make_unique<FakeFrameSource>(std::move(frames));
            auto actionSink  = std::make_unique<CountingActionSink>();
            auto traceSink   = std::make_unique<CollectingTraceSink>();
            auto* const p_source = frameSource.get();
            auto* const p_clicks = actionSink.get();
            auto* const p_traces = traceSink.get();
            auto recorder = std::make_unique<trace::TraceRecorder>(
                std::move(traceSink),
                k_runId,
                k_generationId,
                trace::FrontEnd::Task
            );
            auto session = EngineSession::create(
                std::move(frameSource),
                std::move(actionSink),
                *recorder,
                std::move(config)
            );
            return SessionUnderTest{
                .recorder = std::move(recorder),
                .session  = std::move(session),
                .source   = p_source,
                .clicks   = p_clicks,
                .traces   = p_traces,
            };
        }

        // One session over one matching frame, which is what almost every case
        // below opens with.
        [[nodiscard]]
        auto matchingSession(
            ProjectFingerprint fingerprint,
            EngineSessionConfig config,
            MonotonicInstant capturedAt = MonotonicInstant::now()
        ) -> SessionUnderTest
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(
                grayFrame(fingerprint, matchingPixels(), FrameId{17}, capturedAt)
            );
            return makeSession(std::move(frames), std::move(config));
        }

        [[nodiscard]]
        auto kindsOf(std::span<trace::TraceEvent const> events)
            -> std::vector<trace::TraceEventKind>
        {
            auto kinds = std::vector<trace::TraceEventKind>{};
            kinds.reserve(events.size());
            for (auto const& event : events)
            {
                kinds.emplace_back(event.kind);
            }
            return kinds;
        }

        // The first event of `kind`, or null. The returned pointer observes
        // storage owned by the sink behind `events` and lives as long as it does,
        // which is what the annotation on that parameter states.
        [[nodiscard]]
        auto findEvent(
            std::span<trace::TraceEvent const> events UF_LIFETIME_BOUND,
            trace::TraceEventKind kind
        ) noexcept -> trace::TraceEvent const*
        {
            auto const found = std::ranges::find(
                events,
                kind,
                [](trace::TraceEvent const& event) noexcept { return event.kind; }
            );
            return found == events.end() ? nullptr : &*found;
        }
    }

    TEST_CASE("engine session observes matches and clicks into a single delivery")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        auto found = session.matchTemplate(
            *observation,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK((*found)->matchedRect.x() == 1);
        CHECK((*found)->sadScore == 0);

        auto receipt = session.clickPoint(std::move(*observation), (*found)->clickPixel);
        REQUIRE(receipt.has_value());
        CHECK(under.clicks->clickCount() == 1);
        CHECK(receipt->frameId == FrameId{17});
        REQUIRE(under.clicks->lastClick().has_value());
        CHECK(receipt->clickPoint == *under.clicks->lastClick());

        // The lease reaches the delivery layer unchanged, so its D0 fence fields
        // still identify the observed frame.
        REQUIRE(under.clicks->lastLease().has_value());
        CHECK(under.clicks->lastLease()->frameId() == FrameId{17});

        auto const expected = std::vector<trace::TraceEventKind>{
            trace::TraceEventKind::EngineObserved,
            trace::TraceEventKind::EngineActionFound,
            trace::TraceEventKind::EngineActionAuthorized,
            trace::TraceEventKind::EngineActionDelivered,
            trace::TraceEventKind::EngineObservationInvalidated,
        };
        CHECK(kindsOf(under.traces->events()) == expected);

        // The search's line names the template by the identity it was decoded
        // under, which is the only name a template belonging to no catalog has.
        auto const* p_action = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineActionFound
        );
        REQUIRE(p_action != nullptr);
        REQUIRE(p_action->action.has_value());
        CHECK(p_action->action->outcome == trace::ActionSearch::Found);
        CHECK(p_action->action->matchedRect.has_value());
        REQUIRE(p_action->templateHash.has_value());
        CHECK(*p_action->templateHash == "gray-5");

        for (auto const& event : under.traces->events())
        {
            REQUIRE(event.frame.has_value());
            CHECK(event.frame->frameId() == FrameId{17});
        }
    }

    TEST_CASE("engine session reports a search that had no candidate position")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        // A two-by-two template cannot sit anywhere in a three-by-one region, so
        // the search COMPLETES with nowhere to have looked. That empty optional
        // is the only "nothing" this verb reports: a template that fits always
        // has a best position, and judging it is layer two's.
        auto const found = session.matchTemplate(
            *observation,
            grayTemplate(k_presentGray, 2),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE(found.has_value());
        CHECK_FALSE(found->has_value());

        auto const* p_action = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineActionFound
        );
        REQUIRE(p_action != nullptr);
        REQUIRE(p_action->action.has_value());
        CHECK(p_action->action->outcome == trace::ActionSearch::Absent);
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session scores a template that is not on the frame as far off")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, missingPixels(), FrameId{17}, MonotonicInstant::now())
        );
        auto under = makeSession(std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        // The score, not a verdict: a template that fits reports its best
        // position and the distance there, and the caller decides. The matching
        // case above is the control that a zero is reachable at all.
        auto const found = session.matchTemplate(
            *observation,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK((*found)->sadScore > 0);
        CHECK((*found)->maximumSad == 255);
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session fails closed when the live fingerprint does not match")
    {
        auto const projectPrint = fingerprintOf(3, 1, 96);

        // A different DPI makes the live fingerprint incompatible with the
        // geometry the page model was authored at.
        auto config            = baseConfig(projectPrint);
        config.liveFingerprint = fingerprintOf(3, 1, 120);
        auto under             = matchingSession(projectPrint, config);
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        // The search refuses rather than answering about pixels it must not be
        // compared against.
        auto const found = session.matchTemplate(
            *observation,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE_FALSE(found.has_value());
        requireErrorKind(
            found.error(),
            AutomationErrorKind::TargetCompatibilityUnverified
        );

        // And so does the delivery edge, with no sink call, which is the half of
        // the check that keeps a click off a target of the wrong geometry.
        auto const receipt = session.clickPoint(
            std::move(*observation),
            PixelPoint{1, 0}
        );
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(
            receipt.error(),
            AutomationErrorKind::TargetCompatibilityUnverified
        );
        CHECK(under.clicks->clickCount() == 0);
        CHECK(
            findEvent(
                under.traces->events(),
                trace::TraceEventKind::EngineActionRejected
            )
            != nullptr
        );
    }

    TEST_CASE("engine session refuses a click whose observation lease has expired")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        // A frame captured at the clock epoch with a zero max age produces a
        // lease that is already expired by the time the click is delivered.
        auto config              = baseConfig(fingerprint);
        config.maxActionFrameAge = MonotonicInstant::Duration::zero();
        auto under               = matchingSession(
            fingerprint,
            config,
            instantAt(MonotonicInstant::Duration{0})
        );
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        auto const receipt = session.clickPoint(
            std::move(*observation),
            PixelPoint{1, 0}
        );
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->clickCount() == 0);
        CHECK(
            findEvent(
                under.traces->events(),
                trace::TraceEventKind::EngineActionRejected
            )
            != nullptr
        );
    }

    TEST_CASE("engine session rejects reuse of an invalidated observation")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        auto handle        = *std::move(observation);
        auto const receipt = session.clickPoint(std::move(handle), PixelPoint{1, 0});
        REQUIRE(receipt.has_value());
        CHECK(under.clicks->clickCount() == 1);

        // The consumed observation must fail closed on any further use.
        auto const reuse = session.matchTemplate(
            handle,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE_FALSE(reuse.has_value());
        requireErrorKind(reuse.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->clickCount() == 1);
    }

    TEST_CASE("engine session invalidates a moved-from observation")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto source = session.observe();
        REQUIRE(source.has_value());

        // Moving the handle transfers it to the destination and leaves the source
        // dead, so the moved-from handle fails closed exactly like a consumed one.
        auto moved       = *std::move(source);
        auto const reuse = session.matchTemplate(
            *source,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE_FALSE(reuse.has_value());
        requireErrorKind(reuse.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session move preserves observations already vended")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());

        auto observation = under.session->observe();
        REQUIRE(observation.has_value());

        auto session     = std::move(*under.session);
        auto const found = session.matchTemplate(
            *observation,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE(found.has_value());
        CHECK(found->has_value());
    }

    TEST_CASE("engine session rejects an observation vended by a different session")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto underA = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(underA.session.has_value());
        auto& sessionA = *underA.session;

        // A second session over an equivalent geometry. The observation from
        // session A must never authorize a click on session B.
        auto framesB = std::vector<Frame>{};
        framesB.emplace_back(
            grayFrame(fingerprint, matchingPixels(), FrameId{18}, MonotonicInstant::now())
        );
        auto underB = makeSession(std::move(framesB), baseConfig(fingerprint));
        REQUIRE(underB.session.has_value());
        auto& sessionB = *underB.session;

        auto observation = sessionA.observe();
        REQUIRE(observation.has_value());

        auto const foreignMatch = sessionB.matchTemplate(
            *observation,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE_FALSE(foreignMatch.has_value());
        requireErrorKind(
            foreignMatch.error(),
            AutomationErrorKind::InternalInvariant
        );

        auto const receipt = sessionB.clickPoint(
            std::move(*observation),
            PixelPoint{1, 0}
        );
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::InternalInvariant);
        CHECK(underB.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session invalidates the handle before a failing post-click trace")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, matchingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        // A trace sink that fails exactly on the engine.action_delivered emit,
        // i.e. the first fallible step after the click has already landed.
        auto frameSource = std::make_unique<FakeFrameSource>(std::move(frames));
        auto actionSink  = std::make_unique<CountingActionSink>();
        auto traceSink   = std::make_unique<FailOnKindTraceSink>(
            trace::TraceEventKind::EngineActionDelivered
        );
        auto* const p_clicks = actionSink.get();
        auto recorder        = trace::TraceRecorder{
            std::move(traceSink),
            k_runId,
            k_generationId,
            trace::FrontEnd::Task,
        };
        auto session = EngineSession::create(
            std::move(frameSource),
            std::move(actionSink),
            recorder,
            baseConfig(fingerprint)
        );
        REQUIRE(session.has_value());

        auto observation = session->observe();
        REQUIRE(observation.has_value());

        // Binding the rvalue through a named handle keeps a caller-side alias
        // that survives the call, which the retry below needs.
        auto handle        = *std::move(observation);
        auto const receipt = session->clickPoint(std::move(handle), PixelPoint{1, 0});
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::IoFailure);
        CHECK(p_clicks->clickCount() == 1);

        // The delivery already invalidated the handle, so retrying with the
        // surviving alias fails closed and cannot double-deliver.
        auto const retry = session->clickPoint(std::move(handle), PixelPoint{1, 0});
        REQUIRE_FALSE(retry.has_value());
        requireErrorKind(retry.error(), AutomationErrorKind::StaleObservation);
        CHECK(p_clicks->clickCount() == 1);
    }

    TEST_CASE("engine session surfaces a search budget stop as a failure")
    {
        auto const fingerprint         = fingerprintOf(3, 1, 96);
        auto config                    = baseConfig(fingerprint);
        config.maximumPixelComparisons = 0;
        auto under                     = matchingSession(fingerprint, config);
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const found = session.matchTemplate(
            *observation,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE_FALSE(found.has_value());
        requireErrorKind(found.error(), AutomationErrorKind::RecognitionIncomplete);
        // The search never looked, so it must not report the absence a completed
        // miss reports: a miss is an empty optional with no error, this is not.
        CHECK(failureResponse(found.error()) == FailureResponse::Retry);
        CHECK(under.clicks->clickCount() == 0);

        auto const* p_action = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineActionFound
        );
        REQUIRE(p_action != nullptr);
        REQUIRE(p_action->action.has_value());
        CHECK(p_action->action->outcome == trace::ActionSearch::Stopped);
        CHECK(p_action->stopReason == SadSearchStopReason::ComparisonBudgetExhausted);
    }

    TEST_CASE("engine session surfaces a cancelled search as a cancellation")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto cancellation   = std::stop_source{};
        auto config         = baseConfig(fingerprint);
        config.cancellation = cancellation.get_token();
        auto under          = matchingSession(fingerprint, std::move(config));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        // observe() gates on cancellation, so the stop is requested after the
        // observation exists and the search policy is what surfaces it.
        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);

        auto const found = session.matchTemplate(
            *observation,
            grayTemplate(k_presentGray),
            pixelRectOf(0, 0, 3, 1)
        );
        REQUIRE_FALSE(found.has_value());
        requireErrorKind(found.error(), AutomationErrorKind::Cancelled);
        CHECK(under.clicks->clickCount() == 0);

        auto const* p_action = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineActionFound
        );
        REQUIRE(p_action != nullptr);
        REQUIRE(p_action->action.has_value());
        CHECK(p_action->action->outcome == trace::ActionSearch::Stopped);
        CHECK(p_action->stopReason == SadSearchStopReason::Cancelled);
    }

    TEST_CASE("engine session revalidates the target instance at the delivery edge")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        // Valid during observe, invalid before delivery: an HWND reused between
        // the two, which the delivery-edge revalidation rejects before the sink.
        under.source->invalidateTargetInstance();

        auto const receipt = session.clickPoint(
            std::move(*observation),
            PixelPoint{1, 0}
        );
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::TargetUnavailable);
        CHECK(under.clicks->clickCount() == 0);
        CHECK(
            findEvent(
                under.traces->events(),
                trace::TraceEventKind::EngineActionRejected
            )
            != nullptr
        );
    }

    TEST_CASE("engine session records a click the sink refused")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        // Everything the engine can check passes, and the post is refused
        // anyway. Remove the rejection emit from clickPoint and the frame's
        // stream ends at engine.action_authorized with no terminal line, so a
        // reader cannot tell a refused click from a lost one.
        under.clicks->refuseClicks();

        auto const receipt = session.clickPoint(
            std::move(*observation),
            PixelPoint{1, 0}
        );
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::StaleObservation);

        auto const kinds = kindsOf(under.traces->events());
        CHECK(
            std::ranges::count(
                kinds,
                trace::TraceEventKind::EngineActionAuthorized
            )
            == 1
        );
        CHECK(
            std::ranges::count(
                kinds,
                trace::TraceEventKind::EngineActionDelivered
            )
            == 0
        );

        auto const* p_rejected = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineActionRejected
        );
        REQUIRE(p_rejected != nullptr);
        REQUIRE(p_rejected->errorKind.has_value());
        CHECK(*p_rejected->errorKind == AutomationErrorKind::StaleObservation);
        CHECK(p_rejected->message.has_value());
    }

    TEST_CASE("engine session revalidates the target instance before a keystroke")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        under.source->invalidateTargetInstance();

        auto const key = KeyName::create("E");
        REQUIRE(key.has_value());
        auto const receipt = session.pressKey(std::move(*observation), *key);
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::TargetUnavailable);
        CHECK(under.clicks->keyCount() == 0);

        // The refusal names the key it refused: a rejected line without it says
        // only that something was refused on this frame.
        auto const* p_rejected = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineActionRejected
        );
        REQUIRE(p_rejected != nullptr);
        REQUIRE(p_rejected->key.has_value());
        CHECK(*p_rejected->key == *key);
    }

    TEST_CASE("engine session delivers a keystroke and spends the observation")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const key = KeyName::create("E");
        REQUIRE(key.has_value());

        auto handle        = *std::move(observation);
        auto const receipt = session.pressKey(std::move(handle), *key);
        REQUIRE(receipt.has_value());
        CHECK(under.clicks->keyCount() == 1);
        CHECK(receipt->frameId == FrameId{17});
        REQUIRE(under.clicks->lastKeyGeneration().has_value());
        CHECK(*under.clicks->lastKeyGeneration() == TargetGeneration::fromValue(3));

        // A delivered keystroke changes the screen exactly as a click does, so the
        // observation is spent and a second delivery on the same handle is refused.
        auto const retry = session.pressKey(std::move(handle), *key);
        REQUIRE_FALSE(retry.has_value());
        requireErrorKind(retry.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->keyCount() == 1);
    }

    TEST_CASE("engine session delivers a scroll and spends the observation")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        auto handle        = *std::move(observation);
        auto const receipt = session.scroll(std::move(handle), int32{-2});
        REQUIRE(receipt.has_value());
        CHECK(under.clicks->scrollCount() == 1);
        CHECK(receipt->frameId == FrameId{17});
        CHECK(receipt->notches == int32{-2});
        REQUIRE(under.clicks->lastNotches().has_value());
        CHECK(*under.clicks->lastNotches() == int32{-2});

        // The verb enforces no lease here, but it must not swallow one either:
        // the controller's delivery-time fence needs the observation's own.
        REQUIRE(under.clicks->lastScrollLease().has_value());
        CHECK(under.clicks->lastScrollLease()->frameId() == FrameId{17});

        // A delivered scroll moves the screen, so the observation is spent.
        // Remove the invalidation in EngineSession::scroll and one frame delivers
        // two wheel messages, so this goes red.
        auto const retry = session.scroll(std::move(handle), int32{-2});
        REQUIRE_FALSE(retry.has_value());
        requireErrorKind(retry.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->scrollCount() == 1);

        auto const kinds = kindsOf(under.traces->events());
        CHECK(
            std::ranges::count(kinds, trace::TraceEventKind::EngineScrollDelivered)
            == 1
        );

        // Sign included: nothing downstream could notice a scroll recorded upward
        // that went downward, because the wheel leaves no other trace of itself.
        auto const* p_scroll = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineScrollDelivered
        );
        REQUIRE(p_scroll != nullptr);
        REQUIRE(p_scroll->wheelNotches.has_value());
        CHECK(*p_scroll->wheelNotches == int32{-2});
    }

    TEST_CASE("engine session revalidates the target instance before a scroll")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        // Valid during observe, invalid before delivery: an HWND reused between
        // the two. Remove the revalidation in EngineSession::scroll and the wheel
        // reaches whatever now owns that handle, so this goes red.
        under.source->invalidateTargetInstance();

        auto const receipt = session.scroll(std::move(*observation), int32{1});
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::TargetUnavailable);
        CHECK(under.clicks->scrollCount() == 0);
    }

    TEST_CASE("engine session refuses a scroll on an observation it did not vend")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto underA = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(underA.session.has_value());

        auto framesB = std::vector<Frame>{};
        framesB.emplace_back(
            grayFrame(fingerprint, matchingPixels(), FrameId{18}, MonotonicInstant::now())
        );
        auto underB = makeSession(std::move(framesB), baseConfig(fingerprint));
        REQUIRE(underB.session.has_value());

        auto observation = underA.session->observe();
        REQUIRE(observation.has_value());

        auto const receipt = underB.session->scroll(
            std::move(*observation),
            int32{1}
        );
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::InternalInvariant);
        CHECK(underB.clicks->scrollCount() == 0);
    }

    TEST_CASE("engine session cancels a scroll requested after the observation")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto cancellation   = std::stop_source{};
        auto config         = baseConfig(fingerprint);
        config.cancellation = cancellation.get_token();
        auto under          = matchingSession(fingerprint, std::move(config));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        cancellation.request_stop();

        auto const receipt = session.scroll(std::move(*observation), int32{1});
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::Cancelled);
        CHECK(under.clicks->scrollCount() == 0);
    }

    TEST_CASE("engine session scrolls without the fences a coordinate needs")
    {
        // A scroll names no screen position, so the engine applies NEITHER the
        // project-fingerprint check nor the observation's lease -- both ask
        // whether a coordinate still means what it meant. Add either to
        // EngineSession::scroll and one of these two goes red. The delivery layer
        // still receives the lease and fences the position it chooses on it; the
        // refusal must just not happen a layer too early.
        auto const fingerprint = fingerprintOf(3, 1, 96);

        SUBCASE("a mismatched fingerprint does not refuse a scroll")
        {
            auto config            = baseConfig(fingerprint);
            config.liveFingerprint = fingerprintOf(3, 1, 120);
            auto under             = matchingSession(fingerprint, std::move(config));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            auto const receipt = under.session->scroll(
                std::move(*observation),
                int32{1}
            );
            REQUIRE(receipt.has_value());
            CHECK(under.clicks->scrollCount() == 1);
        }

        SUBCASE("an expired lease does not refuse a scroll")
        {
            auto config              = baseConfig(fingerprint);
            config.maxActionFrameAge = MonotonicInstant::Duration::zero();
            auto under               = matchingSession(fingerprint, std::move(config));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            auto const receipt = under.session->scroll(
                std::move(*observation),
                int32{1}
            );
            REQUIRE(receipt.has_value());
            CHECK(under.clicks->scrollCount() == 1);
        }
    }

    TEST_CASE("engine session delivers a long press and spends the observation")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        auto const hold = MonotonicInstant::Duration{std::chrono::milliseconds{350}};
        auto handle        = *std::move(observation);
        auto const receipt = session.longPress(std::move(handle), PixelPoint{1, 0}, hold);
        REQUIRE(receipt.has_value());
        CHECK(under.clicks->longPressCount() == 1);
        CHECK(receipt->frameId == FrameId{17});
        CHECK(receipt->hold == hold);

        // The hold the caller named reached the PORT: every other assertion here
        // would still hold if the duration were dropped between session and sink.
        // Replace `hold` with a constant in the sink call and only this goes red.
        REQUIRE(under.clicks->lastHold().has_value());
        CHECK(*under.clicks->lastHold() == hold);
        REQUIRE(under.clicks->lastLongPress().has_value());
        CHECK(under.clicks->lastLongPress()->x() == doctest::Approx(1.0));

        // The lease reaching the sink is this observation's own, which keeps the
        // controller's delivery-time fence in the loop as layer two.
        REQUIRE(under.clicks->lastLongPressLease().has_value());
        CHECK(under.clicks->lastLongPressLease()->frameId() == FrameId{17});

        // The press changed the screen, so the observation is spent. Remove the
        // invalidation in EngineSession::longPress and one frame delivers two
        // presses, so this goes red.
        auto const retry = session.longPress(std::move(handle), PixelPoint{1, 0}, hold);
        REQUIRE_FALSE(retry.has_value());
        requireErrorKind(retry.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->longPressCount() == 1);

        auto const kinds = kindsOf(under.traces->events());
        CHECK(
            std::ranges::count(
                kinds,
                trace::TraceEventKind::EngineLongPressDelivered
            )
            == 1
        );

        // And NOT as a click: a reader counting delivered clicks would otherwise
        // count an act that magnified a card rather than pressing a button.
        CHECK(
            std::ranges::count(kinds, trace::TraceEventKind::EngineActionDelivered)
            == 0
        );

        auto const* p_press = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineLongPressDelivered
        );
        REQUIRE(p_press != nullptr);
        REQUIRE(p_press->holdMillis.has_value());
        CHECK(*p_press->holdMillis == uint64{350});
        CHECK(p_press->clickClient.has_value());
    }

    TEST_CASE("engine session fences a long press exactly as it fences a click")
    {
        // The pairing is the point: a long press names a coordinate, so the
        // failure guarded against is a SECOND and laxer path to the same window
        // beside the click's. The first four subcases each have an exact twin
        // among the click cases -- expired lease, mismatched fingerprint and
        // replaced target instance above, cancelled run just below -- and
        // removing the matching check from EngineSession::longPress leaves the
        // twin green while this goes red. The fifth has no twin and cannot:
        // clickPoint names no hold, so the backwards hold is this verb's own
        // argument check rather than a shared gate.
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto const hold = MonotonicInstant::Duration{std::chrono::milliseconds{200}};

        SUBCASE("an expired lease refuses the press")
        {
            auto config              = baseConfig(fingerprint);
            config.maxActionFrameAge = MonotonicInstant::Duration::zero();
            auto under               = matchingSession(fingerprint, std::move(config));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            auto const receipt = under.session->longPress(
                std::move(*observation),
                PixelPoint{1, 0},
                hold
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(receipt.error(), AutomationErrorKind::StaleObservation);
            CHECK(under.clicks->longPressCount() == 0);
        }

        SUBCASE("a mismatched fingerprint refuses the press")
        {
            auto config            = baseConfig(fingerprint);
            config.liveFingerprint = fingerprintOf(3, 1, 120);
            auto under             = matchingSession(fingerprint, std::move(config));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            auto const receipt = under.session->longPress(
                std::move(*observation),
                PixelPoint{1, 0},
                hold
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(
                receipt.error(),
                AutomationErrorKind::TargetCompatibilityUnverified
            );
            CHECK(under.clicks->longPressCount() == 0);
        }

        SUBCASE("a replaced target instance refuses the press")
        {
            auto under = matchingSession(fingerprint, baseConfig(fingerprint));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            under.source->invalidateTargetInstance();

            auto const receipt = under.session->longPress(
                std::move(*observation),
                PixelPoint{1, 0},
                hold
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(receipt.error(), AutomationErrorKind::TargetUnavailable);
            CHECK(under.clicks->longPressCount() == 0);
        }

        SUBCASE("a cancelled run refuses the press before any sink call")
        {
            auto cancellation   = std::stop_source{};
            auto config         = baseConfig(fingerprint);
            config.cancellation = cancellation.get_token();
            auto under          = matchingSession(fingerprint, std::move(config));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            REQUIRE(cancellation.request_stop());

            auto const receipt = under.session->longPress(
                std::move(*observation),
                PixelPoint{1, 0},
                hold
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(receipt.error(), AutomationErrorKind::Cancelled);
            CHECK(under.clicks->longPressCount() == 0);
        }

        SUBCASE("a hold that runs backwards refuses the press and keeps the frame")
        {
            auto under = matchingSession(fingerprint, baseConfig(fingerprint));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            auto const receipt = under.session->longPress(
                std::move(*observation),
                PixelPoint{1, 0},
                MonotonicInstant::Duration{-1}
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(receipt.error(), AutomationErrorKind::ActionRejected);
            CHECK(under.clicks->longPressCount() == 0);
        }
    }

    TEST_CASE("engine session moves the pointer without pressing anything")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto under = matchingSession(fingerprint, baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        auto handle        = *std::move(observation);
        auto const receipt = session.movePointer(std::move(handle), PixelPoint{1, 0});
        REQUIRE(receipt.has_value());
        CHECK(receipt->frameId == FrameId{17});

        // Exactly one pointer message reached the port, and it was a move: route
        // movePointer to the sink's click() and the first pair of counters swap,
        // which is the whole guarantee this verb sells.
        CHECK(under.clicks->moveCount() == 1);
        CHECK(under.clicks->clickCount() == 0);
        CHECK(under.clicks->longPressCount() == 0);
        CHECK(under.clicks->keyCount() == 0);
        CHECK(under.clicks->scrollCount() == 0);
        REQUIRE(under.clicks->lastMove().has_value());
        CHECK(under.clicks->lastMove()->x() == doctest::Approx(1.0));

        // The lease reaching the sink is this observation's own, which keeps the
        // controller's delivery-time fence in the loop as layer two.
        REQUIRE(under.clicks->lastMoveLease().has_value());
        CHECK(under.clicks->lastMoveLease()->frameId() == FrameId{17});

        // A pointer message changes what the target believes is hovered, so the
        // observation is spent. Remove the invalidation in
        // EngineSession::movePointer and one frame delivers two moves, so this
        // goes red.
        auto const retry = session.movePointer(std::move(handle), PixelPoint{1, 0});
        REQUIRE_FALSE(retry.has_value());
        requireErrorKind(retry.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->moveCount() == 1);

        auto const kinds = kindsOf(under.traces->events());
        CHECK(
            std::ranges::count(
                kinds,
                trace::TraceEventKind::EnginePointerMoveDelivered
            )
            == 1
        );

        // And NOT as a click, for the long press's reason: a reader counting
        // delivered clicks would otherwise count a message that pressed nothing.
        CHECK(
            std::ranges::count(kinds, trace::TraceEventKind::EngineActionDelivered)
            == 0
        );

        auto const* p_move = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EnginePointerMoveDelivered
        );
        REQUIRE(p_move != nullptr);
        REQUIRE(p_move->clickClient.has_value());
        CHECK(p_move->clickClient->x() == doctest::Approx(1.0));
    }

    TEST_CASE("engine session fences a pointer move exactly as it fences a click")
    {
        // A move names a coordinate, so it takes the coordinate authorization
        // whole: nothing here may be looser than a click, or the pointer becomes
        // a second and laxer path onto the same window. Each subcase has an exact
        // twin among the click cases above, and removing the matching gate from
        // EngineSession::movePointer leaves the twin green while this goes red.
        auto const fingerprint = fingerprintOf(3, 1, 96);

        SUBCASE("an expired lease refuses the move")
        {
            auto config              = baseConfig(fingerprint);
            config.maxActionFrameAge = MonotonicInstant::Duration::zero();
            auto under               = matchingSession(fingerprint, std::move(config));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            auto const receipt = under.session->movePointer(
                std::move(*observation),
                PixelPoint{1, 0}
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(receipt.error(), AutomationErrorKind::StaleObservation);
            CHECK(under.clicks->moveCount() == 0);
        }

        SUBCASE("a mismatched fingerprint refuses the move")
        {
            auto config            = baseConfig(fingerprint);
            config.liveFingerprint = fingerprintOf(3, 1, 120);
            auto under             = matchingSession(fingerprint, std::move(config));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            auto const receipt = under.session->movePointer(
                std::move(*observation),
                PixelPoint{1, 0}
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(
                receipt.error(),
                AutomationErrorKind::TargetCompatibilityUnverified
            );
            CHECK(under.clicks->moveCount() == 0);
        }

        SUBCASE("a replaced target instance refuses the move")
        {
            auto under = matchingSession(fingerprint, baseConfig(fingerprint));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            under.source->invalidateTargetInstance();

            auto const receipt = under.session->movePointer(
                std::move(*observation),
                PixelPoint{1, 0}
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(receipt.error(), AutomationErrorKind::TargetUnavailable);
            CHECK(under.clicks->moveCount() == 0);
        }

        SUBCASE("a cancelled run refuses the move before any sink call")
        {
            auto cancellation   = std::stop_source{};
            auto config         = baseConfig(fingerprint);
            config.cancellation = cancellation.get_token();
            auto under          = matchingSession(fingerprint, std::move(config));
            REQUIRE(under.session.has_value());

            auto observation = under.session->observe();
            REQUIRE(observation.has_value());
            REQUIRE(cancellation.request_stop());

            auto const receipt = under.session->movePointer(
                std::move(*observation),
                PixelPoint{1, 0}
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(receipt.error(), AutomationErrorKind::Cancelled);
            CHECK(under.clicks->moveCount() == 0);
        }

        SUBCASE("an observation from another session refuses the move")
        {
            auto underA = matchingSession(fingerprint, baseConfig(fingerprint));
            REQUIRE(underA.session.has_value());

            auto framesB = std::vector<Frame>{};
            framesB.emplace_back(
                grayFrame(
                    fingerprint,
                    matchingPixels(),
                    FrameId{18},
                    MonotonicInstant::now()
                )
            );
            auto underB = makeSession(std::move(framesB), baseConfig(fingerprint));
            REQUIRE(underB.session.has_value());

            auto observation = underA.session->observe();
            REQUIRE(observation.has_value());

            auto const receipt = underB.session->movePointer(
                std::move(*observation),
                PixelPoint{1, 0}
            );
            REQUIRE_FALSE(receipt.has_value());
            requireErrorKind(receipt.error(), AutomationErrorKind::InternalInvariant);
            CHECK(underB.clicks->moveCount() == 0);
        }
    }

    TEST_CASE("engine session cancels a click requested after the observation")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto cancellation   = std::stop_source{};
        auto config         = baseConfig(fingerprint);
        config.cancellation = cancellation.get_token();
        auto under          = matchingSession(fingerprint, std::move(config));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        // A stop requested between the observation and delivery aborts the click
        // before authorization or any sink call.
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);

        auto const receipt = session.clickPoint(
            std::move(*observation),
            PixelPoint{1, 0}
        );
        REQUIRE_FALSE(receipt.has_value());
        requireErrorKind(receipt.error(), AutomationErrorKind::Cancelled);
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session cancels an observe requested before capture")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);
        auto cancellation      = std::stop_source{};
        auto const didRequest  = cancellation.request_stop();
        REQUIRE(didRequest);
        auto config         = baseConfig(fingerprint);
        config.cancellation = cancellation.get_token();
        auto under          = matchingSession(fingerprint, std::move(config));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto const observation = session.observe();
        REQUIRE_FALSE(observation.has_value());
        requireErrorKind(observation.error(), AutomationErrorKind::Cancelled);
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session bounds a blocking capture with the configured deadline")
    {
        auto const fingerprint = fingerprintOf(3, 1, 96);

        auto cancellation   = std::stop_source{};
        auto config         = baseConfig(fingerprint);
        config.cancellation = cancellation.get_token();
        config.captureTimeout = std::chrono::duration_cast<
            MonotonicInstant::Duration
        >(std::chrono::milliseconds{50});
        auto const captureTimeout = config.captureTimeout;

        auto frameSource     = std::make_unique<DeadlineHonouringFrameSource>();
        auto* const p_source = frameSource.get();
        auto recorder        = std::make_unique<trace::TraceRecorder>(
            std::make_unique<CollectingTraceSink>(),
            k_runId,
            k_generationId,
            trace::FrontEnd::Task
        );
        auto under = EngineSession::create(
            std::move(frameSource),
            std::make_unique<CountingActionSink>(),
            *recorder,
            std::move(config)
        );
        REQUIRE(under.has_value());

        // A source waiting for a frame that never arrives returns at the deadline
        // instead of hanging, which is the whole of the port's contract.
        auto const start       = MonotonicInstant::now();
        auto const observation = under->observe();
        auto const elapsed     = MonotonicInstant::now().saturatingDurationSince(start);

        REQUIRE_FALSE(observation.has_value());
        requireErrorKind(observation.error(), AutomationErrorKind::Timeout);
        CHECK(
            elapsed < std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::seconds{5}
            )
        );

        // The configured budget reached the port, not a placeholder an adapter
        // could satisfy by returning at once: the deadline is at least the
        // capture timeout away and nowhere near the five-second recognition one.
        auto const floor = start.checkedAdd(captureTimeout);
        REQUIRE(floor.has_value());
        auto const ceiling = start.checkedAdd(
            captureTimeout
            + std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::seconds{2}
            )
        );
        REQUIRE(ceiling.has_value());
        REQUIRE(p_source->deadline().has_value());
        CHECK(*p_source->deadline() >= *floor);
        CHECK(*p_source->deadline() <= *ceiling);
        CHECK(p_source->cancellable());
    }
}
