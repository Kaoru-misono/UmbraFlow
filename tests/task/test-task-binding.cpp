#include "binding-fixture.hpp"

#include <task/capability-surface.hpp>
#include <task/cycle-ledger.hpp>
#include <task/task-context.hpp>

#include <script/engine.hpp>
#include <script/testing/cancel-probe.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
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

        // Fails only when recording a native call, so a test can prove a verb
        // aborts when its task.native_call event cannot be recorded rather than
        // dropping the evidence silently. Letting the engine's own events through
        // keeps the failure exactly where the test aims it.
        class FailOnNativeCallTraceSink final : public trace::ITraceSink
        {
        public:
            [[nodiscard]]
            auto emit(trace::StampedTraceEvent const& event) -> Status override
            {
                if (event.event().kind == trace::TraceEventKind::TaskNativeCall)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "trace sink deliberately failing on a native call"
                    );
                }
                return ok();
            }
        };

        // Fails only when recording a verb's failure. A sink that failed on every
        // event would abort the first successful verb and no test could ever reach
        // the failure path it wants to observe.
        class FailOnFailedTraceSink final : public trace::ITraceSink
        {
        public:
            [[nodiscard]]
            auto emit(trace::StampedTraceEvent const& event) -> Status override
            {
                auto const& call = event.event().nativeCall;
                if (
                    call.has_value()
                    && call->outcome == trace::NativeCallOutcome::Failed
                )
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "trace sink deliberately failing on a failure record"
                    );
                }
                return ok();
            }
        };

        [[nodiscard]]
        auto kindsOf(std::vector<trace::StampedTraceEvent> const& events)
            -> std::vector<trace::TraceEventKind>
        {
            auto kinds = std::vector<trace::TraceEventKind>{};
            kinds.reserve(events.size());
            for (auto const& event : events)
            {
                kinds.emplace_back(event.event().kind);
            }
            return kinds;
        }

        [[nodiscard]]
        auto nativeCallVerbs(std::vector<trace::StampedTraceEvent> const& events)
            -> std::vector<std::string>
        {
            auto verbs = std::vector<std::string>{};
            for (auto const& event : events)
            {
                if (event.event().nativeCall.has_value())
                {
                    verbs.emplace_back(event.event().nativeCall->verb);
                }
            }
            return verbs;
        }

        // The first native call recorded for `verb`, or null when the run made
        // none. The returned pointer observes storage owned by the recording sink
        // behind `events`, which the annotation on that parameter states.
        [[nodiscard]]
        auto findNativeCall(
            std::vector<trace::StampedTraceEvent> const& events UF_LIFETIME_BOUND,
            std::string_view verb
        ) noexcept -> trace::TraceEvent::NativeCall const*
        {
            for (auto const& event : events)
            {
                auto const& call = event.event().nativeCall;
                if (call.has_value() && call->verb == verb)
                {
                    return &*call;
                }
            }
            return nullptr;
        }

        // The run's recorder, a constructed EngineSession over it, the surface
        // built from its own catalog, and a non-owning observer of the click sink.
        // The surface is captured before the runtime moves into the session, so
        // both name the same recognizer and page identities.
        //
        // The recorder is declared first and held through a unique_ptr: the
        // session borrows it (see engine/session.hpp), so it must outlive the
        // session and keep a stable address when this struct is moved.
        struct Built final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
            CapabilitySurface                     surface;
            CountingActionSink*                   clicks;
        };

        // Builds the session from `frameSource` with `cancellation` armed on the
        // engine config, recording into `traceSink`. One recorder serves both the
        // engine session and the TaskContext built over it, which is what puts
        // their events into a single ordered stream.
        [[nodiscard]]
        auto buildBindingWith(
            std::unique_ptr<engine::IFrameSource> frameSource,
            std::stop_token cancellation,
            std::unique_ptr<trace::ITraceSink> traceSink
        ) -> Built
        {
            auto parts   = singlePageRuntime();
            auto surface = CapabilitySurface::create(
                parts.loaded.runtime.manifest().catalog()
            );
            REQUIRE(surface.has_value());

            auto actionSink = std::make_unique<CountingActionSink>();
            auto* const p_clicks = actionSink.get();
            auto config         = baseConfig(parts.fingerprint);
            config.cancellation = std::move(cancellation);
            auto recorder       = std::make_unique<trace::TraceRecorder>(
                std::move(traceSink),
                k_fixtureRunId,
                k_fixtureGenerationId
            );
            auto session = engine::EngineSession::create(
                std::move(parts.loaded),
                std::move(frameSource),
                std::move(actionSink),
                *recorder,
                config
            );
            return Built{
                .recorder = std::move(recorder),
                .session  = std::move(session),
                .surface  = *std::move(surface),
                .clicks   = p_clicks,
            };
        }

        [[nodiscard]]
        auto buildBinding(std::vector<Frame> frames) -> Built
        {
            return buildBindingWith(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
        }

        // One frame that resolves page_a and hits the action target.
        [[nodiscard]]
        auto resolvingFrames(FrameId frameId) -> std::vector<Frame>
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(
                grayFrame(
                    anno::test::fingerprint(3, 1, 96, 96),
                    resolvingPixels(),
                    frameId
                )
            );
            return frames;
        }

        // Runs `source` on a task VM whose only host capability is the umbra table
        // bound to `built`'s session, and returns the script's numeric result. The
        // VM is created and destroyed inside this call, so anything the host still
        // holds afterwards is held by the host, not by a live Lua handle.
        [[nodiscard]]
        auto runBound(TaskContext& context, Built& built, std::string_view source) -> double
        {
            auto engine = script::Engine::create(
                script::EngineConfig{
                    .installHostTables = built.surface.installer(context),
                    .projectGlobals    = CapabilitySurface::projectGlobals(),
                }
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
                ) -> Status
            {
                UF_TRY(surfaceInstaller(state));
                script::testing::installMarkCounter(state, &markCount);
                return ok();
            };
            config.projectGlobals = CapabilitySurface::projectGlobals();
            config.projectGlobals.emplace_back("mark");

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
            auto built = buildBinding(resolvingFrames(FrameId{50}));
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
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

        TEST_CASE("umbra binding runs one observation cycle into one delivered click")
        {
            auto built = buildBinding(resolvingFrames(FrameId{17}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local page = umbra:cycle_page(cycle)
                if page == nil then return 0 end
                if not page:is(umbra.pages.page_a) then return 0 end
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                if hit == nil then return 0 end
                umbra:cycle_click(cycle, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            // The click consumed the cycle, so the host holds no frame afterwards.
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("umbra binding refuses to open a second cycle while one is open")
        {
            // The one-cycle rule. It is a framework bug rather than a script
            // error -- the framework opens and closes one cycle per poll -- so it
            // is InternalInvariant, not a Tier B failure a retry policy would
            // treat as recoverable. The refusal also lands BEFORE the capture, so
            // the second open costs no frame, and the first cycle is untouched.
            auto frameSource = std::make_unique<FakeFrameSource>(
                resolvingFrames(FrameId{18})
            );
            auto* const p_frames = frameSource.get();
            auto built = buildBindingWith(
                std::move(frameSource),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local first = umbra:cycle_open()

                local ok, err = pcall(function() return umbra:cycle_open() end)
                if ok then return 0 end
                if err.kind ~= 'internal_invariant' then return 0 end
                if err.retryable ~= false then return 0 end
                if getmetatable(err) ~= 'umbra.error' then return 0 end

                -- The refused open left the first cycle whole: it still resolves,
                -- finds and clicks.
                local page = umbra:cycle_page(first)
                if page == nil then return 0 end
                local hit = umbra:cycle_find(first, umbra.recognizers.action_target)
                if hit == nil then return 0 end
                umbra:cycle_click(first, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK(p_frames->captureCount() == 1U);
        }

        TEST_CASE("umbra binding refuses a click on a cycle that resolved no page")
        {
            // The structural authorization guarantee. cycle_click takes no page:
            // the host uses the page THIS cycle resolved, so a script cannot hand
            // over evidence from another frame. A cycle that never resolved one
            // has no evidence at all and the click is refused -- and refused
            // without spending the cycle, which the successful click afterwards
            // proves.
            auto built = buildBinding(resolvingFrames(FrameId{19}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                if hit == nil then return 0 end

                local ok, err = pcall(function() return umbra:cycle_click(cycle, hit) end)
                if ok then return 0 end
                if err.kind ~= 'action_rejected' then return 0 end
                if getmetatable(err) ~= 'umbra.error' then return 0 end

                -- Resolving the page gives the cycle its evidence, and the very
                -- same ticket and hit now deliver.
                local page = umbra:cycle_page(cycle)
                if page == nil then return 0 end
                umbra:cycle_click(cycle, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("umbra binding fails every operation on a consumed or closed cycle")
        {
            auto built = buildBinding(resolvingFrames(FrameId{20}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // After the click consumes the cycle, cycle_page, cycle_find and a
            // second cycle_click all fail with a frozen, protected
            // stale_observation error table a script cannot mutate. A closed
            // cycle's ticket is just as dead.
            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local page = umbra:cycle_page(cycle)
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                umbra:cycle_click(cycle, hit)

                local okPage, errPage = pcall(function() return umbra:cycle_page(cycle) end)
                if okPage or errPage.kind ~= 'stale_observation' then return 0 end
                -- The kind a raised error carries is a key of umbra.errors whose
                -- value is that same string: both come from the one domain
                -- mapping, so a script's comparison can never silently miss.
                if umbra.errors[errPage.kind] ~= errPage.kind then return 0 end
                if errPage.retryable ~= true then return 0 end
                if getmetatable(errPage) ~= 'umbra.error' then return 0 end
                if pcall(function() errPage.kind = 'tampered' end) then return 0 end

                local okFind, errFind = pcall(function()
                    return umbra:cycle_find(cycle, umbra.recognizers.action_target)
                end)
                if okFind or errFind.kind ~= 'stale_observation' then return 0 end

                local okClick, errClick = pcall(function()
                    return umbra:cycle_click(cycle, hit)
                end)
                if okClick or errClick.kind ~= 'stale_observation' then return 0 end

                -- A closed cycle is equally dead, and its hit is refused even
                -- against a cycle that IS open.
                local closed = umbra:cycle_open()
                local staleHit = umbra:cycle_find(closed, umbra.recognizers.action_target)
                umbra:cycle_close(closed)
                local okClosed, errClosed = pcall(function()
                    return umbra:cycle_page(closed)
                end)
                if okClosed or errClosed.kind ~= 'stale_observation' then return 0 end

                local reopened = umbra:cycle_open()
                if umbra:cycle_page(reopened) == nil then return 0 end
                local okStale, errStale = pcall(function()
                    return umbra:cycle_click(reopened, staleHit)
                end)
                if okStale or errStale.kind ~= 'stale_observation' then return 0 end
                umbra:cycle_close(reopened)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("umbra cycle_close is idempotent and closing a consumed cycle is a no-op")
        {
            auto built = buildBinding(resolvingFrames(FrameId{21}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Every close below is unguarded: a raise anywhere would abort the
            // script and never reach the return, which is the assertion. This is
            // what lets a framework cleanup path close unconditionally.
            constexpr std::string_view source = R"lua(
                local first = umbra:cycle_open()
                umbra:cycle_close(first)
                umbra:cycle_close(first)

                local second = umbra:cycle_open()
                local page = umbra:cycle_page(second)
                local hit = umbra:cycle_find(second, umbra.recognizers.action_target)
                umbra:cycle_click(second, hit)
                umbra:cycle_close(second)

                -- The first ticket is still dead after all of that, and closing it
                -- once more still does nothing.
                umbra:cycle_close(first)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("Closing a cycle is what releases the frame, and only closing does")
        {
            // The whole point of the protocol: the frame's lifetime is the
            // ledger's, not the collector's. Both halves run with the VM already
            // destroyed, so every Lua handle has been finalised either way -- the
            // only difference is whether the script closed the cycle.
            SUBCASE("a cycle left open still holds its frame after the VM is gone")
            {
                auto built = buildBinding(resolvingFrames(FrameId{22}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    local cycle = umbra:cycle_open()
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK(context.hasOpenCycle());
            }

            SUBCASE("a closed cycle holds nothing")
            {
                auto built = buildBinding(resolvingFrames(FrameId{23}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    local cycle = umbra:cycle_open()
                    umbra:cycle_close(cycle)
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK_FALSE(context.hasOpenCycle());
            }
        }

        TEST_CASE("A ticket minted by one generation is rejected by another")
        {
            // Two contexts, so two ledgers and two generation stamps. Both open
            // their first cycle, so both hold ordinal 1 and only the generation
            // stamp tells the two tickets apart.
            auto firstBuilt = buildBinding(resolvingFrames(FrameId{24}));
            REQUIRE(firstBuilt.session.has_value());
            TaskContext first{*std::move(firstBuilt.session), *firstBuilt.recorder};

            auto secondBuilt = buildBinding(resolvingFrames(FrameId{25}));
            REQUIRE(secondBuilt.session.has_value());
            TaskContext second{*std::move(secondBuilt.session), *secondBuilt.recorder};

            auto const firstTicket = first.openCycle();
            REQUIRE(firstTicket.has_value());
            auto const secondTicket = second.openCycle();
            REQUIRE(secondTicket.has_value());
            REQUIRE(firstTicket->ordinal == secondTicket->ordinal);

            auto const crossed = second.cyclePage(*firstTicket);
            REQUIRE_FALSE(crossed.has_value());
            CHECK(
                automationErrorKind(crossed.error())
                == AutomationErrorKind::StaleObservation
            );

            // The rejection is the stamp and not a dead ledger: the second
            // generation's own ticket of the same ordinal still resolves.
            CHECK(second.cyclePage(*secondTicket).has_value());
        }

        TEST_CASE("umbra binding returns nil for a find that completes without a match")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), unknownPixels(), FrameId{26}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                umbra:cycle_close(cycle)
                return (hit == nil) and 1 or 0
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("umbra binding runs a poll loop that opens and closes one cycle a turn")
        {
            // Twenty frames that resolve nothing, then one that resolves page_a.
            // The loop opens a cycle, inspects it and closes it every turn, so it
            // runs far past anything a retention cap once allowed -- and needs no
            // forced collection to do it, because nothing is ever pinned past the
            // close.
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto       frames      = std::vector<Frame>{};
            for (int index = 0; index < 20; ++index)
            {
                frames.emplace_back(grayFrame(fingerprint, unknownPixels(), FrameId{40}));
            }
            frames.emplace_back(grayFrame(fingerprint, resolvingPixels(), FrameId{41}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local target = umbra.pages.page_a
                while true do
                    local cycle = umbra:cycle_open()
                    local page = umbra:cycle_page(cycle)
                    if page ~= nil and page:is(target) then
                        local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                        if hit ~= nil then
                            umbra:cycle_click(cycle, hit)
                        else
                            umbra:cycle_close(cycle)
                        end
                        break
                    end
                    umbra:cycle_close(cycle)
                end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            // The last cycle was consumed by the click and every earlier one was
            // closed, so nothing stays pinned in the host.
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("umbra wait_for_page hands back a page and the open cycle behind it")
        {
            auto built = buildBinding(resolvingFrames(FrameId{27}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // wait_for_page resolves page_a on the first capture and leaves the
            // cycle open over the frame that resolved it, with that page already
            // recorded as the cycle's authorization evidence -- which is why the
            // click below needs no cycle_page of its own.
            constexpr std::string_view source = R"lua(
                local wait = umbra:wait_for_page(umbra.pages.page_a, {})
                if wait == nil or wait.page == nil or wait.cycle == nil then return 0 end
                if not wait.page:is(umbra.pages.page_a) then return 0 end
                local hit = umbra:cycle_find(wait.cycle, umbra.recognizers.action_target)
                if hit == nil then return 0 end
                umbra:cycle_click(wait.cycle, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("umbra wait_for_page raises a Tier B timeout when the page never resolves")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), unknownPixels(), FrameId{28}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // A short explicit budget keeps the poll loop brief; the unknown frame
            // never resolves page_a, so the wait times out as a Tier B error whose
            // kind is the domain Timeout spelling, whose retryable is false, and
            // which carries the protected umbra.error metatable. A timed-out wait
            // opened no cycle, so the next open still succeeds.
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

                local cycle = umbra:cycle_open()
                umbra:cycle_close(cycle)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("umbra wait_for_page rejects an out-of-range timeout instead of overflowing")
        {
            auto built = buildBinding(resolvingFrames(FrameId{29}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

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
            auto built = buildBinding(resolvingFrames(FrameId{30}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The first click consumes the cycle; a second click inside try is a
            // Tier B stale_observation, returned as (false, errorTable). A plainly
            // successful function returns (true, nil).
            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local page = umbra:cycle_page(cycle)
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                umbra:cycle_click(cycle, hit)

                local ok, err = umbra:try(function() umbra:cycle_click(cycle, hit) end)
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

        TEST_CASE("A Tier B error rejects writes and cannot be cloned into a forgery")
        {
            auto built = buildBinding(resolvingFrames(FrameId{33}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The error table is the project-visible frozen object whose identity
            // the host actually consults: umbra:try classifies a caught value by
            // its metatable, so a mutable copy that kept that metatable would be
            // a working forgery. Three things close that off, and each is checked
            // against a control that would pass if the property were missing:
            //
            //   - the table is read-only, so it cannot be edited in place;
            //   - table.clone refuses it, because __metatable protects it, so
            //     there is no mutable copy to edit at all;
            //   - getmetatable yields a label rather than the metatable, so a
            //     hand-built look-alike has nothing to be given.
            //
            // The last four lines are the discriminator: the genuine table is
            // classified by the host, the look-alike is not, and the difference
            // is observable from the script.
            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local page = umbra:cycle_page(cycle)
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                umbra:cycle_click(cycle, hit)

                local ok, err = umbra:try(function() umbra:cycle_click(cycle, hit) end)
                if ok ~= false or err == nil then return 0 end
                if err.kind ~= 'stale_observation' then return 0 end

                if pcall(function() err.kind = 'timeout' end) then return 0 end
                if pcall(function() err.injected = 1 end) then return 0 end

                local cloned, cloneError = pcall(table.clone, err)
                if cloned then return 0 end
                if string.find(cloneError, 'protected metatable') == nil then return 0 end

                if getmetatable(err) ~= 'umbra.error' then return 0 end
                local forged = {
                    kind = err.kind,
                    message = err.message,
                    retryable = err.retryable,
                }
                if pcall(setmetatable, forged, getmetatable(err)) then return 0 end

                -- Control: the host DOES classify the genuine table it minted.
                local okReal, sameErr = umbra:try(function() error(err) end)
                if okReal ~= false or sameErr ~= err then return 0 end

                -- And it does not classify the look-alike: try re-raises it, so
                -- the outer pcall is what catches it and the value comes back
                -- unchanged rather than as a Tier B result.
                local caught = nil
                local okOuter = pcall(function()
                    umbra:try(function() error(forged) end)
                end)
                if okOuter then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("umbra try lets a script's own error propagate instead of swallowing it")
        {
            auto built = buildBinding(resolvingFrames(FrameId{31}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

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
                auto frame = grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), FrameId{32});
                auto built = buildBindingWith(
                    std::make_unique<StopOnCaptureFrameSource>(std::move(frame), stop),
                    stop.get_token(),
                    std::make_unique<DiscardingTraceSink>()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
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
                    pcall(function() umbra:cycle_open() end)
                    mark()
                    return 1
                )lua");
            }
            SUBCASE("wrapped in umbra:try")
            {
                cancelledRun(R"lua(
                    umbra:try(function() umbra:cycle_open() end)
                    mark()
                    return 1
                )lua");
            }
        }

        TEST_CASE("A failing verb reports its own cause, not the trace sink's failure")
        {
            // A failing verb records its native call before it raises. If that
            // record raised the sink's own IoFailure instead, an author debugging a
            // failed click would be told the trace file was unwritable rather than
            // why the click failed. The verb's cause wins; the lost evidence is
            // latched on the context so the host still learns the trace is
            // incomplete.
            //
            // Clicking consumes the cycle, so the second click fails
            // StaleObservation -- a Tier B failure umbra:try hands back rather than
            // re-raising, which is what makes the substitution observable.
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{88})),
                std::stop_token{},
                std::make_unique<FailOnFailedTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local page = umbra:cycle_page(cycle)
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                if page == nil or hit == nil then return 0 end
                umbra:cycle_click(cycle, hit)

                local ok, err = umbra:try(function() umbra:cycle_click(cycle, hit) end)
                if ok then return 0 end
                if err.kind ~= 'stale_observation' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(context.traceFailed());
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("umbra:now returns a non-negative, non-decreasing whole millisecond count")
        {
            auto built = buildBinding(resolvingFrames(FrameId{60}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

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
            auto built = buildBinding(resolvingFrames(FrameId{61}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

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

        TEST_CASE("an open-to-click cycle produces one ordered stream both layers wrote")
        {
            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_traces = traceSink.get();
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{70})),
                std::stop_token{},
                std::move(traceSink)
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local page = umbra:cycle_page(cycle)
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                umbra:cycle_click(cycle, hit)
                umbra:cycle_close(cycle)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));

            auto const& events = p_traces->events();

            // Every event of the run carries the same run identity and the next
            // sequence number, whichever layer wrote it. This is what the two old
            // schemas could not do: they had no join key at all.
            REQUIRE_FALSE(events.empty());
            for (auto index = std::size_t{0}; index < events.size(); ++index)
            {
                CHECK(events[index].sequence() == index + 1U);
                CHECK(events[index].runId() == k_fixtureRunId);
                CHECK(events[index].generationId() == k_fixtureGenerationId);
            }

            // The engine event and the task.native_call it caused sit adjacent in
            // one stream, so a reader can correlate them: each primitive's native
            // call immediately follows the engine work it drove, and the click's
            // engine.action_delivered names the frame the open observed. The
            // trailing cycle_close reached the host and found nothing to release,
            // which is the deterministic-release path being audited rather than
            // inferred.
            auto const kinds = kindsOf(events);
            auto const expected = std::vector<trace::TraceEventKind>{
                trace::TraceEventKind::EngineObserved,
                trace::TraceEventKind::TaskNativeCall,
                trace::TraceEventKind::EnginePageResolved,
                trace::TraceEventKind::TaskNativeCall,
                trace::TraceEventKind::EngineActionFound,
                trace::TraceEventKind::TaskNativeCall,
                trace::TraceEventKind::EngineActionAuthorized,
                trace::TraceEventKind::EngineActionDelivered,
                trace::TraceEventKind::EngineObservationInvalidated,
                trace::TraceEventKind::TaskNativeCall,
                trace::TraceEventKind::TaskNativeCall,
            };
            CHECK(kinds == expected);

            auto const observed = events[0].event();
            REQUIRE(observed.frame.has_value());
            CHECK(observed.frame->frameId() == FrameId{70});
            auto const delivered = events[7].event();
            REQUIRE(delivered.frame.has_value());
            CHECK(delivered.frame->frameId() == FrameId{70});

            // Each primitive emits exactly one native call, in call order; the pure
            // handle read page:is emits nothing.
            auto const verbs = nativeCallVerbs(events);
            CHECK(
                verbs
                == std::vector<std::string>{
                    "cycle_open",
                    "cycle_page",
                    "cycle_find",
                    "cycle_click",
                    "cycle_close",
                }
            );

            auto const* p_click = findNativeCall(events, "cycle_click");
            REQUIRE(p_click != nullptr);
            CHECK(p_click->outcome == trace::NativeCallOutcome::Succeeded);
            // Both ordinals name the one open cycle, which is what a delivered
            // click always looks like now.
            CHECK(p_click->cycleOrdinal == p_click->hitCycleOrdinal);

            // The close after a consuming click had nothing left to release.
            auto const* p_close = findNativeCall(events, "cycle_close");
            REQUIRE(p_close != nullptr);
            CHECK(p_close->outcome == trace::NativeCallOutcome::Empty);
        }

        TEST_CASE("umbra binding emits an Empty native call for a find that finds nothing")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), unknownPixels(), FrameId{71}));
            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_traces = traceSink.get();
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::stop_token{},
                std::move(traceSink)
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The frame resolves nothing, so cycle_page completes Unknown and
            // cycle_find completes without a match; both record Empty (Tier A)
            // rather than Succeeded or Failed.
            constexpr std::string_view source = R"lua(
                local cycle = umbra:cycle_open()
                local page = umbra:cycle_page(cycle)
                local hit = umbra:cycle_find(cycle, umbra.recognizers.action_target)
                umbra:cycle_close(cycle)
                return (page == nil and hit == nil) and 1 or 0
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));

            auto const& events = p_traces->events();
            CHECK(
                nativeCallVerbs(events)
                == std::vector<std::string>{
                    "cycle_open",
                    "cycle_page",
                    "cycle_find",
                    "cycle_close",
                }
            );

            auto const* p_page = findNativeCall(events, "cycle_page");
            REQUIRE(p_page != nullptr);
            CHECK(p_page->outcome == trace::NativeCallOutcome::Empty);

            auto const* p_find = findNativeCall(events, "cycle_find");
            REQUIRE(p_find != nullptr);
            CHECK(p_find->outcome == trace::NativeCallOutcome::Empty);

            // The close DID release a frame, so it is the one Succeeded call here.
            auto const* p_close = findNativeCall(events, "cycle_close");
            REQUIRE(p_close != nullptr);
            CHECK(p_close->outcome == trace::NativeCallOutcome::Succeeded);
        }

        TEST_CASE("umbra binding aborts a verb as Tier B io_failure when the trace sink fails")
        {
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{72})),
                std::stop_token{},
                std::make_unique<FailOnNativeCallTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // cycle_open succeeds in the engine, but recording its native call
            // fails; losing the trace evidence aborts the primitive as a Tier B
            // io_failure carrying the protected umbra.error metatable, rather than
            // dropping the record silently (the trace's throw-instant discipline).
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function() return umbra:cycle_open() end)
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
