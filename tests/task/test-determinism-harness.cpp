#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <script/engine.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <doctest/doctest.h>

#include <bit>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The determinism regression harness for veto #4: run the same task script over
// a fixed observation sequence and a fixed seed, reduce every run to a canonical
// "action trace" string (delivered click points plus the trace event sequence),
// and assert it is byte-identical across repeated runs. A reverse control with a
// second seed proves the harness observes the run rather than comparing empty
// strings. The veto is "identical on one machine, 1000x" and deliberately NOT a
// cross-platform bit-level claim, so runs are only ever compared on one machine.
namespace uf::task
{
    namespace
    {
        // The representative script: three observed frames, two template
        // searches that complete far from the template and one that completes
        // exactly on it and is clicked, plus ctx:settle and ctx:random -- the
        // two host facilities that make determinism a real risk rather than a
        // triviality. A zero-millisecond settle() is still a traced native call
        // carrying the duration the script asked for, so a pause that changed
        // length would change the canonical string. The random tail draws a
        // fixed number of values and lets each decide whether to run one extra
        // observation cycle, so the native call sequence encodes the random
        // stream and depends on the seed and nothing else.
        [[nodiscard]]
        auto harnessScript() -> std::string
        {
            return "local tpl = ctx:template_load(" + templateLiteral(k_targetActionGray)
                + ")\n" + R"lua(
            ctx:settle(0)

            -- Cycle 1: a frame the template is nowhere on, so its best position
            -- is a nonzero distance away.
            local c1 = ctx:cycle_open()
            local miss1 = ctx:cycle_match(c1, tpl, 0, 0, 3, 1)
            if miss1 == nil or miss1.score == 0 then
                error("frame 1 must not carry the template")
            end
            ctx:cycle_close(c1)

            -- Cycle 2: the same, on a different frame.
            local c2 = ctx:cycle_open()
            local miss2 = ctx:cycle_match(c2, tpl, 0, 0, 3, 1)
            if miss2 == nil or miss2.score == 0 then
                error("frame 2 must not carry the template")
            end
            ctx:cycle_close(c2)

            -- Cycle 3: the template IS on the frame -- distance zero -- and one
            -- click is delivered.
            local c3 = ctx:cycle_open()
            local hit = ctx:cycle_match(c3, tpl, 0, 0, 3, 1)
            if hit == nil or hit.score ~= 0 then
                error("frame 3 must carry the template")
            end
            ctx:cycle_click(c3, hit)

            -- Seed-dependent tail: a fixed number of draws, each deciding whether
            -- to run one extra observation cycle. The resulting native call
            -- pattern encodes the random stream, so a changed seed changes the
            -- emitted action trace while a repeated seed reproduces it exactly.
            for _ = 1, 24 do
                if ctx:random(1, 2) == 1 then
                    local extra = ctx:cycle_open()
                    ctx:cycle_close(extra)
                end
            end

            return 1
        )lua";
        }

        // The fixed frame sequence: two frames the template is not on, then one
        // it is. Materialized fresh on every call so each capture instant is
        // current -- the 750 ms action-frame lease (k_defaultMaxActionFrameAge)
        // would otherwise expire partway through a long veto loop. Pixels and
        // frame identities are identical every call; only the capture instant,
        // which never reaches the output, differs.
        [[nodiscard]]
        auto planFrames() -> std::vector<Frame>
        {
            auto const fp = fixtureFingerprint();
            auto frames   = std::vector<Frame>{};
            frames.emplace_back(grayFrame(fp, unknownPixels(), FrameId{101}));
            frames.emplace_back(grayFrame(fp, resolvedTargetlessPixels(), FrameId{102}));
            // FrameId 103 carries the template; the two above do not.
            frames.emplace_back(grayFrame(fp, resolvingPixels(), FrameId{103}));
            return frames;
        }

        // A session over the fixed frame plan, wired to a recording action sink
        // and one recorder shared with the TaskContext. The recorder is declared
        // first and held through a unique_ptr: the session borrows it and must
        // die first, and its address must survive this struct being moved.
        struct RecordingBuild final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
            RecordingActionSink*                  clicks;
            RecordingTraceSink*                   traces;
        };

        [[nodiscard]]
        auto buildRecordingSession() -> RecordingBuild
        {
            auto        actionSink = std::make_unique<RecordingActionSink>();
            auto        traceSink  = std::make_unique<RecordingTraceSink>();
            auto* const p_clicks   = actionSink.get();
            auto* const p_traces   = traceSink.get();
            auto        recorder   = std::make_unique<trace::TraceRecorder>(
                std::move(traceSink),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Task
            );
            auto session = engine::EngineSession::create(
                std::make_unique<FakeFrameSource>(planFrames()),
                std::move(actionSink),
                *recorder,
                baseConfig(fixtureFingerprint())
            );
            return RecordingBuild{
                .recorder = std::move(recorder),
                .session  = std::move(session),
                .clicks   = p_clicks,
                .traces   = p_traces,
            };
        }

        // Reduces one run to its canonical action-trace string: every delivered
        // click point, then every serialized trace line with the non-golden
        // `meta` member stripped -- the wall clock is the one field that
        // legitimately differs between two runs at the same seed. Click
        // coordinates are emitted as bit patterns, bypassing float-to-text
        // formatting whose stability is not the property under test.
        [[nodiscard]]
        auto canonicalize(
            RecordingActionSink const& clicks,
            std::vector<trace::StampedTraceEvent> const& events
        ) -> std::string
        {
            auto out = std::string{"clicks\n"};
            for (auto const point : clicks.points())
            {
                out += std::format(
                    "{:08x} {:08x}\n",
                    std::bit_cast<uint32>(point.x()),
                    std::bit_cast<uint32>(point.y())
                );
            }
            out += "trace\n";
            for (auto const& event : events)
            {
                out += trace::stripNonGoldenFields(trace::serializeTraceEvent(event));
                out += '\n';
            }
            return out;
        }

        // Runs the harness script once under `seed`. Two runs with the same seed
        // must return byte-identical strings on this machine.
        [[nodiscard]]
        auto runOnce(uint64 seed) -> std::string
        {
            auto built = buildRecordingSession();
            REQUIRE(built.session.has_value());

            // The context borrows the same recorder the session does, so this
            // run records one merged stream, and it is declared before the VM
            // whose verbs reach it.
            auto context = TaskContext{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.randomSeed = seed},
            };

            auto vm = script::Engine::create(taskVmConfig(context));
            REQUIRE(vm.has_value());
            auto const result = vm->runNumber(harnessScript(), "determinism-harness");
            REQUIRE(result.has_value());

            return canonicalize(*built.clicks, built.traces->events());
        }

        // Two seeds known to diverge within a few draws (the same pair the RNG
        // reproducibility test uses).
        constexpr auto k_vetoSeed    = uint64{0x00C0'FFEE};
        constexpr auto k_controlSeed = uint64{0x0BAD'F00D};

        // Repetitions, kept inside the per-test ctest timeout. The veto is
        // defined as 1000x on one machine; raise this to 1000 to run it locally
        // (each round is well under a millisecond). It is not a claim that CI
        // executed 1000 iterations -- it executes exactly this many.
        constexpr int k_vetoIterations = 300;

        TEST_CASE("veto #4: one seed reproduces the action trace byte-for-byte across runs")
        {
            auto const baseline = runOnce(k_vetoSeed);

            // The baseline must carry real content, or the equality checks below
            // would be vacuous.
            REQUIRE(baseline.starts_with("clicks\n"));
            CHECK(baseline.find("\ntrace\n") != std::string::npos);
            CHECK(
                baseline.find(
                    "\"verb\":\"cycle_click\",\"cycleOrdinal\":3"
                    ",\"hitCycleOrdinal\":3,\"outcome\":\"Succeeded\""
                )
                != std::string::npos
            );

            // The merged stream also carries the engine work the click rested on,
            // so the canonical string is not a task-only projection any more.
            CHECK(
                baseline.find("\"kind\":\"engine.action_delivered\"")
                != std::string::npos
            );

            for (int iteration = 0; iteration < k_vetoIterations; ++iteration)
            {
                CHECK(runOnce(k_vetoSeed) == baseline);
            }
        }

        TEST_CASE("veto #4 control: a different seed changes the action trace")
        {
            // Without this, "1000 runs all matched" could mean the harness
            // observes nothing. A second seed drives the random tail down a
            // different path, and that difference must itself be reproducible.
            auto const veto        = runOnce(k_vetoSeed);
            auto const control     = runOnce(k_controlSeed);
            auto const controlAgain = runOnce(k_controlSeed);

            CHECK(veto != control);
            CHECK(control == controlAgain);
        }
    }
}
