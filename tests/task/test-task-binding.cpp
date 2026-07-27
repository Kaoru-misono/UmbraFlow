#include "binding-fixture.hpp"

#include <task/capability-surface.hpp>
#include <task/task-context.hpp>
#include <task/trace.hpp>

#include <script/engine.hpp>
#include <script/testing/cancel-probe.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <doctest/doctest.h>

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
        // Requests a stop the first time a frame is captured, then still hands back
        // a valid frame. Wiring its stop token into both the engine session and the
        // task VM reproduces the single cancel source: once the stop is requested,
        // the VM interrupt hard-breaks the task thread, so no script statement after
        // the cancelled call runs.
        class StopOnCaptureFrameSource final : public engine::IFrameSource
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

        // Fails every emit, so a test can prove a verb aborts when its HostCall
        // event cannot be recorded rather than dropping the evidence silently.
        class FailingTaskTraceSink final : public TaskTraceSink
        {
        public:
            [[nodiscard]] auto emit(TaskTraceEvent const& /*event*/) -> Status override
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "task trace sink deliberately failing"
                );
            }
        };

        // Fails only when recording a verb's failure. A sink that failed on every
        // event would abort the first successful verb and no test could ever reach
        // the failure path it wants to observe.
        class FailOnFailedTraceSink final : public TaskTraceSink
        {
        public:
            [[nodiscard]] auto emit(TaskTraceEvent const& event) -> Status override
            {
                if (event.outcome == HostCallOutcome::Failed)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "task trace sink deliberately failing on a failure record"
                    );
                }
                return ok();
            }
        };

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
            std::unique_ptr<engine::IFrameSource> frameSource,
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

        // Builds a context seeded with `seed` and draws `count` values from its
        // RNG, so two draws can be compared for reproducibility. The frame is never
        // captured; only the seeded RNG is exercised.
        [[nodiscard]]
        auto drawContextDoubles(uint64 seed, std::size_t count) -> std::vector<double>
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(
                grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{50})
            );
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                TaskContextConfig{.randomSeed = seed},
            };

            auto values = std::vector<double>{};
            values.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                values.emplace_back(context.nextRandomUnitDouble());
            }
            return values;
        }

        TEST_CASE("Two contexts with the same seed draw the same random sequence")
        {
            // The seed is what a trace records to replay a run: two contexts given
            // the same seed produce an identical sequence of at least a hundred
            // numbers, and a different seed diverges.
            auto const first  = drawContextDoubles(0x00C0'FFEE, 100);
            auto const second = drawContextDoubles(0x00C0'FFEE, 100);
            CHECK(first == second);

            auto const other = drawContextDoubles(0x0BAD'F00D, 100);
            CHECK(other != first);
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

        TEST_CASE("A failing verb reports its own cause, not the trace sink's failure")
        {
            // A failing verb records its HostCall before it raises. If that record
            // raised the sink's own IoFailure instead, an author debugging a failed
            // click would be told the trace file was unwritable rather than why the
            // click failed. The verb's cause wins; the lost evidence is latched on
            // the context so the host still learns the trace is incomplete.
            //
            // Clicking consumes the observation, so the second click fails
            // StaleObservation -- a Tier B failure umbra:try hands back rather than
            // re-raising, which is what makes the substitution observable.
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{88}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                TaskContextConfig{},
                std::make_unique<FailOnFailedTraceSink>(),
            };

            constexpr std::string_view source = R"lua(
                local frame = umbra:capture()
                local page = frame:resolve_page():resolved()
                local hit = frame:find(umbra.recognizers.action_target)
                if page == nil or hit == nil then return 0 end
                umbra:click(page, hit)

                local ok, err = umbra:try(function() umbra:click(page, hit) end)
                if ok then return 0 end
                if err.kind ~= 'stale_observation' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(context.traceFailed());
            CHECK(built.clicks->clickCount() == 1);
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

        TEST_CASE("umbra:now returns a non-negative, non-decreasing whole millisecond count")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{60}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // now() is a number, never negative, never runs backwards between two
            // calls, and carries no fractional millisecond tail.
            constexpr std::string_view source = R"lua(
                local a = umbra:now()
                local b = umbra:now()
                if type(a) ~= 'number' or type(b) ~= 'number' then return 0 end
                if a < 0 or b < 0 then return 0 end
                if b < a then return 0 end
                if a ~= math.floor(a) or b ~= math.floor(b) then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("umbra:random is the task's only RNG, covers its interval, and rejects empty ones")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{61}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session)};

            // The sandbox removed the native clocks and RNG, so umbra:random is the
            // sole source of randomness; no-argument random() is a float in [0, 1);
            // random(1, 3) stays in the closed interval, is always integral, and
            // over enough draws reaches both endpoints; and an empty or non-integer
            // interval is rejected exactly as math.random would reject it.
            constexpr std::string_view source = R"lua(
                if os.time ~= nil or os.clock ~= nil then return 0 end
                if math.random ~= nil or math.randomseed ~= nil then return 0 end

                local f = umbra:random()
                if type(f) ~= 'number' or f < 0 or f >= 1 then return 0 end

                local lo, hi = 3, 1
                for _ = 1, 400 do
                    local r = umbra:random(1, 3)
                    if type(r) ~= 'number' or r < 1 or r > 3 then return 0 end
                    if r ~= math.floor(r) then return 0 end
                    if r < lo then lo = r end
                    if r > hi then hi = r end
                end
                if lo ~= 1 or hi ~= 3 then return 0 end

                if pcall(function() return umbra:random(0) end) then return 0 end
                if pcall(function() return umbra:random(5, 2) end) then return 0 end
                if pcall(function() return umbra:random(1.5) end) then return 0 end

                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("umbra binding emits a HostCall trace event at each engine-backed verb")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{70}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());

            auto events = std::vector<TaskTraceEvent>{};
            TaskContext context{
                *std::move(built.session),
                TaskContextConfig{},
                std::make_unique<RecordingTaskTraceSink>(&events),
            };

            constexpr std::string_view source = R"lua(
                local frame = umbra:capture()
                local page = frame:resolve_page():resolved()
                local hit = frame:find(umbra.recognizers.action_target)
                umbra:click(page, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));

            // capture, resolve_page, find, click each emit exactly one HostCall in
            // call order; the pure handle reads resolved() and is() emit nothing.
            REQUIRE(events.size() == 4U);
            for (auto const& event : events)
            {
                CHECK(event.kind == TaskTraceEventKind::HostCall);
                CHECK(event.outcome == HostCallOutcome::Succeeded);
            }
            CHECK(events[0].verb == "capture");
            CHECK(events[1].verb == "resolve_page");
            CHECK(events[2].verb == "find");
            CHECK(events[3].verb == "click");
        }

        TEST_CASE("umbra binding emits an Empty HostCall for a find that finds nothing")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), unknownPixels(), FrameId{71}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());

            auto events = std::vector<TaskTraceEvent>{};
            TaskContext context{
                *std::move(built.session),
                TaskContextConfig{},
                std::make_unique<RecordingTaskTraceSink>(&events),
            };

            // The frame resolves nothing, so find completes without a match; its
            // HostCall records Empty (Tier A) rather than Succeeded or Failed.
            constexpr std::string_view source = R"lua(
                local frame = umbra:capture()
                local hit = frame:find(umbra.recognizers.action_target)
                return (hit == nil) and 1 or 0
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));

            REQUIRE(events.size() == 2U);
            CHECK(events[0].verb == "capture");
            CHECK(events[0].outcome == HostCallOutcome::Succeeded);
            CHECK(events[1].verb == "find");
            CHECK(events[1].outcome == HostCallOutcome::Empty);
        }

        TEST_CASE("umbra binding aborts a verb as Tier B io_failure when the trace sink fails")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{72}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());

            TaskContext context{
                *std::move(built.session),
                TaskContextConfig{},
                std::make_unique<FailingTaskTraceSink>(),
            };

            // capture succeeds in the engine, but recording its HostCall fails;
            // losing the trace evidence aborts the verb as a Tier B io_failure
            // carrying the protected umbra.error metatable, rather than dropping
            // the record silently (the engine trace's throw-instant discipline).
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function() return umbra:capture() end)
                if ok then return 0 end
                if err.kind ~= 'io_failure' then return 0 end
                if getmetatable(err) ~= 'umbra.error' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }
    }
}
