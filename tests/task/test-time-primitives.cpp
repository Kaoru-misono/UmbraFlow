#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/ids.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The three time primitives -- deadline, wait and settle -- and the absence
// beside them: no clock reaches a script. Every timing assertion is written so a
// green run finishes in milliseconds and a regression stays inside the suite
// timeout: the scripts ask for pauses of seconds and the checks demand they end
// in a fraction of one, so a primitive that stopped honouring its deadline or
// its stop token fails on the clock rather than hanging the run.
namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        constexpr auto millis(int64 count) -> MonotonicInstant::Duration
        {
            return std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{count}
            );
        }

        // How long DelayedStop waits before requesting the stop. The scripts
        // reach their pausing primitive in microseconds -- every frame is a fake
        // -- so the stop lands mid-sleep, which is the case a stop checked only
        // on entry would pass and only a sliced sleep covers.
        constexpr auto k_stopDelay = millis(200);

        // The floor a "it really did pause" check uses: below the 200 ms the
        // stop takes to arrive and far above the microseconds a run spends on
        // fake frames, so a primitive that returned without pausing fails it.
        constexpr auto k_pausedFloor = millis(150);

        // The ceiling every "returned promptly" check uses: well above the 100 ms
        // slice a stop is observed within and far below the multi-second pause
        // each script asks for, so it separates the two outcomes on any machine.
        constexpr auto k_promptCeiling = millis(5'000);

        // Requests `stop` after k_stopDelay on a joined thread, so the primitive
        // under test observes the stop mid-sleep rather than on entry. The
        // stop_source is copied in and shares its stop-state, so the worker
        // holds no reference into the test frame; std::jthread joins on destroy.
        class DelayedStop final
        {
            std::jthread m_thread;

        public:
            explicit DelayedStop(std::stop_source stop)
                : m_thread{
                      [source = std::move(stop)]() mutable noexcept
                      {
                          std::this_thread::sleep_for(k_stopDelay);
                          static_cast<void>(source.request_stop());
                      }
                  }
            {
            }
        };

        // A binding built over one resolving frame, with observing pointers to
        // the frame source and the trace sink. The frame count is what lets a
        // case prove a refusal happened BEFORE the engine.
        struct TimedBuild final
        {
            Built               built;
            FakeFrameSource*    frames{};
            RecordingTraceSink* traces{};
        };

        [[nodiscard]]
        auto buildTimed(FrameId frameId) -> TimedBuild
        {
            auto frameSource = std::make_unique<FakeFrameSource>(
                resolvingFrames(frameId)
            );
            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_frames = frameSource.get();
            auto* const p_traces = traceSink.get();

            // No stop token on the ENGINE config in any case below: the cancel is
            // armed on the TaskContext alone, so the engine would happily capture
            // again and the only thing that can refuse a primitive after a
            // cancelled pause is the terminal latch the pause itself set.
            auto built = buildBindingWith(
                std::move(frameSource),
                std::stop_token{},
                std::move(traceSink)
            );
            return TimedBuild{
                .built  = std::move(built),
                .frames = p_frames,
                .traces = p_traces,
            };
        }

        // Runs `source`, requiring success, and returns how long the whole run
        // took, so a case can assert a pause was taken or skipped from outside
        // the VM.
        [[nodiscard]]
        auto runTimed(
            TaskContext& context,
            Built& built,
            std::string_view source
        ) -> MonotonicInstant::Duration
        {
            auto const start = MonotonicInstant::now();
            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            return MonotonicInstant::now().saturatingDurationSince(start);
        }

        // Every settle duration on the wire, in call order.
        [[nodiscard]]
        auto settleDurations(std::vector<trace::StampedTraceEvent> const& events)
            -> std::vector<std::optional<uint64>>
        {
            auto durations = std::vector<std::optional<uint64>>{};
            for (auto const& event : events)
            {
                auto const& call = event.event().nativeCall;
                if (call.has_value() && call->verb == "settle")
                {
                    durations.emplace_back(call->durationMillis);
                }
            }
            return durations;
        }

        TEST_CASE("ctx:wait reports an expired deadline instead of blocking on it")
        {
            auto timed = buildTimed(FrameId{80});
            REQUIRE(timed.built.session.has_value());
            TaskContext context{*std::move(timed.built.session), *timed.built.recorder};

            // Both directions are asserted, so neither a wait that always
            // reports expiry nor one that never does can pass. The expired case
            // asks for a minute of poll interval it must not take.
            constexpr std::string_view source = R"lua(
                local expired = ctx:deadline(0)
                if ctx:wait(expired, 60000) ~= false then return 0 end

                local live = ctx:deadline(600000)
                if ctx:wait(live, 0) ~= true then return 0 end
                return 1
            )lua";

            auto const elapsed = runTimed(context, timed.built, source);
            CHECK(elapsed < k_promptCeiling);
        }

        TEST_CASE("ctx:settle pauses for what it was asked, records it, and caps the request")
        {
            auto timed = buildTimed(FrameId{81});
            REQUIRE(timed.built.session.has_value());
            TaskContext context{*std::move(timed.built.session), *timed.built.recorder};

            // A settle past the host ceiling is Tier B -- a project error the
            // author can catch -- and it is refused before it sleeps, which is
            // why this run finishes in milliseconds despite asking ten minutes.
            constexpr std::string_view source = R"lua(
                ctx:settle(0)
                ctx:settle(200)

                local ok, err = ctx:try(function() ctx:settle(600000) end)
                if ok then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end
                if err.kind ~= uf.errors.invalid_resource then return 0 end
                return 1
            )lua";

            auto const elapsed = runTimed(context, timed.built, source);

            // It really paused for the 200 ms it was asked for, and it really
            // stopped: a settle that ignored its argument fails the first bound,
            // one that slept the refused ten minutes fails the second.
            CHECK(elapsed >= k_pausedFloor);
            CHECK(elapsed < k_promptCeiling);

            // Both accepted settles are on the wire with the duration they
            // declared, because a run cannot be replayed from a pause nobody
            // wrote down. The refused one is an argument rejection: it records
            // nothing, exactly like a wrong handle type.
            auto const& events = timed.traces->events();
            CHECK(
                nativeCallVerbs(events)
                == std::vector<std::string>{"settle", "settle"}
            );
            CHECK(
                settleDurations(events)
                == std::vector<std::optional<uint64>>{uint64{0}, uint64{200}}
            );

            auto const* p_settle = findNativeCall(events, "settle");
            REQUIRE(p_settle != nullptr);
            CHECK(p_settle->outcome == trace::NativeCallOutcome::Succeeded);
        }

        TEST_CASE("A stop already requested refuses every time primitive before the engine")
        {
            auto stop = std::stop_source{};
            REQUIRE(stop.request_stop());

            auto timed = buildTimed(FrameId{82});
            REQUIRE(timed.built.session.has_value());
            TaskContext context{
                *std::move(timed.built.session),
                *timed.built.recorder,
                TaskContextConfig{.cancellation = stop.get_token()},
            };

            // Each of the three refuses with the Tier C sentinel rather than a
            // catchable automation error: a plain string, so ctx:try would
            // re-raise it and only a raw pcall can hold it. Swallowing it buys
            // nothing -- the cycle_open after it is refused by the same latch.
            constexpr std::string_view source = R"lua(
                local d = pcall(function() return ctx:deadline(1000) end)
                if d then return 0 end

                local s = pcall(function() ctx:settle(10000) end)
                if s then return 0 end

                local w = pcall(function() return ctx:wait(nil, 10000) end)
                if w then return 0 end

                local o, err = pcall(function() return ctx:cycle_open() end)
                if o then return 0 end
                if type(err) ~= 'string' then return 0 end
                return 1
            )lua";

            auto const elapsed = runTimed(context, timed.built, source);
            CHECK(elapsed < k_promptCeiling);

            // The refusals reached no engine verb: not one frame was spent.
            CHECK(timed.frames->captureCount() == 0U);
            CHECK(timed.built.clicks->clickCount() == 0);
            CHECK(context.fatal());
        }

        TEST_CASE("A stop requested while ctx:settle sleeps ends it on the terminal path")
        {
            auto stop = std::stop_source{};

            auto timed = buildTimed(FrameId{83});
            REQUIRE(timed.built.session.has_value());
            TaskContext context{
                *std::move(timed.built.session),
                *timed.built.recorder,
                TaskContextConfig{.cancellation = stop.get_token()},
            };

            // One cycle runs first so the capture count below has something to
            // stay at: an always-zero count could not show that the refusal
            // after the cancelled settle did not advance it.
            constexpr std::string_view source = R"lua(
                local c = ctx:cycle_open()
                ctx:cycle_close(c)

                local ok = pcall(function() ctx:settle(10000) end)
                if ok then return 0 end

                local again = pcall(function() return ctx:cycle_open() end)
                if again then return 0 end
                return 1
            )lua";

            auto const stopper = DelayedStop{stop};
            auto const elapsed = runTimed(context, timed.built, source);

            // It slept until the stop arrived, not the ten seconds it asked for.
            CHECK(elapsed >= k_pausedFloor);
            CHECK(elapsed < k_promptCeiling);
            CHECK(timed.frames->captureCount() == 1U);
            CHECK(context.fatal());
        }

        TEST_CASE("A stop requested while ctx:wait sleeps ends it on the terminal path")
        {
            auto stop = std::stop_source{};

            auto timed = buildTimed(FrameId{84});
            REQUIRE(timed.built.session.has_value());
            TaskContext context{
                *std::move(timed.built.session),
                *timed.built.recorder,
                TaskContextConfig{.cancellation = stop.get_token()},
            };

            // The deadline is minted before the stop lands, so the wait enters
            // its sleep with a full ten seconds of budget: nothing but the stop
            // ends it this early.
            constexpr std::string_view source = R"lua(
                local c = ctx:cycle_open()
                ctx:cycle_close(c)

                local d = ctx:deadline(10000)
                local ok = pcall(function() return ctx:wait(d, 10000) end)
                if ok then return 0 end

                local again = pcall(function() return ctx:cycle_open() end)
                if again then return 0 end
                return 1
            )lua";

            auto const stopper = DelayedStop{stop};
            auto const elapsed = runTimed(context, timed.built, source);

            CHECK(elapsed >= k_pausedFloor);
            CHECK(elapsed < k_promptCeiling);
            CHECK(timed.frames->captureCount() == 1U);
            CHECK(context.fatal());
        }

        TEST_CASE("No clock reaches a script and the seeded RNG still does")
        {
            auto timed = buildTimed(FrameId{85});
            REQUIRE(timed.built.session.has_value());
            TaskContext context{*std::move(timed.built.session), *timed.built.recorder};

            // ctx:now is gone by name, and so is every native clock the sandbox
            // removed. The controls are the surface that DID survive: deadline,
            // wait, settle, and random -- the other half of
            // modules/task/deterministic -- which still draws.
            constexpr std::string_view source = R"lua(
                if ctx.now ~= nil then return 0 end
                if now ~= nil then return 0 end
                if os.time ~= nil or os.clock ~= nil then return 0 end

                if type(ctx.deadline) ~= 'function' then return 0 end
                if type(ctx.wait) ~= 'function' then return 0 end
                if type(ctx.settle) ~= 'function' then return 0 end
                if type(ctx.random) ~= 'function' then return 0 end

                local f = ctx:random()
                if type(f) ~= 'number' or f < 0 or f >= 1 then return 0 end
                if ctx:random(7, 7) ~= 7 then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, timed.built, source) == doctest::Approx(1.0));
        }
    }
}
