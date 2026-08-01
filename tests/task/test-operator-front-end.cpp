#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/operator-session.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/stream-validator.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The operator front-end, tested against the property that gives it the right to
// exist: it is a SIBLING consumer of the private capability surface, so every
// guarantee a task gets, an operator gets -- identically, and for the same reason,
// because both call the same TaskContext.
//
// Each case that claims "identically" proves it by driving BOTH paths over the same
// runtime and comparing the refusal, rather than by asserting a kind on one side and
// trusting the other.
namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto keyName(std::string_view name) -> KeyName
        {
            auto const created = KeyName::create(name);
            REQUIRE(created.has_value());
            return *created;
        }

        [[nodiscard]]
        auto uniqueTracePath() -> std::filesystem::path
        {
            static auto s_sequence = std::atomic<uint64>{1};
            return std::filesystem::temp_directory_path()
                / (
                    "uf-operator-trace-"
                    + std::to_string(
                        s_sequence.fetch_add(1U, std::memory_order_relaxed)
                    )
                    + ".jsonl"
                );
        }

        // A frame stamped at the clock epoch, so a session configured with a zero
        // maximum action frame age mints a lease that is already expired by the time
        // the action reaches the delivery edge. It is how both paths are handed the
        // same "expired frame" without either of them waiting.
        [[nodiscard]]
        auto epochResolvingFrame() -> Frame
        {
            auto const fingerprint = test::fingerprint(3, 1, 96, 96);
            auto const transform   = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(fingerprint.width()),
                static_cast<float>(fingerprint.height()),
                fingerprint.width(),
                fingerprint.height()
            );
            REQUIRE(transform.has_value());

            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(resolvingPixels())
            };
            auto frame = Frame::create(
                FrameId{51},
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                test::instantAt(MonotonicInstant::Duration{0}),
                fingerprint.width(),
                fingerprint.height(),
                static_cast<std::size_t>(fingerprint.width())
                    * bytesPerPixel(PixelFormat::Gray8),
                PixelFormat::Gray8,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        // The task side: a TaskContext over the fixture geometry, plus observers
        // of the click sink and the trace sink.
        struct TaskSide final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            std::unique_ptr<TaskContext>          context;
            CountingActionSink*                   actions;
            RecordingTraceSink*                   traces;
        };

        [[nodiscard]]
        auto buildTaskSide(
            std::vector<Frame> frames,
            MonotonicInstant::Duration maxActionFrameAge
        ) -> TaskSide
        {
            auto actionSink      = std::make_unique<CountingActionSink>();
            auto* const p_clicks = actionSink.get();
            auto config              = baseConfig(fixtureFingerprint());
            config.maxActionFrameAge = maxActionFrameAge;

            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_traces = traceSink.get();
            auto recorder        = std::make_unique<trace::TraceRecorder>(
                std::move(traceSink),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Task
            );
            auto session = engine::EngineSession::create(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::move(actionSink),
                *recorder,
                config
            );
            REQUIRE(session.has_value());

            auto context = std::make_unique<TaskContext>(
                *std::move(session),
                *recorder
            );
            return TaskSide{
                .recorder = std::move(recorder),
                .context  = std::move(context),
                .actions  = p_clicks,
                .traces   = p_traces,
            };
        }

        // The operator side over the same runtime, the same frames and the same
        // bounds, so a comparison between the two isolates the front-end and nothing
        // else.
        struct OperatorSide final
        {
            std::unique_ptr<OperatorSession> session;
            CountingActionSink*              actions;
            std::filesystem::path            tracePath;
        };

        [[nodiscard]]
        auto buildOperatorSide(
            std::vector<Frame> frames,
            MonotonicInstant::Duration maxActionFrameAge
        ) -> OperatorSide
        {
            auto actionSink      = std::make_unique<CountingActionSink>();
            auto* const p_clicks = actionSink.get();
            auto const tracePath = uniqueTracePath();
            std::filesystem::remove(tracePath);

            auto session = OperatorSession::create(
                TaskRunConfig{
                    .frameSource = std::make_unique<FakeFrameSource>(
                        std::move(frames)
                    ),
                    .actionSink      = std::move(actionSink),
                    .liveFingerprint = fixtureFingerprint(),
                    .maximumPixelComparisons = 1'000,
                    .recognitionTimeout      = std::chrono::duration_cast<
                        MonotonicInstant::Duration
                    >(std::chrono::seconds{5}),
                    .maxActionFrameAge = maxActionFrameAge,
                    .tracePath         = tracePath,
                },
                OperatorSession::Spec{
                    .projectId          = "chaos-fixture",
                    .projectFingerprint = fixtureFingerprint(),
                },
                TaskRunId{5},
                GenerationId{1}
            );
            REQUIRE(session.has_value());
            return OperatorSide{
                .session   = *std::move(session),
                .actions   = p_clicks,
                .tracePath = tracePath,
            };
        }

        [[nodiscard]]
        auto lenientFrameAge() -> MonotonicInstant::Duration
        {
            return std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::seconds{5}
            );
        }

        [[nodiscard]]
        auto readLines(std::filesystem::path const& path) -> std::vector<std::string>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            auto lines = std::vector<std::string>{};
            auto line  = std::string{};
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.emplace_back(std::move(line));
            }
            return lines;
        }
    }

    TEST_CASE("a click on an expired frame is refused on the task path")
    {
        // A zero maximum action frame age makes the lease of an epoch-stamped frame
        // expired at the delivery edge. The script matches the template first, so
        // the only thing left to refuse the click is the lease itself.
        //
        // THE OPERATOR HALF OF THIS PARITY IS GONE, and the absence is the point:
        // an operator can no longer produce a match at all, because searching for
        // one means naming a template a project file holds and this front-end
        // reaches no project file
        // (docs/plans/2026-07-31-script-owned-page-model.md 9). What survives on
        // both paths -- the open cycle a key demands, and the ledger's answer
        // about a spent one -- is compared in the cases below.
        auto frames = std::vector<Frame>{};
        frames.emplace_back(epochResolvingFrame());
        auto side = buildTaskSide(
            std::move(frames),
            MonotonicInstant::Duration::zero()
        );
        auto const source = withTemplate(
            "local template = ctx:template_load(TEMPLATE)\n"
            "local cycle = ctx:cycle_open()\n"
            "local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)\n"
            "ctx:cycle_click(cycle, hit)\n"
            "return 1\n"
        );
        auto engine = script::Engine::create(taskVmConfig(*side.context));
        REQUIRE(engine.has_value());
        auto const result = engine->runNumber(source, "operator-parity");
        REQUIRE_FALSE(result.has_value());
        CHECK(side.actions->clickCount() == 0U);
        CHECK(
            automationErrorKind(result.error())
            == AutomationErrorKind::StaleObservation
        );
    }

    TEST_CASE("key requires an open cycle on both paths")
    {
        // The contract `key` DOES impose. It names no screen position, so it demands
        // no detection and no fresh lease -- but it must not be deliverable outside an
        // observation cycle, because that is what orders it against the observations
        // around it and what its trace line joins on.
        auto const taskKind = [&]
        {
            auto side = buildTaskSide(resolvingFrames(FrameId{33}), lenientFrameAge());
            auto const source = std::string{
                "local cycle = ctx:cycle_open()\n"
                "ctx:cycle_close(cycle)\n"
                "ctx:key(cycle, \"E\")\n"
                "return 1\n"
            };
            auto engine = script::Engine::create(taskVmConfig(*side.context));
            REQUIRE(engine.has_value());
            auto const result = engine->runNumber(source, "operator-parity");
            REQUIRE_FALSE(result.has_value());
            CHECK(side.actions->keys().empty());
            return automationErrorKind(result.error());
        }();

        auto const operatorKind = [&]
        {
            auto side  = buildOperatorSide(
                resolvingFrames(FrameId{33}),
                lenientFrameAge()
            );
            auto cycle = side.session->cycleOpen();
            REQUIRE(cycle.has_value());
            auto const closed = side.session->cycleClose(*cycle);
            REQUIRE(closed.has_value());

            auto const pressed = side.session->key(*cycle, keyName("E"));
            REQUIRE_FALSE(pressed.has_value());
            CHECK(side.actions->keys().empty());
            return automationErrorKind(pressed.error());
        }();

        CHECK(taskKind == AutomationErrorKind::StaleObservation);
        CHECK(operatorKind == taskKind);
    }

    TEST_CASE("key with no cycle ever opened is refused rather than delivered")
    {
        // The operator addresses a cycle by ordinal over a text protocol, so it can
        // name one that never existed. The LEDGER decides, not the session: an ordinal
        // that is not the open cycle's names a cycle that no longer exists.
        auto side = buildOperatorSide(
            resolvingFrames(FrameId{34}),
            lenientFrameAge()
        );

        auto const pressed = side.session->key(uint64{1}, keyName("E"));
        REQUIRE_FALSE(pressed.has_value());
        CHECK(automationErrorKind(pressed.error()) == AutomationErrorKind::StaleObservation);
        CHECK(side.actions->keys().empty());
    }

    TEST_CASE("a delivered key consumes its cycle and reaches the sink once")
    {
        // The one respect in which `key` is STRICTER than a click's contract needs it
        // to be: a delivered keystroke changed the screen, so the frame the cycle
        // retains no longer describes the target and the cycle is spent.
        auto side = buildOperatorSide(
            resolvingFrames(FrameId{35}),
            lenientFrameAge()
        );

        auto cycle = side.session->cycleOpen();
        REQUIRE(cycle.has_value());
        auto const pressed = side.session->key(*cycle, keyName("E"));
        REQUIRE(pressed.has_value());
        REQUIRE(side.actions->keys().size() == 1U);
        CHECK(side.actions->keys().front() == keyName("E"));

        auto const again = side.session->key(*cycle, keyName("A"));
        REQUIRE_FALSE(again.has_value());
        CHECK(automationErrorKind(again.error()) == AutomationErrorKind::StaleObservation);
        CHECK(side.actions->keys().size() == 1U);

        // The refusal must not name a click. It is the only account the operator
        // gets of why the cycle is gone, and a keystroke spent this one: the
        // message used to say "already consumed by a click" on exactly this path,
        // which sent a reader looking for a click that never happened.
        CHECK_FALSE(again.error().message().contains("click"));
        CHECK(again.error().message().contains("delivered input"));
    }

    TEST_CASE("a key needs nothing found on its frame, only an open cycle")
    {
        // Stated as its own case because it is the design decision, not an accident:
        // a click needs a match this frame produced, and a key on a cycle that
        // matched nothing at all is delivered.
        auto side = buildOperatorSide(
            resolvingFrames(FrameId{36}),
            lenientFrameAge()
        );

        auto cycle = side.session->cycleOpen();
        REQUIRE(cycle.has_value());
        auto const pressed = side.session->key(*cycle, keyName("F3"));
        REQUIRE(pressed.has_value());
        CHECK(side.actions->keys().size() == 1U);
    }

    TEST_CASE("a task presses a key through the cycle view its block was handed")
    {
        // The shape a real task writes. ctx:cycle hands the block a frozen view, and
        // view:key goes dead afterwards exactly as view:click does -- so the framework's
        // own bookkeeping agrees with the ledger about the cycle a keystroke spent,
        // which is what stops the block from finding or clicking on a screen the key
        // already changed.
        // WHICH LAYER refuses the second key is what this case pins, and it is why the
        // script reports a code rather than a boolean. The framework's refusal is a
        // plain string -- its own closed-cycle sentence -- while the host's is a Tier B
        // error carrier, which is userdata a project cannot produce. Distinguishing
        // them is the only way to tell "the framework caught it first" from "the
        // framework let it through and the ledger caught it", and the design's layering
        // is the former: C++ owns the guarantee, the framework owns the good message.
        auto side = buildTaskSide(resolvingFrames(FrameId{41}), lenientFrameAge());
        auto const source = std::string{
            "local delivered = 0\n"
            "ctx:cycle(function(cycle)\n"
            "    cycle:key(\"E\")\n"
            "    delivered = 1\n"
            "    local ok, err = pcall(function() cycle:key(\"A\") end)\n"
            "    if ok then\n"
            "        delivered = 2\n"
            "    elseif type(err) == \"string\" then\n"
            "        delivered = 3\n"
            "    end\n"
            "end)\n"
            "return delivered\n"
        };
        auto engine = script::Engine::create(taskVmConfig(*side.context));
        REQUIRE(engine.has_value());
        auto const result = engine->runNumber(source, "operator-parity");
        REQUIRE(result.has_value());

        // 3: the framework refused the second key with its own sentence. 2 would mean
        // the keystroke was delivered twice on one observation, and 1 would mean the
        // framework let it reach the host and the ledger did the refusing.
        CHECK(*result == 3.0);
        REQUIRE(side.actions->keys().size() == 1U);
        CHECK(side.actions->keys().front() == keyName("E"));
        CHECK_FALSE(side.context->hasOpenCycle());
    }

    TEST_CASE("an unresolvable key name is refused before the cycle is spent")
    {
        auto side = buildOperatorSide(
            resolvingFrames(FrameId{37}),
            lenientFrameAge()
        );

        auto cycle = side.session->cycleOpen();
        REQUIRE(cycle.has_value());

        auto const lower = KeyName::create("e");
        REQUIRE_FALSE(lower.has_value());
        CHECK(automationErrorKind(lower.error()) == AutomationErrorKind::ActionRejected);

        // The cycle a refused name never reached is still open, so nothing was spent.
        auto const closed = side.session->cycleClose(*cycle);
        REQUIRE(closed.has_value());
        CHECK(*closed);
    }

    TEST_CASE("every operator trace line is attributed to the operator front-end")
    {
        auto side = buildOperatorSide(
            resolvingFrames(FrameId{39}),
            lenientFrameAge()
        );

        auto cycle = side.session->cycleOpen();
        REQUIRE(cycle.has_value());
        auto const pressed = side.session->key(*cycle, keyName("E"));
        REQUIRE(pressed.has_value());
        auto const report = side.session->finish(std::nullopt);
        CHECK(report.outcome() == TaskRunOutcome::Completed);

        auto const lines = readLines(side.tracePath);
        REQUIRE_FALSE(lines.empty());
        for (auto const& line : lines)
        {
            CAPTURE(line);
            CHECK(line.contains("\"frontEnd\":\"operator\""));
            CHECK_FALSE(line.contains("\"frontEnd\":\"task\""));
        }

        // The keystroke reached the wire under its own event kind carrying the key,
        // so a reader can tell which key an operator pressed and on which frame.
        auto delivered = std::size_t{0};
        for (auto const& line : lines)
        {
            if (line.contains("\"kind\":\"engine.key_delivered\""))
            {
                ++delivered;
                CHECK(line.contains("\"key\":\"E\""));
            }
        }
        CHECK(delivered == 1U);

        // The sink is the session's, so the file stays open until the session dies.
        side.session.reset();
        auto error = std::error_code{};
        static_cast<void>(std::filesystem::remove(side.tracePath, error));
    }

    TEST_CASE("a task run's trace lines are attributed to the task front-end")
    {
        // The other half of "the two are distinguishable": the same recorder, over the
        // same runtime, stamps the other value, and a reader can tell the streams
        // apart on that member alone.
        auto side = buildTaskSide(resolvingFrames(FrameId{40}), lenientFrameAge());
        auto const status = side.recorder->emit(
            trace::TraceEvent{.kind = trace::TraceEventKind::RunStarted}
        );
        REQUIRE(status.has_value());

        auto cycle = side.context->openCycle();
        REQUIRE(cycle.has_value());
        auto const closed = side.context->closeCycle(*cycle);
        CHECK(closed);

        CHECK(side.recorder->frontEnd() == trace::FrontEnd::Task);
        REQUIRE_FALSE(side.traces->events().empty());
        for (auto const& event : side.traces->events())
        {
            CHECK(event.frontEnd() == trace::FrontEnd::Task);
            CHECK(event.frontEnd() != trace::FrontEnd::Operator);
        }
    }

    TEST_CASE("an operator stream refuses a framework event outright")
    {
        // The validator is authoritative for the new field rather than merely carrying
        // it: framework.* events describe the trusted Luau framework's own structure,
        // and on an operator stream that framework does not exist, so such a line
        // could only be a host bug attributing task structure to the operator.
        auto validator = trace::TraceStreamValidator{trace::FrontEnd::Operator};
        auto const refused = validator.admit(
            trace::TraceEvent{
                .kind      = trace::TraceEventKind::FrameworkStepStarted,
                .framework = trace::TraceEvent::Framework{.label = "daily"},
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            automationErrorKind(refused.error())
            == AutomationErrorKind::InternalInvariant
        );

        auto taskStream = trace::TraceStreamValidator{trace::FrontEnd::Task};
        auto const admitted = taskStream.admit(
            trace::TraceEvent{
                .kind      = trace::TraceEventKind::FrameworkStepStarted,
                .framework = trace::TraceEvent::Framework{.label = "daily"},
            }
        );
        CHECK(admitted.has_value());
    }
}
