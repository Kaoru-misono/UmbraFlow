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

        // Fails the FIRST capture with Cancelled and serves a good frame on every
        // one after it, with no stop token armed anywhere.
        //
        // That combination is what isolates the terminal latch. In a real cancel
        // the VM interrupt also breaks the thread, so a test cannot tell which
        // layer refused the next call. Here the interrupt never fires and the
        // engine would happily capture again, so the only thing that can refuse
        // the second primitive is the fatal latch the first one set -- which is
        // exactly the guarantee that has to survive a script that swallowed the
        // Tier C sentinel and kept running.
        class CancelOnceFrameSource final : public engine::IFrameSource
        {
            Frame       m_frame;
            std::size_t m_captureCount{0};

        public:
            explicit CancelOnceFrameSource(Frame frame) noexcept
                : m_frame{std::move(frame)}
            {
            }

            [[nodiscard]] auto capture() -> Result<Frame> override
            {
                ++m_captureCount;
                if (m_captureCount == 1U)
                {
                    return fail(
                        AutomationErrorKind::Cancelled,
                        "capture cancelled once"
                    );
                }
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }

            // How many captures this source served. A refusal that still spent a
            // frame would be a weaker guarantee than the one under test.
            [[nodiscard]] auto captureCount() const noexcept -> std::size_t
            {
                return m_captureCount;
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

        // Runs `source` on a real task VM bound to `built`'s session and returns
        // the script's numeric result. The VM is created and destroyed inside
        // this call, so anything the host still holds afterwards is held by the
        // host, not by a live Lua handle.
        [[nodiscard]]
        auto runBound(TaskContext& context, Built& built, std::string_view source) -> double
        {
            auto engine = script::Engine::create(taskVmConfig(built.surface, context));
            REQUIRE(engine.has_value());
            auto const result = engine->runNumber(source, "task-binding");
            REQUIRE(result.has_value());
            return *result;
        }

        // The same run, without requiring it to succeed. A test that asks how
        // the HOST classified a value the script let escape needs the failure
        // itself, which runBound deliberately refuses to hand back.
        [[nodiscard]]
        auto runBoundResult(
            TaskContext& context,
            Built& built,
            std::string_view source
        ) -> Result<double>
        {
            auto engine = script::Engine::create(taskVmConfig(built.surface, context));
            REQUIRE(engine.has_value());
            return engine->runNumber(source, "task-binding");
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
            auto   config           = taskVmConfig(built.surface, context);
            auto   surfaceInstaller = std::move(config.installHostTables);
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

        TEST_CASE("The task binding runs one observation cycle into one delivered click")
        {
            auto built = buildBinding(resolvingFrames(FrameId{17}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                if page == nil then return 0 end
                if not page:is(uf.pages.page_a) then return 0 end
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                if hit == nil then return 0 end
                ctx:cycle_click(cycle, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            // The click consumed the cycle, so the host holds no frame afterwards.
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("The task binding refuses to open a second cycle while one is open")
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
                local first = ctx:cycle_open()

                local ok, err = pcall(function() return ctx:cycle_open() end)
                if ok then return 0 end
                if err.kind ~= 'internal_invariant' then return 0 end
                if err.retryable ~= false then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                -- The refused open left the first cycle whole: it still resolves,
                -- finds and clicks.
                local page = ctx:cycle_page(first)
                if page == nil then return 0 end
                local hit = ctx:cycle_find(first, uf.recognizers.action_target)
                if hit == nil then return 0 end
                ctx:cycle_click(first, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK(p_frames->captureCount() == 1U);
        }

        TEST_CASE("The task binding refuses a click on a cycle that resolved no page")
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
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                if hit == nil then return 0 end

                local ok, err = pcall(function() return ctx:cycle_click(cycle, hit) end)
                if ok then return 0 end
                if err.kind ~= 'action_rejected' then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                -- Resolving the page gives the cycle its evidence, and the very
                -- same ticket and hit now deliver.
                local page = ctx:cycle_page(cycle)
                if page == nil then return 0 end
                ctx:cycle_click(cycle, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("The task binding fails every operation on a consumed or closed cycle")
        {
            auto built = buildBinding(resolvingFrames(FrameId{20}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // After the click consumes the cycle, cycle_page, cycle_find and a
            // second cycle_click all fail with a frozen, protected
            // stale_observation error table a script cannot mutate. A closed
            // cycle's ticket is just as dead.
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                ctx:cycle_click(cycle, hit)

                local okPage, errPage = pcall(function() return ctx:cycle_page(cycle) end)
                if okPage or errPage.kind ~= 'stale_observation' then return 0 end
                -- The kind a raised error carries is a key of uf.errors whose
                -- value is that same string: both come from the one domain
                -- mapping, so a script's comparison can never silently miss.
                if uf.errors[errPage.kind] ~= errPage.kind then return 0 end
                if errPage.retryable ~= true then return 0 end
                if getmetatable(errPage) ~= 'uf.error' then return 0 end
                if pcall(function() errPage.kind = 'tampered' end) then return 0 end

                local okFind, errFind = pcall(function()
                    return ctx:cycle_find(cycle, uf.recognizers.action_target)
                end)
                if okFind or errFind.kind ~= 'stale_observation' then return 0 end

                local okClick, errClick = pcall(function()
                    return ctx:cycle_click(cycle, hit)
                end)
                if okClick or errClick.kind ~= 'stale_observation' then return 0 end

                -- A closed cycle is equally dead, and its hit is refused even
                -- against a cycle that IS open.
                local closed = ctx:cycle_open()
                local staleHit = ctx:cycle_find(closed, uf.recognizers.action_target)
                ctx:cycle_close(closed)
                local okClosed, errClosed = pcall(function()
                    return ctx:cycle_page(closed)
                end)
                if okClosed or errClosed.kind ~= 'stale_observation' then return 0 end

                local reopened = ctx:cycle_open()
                if ctx:cycle_page(reopened) == nil then return 0 end
                local okStale, errStale = pcall(function()
                    return ctx:cycle_click(reopened, staleHit)
                end)
                if okStale or errStale.kind ~= 'stale_observation' then return 0 end
                ctx:cycle_close(reopened)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("ctx:cycle_close is idempotent and closing a consumed cycle is a no-op")
        {
            auto built = buildBinding(resolvingFrames(FrameId{21}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Every close below is unguarded: a raise anywhere would abort the
            // script and never reach the return, which is the assertion. This is
            // what lets a framework cleanup path close unconditionally.
            constexpr std::string_view source = R"lua(
                local first = ctx:cycle_open()
                ctx:cycle_close(first)
                ctx:cycle_close(first)

                local second = ctx:cycle_open()
                local page = ctx:cycle_page(second)
                local hit = ctx:cycle_find(second, uf.recognizers.action_target)
                ctx:cycle_click(second, hit)
                ctx:cycle_close(second)

                -- The first ticket is still dead after all of that, and closing it
                -- once more still does nothing.
                ctx:cycle_close(first)
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
                    local cycle = ctx:cycle_open()
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
                    local cycle = ctx:cycle_open()
                    ctx:cycle_close(cycle)
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

        TEST_CASE("The task binding returns nil for a find that completes without a match")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), unknownPixels(), FrameId{26}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                ctx:cycle_close(cycle)
                return (hit == nil) and 1 or 0
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("The task binding runs a poll loop that opens and closes one cycle a turn")
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
                local target = uf.pages.page_a
                while true do
                    local cycle = ctx:cycle_open()
                    local page = ctx:cycle_page(cycle)
                    if page ~= nil and page:is(target) then
                        local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                        if hit ~= nil then
                            ctx:cycle_click(cycle, hit)
                        else
                            ctx:cycle_close(cycle)
                        end
                        break
                    end
                    ctx:cycle_close(cycle)
                end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            // The last cycle was consumed by the click and every earlier one was
            // closed, so nothing stays pinned in the host.
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("ctx:wait_for_page hands back a page and the open cycle behind it")
        {
            auto built = buildBinding(resolvingFrames(FrameId{27}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // wait_for_page resolves page_a on the first capture and leaves the
            // cycle open over the frame that resolved it, with that page already
            // recorded as the cycle's authorization evidence -- which is why the
            // click below needs no cycle_page of its own.
            constexpr std::string_view source = R"lua(
                local wait = ctx:wait_for_page(uf.pages.page_a, {})
                if wait == nil or wait.page == nil or wait.cycle == nil then return 0 end
                if not wait.page:is(uf.pages.page_a) then return 0 end
                local hit = ctx:cycle_find(wait.cycle, uf.recognizers.action_target)
                if hit == nil then return 0 end
                ctx:cycle_click(wait.cycle, hit)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("ctx:wait_for_page raises a Tier B timeout when the page never resolves")
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(grayFrame(anno::test::fingerprint(3, 1, 96, 96), unknownPixels(), FrameId{28}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // A short explicit budget keeps the poll loop brief; the unknown frame
            // never resolves page_a, so the wait times out as a Tier B error whose
            // kind is the domain Timeout spelling, whose retryable is false, and
            // which carries the protected uf.error metatable. A timed-out wait
            // opened no cycle, so the next open still succeeds.
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function()
                    return ctx:wait_for_page(
                        uf.pages.page_a,
                        { timeout_ms = 30, poll_interval_ms = 5 }
                    )
                end)
                if ok then return 0 end
                if err.kind ~= 'timeout' then return 0 end
                if err.retryable ~= false then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                local cycle = ctx:cycle_open()
                ctx:cycle_close(cycle)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("ctx:wait_for_page rejects an out-of-range timeout instead of overflowing")
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
                    return ctx:wait_for_page(uf.pages.page_a, { timeout_ms = 1e15 })
                end)
                if ok then return 0 end
                if err.kind ~= 'invalid_resource' then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("ctx:try catches a Tier B automation error and returns the carrier")
        {
            auto built = buildBinding(resolvingFrames(FrameId{30}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The first click consumes the cycle; a second click inside try is a
            // Tier B stale_observation, returned as (false, carrier). A plainly
            // successful function returns (true, nil).
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                ctx:cycle_click(cycle, hit)

                local ok, err = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok ~= false then return 0 end
                if err == nil or err.kind ~= 'stale_observation' then return 0 end
                if err.retryable ~= true then return 0 end
                -- The carrier is host-minted userdata, not a table: that is what
                -- a project script has no way to produce.
                if type(err) ~= 'userdata' then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                local okDone, errDone = ctx:try(function() return 7 end)
                if okDone ~= true or errDone ~= nil then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("A project script cannot forge a Tier B error that ctx:try accepts")
        {
            auto built = buildBinding(resolvingFrames(FrameId{33}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Forgery is not a check to defeat any more: the carrier is a
            // userdata under a host tag, and a project script has no way to mint
            // a userdata at all. Every route it does have is exercised here,
            // each against the control that the GENUINE carrier is accepted by
            // the same path -- so a try that classified nothing would fail the
            // control rather than pass this vacuously.
            //
            // The second route is the one that matters most: it is the forgery
            // the frozen error TABLE could not refuse. With identity resting on
            // the __metatable label, a hand-built table wearing that label was
            // accepted; with identity resting on being userdata, it is not.
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                ctx:cycle_click(cycle, hit)

                local ok, real = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok ~= false or real == nil then return 0 end
                if real.kind ~= 'stale_observation' then return 0 end

                -- Control: re-raised, the genuine carrier comes back as a Tier B
                -- result, unchanged.
                local okReal, sameErr = ctx:try(function() error(real) end)
                if okReal ~= false or sameErr ~= real then return 0 end

                local fields = function()
                    return {
                        kind = real.kind,
                        message = real.message,
                        retryable = real.retryable,
                    }
                end

                -- 1. A plain table carrying the same fields.
                local plain = fields()
                -- 2. The same table wearing a metatable that answers
                --    getmetatable with exactly the host's own label.
                local labelled = setmetatable(
                    fields(),
                    { __metatable = getmetatable(real) }
                )
                -- Control: the disguise really is complete on the axis the old
                -- carrier was identified by.
                if getmetatable(labelled) ~= getmetatable(real) then return 0 end

                -- 3. table.clone of a real one yields no value to dress up.
                if pcall(table.clone, real) then return 0 end
                -- 4. newproxy, the one base-library way to mint a userdata, is
                --    absent from the project environment.
                if newproxy ~= nil then return 0 end

                for _, forgery in ipairs({ plain, labelled }) do
                    -- try refuses to classify it and re-raises it, so the OUTER
                    -- pcall is what catches, and it catches the forgery itself
                    -- rather than a Tier B result.
                    local through, back = pcall(function()
                        ctx:try(function() error(forgery) end)
                    end)
                    if through then return 0 end
                    if back ~= forgery then return 0 end
                end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("The host names an uncaught Tier B error by its tag, never by its fields")
        {
            // The C++ half of the same guarantee. A value that escapes a run
            // uncaught is classified by the carrier's userdata tag, so the kind
            // in the run report -- and so in run.finished -- is the one that
            // really failed. A forged look-alike carries no tag and therefore
            // cannot choose the kind its run is reported under.
            SUBCASE("a genuine carrier names its kind")
            {
                auto built = buildBinding(resolvingFrames(FrameId{38}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    local cycle = ctx:cycle_open()
                    local page = ctx:cycle_page(cycle)
                    local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                    ctx:cycle_click(cycle, hit)
                    -- Unguarded: the stale click raises and nothing catches it.
                    ctx:cycle_click(cycle, hit)
                    return 1
                )lua";

                auto const result = runBoundResult(context, built, source);
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::StaleObservation
                );
            }

            SUBCASE("a forged look-alike names nothing")
            {
                auto built = buildBinding(resolvingFrames(FrameId{39}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                // Same fields and the same label, read off a real error so the
                // disguise cannot go stale, raised the same way -- and the host
                // reports the script's own failure instead of the claimed kind.
                constexpr std::string_view source = R"lua(
                    local cycle = ctx:cycle_open()
                    local page = ctx:cycle_page(cycle)
                    local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                    ctx:cycle_click(cycle, hit)

                    local ok, real = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                    if ok ~= false or real == nil then return 1 end
                    error(setmetatable(
                        {
                            kind = real.kind,
                            message = real.message,
                            retryable = real.retryable,
                        },
                        { __metatable = getmetatable(real) }
                    ))
                )lua";

                auto const result = runBoundResult(context, built, source);
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::InvalidResource
                );
            }
        }

        TEST_CASE("A Tier B error is immutable and hands out nothing a forgery could wear")
        {
            auto built = buildBinding(resolvingFrames(FrameId{40}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The carrier a script catches is the host's own object and stays
            // the host's: its fields are readable and every write route is
            // refused, its metatable can neither be replaced nor obtained, and
            // its printed form names the kind rather than an address.
            //
            // The read controls are load-bearing. Without them an object that
            // simply answered nothing would pass every refusal below.
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                ctx:cycle_click(cycle, hit)

                local ok, err = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok ~= false or err == nil then return 0 end

                -- Control: the fields ARE readable.
                if err.kind ~= 'stale_observation' then return 0 end
                if err.retryable ~= true then return 0 end
                if type(err.message) ~= 'string' then return 0 end
                if string.find(tostring(err), 'stale_observation', 1, true) == nil then
                    return 0
                end

                -- Every write route fails ...
                if pcall(function() err.kind = 'timeout' end) then return 0 end
                if pcall(function() err.injected = 1 end) then return 0 end
                if pcall(rawset, err, 'kind', 'timeout') then return 0 end
                -- ... and none of them took.
                if err.kind ~= 'stale_observation' then return 0 end
                if err.injected ~= nil then return 0 end

                -- The metatable cannot be replaced ...
                if pcall(setmetatable, err, {}) then return 0 end
                -- ... and cannot be read either: getmetatable hands back the
                -- label, so the fields table behind __index is unreachable and
                -- the value handed out is not something setmetatable accepts.
                local exposed = getmetatable(err)
                if type(exposed) ~= 'string' then return 0 end
                if pcall(setmetatable, {}, exposed) then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("ctx:try lets a script's own error propagate instead of swallowing it")
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
                    ctx:try(function() error('boom') end)
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

        TEST_CASE("A cancellation is unrecoverable through pcall or try, and mark never runs")
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
                    pcall(function() ctx:cycle_open() end)
                    mark()
                    return 1
                )lua");
            }
            SUBCASE("wrapped in ctx:try")
            {
                cancelledRun(R"lua(
                    ctx:try(function() ctx:cycle_open() end)
                    mark()
                    return 1
                )lua");
            }
        }

        TEST_CASE("A swallowed cancellation still leaves the next primitive refused")
        {
            // The Tier C contract, split from the interrupt that normally hides
            // it: a project pcall MAY observe the sentinel, and observing it buys
            // nothing, because the terminal latch is checked at the C guard entry
            // of every primitive rather than by ctx:try.
            SUBCASE("the latch refuses every later primitive, without capturing")
            {
                auto frameSource = std::make_unique<CancelOnceFrameSource>(
                    grayFrame(
                        anno::test::fingerprint(3, 1, 96, 96),
                        resolvingPixels(),
                        FrameId{34}
                    )
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
                    -- The first primitive is cancelled. The host latches the
                    -- generation terminal and raises the Tier C sentinel, which
                    -- is a plain string a project pcall can catch.
                    local caught, err = pcall(function() return ctx:cycle_open() end)
                    if caught then return 0 end
                    if type(err) ~= 'string' then return 0 end

                    -- Catching it changed nothing: the next call is refused.
                    local again = pcall(function() return ctx:cycle_open() end)
                    if again then return 0 end

                    -- Including one routed through ctx:try, which is pure Luau
                    -- and consults no latch of its own -- it re-raises the
                    -- sentinel, so the outer pcall is what catches it.
                    local through = pcall(function()
                        ctx:try(function() return ctx:cycle_open() end)
                    end)
                    if through then return 0 end
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                // One capture: the cancelled one. Neither refusal reached the
                // frame source, so the guard ran before the engine, not after.
                CHECK(p_frames->captureCount() == 1U);
                CHECK(built.clicks->clickCount() == 0);
            }

            SUBCASE("control: with nothing latched the second open succeeds")
            {
                // Without this the case above would also pass on a binding that
                // refuses every second cycle_open for some unrelated reason.
                auto frames = std::vector<Frame>{};
                frames.emplace_back(
                    grayFrame(
                        anno::test::fingerprint(3, 1, 96, 96),
                        resolvingPixels(),
                        FrameId{35}
                    )
                );
                auto frameSource = std::make_unique<FakeFrameSource>(
                    std::move(frames)
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
                    local first = ctx:cycle_open()
                    ctx:cycle_close(first)
                    local second = ctx:cycle_open()
                    ctx:cycle_close(second)
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK(p_frames->captureCount() == 2U);
            }
        }

        TEST_CASE("The retired root spelling names nothing on a real task VM")
        {
            auto built = buildBinding(resolvingFrames(FrameId{37}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // `umbra` was this root's global name before 2026-07-29. The project
            // environment is a whitelist with no metatable, so a name the
            // installer no longer registers is structurally absent: it reads nil
            // and indexing it raises. The controls are `uf` itself, which must
            // still carry the same handles, and the handle labels, which are
            // rooted at the same spelling.
            constexpr std::string_view source = R"lua(
                if umbra ~= nil then return 0 end
                if pcall(function() return umbra.pages.page_a end) then return 0 end

                if uf.pages.page_a == nil then return 0 end
                if uf.recognizers.action_target == nil then return 0 end
                if getmetatable(uf.pages.page_a) ~= 'uf.page' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("No project route reaches the private capability surface")
        {
            auto frameSource = std::make_unique<FakeFrameSource>(
                resolvingFrames(FrameId{36})
            );
            auto* const p_frames = frameSource.get();
            auto built = buildBindingWith(
                std::move(frameSource),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The primitives are upvalues of the framework's closures. A project
            // script must not reach one as a global, as a field of uf, through
            // rawget, through a table.clone of uf, or by walking values, keys
            // and metatables from anything it can name.
            //
            // ctx is excluded from the walk by identity, and only by identity:
            // its methods are the framework's own Luau wrappers, which ARE the
            // published surface. The claim is about the native table behind them.
            //
            // Three controls keep the claim from being vacuous: the scanner is
            // shown finding a planted primitive by the same route; the two
            // by-name routes are shown finding a key that really is on uf; and
            // the run ends by driving a real capture through ctx, so the
            // framework demonstrably holds what no project route can name.
            constexpr std::string_view source = R"lua(
                local target = 'cycle_open'

                local function scan(value, depth, seen)
                    if depth > 6 then return false end
                    if type(value) ~= 'table' then return false end
                    if seen[value] then return false end
                    seen[value] = true
                    if value ~= ctx and rawget(value, target) ~= nil then
                        return true
                    end
                    for key, entry in pairs(value) do
                        if scan(key, depth + 1, seen) then return true end
                        if scan(entry, depth + 1, seen) then return true end
                    end
                    return scan(getmetatable(value), depth + 1, seen)
                end

                -- Control: the scanner really does find one when it is there.
                if not scan({ nest = { { cycle_open = print } } }, 0, {}) then
                    return 0
                end

                -- No primitive is a project global.
                if cycle_open ~= nil or cycle_close ~= nil then return 0 end
                if cycle_page ~= nil or cycle_find ~= nil then return 0 end
                if cycle_click ~= nil or wait_for_page ~= nil then return 0 end
                if now ~= nil or random ~= nil or try ~= nil then return 0 end

                -- Nor a field of uf, by index or by rawget.
                local names = {
                    'cycle_open', 'cycle_close', 'cycle_page', 'cycle_find',
                    'cycle_click', 'wait_for_page', 'now', 'random', 'try',
                }
                for _, name in ipairs(names) do
                    if uf[name] ~= nil then return 0 end
                    if rawget(uf, name) ~= nil then return 0 end
                end
                -- Control: both routes DO see a key that is on uf.
                if uf.pages == nil or rawget(uf, 'pages') == nil then
                    return 0
                end
                -- And the old method spelling now calls a nil.
                if pcall(function() return uf:cycle_open() end) then return 0 end

                -- No walk from anything nameable reaches one, uf's clone and
                -- the framework's own ctx included.
                if scan({
                    uf, ctx, table.clone(uf), getmetatable(uf),
                    _G, getfenv, setfenv, newproxy, gcinfo, coroutine, debug,
                    _VERSION, assert, error, getmetatable, ipairs, next, pairs,
                    pcall, print, rawequal, rawget, rawlen, rawset, select,
                    setmetatable, tonumber, tostring, type, typeof, unpack,
                    xpcall, bit32, buffer, math, os, string, table, utf8, vector,
                }, 0, {}) then
                    return 0
                end

                -- Control: the framework DOES hold the surface, and can spend a
                -- real capture with it.
                local cycle = ctx:cycle_open()
                if cycle == nil then return 0 end
                ctx:cycle_close(cycle)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(p_frames->captureCount() == 1U);
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
            // StaleObservation -- a Tier B failure ctx:try hands back rather than
            // re-raising, which is what makes the substitution observable.
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{88})),
                std::stop_token{},
                std::make_unique<FailOnFailedTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                if page == nil or hit == nil then return 0 end
                ctx:cycle_click(cycle, hit)

                local ok, err = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok then return 0 end
                if err.kind ~= 'stale_observation' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(context.traceFailed());
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("ctx:now returns a non-negative, non-decreasing whole millisecond count")
        {
            auto built = buildBinding(resolvingFrames(FrameId{60}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // now() is a number, never negative, never runs backwards between two
            // calls, and carries no fractional millisecond tail.
            constexpr std::string_view source = R"lua(
                local a = ctx:now()
                local b = ctx:now()
                if type(a) ~= 'number' or type(b) ~= 'number' then return 0 end
                if a < 0 or b < 0 then return 0 end
                if b < a then return 0 end
                if a ~= math.floor(a) or b ~= math.floor(b) then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("ctx:random is the task's only RNG, covers its interval, and rejects empty ones")
        {
            auto built = buildBinding(resolvingFrames(FrameId{61}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The sandbox removed the native clocks and RNG, so ctx:random is the
            // sole source of randomness; no-argument random() is a float in [0, 1);
            // random(1, 3) stays in the closed interval, is always integral, and
            // over enough draws reaches both endpoints; and an empty or non-integer
            // interval is rejected exactly as math.random would reject it.
            constexpr std::string_view source = R"lua(
                if os.time ~= nil or os.clock ~= nil then return 0 end
                if math.random ~= nil or math.randomseed ~= nil then return 0 end

                local f = ctx:random()
                if type(f) ~= 'number' or f < 0 or f >= 1 then return 0 end

                local lo, hi = 3, 1
                for _ = 1, 400 do
                    local r = ctx:random(1, 3)
                    if type(r) ~= 'number' or r < 1 or r > 3 then return 0 end
                    if r ~= math.floor(r) then return 0 end
                    if r < lo then lo = r end
                    if r > hi then hi = r end
                end
                if lo ~= 1 or hi ~= 3 then return 0 end

                if pcall(function() return ctx:random(0) end) then return 0 end
                if pcall(function() return ctx:random(5, 2) end) then return 0 end
                if pcall(function() return ctx:random(1.5) end) then return 0 end

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
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                ctx:cycle_click(cycle, hit)
                ctx:cycle_close(cycle)
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

        TEST_CASE("The task binding emits an Empty native call for a find that finds nothing")
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
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                ctx:cycle_close(cycle)
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

        TEST_CASE("The task binding aborts a verb as Tier B io_failure when the trace sink fails")
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
            // io_failure carrying the protected uf.error metatable, rather than
            // dropping the record silently (the trace's throw-instant discipline).
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function() return ctx:cycle_open() end)
                if ok then return 0 end
                if err.kind ~= 'io_failure' then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }
    }
}
