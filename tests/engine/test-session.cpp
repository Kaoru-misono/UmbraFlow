#include "../annotation/test-helpers.hpp"

#include <session.hpp>

#include <annotation/resource.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/runtime-manifest.hpp>

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

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

namespace uf::engine
{
    namespace
    {
        namespace anno = annotation;

        constexpr auto k_anchorAId  = "00000000-0000-0000-0000-000000000011";
        constexpr auto k_anchorBId  = "00000000-0000-0000-0000-000000000012";
        constexpr auto k_actionId   = "00000000-0000-0000-0000-000000000013";
        constexpr auto k_pageAId    = "00000000-0000-0000-0000-000000000111";
        constexpr auto k_awayPageId = "00000000-0000-0000-0000-000000000112";

        // The identity the session's recorder stamps. The engine never authors it
        // -- task::TaskHost does -- so any fixed pair proves the same thing:
        // every engine event lands in one identified run.
        constexpr auto k_runId        = TaskRunId{11};
        constexpr auto k_generationId = GenerationId{2};

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        // A one-by-one grey RGBA template encoded as a PNG, addressed by its
        // content hash. Mirrors the synthetic template fixture the recognition
        // runtime tests use so page and action evaluation behave identically.
        [[nodiscard]]
        auto encodedTemplate(uint8 gray) -> anno::EncodedRuntimeTemplate
        {
            auto const rgba = std::vector<std::byte>{
                asByte(gray),
                asByte(gray),
                asByte(gray),
                asByte(255),
            };
            auto encoded = image::encodeRgbaPng("engine-session-template.png", 1, 1, rgba);
            REQUIRE(encoded.has_value());
            auto const hash = anno::sha256(*encoded);
            REQUIRE(hash.has_value());
            return anno::EncodedRuntimeTemplate{
                .hash     = *hash,
                .pngBytes = *std::move(encoded),
            };
        }

        struct RuntimeParts final
        {
            LoadedRuntime            loaded;
            anno::ProjectFingerprint fingerprint;
            anno::RecognizerId       actionTarget;
        };

        // A runtime with one page (page_a, required anchor_a) and one action
        // target authorized for that page. A frame carrying grey 2 and grey 5
        // resolves page_a and the action target hits.
        [[nodiscard]]
        auto singlePageRuntime() -> RuntimeParts
        {
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto const anchorA     = anno::test::recognizerId(k_anchorAId);
            auto const actionT     = anno::test::recognizerId(k_actionId);
            auto const pageA       = anno::test::pageId(k_pageAId);
            auto anchorTemplate = encodedTemplate(2);
            auto actionTemplate = encodedTemplate(5);
            auto const sourceBytes = std::array{asByte(42)};
            auto const sourceHash  = anno::sha256(sourceBytes);
            REQUIRE(sourceHash.has_value());

            auto manifest = anno::RuntimeManifest::create(
                anno::test::projectId("personal.engine_session"),
                fingerprint,
                {
                    anno::RuntimeRecognizerSpec{
                        .definition = anno::test::recognizer(
                            fingerprint,
                            anchorA,
                            "anchor_a",
                            anno::AnnotationType::PageAnchor,
                            anno::test::pixelRect(0, 0, 1, 1),
                            anno::test::pixelRect(0, 0, 3, 1),
                            {},
                            std::nullopt,
                            anno::test::threshold(10'000)
                        ),
                        .templateHash = anchorTemplate.hash,
                        .sourceHash   = *sourceHash,
                    },
                    anno::RuntimeRecognizerSpec{
                        .definition = anno::test::recognizer(
                            fingerprint,
                            actionT,
                            "action_target",
                            anno::AnnotationType::ActionTarget,
                            anno::test::pixelRect(0, 0, 1, 1),
                            anno::test::pixelRect(0, 0, 3, 1),
                            {pageA},
                            std::nullopt,
                            anno::test::threshold(10'000)
                        ),
                        .templateHash = actionTemplate.hash,
                        .sourceHash   = *sourceHash,
                    },
                },
                {anno::test::page(pageA, "page_a", {anchorA})}
            );
            REQUIRE(manifest.has_value());
            auto templates = std::vector<anno::EncodedRuntimeTemplate>{};
            templates.emplace_back(std::move(anchorTemplate));
            templates.emplace_back(std::move(actionTemplate));
            auto runtime = anno::RecognitionRuntime::create(
                *std::move(manifest),
                std::move(templates)
            );
            REQUIRE(runtime.has_value());
            return RuntimeParts{
                .loaded       = LoadedRuntime{.runtime = *std::move(runtime)},
                .fingerprint  = fingerprint,
                .actionTarget = actionT,
            };
        }

        // A runtime whose action target is authorized only for the away page,
        // while a grey-2 frame resolves the home page. Exercises the coordinate
        // authorization refusal without contriving a malformed detection.
        [[nodiscard]]
        auto wrongPageRuntime() -> RuntimeParts
        {
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto const anchorA     = anno::test::recognizerId(k_anchorAId);
            auto const anchorB     = anno::test::recognizerId(k_anchorBId);
            auto const actionT     = anno::test::recognizerId(k_actionId);
            auto const homePage    = anno::test::pageId(k_pageAId);
            auto const awayPage    = anno::test::pageId(k_awayPageId);
            auto anchorATemplate = encodedTemplate(2);
            auto anchorBTemplate = encodedTemplate(3);
            auto actionTemplate  = encodedTemplate(5);
            auto const sourceBytes = std::array{asByte(42)};
            auto const sourceHash  = anno::sha256(sourceBytes);
            REQUIRE(sourceHash.has_value());

            auto manifest = anno::RuntimeManifest::create(
                anno::test::projectId("personal.engine_session_wrong_page"),
                fingerprint,
                {
                    anno::RuntimeRecognizerSpec{
                        .definition = anno::test::recognizer(
                            fingerprint,
                            anchorA,
                            "anchor_a",
                            anno::AnnotationType::PageAnchor,
                            anno::test::pixelRect(0, 0, 1, 1),
                            anno::test::pixelRect(0, 0, 3, 1),
                            {},
                            std::nullopt,
                            anno::test::threshold(10'000)
                        ),
                        .templateHash = anchorATemplate.hash,
                        .sourceHash   = *sourceHash,
                    },
                    anno::RuntimeRecognizerSpec{
                        .definition = anno::test::recognizer(
                            fingerprint,
                            anchorB,
                            "anchor_b",
                            anno::AnnotationType::PageAnchor,
                            anno::test::pixelRect(0, 0, 1, 1),
                            anno::test::pixelRect(0, 0, 3, 1),
                            {},
                            std::nullopt,
                            anno::test::threshold(10'000)
                        ),
                        .templateHash = anchorBTemplate.hash,
                        .sourceHash   = *sourceHash,
                    },
                    anno::RuntimeRecognizerSpec{
                        .definition = anno::test::recognizer(
                            fingerprint,
                            actionT,
                            "action_target",
                            anno::AnnotationType::ActionTarget,
                            anno::test::pixelRect(0, 0, 1, 1),
                            anno::test::pixelRect(0, 0, 3, 1),
                            {awayPage},
                            std::nullopt,
                            anno::test::threshold(10'000)
                        ),
                        .templateHash = actionTemplate.hash,
                        .sourceHash   = *sourceHash,
                    },
                },
                {
                    anno::test::page(homePage, "home", {anchorA}),
                    anno::test::page(awayPage, "away", {anchorB}),
                }
            );
            REQUIRE(manifest.has_value());
            auto templates = std::vector<anno::EncodedRuntimeTemplate>{};
            templates.emplace_back(std::move(anchorATemplate));
            templates.emplace_back(std::move(anchorBTemplate));
            templates.emplace_back(std::move(actionTemplate));
            auto runtime = anno::RecognitionRuntime::create(
                *std::move(manifest),
                std::move(templates)
            );
            REQUIRE(runtime.has_value());
            return RuntimeParts{
                .loaded       = LoadedRuntime{.runtime = *std::move(runtime)},
                .fingerprint  = fingerprint,
                .actionTarget = actionT,
            };
        }

        [[nodiscard]]
        auto grayFrame(
            anno::ProjectFingerprint fingerprint,
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

        [[nodiscard]]
        auto resolvingPixels() -> std::vector<std::byte>
        {
            return std::vector<std::byte>{asByte(2), asByte(5), asByte(0)};
        }

        [[nodiscard]]
        auto unknownPixels() -> std::vector<std::byte>
        {
            return std::vector<std::byte>{asByte(0), asByte(0), asByte(0)};
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

            // Flips the bound-target revalidation to fail, modeling the HWND-reuse
            // window the delivery-edge guard closes: the instance is valid during
            // observe and invalid by the time act revalidates it.
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

        // Blocks until the deadline its budget carries, then reports the expiry,
        // and records what it was handed.
        //
        // It is the one frame source in the suite that HONOURS its budget rather
        // than satisfying it vacuously. Every other fake returns at once, which
        // says nothing about a deadline, because it would return without one:
        // only a source that actually waits can show that the wait ends where the
        // session said it would, and only a source that keeps the budget can show
        // the session minted a real one rather than an instant an adapter would
        // be free to ignore.
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

            // Whether the token that arrived could ever request a stop. A default
            // std::stop_token cannot, so this tells a session that forwarded its
            // own cancel source from one that forwarded nothing.
            [[nodiscard]] auto cancellable() const noexcept -> bool
            {
                return m_cancellable;
            }
        };

        // Counts delivered clicks so fail-closed cases can assert that none
        // escaped, and keeps the last client point and lease so the happy path can
        // confirm the delivered lease still carries the observation's identity.
        class CountingActionSink final : public IActionSink
        {
            uint32                            m_clickCount{0};
            std::optional<Point<ClientSpace>> m_lastClick{};
            std::optional<ObservationLease>   m_lastLease{};

        public:
            [[nodiscard]]
            auto click(
                Point<ClientSpace> point,
                ObservationLease const& lease
            ) -> Status override
            {
                ++m_clickCount;
                m_lastClick = point;
                m_lastLease = lease;
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

        // Owns the run's recorder and the session built over it, plus non-owning
        // observers of the sinks living inside them. Declaration order is the
        // lifetime order the trace contract requires: the recorder outlives the
        // session that borrows it, and holding it through a unique_ptr keeps its
        // address stable when this struct is moved out of makeSession.
        struct SessionUnderTest final
        {
            std::unique_ptr<trace::TraceRecorder> recorder{};
            Result<EngineSession>                 session;
            FakeFrameSource*                      source{};
            CountingActionSink*                   clicks{};
            CollectingTraceSink*                  traces{};
        };

        [[nodiscard]]
        auto baseConfig(anno::ProjectFingerprint fingerprint) -> EngineSessionConfig
        {
            return EngineSessionConfig{
                .liveFingerprint         = fingerprint,
                .maximumPixelComparisons = 1'000,
                .recognitionTimeout      = std::chrono::duration_cast<
                    MonotonicInstant::Duration
                >(std::chrono::seconds{5}),
            };
        }

        [[nodiscard]]
        auto makeSession(
            RuntimeParts parts,
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
                k_generationId
            );
            auto session = EngineSession::create(
                std::move(parts.loaded),
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

        // The first event of `kind`, or null when the run never emitted one. The
        // returned pointer observes storage owned by the collecting sink behind
        // `events`, so it stays valid for as long as that sink lives -- which is
        // what the annotation on that parameter states.
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

    TEST_CASE("engine session observes resolves finds and acts into a single click")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto const actionT     = parts.actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        auto outcome = session.resolvePage(*observation);
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);

        auto found = session.findAction(*observation, actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE(receipt.has_value());
        CHECK(under.clicks->clickCount() == 1);
        CHECK(receipt->frameId == FrameId{17});
        REQUIRE(under.clicks->lastClick().has_value());
        CHECK(receipt->clickPoint == *under.clicks->lastClick());

        // The lease reaches the delivery layer unchanged, so its D0 fence fields
        // still identify the observed frame.
        REQUIRE(under.clicks->lastLease().has_value());
        CHECK(under.clicks->lastLease()->frameId() == FrameId{17});

        // engine-trace/v1's SessionStarted has no successor: the run's own
        // run.started, written by task::TaskHost, records the same instant with
        // the project, task, seed and run identity on it.
        auto const expected = std::vector<trace::TraceEventKind>{
            trace::TraceEventKind::EngineObserved,
            trace::TraceEventKind::EnginePageResolved,
            trace::TraceEventKind::EngineActionFound,
            trace::TraceEventKind::EngineActionAuthorized,
            trace::TraceEventKind::EngineActionDelivered,
            trace::TraceEventKind::EngineObservationInvalidated,
        };
        CHECK(kindsOf(under.traces->events()) == expected);

        // The two collapsed kinds keep their distinction in the outcome field,
        // and every event carries the frame identity that joins it to the capture.
        auto const* p_page = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EnginePageResolved
        );
        REQUIRE(p_page != nullptr);
        REQUIRE(p_page->page.has_value());
        CHECK(p_page->page->outcome == trace::PageResolution::Resolved);
        CHECK(p_page->page->pageId.has_value());

        auto const* p_action = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineActionFound
        );
        REQUIRE(p_action != nullptr);
        REQUIRE(p_action->action.has_value());
        CHECK(p_action->action->outcome == trace::ActionSearch::Found);
        CHECK(p_action->action->matchedRect.has_value());

        for (auto const& event : under.traces->events())
        {
            REQUIRE(event.frame.has_value());
            CHECK(event.frame->frameId() == FrameId{17});
        }
    }

    TEST_CASE("engine session fails closed when the live fingerprint does not match")
    {
        auto parts             = singlePageRuntime();
        auto const runtimePrint = parts.fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(runtimePrint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        // A different DPI makes the live fingerprint incompatible with the
        // project the runtime was built for.
        auto const config = baseConfig(anno::test::fingerprint(3, 1, 120, 120));
        auto under        = makeSession(std::move(parts), std::move(frames), config);
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const outcome = session.resolvePage(*observation);
        REQUIRE_FALSE(outcome.has_value());
        anno::test::requireErrorKind(
            outcome.error(),
            AutomationErrorKind::TargetCompatibilityUnverified
        );
        CHECK(under.clicks->clickCount() == 0);

        // The old Failure kind is now the Failed outcome of the step that failed,
        // so the trace also names which step that was.
        auto const* p_page = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EnginePageResolved
        );
        REQUIRE(p_page != nullptr);
        REQUIRE(p_page->page.has_value());
        CHECK(p_page->page->outcome == trace::PageResolution::Failed);
        CHECK(p_page->errorKind.has_value());
    }

    TEST_CASE("engine session refuses an action the recognizer does not authorize")
    {
        auto parts             = wrongPageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto const actionT     = parts.actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = session.resolvePage(*observation);
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);

        auto found = session.findAction(*observation, actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::ActionRejected);
        CHECK(under.clicks->clickCount() == 0);
        CHECK(
            findEvent(
                under.traces->events(),
                trace::TraceEventKind::EngineActionRejected
            )
            != nullptr
        );
    }

    TEST_CASE("engine session refuses an action whose observation lease has expired")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto const actionT     = parts.actionTarget;
        auto frames            = std::vector<Frame>{};
        // A frame captured at the clock epoch with a zero max age produces a
        // lease that is already expired by the time the action is delivered.
        frames.emplace_back(
            grayFrame(
                fingerprint,
                resolvingPixels(),
                FrameId{17},
                anno::test::instantAt(MonotonicInstant::Duration{0})
            )
        );

        auto config              = baseConfig(fingerprint);
        config.maxActionFrameAge = MonotonicInstant::Duration::zero();
        auto under               = makeSession(std::move(parts), std::move(frames), config);
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = session.resolvePage(*observation);
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);

        auto found = session.findAction(*observation, actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::StaleObservation);
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
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto const actionT     = parts.actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = session.resolvePage(*observation);
        REQUIRE(outcome.has_value());
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = session.findAction(*observation, actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE(receipt.has_value());
        CHECK(under.clicks->clickCount() == 1);

        // The consumed observation must fail closed on any further use.
        auto const reuse = session.resolvePage(*observation);
        REQUIRE_FALSE(reuse.has_value());
        anno::test::requireErrorKind(reuse.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->clickCount() == 1);
    }

    TEST_CASE("engine session invalidates a moved-from observation")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto source = session.observe();
        REQUIRE(source.has_value());

        // Moving the handle transfers it to the destination and leaves the source
        // dead, so the moved-from handle fails closed exactly like a consumed one.
        auto moved = *std::move(source);
        auto const reuse = session.resolvePage(*source);
        REQUIRE_FALSE(reuse.has_value());
        anno::test::requireErrorKind(reuse.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session move preserves observations already vended")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.session.has_value());

        auto observation = under.session->observe();
        REQUIRE(observation.has_value());

        auto session = std::move(*under.session);
        auto outcome = session.resolvePage(*observation);
        REQUIRE(outcome.has_value());
        CHECK(std::holds_alternative<anno::ResolvedPage>(*outcome));
    }

    TEST_CASE("engine session rejects an observation vended by a different session")
    {
        auto partsA            = singlePageRuntime();
        auto const fingerprint = partsA.fingerprint;
        auto const actionT     = partsA.actionTarget;
        auto framesA           = std::vector<Frame>{};
        framesA.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );
        auto underA = makeSession(std::move(partsA), std::move(framesA), baseConfig(fingerprint));
        REQUIRE(underA.session.has_value());
        auto& sessionA = *underA.session;

        // A second session built over an equivalent runtime. The observation from
        // session A must never authorize a click on session B.
        auto partsB  = singlePageRuntime();
        auto framesB = std::vector<Frame>{};
        framesB.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{18}, MonotonicInstant::now())
        );
        auto underB = makeSession(std::move(partsB), std::move(framesB), baseConfig(fingerprint));
        REQUIRE(underB.session.has_value());
        auto& sessionB = *underB.session;

        auto observation = sessionA.observe();
        REQUIRE(observation.has_value());

        auto const foreignResolve = sessionB.resolvePage(*observation);
        REQUIRE_FALSE(foreignResolve.has_value());
        anno::test::requireErrorKind(
            foreignResolve.error(),
            AutomationErrorKind::InternalInvariant
        );
        auto const foreignFind = sessionB.findAction(*observation, actionT);
        REQUIRE_FALSE(foreignFind.has_value());
        anno::test::requireErrorKind(
            foreignFind.error(),
            AutomationErrorKind::InternalInvariant
        );

        auto outcome = sessionA.resolvePage(*observation);
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = sessionA.findAction(*observation, actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto const receipt = sessionB.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::InternalInvariant);
        CHECK(underB.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session invalidates the handle before a failing post-click trace")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto const actionT     = parts.actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
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
        };
        auto session = EngineSession::create(
            std::move(parts.loaded),
            std::move(frameSource),
            std::move(actionSink),
            recorder,
            baseConfig(fingerprint)
        );
        REQUIRE(session.has_value());

        auto observation = session->observe();
        REQUIRE(observation.has_value());
        auto outcome = session->resolvePage(*observation);
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = session->findAction(*observation, actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        // act takes Observation&&, so binding the rvalue through a named handle
        // keeps a caller-side alias that survives the call. The first act delivers
        // the click, then the ClickDelivered emit fails and propagates.
        auto handle        = *std::move(observation);
        auto const receipt = session->act(std::move(handle), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::IoFailure);
        CHECK(p_clicks->clickCount() == 1);

        // The delivery already invalidated the handle, so retrying with the
        // surviving alias fails closed and cannot double-deliver.
        auto const retry = session->act(std::move(handle), resolved, **found);
        REQUIRE_FALSE(retry.has_value());
        anno::test::requireErrorKind(retry.error(), AutomationErrorKind::StaleObservation);
        CHECK(p_clicks->clickCount() == 1);
    }

    TEST_CASE("engine session resolves an unrecognized frame to an unknown page")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto frames            = std::vector<Frame>{};
        // Grey-0 pixels match no page anchor, so no page signature resolves.
        frames.emplace_back(
            grayFrame(fingerprint, unknownPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = session.resolvePage(*observation);
        REQUIRE(outcome.has_value());

        // The outcome is the UnknownPage alternative: no ResolvedPage exists, so
        // there is nothing of the type act requires to feed a click.
        CHECK(std::holds_alternative<anno::UnknownPage>(*outcome));
        CHECK_FALSE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const* p_page = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EnginePageResolved
        );
        REQUIRE(p_page != nullptr);
        REQUIRE(p_page->page.has_value());
        CHECK(p_page->page->outcome == trace::PageResolution::Unknown);
        CHECK_FALSE(p_page->page->pageId.has_value());
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session surfaces a recognition budget stop as a failure")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto const actionT     = parts.actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto config                    = baseConfig(fingerprint);
        config.maximumPixelComparisons = 0;
        auto under                     = makeSession(std::move(parts), std::move(frames), config);
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const found = session.findAction(*observation, actionT);
        REQUIRE_FALSE(found.has_value());
        anno::test::requireErrorKind(
            found.error(),
            AutomationErrorKind::RecognitionIncomplete
        );
        // The search never looked, so the session must not report the absence a
        // completed miss reports: a miss returns an empty optional with no error,
        // and this returns an error whose response is Retry.
        CHECK(failureResponse(found.error()) == FailureResponse::Retry);
        CHECK(under.clicks->clickCount() == 0);

        // The old stage-blind RecognitionStopped kind is now the Stopped outcome
        // of the action search, which additionally names the stage it stopped in.
        auto const* p_action = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EngineActionFound
        );
        REQUIRE(p_action != nullptr);
        REQUIRE(p_action->action.has_value());
        CHECK(p_action->action->outcome == trace::ActionSearch::Stopped);
        CHECK(p_action->stopReason == SadSearchStopReason::ComparisonBudgetExhausted);
    }

    TEST_CASE("engine session surfaces a cancelled recognition as a cancellation")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto cancellation   = std::stop_source{};
        auto config         = baseConfig(fingerprint);
        config.cancellation = cancellation.get_token();
        auto under          = makeSession(std::move(parts), std::move(frames), std::move(config));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        // observe() now gates on cancellation, so the stop is requested only after
        // the observation exists. The recognition policy then surfaces it as
        // Cancelled through resolvePage rather than the observe or delivery guards.
        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);

        auto const outcome = session.resolvePage(*observation);
        REQUIRE_FALSE(outcome.has_value());
        anno::test::requireErrorKind(outcome.error(), AutomationErrorKind::Cancelled);
        CHECK(under.clicks->clickCount() == 0);

        auto const* p_page = findEvent(
            under.traces->events(),
            trace::TraceEventKind::EnginePageResolved
        );
        REQUIRE(p_page != nullptr);
        REQUIRE(p_page->page.has_value());
        CHECK(p_page->page->outcome == trace::PageResolution::Stopped);
        CHECK(p_page->stopReason == SadSearchStopReason::Cancelled);
    }

    TEST_CASE("engine session revalidates the target instance at the delivery edge")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto const actionT     = parts.actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = session.resolvePage(*observation);
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = session.findAction(*observation, actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        // The bound target instance passed validation during observe but is
        // switched to fail before delivery, modeling an HWND reused between the
        // two. The delivery-edge revalidation rejects the click before the sink.
        under.source->invalidateTargetInstance();

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::TargetUnavailable);
        CHECK(under.clicks->clickCount() == 0);
        CHECK(
            findEvent(
                under.traces->events(),
                trace::TraceEventKind::EngineActionRejected
            )
            != nullptr
        );
    }

    TEST_CASE("engine session cancels an act requested after the observation")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto const actionT     = parts.actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto cancellation   = std::stop_source{};
        auto config         = baseConfig(fingerprint);
        config.cancellation = cancellation.get_token();
        auto under          = makeSession(std::move(parts), std::move(frames), std::move(config));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = session.resolvePage(*observation);
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = session.findAction(*observation, actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        // A stop requested between the observation and delivery aborts the click
        // before authorization or any sink call.
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::Cancelled);
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session cancels an observe requested before capture")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto cancellation     = std::stop_source{};
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);
        auto config         = baseConfig(fingerprint);
        config.cancellation = cancellation.get_token();
        auto under          = makeSession(std::move(parts), std::move(frames), std::move(config));
        REQUIRE(under.session.has_value());
        auto& session = *under.session;

        auto const observation = session.observe();
        REQUIRE_FALSE(observation.has_value());
        anno::test::requireErrorKind(observation.error(), AutomationErrorKind::Cancelled);
        CHECK(under.clicks->clickCount() == 0);
    }

    TEST_CASE("engine session bounds a blocking capture with the configured deadline")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.fingerprint;

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
            k_generationId
        );
        auto under = EngineSession::create(
            std::move(parts.loaded),
            std::move(frameSource),
            std::make_unique<CountingActionSink>(),
            *recorder,
            std::move(config)
        );
        REQUIRE(under.has_value());

        // A source that waits for a frame that never arrives returns at the
        // deadline instead of hanging, which is the whole content of the port's
        // new contract.
        auto const start       = MonotonicInstant::now();
        auto const observation = under->observe();
        auto const elapsed     = MonotonicInstant::now().saturatingDurationSince(start);

        REQUIRE_FALSE(observation.has_value());
        anno::test::requireErrorKind(observation.error(), AutomationErrorKind::Timeout);
        CHECK(
            elapsed < std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::seconds{5}
            )
        );

        // What reached the port was the configured budget, not a placeholder an
        // adapter could satisfy by returning immediately: the deadline is at
        // least the configured timeout away and nowhere near the five-second
        // recognition timeout, and the token can actually request a stop.
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
