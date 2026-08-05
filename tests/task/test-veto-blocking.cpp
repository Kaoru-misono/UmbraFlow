#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/time/poll-sleep.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>
#include <engine/session.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Roadmap veto #6, made executable: block every long-running host binding in
// turn and prove the whole run still exits inside its budget once cancelled.
//
// Where each blocking primitive of design section 5 is blocked. cycle_open
// blocks in IFrameSource::capture, the only port whose contract carries a
// cancellation channel of its own (CaptureBudget), and it is blocked on THAT
// token, so a session that stopped forwarding the run's stop fails this case.
// cycle_close, cycle_match, cycle_read and emit block in the trace recorder,
// the only host call any of them can wait in; cycle_click in
// IActionSink::click; wait and settle in their own sliced sleep. The other
// four -- deadline, raise, terminal, random -- reach no host call at all and
// have no case here.
//
// Wherever a port exists the stop is requested the instant that port reports
// it has entered its block, so "the cancel landed while the binding was
// blocked" is a fixed point of the sequence rather than a wall-clock race. The
// two sleeping primitives have no port, so they use a timed stop and both
// bounds asserted there are one-sided: a loaded machine can only make the
// delay longer. Every fake block is bounded by k_blockCeiling, so a regression
// that stops releasing it fails the exit-budget assertion after a few seconds
// instead of hanging the suite.
namespace uf::task
{
    namespace
    {
        // The whole run must return within this once the stop is requested. It
        // is far above the couple of hundred milliseconds a healthy exit takes
        // and far below the ceiling a stuck block would sit out.
        constexpr auto k_exitBudget = MonotonicInstant::Duration{
            std::chrono::milliseconds{2'000}
        };

        // The longest a fake block waits for a cancellation that never comes.
        constexpr auto k_blockCeiling = MonotonicInstant::Duration{
            std::chrono::milliseconds{4'000}
        };

        // How long the timed trigger waits before requesting the stop. The
        // scripts reach their sleeping primitive in microseconds, so the stop
        // lands while that primitive is already sleeping.
        constexpr auto k_stopDelay = std::chrono::milliseconds{200};

        // The floor the two timed cases assert the run took: below the delay
        // above and far above the microseconds a run spends on fake ports, so a
        // primitive that returned without sleeping at all fails it.
        constexpr auto k_pausedFloor = MonotonicInstant::Duration{
            std::chrono::milliseconds{150}
        };

        enum class StopTrigger : uint8
        {
            // Request the stop the moment a fake port reports it is blocked.
            OnFirstBlock,

            // Request the stop after k_stopDelay, for the primitives whose block
            // is their own sleep and so have no port to report from.
            AfterDelay,
        };

        // The state a blocked port, the trigger thread and the case all share. A
        // shared_ptr rather than a reference capture, so the trigger thread owns
        // a share of what it touches instead of pointing back into the gate.
        struct GateState final
        {
            std::stop_source      stop{};
            std::binary_semaphore entered{0};
            std::atomic<bool>     blocked{false};
        };

        // One cancellation for one veto case: the token every layer of the run
        // shares, the one-shot block a fake port takes, and the thread that
        // requests the stop.
        class CancelGate final
        {
            std::shared_ptr<GateState> m_state;
            std::jthread               m_trigger;

        public:
            explicit CancelGate(StopTrigger trigger)
                : m_state{std::make_shared<GateState>()}
                , m_trigger{
                      [state = m_state, trigger]() noexcept
                      {
                          if (trigger == StopTrigger::AfterDelay)
                          {
                              std::this_thread::sleep_for(k_stopDelay);
                          }
                          else
                          {
                              // The ceiling keeps this thread joinable when a
                              // case never reaches its block: the stop is
                              // requested either way, so the case fails on its
                              // own assertions rather than on a hang.
                              static_cast<void>(
                                  state->entered.try_acquire_for(k_blockCeiling)
                              );
                          }
                          static_cast<void>(state->stop.request_stop());
                      }
                  }
            {
            }

            [[nodiscard]] auto token() const -> std::stop_token
            {
                return m_state->stop.get_token();
            }

            // Takes the one-shot block, waiting on `cancellation` rather than on
            // the gate's own token, so a port with a real cancellation channel
            // is tested against that channel and not a back door the fake gave
            // itself. Later calls return at once.
            auto blockOn(std::stop_token const& cancellation) -> void
            {
                if (m_state->blocked.exchange(true))
                {
                    return;
                }
                m_state->entered.release();

                auto const deadline = MonotonicInstant::now().checkedAdd(
                    k_blockCeiling
                );
                if (deadline.has_value())
                {
                    pollSleep(k_blockCeiling, *deadline, cancellation);
                }
            }

            // Whether a port ever entered the block: the control that keeps a
            // fast, cancelled exit from passing with nothing having blocked.
            [[nodiscard]] auto blocked() const noexcept -> bool
            {
                return m_state->blocked.load();
            }
        };

        // Blocks inside capture(), on the budget's OWN cancellation token: a
        // session that stopped putting the run's stop token into the budget
        // would leave this capture sitting out the whole ceiling, and the case
        // would fail on the exit budget.
        class BlockingFrameSource final : public engine::IFrameSource
        {
            CancelGate* m_gate;
            Frame       m_frame;

        public:
            BlockingFrameSource(CancelGate* p_gate, Frame frame) noexcept
                : m_gate{p_gate}
                , m_frame{std::move(frame)}
            {
            }

            [[nodiscard]]
            auto capture(CaptureBudget const& budget) -> Result<Frame> override
            {
                m_gate->blockOn(budget.cancellation);
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }
        };

        // Blocks inside click(). IActionSink::click carries no cancellation
        // parameter -- the port is bounded by the controller's own input
        // timeouts -- so this fake waits on the run's stop token directly, and
        // what the case proves is the half that IS the host's: once delivery
        // returns, the run ends terminally inside its budget instead of
        // carrying on to the next verb.
        class BlockingActionSink final : public engine::IActionSink
        {
            CancelGate* m_gate;

        public:
            explicit BlockingActionSink(CancelGate* p_gate) noexcept
                : m_gate{p_gate}
            {
            }

            [[nodiscard]]
            auto click(
                Point<ClientSpace> /*point*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                m_gate->blockOn(m_gate->token());
                return ok();
            }

            // A keystroke reaches the same port, so what the veto cases prove
            // about a blocked click holds for a blocked key.
            [[nodiscard]]
            auto pressKey(
                KeyName /*key*/,
                TargetGeneration /*actionGeneration*/
            ) -> Status override
            {
                m_gate->blockOn(m_gate->token());
                return ok();
            }

            // And for a blocked scroll, which reaches the same port again.
            [[nodiscard]]
            auto scroll(
                int32 /*notches*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                m_gate->blockOn(m_gate->token());
                return ok();
            }

            // And a blocked long press. It blocks on the gate rather than
            // sleeping for the hold: the cases are about a sink that does not
            // return, and a real hold would only make this one slower.
            [[nodiscard]]
            auto longPress(
                Point<ClientSpace> /*point*/,
                MonotonicInstant::Duration /*hold*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                m_gate->blockOn(m_gate->token());
                return ok();
            }

            // A blocked drag, for the long press's reason: it spans time on the
            // delivery thread, so a veto arriving mid-gesture has to be seen.
            [[nodiscard]]
            auto drag(
                Point<ClientSpace> /*start*/,
                Point<ClientSpace> /*end*/,
                MonotonicInstant::Duration /*travel*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                m_gate->blockOn(m_gate->token());
                return ok();
            }

            // And a blocked pointer move, the last verb the port carries.
            [[nodiscard]]
            auto movePointer(
                Point<ClientSpace> /*point*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                m_gate->blockOn(m_gate->token());
                return ok();
            }
        };

        // Blocks inside emit(), on the first event matching its target. Like the
        // action sink, ITraceSink::emit carries no cancellation channel of its
        // own, so the fake waits on the run's stop token.
        class BlockingTraceSink final : public trace::ITraceSink
        {
        public:
            // What the sink waits for before it blocks: one task.native_call
            // named by its verb, or the first framework semantic event.
            struct Target final
            {
                std::string nativeVerb{};
                bool        frameworkEvent{false};
            };

        private:
            CancelGate* m_gate;
            Target      m_target;

        public:
            BlockingTraceSink(CancelGate* p_gate, Target target)
                : m_gate{p_gate}
                , m_target{std::move(target)}
            {
            }

            [[nodiscard]]
            auto emit(trace::StampedTraceEvent const& event) -> Status override
            {
                auto const& call = event.event().nativeCall;
                bool const  matched =
                    m_target.frameworkEvent
                        ? event.event().framework.has_value()
                        : (call.has_value() && call->verb == m_target.nativeVerb);
                if (matched)
                {
                    m_gate->blockOn(m_gate->token());
                }
                return ok();
            }
        };

        // The recorder and the session over it, held through a unique_ptr
        // because the session borrows it. This suite needs its own rather than
        // binding-fixture's Built because it varies the ACTION sink, which the
        // shared builder always supplies itself.
        struct VetoParts final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
        };

        [[nodiscard]]
        auto buildVeto(
            std::unique_ptr<engine::IFrameSource> frameSource,
            std::unique_ptr<engine::IActionSink> actionSink,
            std::unique_ptr<trace::ITraceSink> traceSink,
            std::stop_token cancellation
        ) -> VetoParts
        {
            auto config         = baseConfig(fixtureFingerprint());
            config.cancellation = std::move(cancellation);

            auto recorder = std::make_unique<trace::TraceRecorder>(
                std::move(traceSink),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Task
            );
            auto session = engine::EngineSession::create(
                std::move(frameSource),
                std::move(actionSink),
                *recorder,
                config
            );
            return VetoParts{
                .recorder = std::move(recorder),
                .session  = std::move(session),
            };
        }

        struct BlockedRun final
        {
            MonotonicInstant::Duration elapsed{};
            Result<double>             result{};
            uint64                     markCount{0};
        };

        // Runs `source` with the gate's token armed on all three layers a real
        // run arms it on -- the engine session, the TaskContext and the VM
        // interrupt -- and reports how long the whole run took to return.
        [[nodiscard]]
        auto runBlocked(
            CancelGate& gate,
            std::unique_ptr<engine::IFrameSource> frameSource,
            std::unique_ptr<engine::IActionSink> actionSink,
            std::unique_ptr<trace::ITraceSink> traceSink,
            std::string_view source
        ) -> BlockedRun
        {
            auto parts = buildVeto(
                std::move(frameSource),
                std::move(actionSink),
                std::move(traceSink),
                gate.token()
            );
            REQUIRE(parts.session.has_value());
            TaskContext context{
                *std::move(parts.session),
                *parts.recorder,
                TaskContextConfig{.cancellation = gate.token()},
            };

            auto const start = MonotonicInstant::now();
            auto       run   = runWithMark(context, gate.token(), source);
            auto const elapsed = MonotonicInstant::now().saturatingDurationSince(start);
            return BlockedRun{
                .elapsed   = elapsed,
                .result    = std::move(run.result),
                .markCount = run.markCount,
            };
        }

        // The three things a blocked binding must not cost the run: time, the
        // correct verdict, and control. markCount is the discriminator: every
        // script below calls mark() BEFORE the blocked primitive and again
        // after, so exactly one asserts the first ran and the second did not --
        // an unreachable mark() reads zero, and a cancel the script rode
        // through reads two.
        auto expectPromptCancelledExit(BlockedRun const& run) -> void
        {
            CHECK(run.elapsed < k_exitBudget);
            REQUIRE_FALSE(run.result.has_value());
            CHECK(
                automationErrorKind(run.result.error()) == AutomationErrorKind::Cancelled
            );
            CHECK(run.markCount == 1U);
        }

        [[nodiscard]]
        auto oneFrame(FrameId frameId) -> Frame
        {
            return grayFrame(fixtureFingerprint(), resolvingPixels(), frameId);
        }

        // The frame source for every case whose block is somewhere else: one
        // frame the fixture template is on, so the script reaches the verb under
        // test without any of the setup blocking on its way there.
        [[nodiscard]]
        auto servingFrameSource() -> std::unique_ptr<engine::IFrameSource>
        {
            return std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{900}));
        }

        TEST_CASE("Veto #6: a blocked cycle_open still exits inside the budget")
        {
            auto gate = CancelGate{StopTrigger::OnFirstBlock};

            constexpr std::string_view source = R"lua(
                mark()
                pcall(function() return ctx:cycle_open() end)
                mark()
                return 1
            )lua";

            auto const run = runBlocked(
                gate,
                std::make_unique<BlockingFrameSource>(&gate, oneFrame(FrameId{901})),
                std::make_unique<CountingActionSink>(),
                std::make_unique<DiscardingTraceSink>(),
                source
            );

            CHECK(gate.blocked());
            expectPromptCancelledExit(run);
        }

        TEST_CASE("Veto #6: a blocked cycle_close still exits inside the budget")
        {
            auto gate = CancelGate{StopTrigger::OnFirstBlock};

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                mark()
                pcall(function() ctx:cycle_close(ticket) end)
                mark()
                return 1
            )lua";

            auto const run = runBlocked(
                gate,
                servingFrameSource(),
                std::make_unique<CountingActionSink>(),
                std::make_unique<BlockingTraceSink>(
                    &gate,
                    BlockingTraceSink::Target{.nativeVerb = "cycle_close"}
                ),
                source
            );

            CHECK(gate.blocked());
            expectPromptCancelledExit(run);
        }

        TEST_CASE("Veto #6: a blocked cycle_match still exits inside the budget")
        {
            auto gate = CancelGate{StopTrigger::OnFirstBlock};

            auto const source = std::string{"local TEMPLATE = "}
                + templateLiteral(k_targetActionGray) + "\n" + R"lua(
                local template = ctx:template_load(TEMPLATE)
                local ticket = ctx:cycle_open()
                mark()
                pcall(function()
                    return ctx:cycle_match(ticket, template, 0, 0, 3, 1)
                end)
                mark()
                return 1
            )lua";

            auto const run = runBlocked(
                gate,
                servingFrameSource(),
                std::make_unique<CountingActionSink>(),
                std::make_unique<BlockingTraceSink>(
                    &gate,
                    BlockingTraceSink::Target{.nativeVerb = "cycle_match"}
                ),
                source
            );

            CHECK(gate.blocked());
            expectPromptCancelledExit(run);
        }

        TEST_CASE("Veto #6: a blocked cycle_read still exits inside the budget")
        {
            auto gate = CancelGate{StopTrigger::OnFirstBlock};

            // No OCR adapter is bound, so the read refuses -- but it refuses
            // THROUGH the trace recorder, the blocking surface under test. What
            // is being timed is the exit, not the answer.
            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                mark()
                pcall(function() return ctx:cycle_read(ticket, 0, 0, 1, 1) end)
                mark()
                return 1
            )lua";

            auto const run = runBlocked(
                gate,
                servingFrameSource(),
                std::make_unique<CountingActionSink>(),
                std::make_unique<BlockingTraceSink>(
                    &gate,
                    BlockingTraceSink::Target{.nativeVerb = "cycle_read"}
                ),
                source
            );

            CHECK(gate.blocked());
            expectPromptCancelledExit(run);
        }

        TEST_CASE("Veto #6: a blocked cycle_click still exits inside the budget")
        {
            auto gate = CancelGate{StopTrigger::OnFirstBlock};

            // The click needs a real match to reach the sink at all, so
            // gate.blocked() below is also proof the whole authorization path
            // ran before delivery blocked.
            auto const source = std::string{"local TEMPLATE = "}
                + templateLiteral(k_targetActionGray) + "\n" + R"lua(
                local template = ctx:template_load(TEMPLATE)
                local ticket = ctx:cycle_open()
                local hit = ctx:cycle_match(ticket, template, 0, 0, 3, 1)
                if hit == nil then return 0 end
                mark()
                pcall(function() ctx:cycle_click(ticket, hit) end)
                mark()
                return 1
            )lua";

            auto const run = runBlocked(
                gate,
                servingFrameSource(),
                std::make_unique<BlockingActionSink>(&gate),
                std::make_unique<DiscardingTraceSink>(),
                source
            );

            CHECK(gate.blocked());
            expectPromptCancelledExit(run);
        }

        TEST_CASE("Veto #6: a blocked emit still exits inside the budget")
        {
            auto gate = CancelGate{StopTrigger::OnFirstBlock};

            // ctx:step is the only route a project has to `emit`: the primitive
            // is a framework upvalue, and step_started is the first thing it
            // asks the host to record.
            constexpr std::string_view source = R"lua(
                mark()
                pcall(function() ctx:step('blocked', function() end) end)
                mark()
                return 1
            )lua";

            auto const run = runBlocked(
                gate,
                servingFrameSource(),
                std::make_unique<CountingActionSink>(),
                std::make_unique<BlockingTraceSink>(
                    &gate,
                    BlockingTraceSink::Target{.frameworkEvent = true}
                ),
                source
            );

            CHECK(gate.blocked());
            expectPromptCancelledExit(run);
        }

        TEST_CASE("Veto #6: a blocked wait still exits inside the budget")
        {
            auto gate = CancelGate{StopTrigger::AfterDelay};

            // Twenty seconds of poll interval against a minute of deadline: only
            // the stop can end this early, and the floor asserted below says the
            // sleep was really entered rather than refused on the way in.
            constexpr std::string_view source = R"lua(
                mark()
                pcall(function()
                    return ctx:wait(ctx:deadline(60000), 20000)
                end)
                mark()
                return 1
            )lua";

            auto const run = runBlocked(
                gate,
                servingFrameSource(),
                std::make_unique<CountingActionSink>(),
                std::make_unique<DiscardingTraceSink>(),
                source
            );

            CHECK(run.elapsed >= k_pausedFloor);
            expectPromptCancelledExit(run);
        }

        TEST_CASE("Veto #6: a blocked settle still exits inside the budget")
        {
            auto gate = CancelGate{StopTrigger::AfterDelay};

            constexpr std::string_view source = R"lua(
                mark()
                pcall(function() ctx:settle(20000) end)
                mark()
                return 1
            )lua";

            auto const run = runBlocked(
                gate,
                servingFrameSource(),
                std::make_unique<CountingActionSink>(),
                std::make_unique<DiscardingTraceSink>(),
                source
            );

            CHECK(run.elapsed >= k_pausedFloor);
            expectPromptCancelledExit(run);
        }
    }
}
