#include "binding-fixture.hpp"

#include <task/capability-surface.hpp>
#include <task/task-context.hpp>
#include <task/trace.hpp>

#include <script/engine.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

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
// a fixed synthetic observation sequence and a fixed seed, reduce every run to a
// canonical "action trace" string (the delivered click points plus the
// task-trace event sequence), and assert the string is byte-identical across
// repeated runs. A reverse control with a second seed proves the harness
// actually observes the run rather than comparing empty strings.
//
// Scope of the guarantee, per the hardening ledger: the veto is "identical on
// one machine, 1000x". It is deliberately NOT a cross-platform bit-level claim
// (transcendental functions and libc formatting are not bit-identical across
// CPUs/libm), so this suite only ever compares runs on the same machine.
namespace uf::task
{
    namespace
    {
        // The representative script. It walks a PageUnknown -> PageResolved
        // transition (frame 1 then frame 2), records a find that misses on the
        // unknown frame and a successful recognition + click on the resolved one,
        // and reads umbra:now / umbra:random -- the two host facilities that make
        // determinism a real risk rather than a triviality.
        //
        // now() is the logical clock: it is fully virtualized (a fixed tick per
        // read, no wall clock), so its readings are reproducible run to run and
        // could safely shape the output; the script still uses it only for a
        // monotonic guard, keeping this suite's focus on the RNG path. random() is
        // the seeded RNG: the tail loop draws a fixed number of values and lets each
        // decide whether to take one extra capture, so the emitted HostCall sequence
        // encodes the random stream and depends on the seed and nothing else.
        constexpr std::string_view k_harnessScript = R"lua(
            local t0 = umbra:now()

            -- Frame 1: an unknown page and a find that completes with no match.
            local f1 = umbra:capture()
            local page1 = f1:resolve_page():resolved()
            local miss = f1:find(umbra.recognizers.action_target)
            if page1 ~= nil then error("frame 1 must be an unknown page") end
            if miss ~= nil then error("frame 1 find must miss") end

            -- Frame 2: resolves page_a, the target hits, one click is delivered.
            local f2 = umbra:capture()
            local page2 = f2:resolve_page():resolved()
            if page2 == nil or not page2:is(umbra.pages.page_a) then
                error("frame 2 must resolve page_a")
            end
            local hit = f2:find(umbra.recognizers.action_target)
            if hit == nil then error("frame 2 target must hit") end
            umbra:click(page2, hit)

            -- Seed-dependent tail: a fixed number of draws, each deciding whether
            -- to take one extra capture. The resulting HostCall pattern encodes the
            -- random stream, so a changed seed changes the emitted action trace
            -- while a repeated seed reproduces it exactly.
            for _ = 1, 24 do
                if umbra:random(1, 2) == 1 then
                    umbra:capture()
                end
            end

            local t1 = umbra:now()
            if t1 < t0 then error("now went backwards") end
            return 1
        )lua";

        // The fixed synthetic frame sequence: one unknown frame then one that
        // resolves page_a. Materialized fresh on every call so each frame's
        // capture instant is current -- the 750 ms action-frame lease
        // (k_defaultMaxActionFrameAge) would otherwise expire partway through a
        // long veto loop and make late runs diverge from early ones. The pixel
        // content and frame identities are identical every call, so the
        // observation each run sees is the same; only the capture instant (which
        // never reaches the output) differs.
        [[nodiscard]]
        auto planFrames() -> std::vector<Frame>
        {
            auto const fp = anno::test::fingerprint(3, 1, 96, 96);
            auto frames   = std::vector<Frame>{};
            frames.emplace_back(grayFrame(fp, unknownPixels(), FrameId{101}));
            frames.emplace_back(grayFrame(fp, resolvingPixels(), FrameId{102}));
            return frames;
        }

        // A session over the fixed frame plan wired to a recording action sink, so
        // the harness can read back every delivered click point. The surface is
        // built from the runtime's own catalog before the runtime moves into the
        // session, exactly as the binding fixture does.
        struct RecordingBuild final
        {
            Result<engine::EngineSession> session;
            CapabilitySurface             surface;
            RecordingActionSink*          clicks;
        };

        [[nodiscard]]
        auto buildRecordingSession() -> RecordingBuild
        {
            auto parts   = singlePageRuntime();
            auto surface = CapabilitySurface::create(
                parts.loaded.runtime.manifest().catalog()
            );
            REQUIRE(surface.has_value());

            auto        actionSink = std::make_unique<RecordingActionSink>();
            auto        traceSink  = std::make_unique<DiscardingTraceSink>();
            auto* const p_clicks   = actionSink.get();
            auto        session    = engine::EngineSession::create(
                std::move(parts.loaded),
                std::make_unique<FakeFrameSource>(planFrames()),
                std::move(actionSink),
                std::move(traceSink),
                baseConfig(parts.fingerprint)
            );
            return RecordingBuild{
                .session = std::move(session),
                .surface = *std::move(surface),
                .clicks  = p_clicks,
            };
        }

        // Reduces one run to its canonical action-trace string: every delivered
        // click point, then every task-trace event. Click coordinates are floats
        // derived deterministically from integer recognition, so their bit
        // patterns are emitted directly -- bypassing any float-to-text formatting
        // whose stability is not the property under test.
        [[nodiscard]]
        auto canonicalize(
            RecordingActionSink const& clicks,
            std::vector<TaskTraceEvent> const& events
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
                out += serializeTaskTraceEvent(event);
                out += '\n';
            }
            return out;
        }

        // Runs the harness script once under `seed` and returns its canonical
        // action-trace string. Two runs with the same seed must return
        // byte-identical strings on this machine.
        [[nodiscard]]
        auto runOnce(uint64 seed) -> std::string
        {
            auto built = buildRecordingSession();
            REQUIRE(built.session.has_value());

            // `events` outlives the context that owns the recording sink writing
            // into it, so it is declared first; the context outlives the VM whose
            // verbs reach it, so it is declared before the VM.
            auto events  = std::vector<TaskTraceEvent>{};
            auto context = TaskContext{
                *std::move(built.session),
                TaskContextConfig{
                    // Disable the retention guard so no forced VM collection ever
                    // runs mid-script: the tail's extra captures simply accumulate.
                    // GC timing does not reach the output, but removing the guard
                    // keeps the run's shape one variable simpler.
                    .maxLiveObservations = 0,
                    .randomSeed          = seed,
                },
                std::make_unique<RecordingTaskTraceSink>(&events),
            };

            auto vm = script::Engine::create(
                script::EngineConfig{
                    .installHostTables = built.surface.installer(context),
                }
            );
            REQUIRE(vm.has_value());
            auto const result = vm->runNumber(k_harnessScript, "determinism-harness");
            REQUIRE(result.has_value());

            return canonicalize(*built.clicks, events);
        }

        // Two seeds known to diverge within a few draws (the same pair the RNG
        // reproducibility test uses). The control seed must produce a different
        // action trace than the veto seed, which the reverse-control case asserts.
        constexpr auto k_vetoSeed    = uint64{0x00C0'FFEE};
        constexpr auto k_controlSeed = uint64{0x0BAD'F00D};

        // CI runs this many repetitions to stay within the per-test ctest timeout.
        // The veto is defined as 1000x on one machine; raise this constant to 1000
        // to run the full veto locally (each round is well under a millisecond, so
        // 1000 still finishes in about a second). The value is not a claim that CI
        // executed 1000 iterations -- it executes exactly this many.
        constexpr int k_vetoIterations = 300;

        TEST_CASE("veto #4: one seed reproduces the action trace byte-for-byte across runs")
        {
            auto const baseline = runOnce(k_vetoSeed);

            // The baseline must carry real content, or the equality checks below
            // would be vacuous: it starts with the clicks section, records exactly
            // one delivered click (frame 2), and contains the successful click
            // HostCall the script's automation flow emits.
            REQUIRE(baseline.starts_with("clicks\n"));
            CHECK(baseline.find("\ntrace\n") != std::string::npos);
            CHECK(
                baseline.find(
                    "\"kind\":\"HostCall\",\"verb\":\"click\",\"outcome\":\"Succeeded\""
                )
                != std::string::npos
            );

            for (int iteration = 0; iteration < k_vetoIterations; ++iteration)
            {
                CHECK(runOnce(k_vetoSeed) == baseline);
            }
        }

        TEST_CASE("veto #4 control: a different seed changes the action trace")
        {
            // Without this, "1000 runs all matched" could mean the harness observes
            // nothing. A second seed drives the random tail down a different path,
            // so its canonical action trace must differ from the veto seed's -- and
            // that difference must itself be reproducible, not run-to-run noise.
            auto const veto        = runOnce(k_vetoSeed);
            auto const control     = runOnce(k_controlSeed);
            auto const controlAgain = runOnce(k_controlSeed);

            CHECK(veto != control);
            CHECK(control == controlAgain);
        }
    }
}
