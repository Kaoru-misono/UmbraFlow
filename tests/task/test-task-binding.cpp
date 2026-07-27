#include "../annotation/test-helpers.hpp"

#include <task/capability-surface.hpp>
#include <task/task-context.hpp>

#include <script/engine.hpp>
#include <script/testing/cancel-probe.hpp>

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

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>
#include <engine/runtime-loader.hpp>
#include <engine/session.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        namespace anno = annotation;

        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000011";
        constexpr auto k_actionId = "00000000-0000-0000-0000-000000000013";
        constexpr auto k_pageId   = "00000000-0000-0000-0000-000000000111";

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        // A one-by-one grey RGBA template addressed by its content hash, mirroring
        // the engine session fixtures so page and action evaluation behave exactly
        // as they do in the real loop.
        [[nodiscard]]
        auto encodedTemplate(uint8 gray) -> anno::EncodedRuntimeTemplate
        {
            auto const rgba = std::vector<std::byte>{
                asByte(gray),
                asByte(gray),
                asByte(gray),
                asByte(255),
            };
            auto encoded = image::encodeRgbaPng("task-binding-template.png", 1, 1, rgba);
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
            engine::LoadedRuntime    loaded;
            anno::ProjectFingerprint fingerprint;
        };

        // One page (page_a, required anchor grey 2) plus one action target
        // (grey 5) authorized for that page. A grey [2,5,0] frame resolves page_a
        // and the target hits at position 1; a grey [0,0,0] frame resolves nothing
        // and the target misses.
        [[nodiscard]]
        auto singlePageRuntime() -> RuntimeParts
        {
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto const anchorA     = anno::test::recognizerId(k_anchorId);
            auto const actionT     = anno::test::recognizerId(k_actionId);
            auto const pageA       = anno::test::pageId(k_pageId);
            auto anchorTemplate = encodedTemplate(2);
            auto actionTemplate = encodedTemplate(5);
            auto const sourceBytes = std::array{asByte(42)};
            auto const sourceHash  = anno::sha256(sourceBytes);
            REQUIRE(sourceHash.has_value());

            auto manifest = anno::RuntimeManifest::create(
                anno::test::projectId("personal.task_binding"),
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
                .loaded      = engine::LoadedRuntime{.runtime = *std::move(runtime)},
                .fingerprint = fingerprint,
            };
        }

        [[nodiscard]]
        auto grayFrame(
            anno::ProjectFingerprint fingerprint,
            std::vector<std::byte> pixels,
            FrameId frameId
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
                MonotonicInstant::now(),
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

        // Replays a fixed sequence of frames, repeating the last once exhausted.
        class FakeFrameSource final : public engine::FrameSource
        {
            std::vector<Frame> m_frames;
            std::size_t        m_index{0};

        public:
            explicit FakeFrameSource(std::vector<Frame> frames) noexcept
                : m_frames{std::move(frames)}
            {
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
                return ok();
            }
        };

        // Requests a stop the first time a frame is captured, then still hands back
        // a valid frame. Wiring its stop token into both the engine session and the
        // task VM reproduces the single cancel source: once the stop is requested,
        // the VM interrupt hard-breaks the task thread, so no script statement after
        // the cancelled call runs.
        class StopOnCaptureFrameSource final : public engine::FrameSource
        {
            Frame            m_frame;
            std::stop_source m_stop;

        public:
            StopOnCaptureFrameSource(Frame frame, std::stop_source stop) noexcept
                : m_frame{std::move(frame)}
                , m_stop{std::move(stop)}
            {
            }

            [[nodiscard]] auto capture() -> Result<Frame> override
            {
                m_stop.request_stop();
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }
        };

        // Counts delivered clicks so fail-closed cases can assert none escaped.
        class CountingActionSink final : public engine::ActionSink
        {
            uint32 m_clickCount{0};

        public:
            [[nodiscard]]
            auto click(
                Point<ClientSpace> /*point*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                ++m_clickCount;
                return ok();
            }

            [[nodiscard]] auto clickCount() const noexcept -> uint32
            {
                return m_clickCount;
            }
        };

        class DiscardingTraceSink final : public engine::TraceSink
        {
        public:
            [[nodiscard]] auto emit(engine::TraceEvent const& /*event*/) -> Status override
            {
                return ok();
            }
        };

        [[nodiscard]]
        auto baseConfig(anno::ProjectFingerprint fingerprint) -> engine::EngineSessionConfig
        {
            return engine::EngineSessionConfig{
                .liveFingerprint         = fingerprint,
                .maximumPixelComparisons = 1'000,
                .recognitionTimeout      = std::chrono::duration_cast<
                    MonotonicInstant::Duration
                >(std::chrono::seconds{5}),
            };
        }

        // A constructed EngineSession plus the surface built from its own catalog
        // and a non-owning observer of the click sink. The surface is captured
        // before the runtime moves into the session, so both name the same
        // recognizer and page identities.
        struct Built final
        {
            Result<engine::EngineSession> session;
            CapabilitySurface             surface;
            CountingActionSink*           clicks;
        };

        // Builds the session from `frameSource` with `cancellation` armed on the
        // engine config, plus the surface built from its own catalog and a
        // non-owning observer of the click sink. The surface is captured before the
        // runtime moves into the session, so both name the same identities.
        [[nodiscard]]
        auto buildBindingWith(
            std::unique_ptr<engine::FrameSource> frameSource,
            std::stop_token cancellation
        ) -> Built
        {
            auto parts   = singlePageRuntime();
            auto surface = CapabilitySurface::create(
                parts.loaded.runtime.manifest().catalog()
            );
            REQUIRE(surface.has_value());

            auto actionSink = std::make_unique<CountingActionSink>();
            auto traceSink  = std::make_unique<DiscardingTraceSink>();
            auto* const p_clicks = actionSink.get();
            auto config         = baseConfig(parts.fingerprint);
            config.cancellation = std::move(cancellation);
            auto session         = engine::EngineSession::create(
                std::move(parts.loaded),
                std::move(frameSource),
                std::move(actionSink),
                std::move(traceSink),
                config
            );
            return Built{
                .session = std::move(session),
                .surface = *std::move(surface),
                .clicks  = p_clicks,
            };
        }

        [[nodiscard]]
        auto buildBinding(std::vector<Frame> frames) -> Built
        {
            return buildBindingWith(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::stop_token{}
            );
        }

        // Runs `source` on a task VM whose only host capability is the umbra table
        // bound to `built`'s session, and returns the script's numeric result.
        [[nodiscard]]
        auto runBound(TaskContext& context, Built& built, std::string_view source) -> double
        {
            auto engine = script::Engine::create(
                script::EngineConfig{.installHostTables = built.surface.installer(context)}
            );
            REQUIRE(engine.has_value());
            auto const result = engine->runNumber(source, "task-binding");
            REQUIRE(result.has_value());
            return *result;
        }

        // Outcome of a discriminator run: the run result (an error on a
        // cancellation) and how many times the host mark() ran.
        struct DiscriminatorRun final
        {
            Result<double> result;
            uint64         markCount{0};
        };

        // Runs `source` on a task VM bound to `built` with `cancellation` armed on
        // the VM interrupt (the session already shares the same token), plus a host
        // mark() the script can call. Returns the run result and how many times
        // mark() reached. markCount is declared before the Engine, so it outlives
        // the VM and the closure's pointer into it stays valid for every call.
        [[nodiscard]]
        auto runWithMark(
            TaskContext& context,
            Built& built,
            std::stop_token cancellation,
            std::string_view source
        ) -> DiscriminatorRun
        {
            uint64 markCount        = 0;
            auto   surfaceInstaller = built.surface.installer(context);
            auto   config           = script::EngineConfig{};
            config.cancellation     = std::move(cancellation);
            config.installHostTables =
                [surfaceInstaller = std::move(surfaceInstaller), &markCount](
                    lua_State* state
                ) -> void
            {
                surfaceInstaller(state);
                script::testing::installMarkCounter(state, &markCount);
            };

            auto engine = script::Engine::create(config);
            REQUIRE(engine.has_value());
            auto result = engine->runNumber(source, "task-tier-c");
            return DiscriminatorRun{
                .result    = std::move(result),
                .markCount = markCount,
            };
        }

        TEST_CASE("umbra binding runs capture resolve find click into one delivered click")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{17}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            constexpr std::string_view source = R"lua(
                local frame = umbra:capture()
                local outcome = frame:resolve_page()
                local page = outcome:resolved()
                if page == nil then return 0 end
                if not page:is(umbra.pages.page_a) then return 0 end
                local hit = frame:find(umbra.recognizers.action_target)
                if hit == nil then return 0 end
                umbra:click(page, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("umbra binding fails every method on a frame whose click consumed it")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{17}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // After the click consumes the observation, resolve_page, find, and a
            // second click all fail with a frozen, protected stale_observation
            // error table that a script cannot mutate.
            constexpr std::string_view source = R"lua(
                local frame = umbra:capture()
                local page = frame:resolve_page():resolved()
                local hit = frame:find(umbra.recognizers.action_target)
                umbra:click(page, hit)

                local okResolve, errResolve = pcall(function() return frame:resolve_page() end)
                if okResolve or errResolve.kind ~= 'stale_observation' then return 0 end
                if errResolve.retryable ~= true then return 0 end
                if getmetatable(errResolve) ~= 'umbra.error' then return 0 end
                if pcall(function() errResolve.kind = 'tampered' end) then return 0 end

                local okFind, errFind = pcall(function()
                    return frame:find(umbra.recognizers.action_target)
                end)
                if okFind or errFind.kind ~= 'stale_observation' then return 0 end

                local okClick, errClick = pcall(function() return umbra:click(page, hit) end)
                if okClick or errClick.kind ~= 'stale_observation' then return 0 end

                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("umbra binding rejects a click that mixes objects from two frames")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{17}));
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{18}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // p1 belongs to the first capture, h2 to the second. The binding layer
            // rejects the mix before the engine, and neither frame is consumed.
            constexpr std::string_view source = R"lua(
                local f1 = umbra:capture()
                local p1 = f1:resolve_page():resolved()
                local f2 = umbra:capture()
                local h2 = f2:find(umbra.recognizers.action_target)
                if p1 == nil or h2 == nil then return 0 end

                local ok, err = pcall(function() return umbra:click(p1, h2) end)
                if ok or err.kind ~= 'action_rejected' then return 0 end

                local h1 = f1:find(umbra.recognizers.action_target)
                if h1 == nil then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("umbra binding returns nil for a find that completes without a match")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), unknownPixels(), FrameId{17}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            constexpr std::string_view source = R"lua(
                local frame = umbra:capture()
                local hit = frame:find(umbra.recognizers.action_target)
                return (hit == nil) and 1 or 0
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("umbra wait_for_page returns a paired page and frame that click together")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{21}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // wait_for_page resolves page_a on the first capture and hands back a
            // { page, frame } sharing one observation sequence, so find on the frame
            // and click on the page consume the same wait and deliver one click.
            constexpr std::string_view source = R"lua(
                local wait = umbra:wait_for_page(umbra.pages.page_a, {})
                if wait == nil or wait.page == nil or wait.frame == nil then return 0 end
                if not wait.page:is(umbra.pages.page_a) then return 0 end
                local hit = wait.frame:find(umbra.recognizers.action_target)
                if hit == nil then return 0 end
                umbra:click(wait.page, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("umbra wait_for_page raises a Tier B timeout when the page never resolves")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), unknownPixels(), FrameId{22}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // A short explicit budget keeps the poll loop brief; the unknown frame
            // never resolves page_a, so the wait times out as a Tier B error whose
            // kind is the domain Timeout spelling, whose retryable is false, and
            // which carries the protected umbra.error metatable.
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function()
                    return umbra:wait_for_page(
                        umbra.pages.page_a,
                        { timeout_ms = 30, poll_interval_ms = 5 }
                    )
                end)
                if ok then return 0 end
                if err.kind ~= 'timeout' then return 0 end
                if err.retryable ~= false then return 0 end
                if getmetatable(err) ~= 'umbra.error' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("umbra wait_for_page rejects an out-of-range timeout instead of overflowing")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{24}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // 1e15 ms cleared the old <= 1e15 bound yet overflowed the nanosecond
            // tick rep inside duration_cast (undefined behaviour). It is now a clean
            // Tier B InvalidResource raised while reading the options, before any
            // capture, so the frame source is never touched.
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function()
                    return umbra:wait_for_page(umbra.pages.page_a, { timeout_ms = 1e15 })
                end)
                if ok then return 0 end
                if err.kind ~= 'invalid_resource' then return 0 end
                if getmetatable(err) ~= 'umbra.error' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("umbra try catches a Tier B automation error and returns its table")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{23}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // The first click consumes the frame; a second click inside try is a
            // Tier B stale_observation, returned as (false, errorTable). A plainly
            // successful function returns (true, nil).
            constexpr std::string_view source = R"lua(
                local frame = umbra:capture()
                local page = frame:resolve_page():resolved()
                local hit = frame:find(umbra.recognizers.action_target)
                umbra:click(page, hit)

                local ok, err = umbra:try(function() umbra:click(page, hit) end)
                if ok ~= false then return 0 end
                if err == nil or err.kind ~= 'stale_observation' then return 0 end
                if err.retryable ~= true then return 0 end
                if getmetatable(err) ~= 'umbra.error' then return 0 end

                local okDone, errDone = umbra:try(function() return 7 end)
                if okDone ~= true or errDone ~= nil then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("umbra try lets a script's own error propagate instead of swallowing it")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{24}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // error('boom') is the script's own failure, not a Tier B automation
            // error: try must re-raise it, so the outer pcall -- not try -- is what
            // catches it, the statement after try never runs, and the caught value
            // is the raw string rather than an error table.
            constexpr std::string_view source = R"lua(
                local reachedAfter = false
                local ok, err = pcall(function()
                    umbra:try(function() error('boom') end)
                    reachedAfter = true
                end)
                if ok then return 0 end
                if reachedAfter then return 0 end
                if type(err) ~= 'string' then return 0 end
                if string.find(err, 'boom') == nil then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("umbra cancellation is unrecoverable through pcall or try, and mark never runs")
        {
            // The shared stop source drives both the engine (returns Cancelled) and
            // the VM interrupt (hard-breaks the thread). A capture requests the stop,
            // so whatever the script wraps the call in, no statement after it runs:
            // the host-visible mark() -- a non-automation witness -- stays at zero
            // and the run ends Cancelled.
            auto const cancelledRun = [](std::string_view guarded) -> void
            {
                auto stop  = std::stop_source{};
                auto frame = grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{25});
                auto built = buildBindingWith(
                    std::make_unique<StopOnCaptureFrameSource>(std::move(frame), stop),
                    stop.get_token()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    TaskContextConfig{.cancellation = stop.get_token()},
                };

                auto const run = runWithMark(context, built, stop.get_token(), guarded);
                REQUIRE_FALSE(run.result.has_value());
                CHECK(
                    automationErrorKind(run.result.error())
                    == AutomationErrorKind::Cancelled
                );
                CHECK(run.markCount == 0);
                CHECK(built.clicks->clickCount() == 0);
            };

            SUBCASE("wrapped in the native pcall")
            {
                cancelledRun(R"lua(
                    pcall(function() umbra:capture() end)
                    mark()
                    return 1
                )lua");
            }
            SUBCASE("wrapped in umbra:try")
            {
                cancelledRun(R"lua(
                    umbra:try(function() umbra:capture() end)
                    mark()
                    return 1
                )lua");
            }
        }

        TEST_CASE("umbra binding reclaims the observation a dropped frame handle pinned")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{31}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());

            // Cap disabled so the guardrail never forces a mid-run collection:
            // this isolates the ownership fix -- a frame handle releasing its
            // retained observation when it dies -- from the guardrail that
            // backstops it. The VM (created and destroyed inside runBound)
            // finalises every frame userdata on teardown, so each of the fifty
            // captures must have released its observation for the map to drain.
            TaskContext context{
                *std::move(built.session),
                TaskContextConfig{.maxLiveObservations = 0},
            };

            // Fifty captures, none retained. Before the fix the observations
            // lingered in the host map for the whole run (only a click erased
            // one), so this stayed at fifty; a frame handle that releases on
            // collection drains it to zero.
            constexpr std::string_view source = R"lua(
                for i = 1, 50 do
                    umbra:capture()
                end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(context.liveObservationCount() == 0);
        }

        TEST_CASE("umbra binding fails Tier B when a script pins too many frames at once")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{32}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());

            // A small explicit cap makes the guardrail observable without pinning
            // many frames. The script stashes every frame in a table, so the
            // frames stay reachable and the forced collection reclaims nothing:
            // the capture that would exceed the cap raises a Tier B
            // InvalidResource, and the table holds exactly `cap` frames then.
            TaskContext context{
                *std::move(built.session),
                TaskContextConfig{.maxLiveObservations = 3},
            };

            constexpr std::string_view source = R"lua(
                local frames = {}
                local ok, err = pcall(function()
                    while true do
                        frames[#frames + 1] = umbra:capture()
                    end
                end)
                if ok then return 0 end
                if err.kind ~= 'invalid_resource' then return 0 end
                if err.retryable ~= false then return 0 end
                if getmetatable(err) ~= 'umbra.error' then return 0 end
                if #frames ~= 3 then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("umbra binding lets a polling loop run past the retention cap")
        {
            // Twenty frames that resolve nothing, then one that resolves page_a.
            // A capture-resolve poll loop drops each frame as it advances, so it
            // runs well past the default retention cap; the guardrail's forced
            // collection reclaims the dropped frames every time, so the loop is
            // never falsely failed and reaches the resolving frame to click.
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto       frames      = std::vector<Frame>{};
            for (int index = 0; index < 20; ++index)
            {
                frames.emplace_back(grayFrame(fingerprint, unknownPixels(), FrameId{40}));
            }
            frames.emplace_back(grayFrame(fingerprint, resolvingPixels(), FrameId{41}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            constexpr std::string_view source = R"lua(
                local target = umbra.pages.page_a
                while true do
                    local frame = umbra:capture()
                    local page = frame:resolve_page():resolved()
                    if page ~= nil and page:is(target) then
                        local hit = frame:find(umbra.recognizers.action_target)
                        if hit ~= nil then
                            umbra:click(page, hit)
                        end
                        break
                    end
                end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            // The loop's last frame was consumed by the click and every earlier
            // frame was reclaimed, so nothing stays pinned in the host.
            CHECK(context.liveObservationCount() == 0);
        }

        TEST_CASE("TaskContext release drops a live observation and is a no-op after consume")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{33}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            auto const seq = context.capture();
            REQUIRE(seq.has_value());
            CHECK(context.liveObservationCount() == 1);

            context.release(*seq);
            CHECK(context.liveObservationCount() == 0);

            // Releasing again -- the path a frame handle's collection takes after
            // a click already consumed the observation -- is a harmless no-op,
            // never a double-erase, and leaves the count at zero.
            context.release(*seq);
            CHECK(context.liveObservationCount() == 0);
        }
    }
}
