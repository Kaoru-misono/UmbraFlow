#include "../annotation/test-helpers.hpp"

#include <session.hpp>

#include <annotation/catalog.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/runtime-manifest.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace uf::engine
{
    namespace
    {
        namespace anno = annotation;

        constexpr auto g_anchorAId  = "00000000-0000-0000-0000-000000000011";
        constexpr auto g_anchorBId  = "00000000-0000-0000-0000-000000000012";
        constexpr auto g_actionId   = "00000000-0000-0000-0000-000000000013";
        constexpr auto g_pageAId    = "00000000-0000-0000-0000-000000000111";
        constexpr auto g_awayPageId = "00000000-0000-0000-0000-000000000112";

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
                .m_hash     = *hash,
                .m_pngBytes = *std::move(encoded),
            };
        }

        struct RuntimeParts final
        {
            LoadedRuntime            m_loaded;
            anno::ProjectFingerprint m_fingerprint;
            anno::RecognizerId       m_actionTarget;
            anno::PageId             m_page;
        };

        // A runtime with one page (page_a, required anchor_a) and one action
        // target authorized for that page. A frame carrying grey 2 and grey 5
        // resolves page_a and the action target hits.
        [[nodiscard]]
        auto singlePageRuntime() -> RuntimeParts
        {
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto const anchorA     = anno::test::recognizerId(g_anchorAId);
            auto const actionT     = anno::test::recognizerId(g_actionId);
            auto const pageA       = anno::test::pageId(g_pageAId);
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
                        .m_definition = anno::test::recognizer(
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
                        .m_templateHash = anchorTemplate.m_hash,
                        .m_sourceHash   = *sourceHash,
                    },
                    anno::RuntimeRecognizerSpec{
                        .m_definition = anno::test::recognizer(
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
                        .m_templateHash = actionTemplate.m_hash,
                        .m_sourceHash   = *sourceHash,
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
                .m_loaded       = LoadedRuntime{.m_runtime = *std::move(runtime)},
                .m_fingerprint  = fingerprint,
                .m_actionTarget = actionT,
                .m_page         = pageA,
            };
        }

        // A runtime whose action target is authorized only for the away page,
        // while a grey-2 frame resolves the home page. Exercises the coordinate
        // authorization refusal without contriving a malformed detection.
        [[nodiscard]]
        auto wrongPageRuntime() -> RuntimeParts
        {
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto const anchorA     = anno::test::recognizerId(g_anchorAId);
            auto const anchorB     = anno::test::recognizerId(g_anchorBId);
            auto const actionT     = anno::test::recognizerId(g_actionId);
            auto const homePage    = anno::test::pageId(g_pageAId);
            auto const awayPage    = anno::test::pageId(g_awayPageId);
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
                        .m_definition = anno::test::recognizer(
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
                        .m_templateHash = anchorATemplate.m_hash,
                        .m_sourceHash   = *sourceHash,
                    },
                    anno::RuntimeRecognizerSpec{
                        .m_definition = anno::test::recognizer(
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
                        .m_templateHash = anchorBTemplate.m_hash,
                        .m_sourceHash   = *sourceHash,
                    },
                    anno::RuntimeRecognizerSpec{
                        .m_definition = anno::test::recognizer(
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
                        .m_templateHash = actionTemplate.m_hash,
                        .m_sourceHash   = *sourceHash,
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
                .m_loaded       = LoadedRuntime{.m_runtime = *std::move(runtime)},
                .m_fingerprint  = fingerprint,
                .m_actionTarget = actionT,
                .m_page         = homePage,
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
                SessionId{7},
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
        class FakeFrameSource final : public FrameSource
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

            [[nodiscard]] auto capture() -> Result<Frame> override
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

        // Counts delivered clicks so fail-closed cases can assert that none
        // escaped, and keeps the last client point and lease so the happy path can
        // confirm the delivered lease still carries the observation's identity.
        class CountingActionSink final : public ActionSink
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

        class CollectingTraceSink final : public TraceSink
        {
            std::vector<TraceEvent> m_events{};

        public:
            [[nodiscard]] auto emit(TraceEvent const& event) -> Status override
            {
                m_events.emplace_back(event);
                return ok();
            }

            [[nodiscard]]
            auto events() const noexcept UF_LIFETIME_BOUND -> std::span<TraceEvent const>
            {
                return m_events;
            }
        };

        // Records every event but fails the emit whose kind matches a nominated
        // trigger, so a fallible post-click trace can be exercised deterministically.
        class FailOnKindTraceSink final : public TraceSink
        {
            std::vector<TraceEvent> m_events{};
            TraceEventKind          m_failKind;

        public:
            explicit FailOnKindTraceSink(TraceEventKind failKind) noexcept
                : m_failKind{failKind}
            {
            }

            [[nodiscard]] auto emit(TraceEvent const& event) -> Status override
            {
                m_events.emplace_back(event);
                if (event.m_kind == m_failKind)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "trace sink failed to emit"
                    );
                }
                return ok();
            }
        };

        // Owns a constructed session plus non-owning observers of its sinks. The
        // sink objects live inside the session's unique_ptr members, so these
        // pointers stay valid for as long as the session member is alive.
        struct SessionUnderTest final
        {
            Result<EngineSession> m_session;
            FakeFrameSource*      m_source{};
            CountingActionSink*   m_clicks{};
            CollectingTraceSink*  m_traces{};
        };

        [[nodiscard]]
        auto baseConfig(anno::ProjectFingerprint fingerprint) -> EngineSessionConfig
        {
            return EngineSessionConfig{
                .m_liveFingerprint         = fingerprint,
                .m_maximumPixelComparisons = 1'000,
                .m_recognitionTimeout      = std::chrono::duration_cast<
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
            auto session = EngineSession::create(
                std::move(parts.m_loaded),
                std::move(frameSource),
                std::move(actionSink),
                std::move(traceSink),
                std::move(config)
            );
            return SessionUnderTest{
                .m_session = std::move(session),
                .m_source  = p_source,
                .m_clicks  = p_clicks,
                .m_traces  = p_traces,
            };
        }

        [[nodiscard]]
        auto kindsOf(std::span<TraceEvent const> events) -> std::vector<TraceEventKind>
        {
            auto kinds = std::vector<TraceEventKind>{};
            kinds.reserve(events.size());
            for (auto const& event : events)
            {
                kinds.emplace_back(event.m_kind);
            }
            return kinds;
        }

        [[nodiscard]]
        auto traceContains(
            std::span<TraceEvent const> events,
            TraceEventKind kind
        ) -> bool
        {
            return std::ranges::any_of(
                events,
                [kind](TraceEvent const& event) noexcept
                {
                    return event.m_kind == kind;
                }
            );
        }
    }

    TEST_CASE("engine session observes resolves finds and acts into a single click")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const actionT     = parts.m_actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());

        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);

        auto found = observation->findAction(actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE(receipt.has_value());
        CHECK(under.m_clicks->clickCount() == 1);
        CHECK(receipt->m_frameId == FrameId{17});
        REQUIRE(under.m_clicks->lastClick().has_value());
        CHECK(receipt->m_clickPoint == *under.m_clicks->lastClick());

        // The lease reaches the delivery layer unchanged, so its D0 fence fields
        // still identify the observed frame.
        REQUIRE(under.m_clicks->lastLease().has_value());
        CHECK(under.m_clicks->lastLease()->frameId() == FrameId{17});

        auto const expected = std::vector<TraceEventKind>{
            TraceEventKind::SessionStarted,
            TraceEventKind::Observed,
            TraceEventKind::PageResolved,
            TraceEventKind::ActionFound,
            TraceEventKind::ActionAuthorized,
            TraceEventKind::ClickDelivered,
            TraceEventKind::ObservationInvalidated,
        };
        CHECK(kindsOf(under.m_traces->events()) == expected);
    }

    TEST_CASE("engine session fails closed when the live fingerprint does not match")
    {
        auto parts             = singlePageRuntime();
        auto const runtimePrint = parts.m_fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(runtimePrint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        // A different DPI makes the live fingerprint incompatible with the
        // project the runtime was built for.
        auto const config = baseConfig(anno::test::fingerprint(3, 1, 120, 120));
        auto under        = makeSession(std::move(parts), std::move(frames), config);
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const outcome = observation->resolvePage();
        REQUIRE_FALSE(outcome.has_value());
        anno::test::requireErrorKind(
            outcome.error(),
            AutomationErrorKind::TargetCompatibilityUnverified
        );
        CHECK(under.m_clicks->clickCount() == 0);
        CHECK(traceContains(under.m_traces->events(), TraceEventKind::Failure));
    }

    TEST_CASE("engine session refuses an action the recognizer does not authorize")
    {
        auto parts             = wrongPageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const actionT     = parts.m_actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);

        auto found = observation->findAction(actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::ActionRejected);
        CHECK(under.m_clicks->clickCount() == 0);
        CHECK(traceContains(under.m_traces->events(), TraceEventKind::ActionRejected));
    }

    TEST_CASE("engine session refuses an action whose observation lease has expired")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const actionT     = parts.m_actionTarget;
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

        auto config                = baseConfig(fingerprint);
        config.m_maxActionFrameAge = MonotonicInstant::Duration::zero();
        auto under                 = makeSession(std::move(parts), std::move(frames), config);
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);

        auto found = observation->findAction(actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.m_clicks->clickCount() == 0);
        CHECK(traceContains(under.m_traces->events(), TraceEventKind::ActionRejected));
    }

    TEST_CASE("engine session rejects reuse of an invalidated observation")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const actionT     = parts.m_actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = observation->findAction(actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE(receipt.has_value());
        CHECK(under.m_clicks->clickCount() == 1);

        // The consumed observation must fail closed on any further use.
        auto const reuse = observation->resolvePage();
        REQUIRE_FALSE(reuse.has_value());
        anno::test::requireErrorKind(reuse.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.m_clicks->clickCount() == 1);
    }

    TEST_CASE("engine session invalidates a moved-from observation")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto source = session.observe();
        REQUIRE(source.has_value());

        // Moving the handle transfers it to the destination and leaves the source
        // dead, so the moved-from handle fails closed exactly like a consumed one.
        auto moved = *std::move(source);
        auto const reuse = source->resolvePage();
        REQUIRE_FALSE(reuse.has_value());
        anno::test::requireErrorKind(reuse.error(), AutomationErrorKind::StaleObservation);
        CHECK(under.m_clicks->clickCount() == 0);
    }

    TEST_CASE("engine session rejects an observation vended by a different session")
    {
        auto partsA            = singlePageRuntime();
        auto const fingerprint = partsA.m_fingerprint;
        auto const actionT     = partsA.m_actionTarget;
        auto framesA           = std::vector<Frame>{};
        framesA.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );
        auto underA = makeSession(std::move(partsA), std::move(framesA), baseConfig(fingerprint));
        REQUIRE(underA.m_session.has_value());
        auto& sessionA = *underA.m_session;

        // A second session built over an equivalent runtime. The observation from
        // session A must never authorize a click on session B.
        auto partsB  = singlePageRuntime();
        auto framesB = std::vector<Frame>{};
        framesB.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{18}, MonotonicInstant::now())
        );
        auto underB = makeSession(std::move(partsB), std::move(framesB), baseConfig(fingerprint));
        REQUIRE(underB.m_session.has_value());
        auto& sessionB = *underB.m_session;

        auto observation = sessionA.observe();
        REQUIRE(observation.has_value());
        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = observation->findAction(actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        auto const receipt = sessionB.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::InternalInvariant);
        CHECK(underB.m_clicks->clickCount() == 0);
    }

    TEST_CASE("engine session invalidates the handle before a failing post-click trace")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const actionT     = parts.m_actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        // A trace sink that fails exactly on the ClickDelivered emit, i.e. the
        // first fallible step after the click has already landed.
        auto frameSource = std::make_unique<FakeFrameSource>(std::move(frames));
        auto actionSink  = std::make_unique<CountingActionSink>();
        auto traceSink   = std::make_unique<FailOnKindTraceSink>(
            TraceEventKind::ClickDelivered
        );
        auto* const p_clicks = actionSink.get();
        auto session         = EngineSession::create(
            std::move(parts.m_loaded),
            std::move(frameSource),
            std::move(actionSink),
            std::move(traceSink),
            baseConfig(fingerprint)
        );
        REQUIRE(session.has_value());

        auto observation = session->observe();
        REQUIRE(observation.has_value());
        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = observation->findAction(actionT);
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
        auto const fingerprint = parts.m_fingerprint;
        auto frames            = std::vector<Frame>{};
        // Grey-0 pixels match no page anchor, so no page signature resolves.
        frames.emplace_back(
            grayFrame(fingerprint, unknownPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());

        // The outcome is the UnknownPage alternative: no ResolvedPage exists, so
        // there is nothing of the type act requires to feed a click.
        CHECK(std::holds_alternative<anno::UnknownPage>(*outcome));
        CHECK_FALSE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        CHECK(traceContains(under.m_traces->events(), TraceEventKind::PageUnknown));
        CHECK(under.m_clicks->clickCount() == 0);
    }

    TEST_CASE("engine session surfaces a recognition budget stop as a failure")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const actionT     = parts.m_actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto config                      = baseConfig(fingerprint);
        config.m_maximumPixelComparisons = 0;
        auto under                       = makeSession(std::move(parts), std::move(frames), config);
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const found = observation->findAction(actionT);
        REQUIRE_FALSE(found.has_value());
        anno::test::requireErrorKind(found.error(), AutomationErrorKind::RecognitionFailed);
        CHECK(under.m_clicks->clickCount() == 0);
        CHECK(traceContains(under.m_traces->events(), TraceEventKind::RecognitionStopped));
    }

    TEST_CASE("engine session surfaces a cancelled recognition as a cancellation")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto cancellation     = std::stop_source{};
        auto config           = baseConfig(fingerprint);
        config.m_cancellation = cancellation.get_token();
        auto under            = makeSession(std::move(parts), std::move(frames), std::move(config));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        // observe() now gates on cancellation, so the stop is requested only after
        // the observation exists. The recognition policy then surfaces it as
        // Cancelled through resolvePage rather than the observe or delivery guards.
        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);

        auto const outcome = observation->resolvePage();
        REQUIRE_FALSE(outcome.has_value());
        anno::test::requireErrorKind(outcome.error(), AutomationErrorKind::Cancelled);
        CHECK(under.m_clicks->clickCount() == 0);
        CHECK(traceContains(under.m_traces->events(), TraceEventKind::RecognitionStopped));
    }

    TEST_CASE("engine session revalidates the target instance at the delivery edge")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const actionT     = parts.m_actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = observation->findAction(actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        // The bound target instance passed validation during observe but is
        // switched to fail before delivery, modeling an HWND reused between the
        // two. The delivery-edge revalidation rejects the click before the sink.
        under.m_source->invalidateTargetInstance();

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::TargetUnavailable);
        CHECK(under.m_clicks->clickCount() == 0);
        CHECK(traceContains(under.m_traces->events(), TraceEventKind::ActionRejected));
    }

    TEST_CASE("engine session cancels an act requested after the observation")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const actionT     = parts.m_actionTarget;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto cancellation     = std::stop_source{};
        auto config           = baseConfig(fingerprint);
        config.m_cancellation = cancellation.get_token();
        auto under            = makeSession(std::move(parts), std::move(frames), std::move(config));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto observation = session.observe();
        REQUIRE(observation.has_value());
        auto outcome = observation->resolvePage();
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        auto const& resolved = std::get<anno::ResolvedPage>(*outcome);
        auto found           = observation->findAction(actionT);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());

        // A stop requested between the observation and delivery aborts the click
        // before authorization or any sink call.
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);

        auto const receipt = session.act(std::move(*observation), resolved, **found);
        REQUIRE_FALSE(receipt.has_value());
        anno::test::requireErrorKind(receipt.error(), AutomationErrorKind::Cancelled);
        CHECK(under.m_clicks->clickCount() == 0);
    }

    TEST_CASE("engine session cancels an observe requested before capture")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto cancellation     = std::stop_source{};
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);
        auto config           = baseConfig(fingerprint);
        config.m_cancellation = cancellation.get_token();
        auto under            = makeSession(std::move(parts), std::move(frames), std::move(config));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto const observation = session.observe();
        REQUIRE_FALSE(observation.has_value());
        anno::test::requireErrorKind(observation.error(), AutomationErrorKind::Cancelled);
        CHECK(under.m_clicks->clickCount() == 0);
    }

    TEST_CASE("engine session waitForPage returns promptly when cancelled mid-sleep")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const pageA       = parts.m_page;
        auto frames            = std::vector<Frame>{};
        // Grey-0 pixels never resolve pageA, so the wait loop keeps polling until
        // the timeout, the deadline, or a cancellation ends it.
        frames.emplace_back(
            grayFrame(fingerprint, unknownPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto cancellation     = std::stop_source{};
        auto config           = baseConfig(fingerprint);
        config.m_cancellation = cancellation.get_token();
        auto under            = makeSession(std::move(parts), std::move(frames), std::move(config));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        // Request stop ~50ms into a 10s poll sleep from another thread. The
        // sliced sleep observes it within one 100ms slice, so waitForPage returns
        // Cancelled far sooner than the poll interval. Copying the stop_source
        // shares its stop-state, so the worker needs no reference capture.
        auto const start = MonotonicInstant::now();
        auto stopper     = std::jthread{
            [source = cancellation]() mutable noexcept
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                static_cast<void>(source.request_stop());
            }
        };

        auto const result = session.waitForPage(
            pageA,
            std::chrono::duration_cast<MonotonicInstant::Duration>(std::chrono::seconds{60}),
            std::chrono::duration_cast<MonotonicInstant::Duration>(std::chrono::seconds{10})
        );
        auto const elapsed = MonotonicInstant::now().saturatingDurationSince(start);

        REQUIRE_FALSE(result.has_value());
        anno::test::requireErrorKind(result.error(), AutomationErrorKind::Cancelled);
        CHECK(
            elapsed < std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::seconds{2}
            )
        );
        CHECK(under.m_clicks->clickCount() == 0);
    }

    TEST_CASE("engine session waitForPage times out when the page never resolves")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const pageA       = parts.m_page;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, unknownPixels(), FrameId{17}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto const result = session.waitForPage(
            pageA,
            MonotonicInstant::Duration::zero(),
            MonotonicInstant::Duration::zero()
        );
        REQUIRE_FALSE(result.has_value());
        anno::test::requireErrorKind(result.error(), AutomationErrorKind::Timeout);
        CHECK(under.m_clicks->clickCount() == 0);
    }

    TEST_CASE("engine session waitForPage resolves on a later frame")
    {
        auto parts             = singlePageRuntime();
        auto const fingerprint = parts.m_fingerprint;
        auto const pageA       = parts.m_page;
        auto frames            = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(fingerprint, unknownPixels(), FrameId{17}, MonotonicInstant::now())
        );
        frames.emplace_back(
            grayFrame(fingerprint, resolvingPixels(), FrameId{18}, MonotonicInstant::now())
        );

        auto under = makeSession(std::move(parts), std::move(frames), baseConfig(fingerprint));
        REQUIRE(under.m_session.has_value());
        auto& session = *under.m_session;

        auto result = session.waitForPage(
            pageA,
            std::chrono::duration_cast<MonotonicInstant::Duration>(std::chrono::seconds{5}),
            MonotonicInstant::Duration::zero()
        );
        REQUIRE(result.has_value());
        CHECK(result->m_page.pageId() == pageA);
        CHECK(under.m_clicks->clickCount() == 0);
    }
}
