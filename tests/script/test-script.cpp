#include <script/engine.hpp>
#include <script/testing/cancel-probe.hpp>
#include <script/testing/memory-probe.hpp>
#include <script/testing/sandbox-probe.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace uf::script
{
    namespace
    {
        // Fills fast under a runaway allocator, yet clears the openlibs/sandbox baseline.
        constexpr uint64 k_smallQuotaBytes = uint64{16} * 1024 * 1024;

        // Run `return (<expr>) and 1 or 0`; 1.0 means the expression was truthy.
        [[nodiscard]]
        auto truthOf(Engine& engine, std::string_view expr) -> Result<double>
        {
            auto const source = "return (" + std::string{expr} + ") and 1 or 0";
            return engine.runNumber(source, "sandbox-expr");
        }

        // Isolate an external stop token as the only break lever, then assert the
        // run returns Cancelled well inside the 500ms cancellation budget.
        auto expectExternalStopCancels(
            std::string_view source,
            std::string_view chunkName
        ) -> void
        {
            auto stopSource     = std::stop_source{};
            auto config         = EngineConfig{};
            config.cancellation = stopSource.get_token();
            config.interruptBudgetTicks = 0;                       // isolate the stop token
            config.maxRuntime           = std::chrono::hours{1};

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            auto watchdog = std::jthread{
                [canceller = stopSource]() mutable
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{30});
                    canceller.request_stop();
                }
            };

            auto const start   = std::chrono::steady_clock::now();
            auto const result  = engine->runNumber(source, chunkName);
            auto const elapsed = std::chrono::steady_clock::now() - start;

            CHECK(elapsed < std::chrono::milliseconds{500});
            REQUIRE_FALSE(result.has_value());
            CHECK(automationErrorKind(result.error()) == AutomationErrorKind::Cancelled);
        }

        TEST_CASE("Engine runs a Luau script and returns its numeric result")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            auto const result = engine->runNumber("return 1 + 2", "sum");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(3.0));
        }

        TEST_CASE("Engine reports a compile/load error as a recoverable failure")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            auto const result = engine->runNumber("return 1 +", "broken");
            REQUIRE_FALSE(result.has_value());

            auto const kind = automationErrorKind(result.error());
            REQUIRE(kind.has_value());
            CHECK(kind == AutomationErrorKind::InvalidResource);
        }

        TEST_CASE("Engine reports a runtime error as a recoverable failure")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            auto const result = engine->runNumber("error('boom')", "runtime");
            REQUIRE_FALSE(result.has_value());

            auto const kind = automationErrorKind(result.error());
            REQUIRE(kind.has_value());
            CHECK(kind == AutomationErrorKind::InvalidResource);
        }

        TEST_CASE("Engine returns zero when there is no numeric result")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            SUBCASE("no return value")
            {
                auto const result = engine->runNumber("local x = 5", "noreturn");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(0.0));
            }
            SUBCASE("non-numeric return value")
            {
                auto const result = engine->runNumber("return 'text'", "nonnumeric");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(0.0));
            }
        }

        TEST_CASE("Engine does not accumulate state across repeated runs")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            // A per-call thread/stack leak would eventually destabilize the main state.
            for (int i = 0; i < 500; ++i)
            {
                auto const ok = engine->runNumber("return 41 + 1", "loop");
                REQUIRE(ok.has_value());
                CHECK(*ok == doctest::Approx(42.0));
            }
        }

        TEST_CASE("Engine is move-only and usable after a move")
        {
            auto source = Engine::create();
            REQUIRE(source.has_value());

            auto moved = std::move(*source);
            auto const result = moved.runNumber("return 7", "moved");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(7.0));
        }

        TEST_CASE("Sandbox removes the dangerous loaders, escapes, and clocks")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            // Bytecode/loader egress, the survivors luaL_sandbox leaves behind, and
            // the residual clock/RNG.
            constexpr auto absent = std::to_array<std::string_view>({
                "load == nil",
                "loadstring == nil",
                "dofile == nil",
                "loadfile == nil",
                "string.dump == nil",
                "getfenv == nil",
                "setfenv == nil",
                "newproxy == nil",
                "_G == nil",
                "coroutine == nil",
                "debug == nil",
                "gcinfo == nil",
                "os.time == nil",
                "os.clock == nil",
                "os.date == nil",
                "math.random == nil",
                "math.randomseed == nil",
            });
            for (std::string_view const expr : absent)
            {
                INFO("expression: ", expr);
                auto const result = truthOf(*engine, expr);
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("Sandbox keeps the deterministic standard library usable")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            // Anti-vacuity guard for the removal case above: the safe library surface
            // must survive the sandbox intact.
            constexpr auto present = std::to_array<std::string_view>({
                "type('x') == 'string'",
                "math.floor(3.7) == 3",
                "string.rep('a', 3) == 'aaa'",
                "table.concat({10, 20}, '-') == '10-20'",
                "select('#', 1, 2, 3) == 3",
            });
            for (std::string_view const expr : present)
            {
                INFO("expression: ", expr);
                auto const result = truthOf(*engine, expr);
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("Sandbox rejects writes to the frozen library tables")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            SUBCASE("new field on a frozen library")
            {
                auto const result = engine->runNumber("string.injected = 1", "freeze-lib");
                REQUIRE_FALSE(result.has_value());
                CHECK(automationErrorKind(result.error()) == AutomationErrorKind::InvalidResource);
            }
            SUBCASE("existing field on a frozen library")
            {
                auto const result = engine->runNumber("math.pi = 4", "freeze-const");
                REQUIRE_FALSE(result.has_value());
                CHECK(automationErrorKind(result.error()) == AutomationErrorKind::InvalidResource);
            }
        }

        TEST_CASE("A task thread does not share mutable globals with the next run")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            auto const first = engine->runNumber("run_marker = 7\nreturn run_marker", "iso-1");
            REQUIRE(first.has_value());
            CHECK(*first == doctest::Approx(7.0));

            // A fresh luaL_sandboxthread each run means run_marker is gone.
            auto const second = truthOf(*engine, "run_marker == nil");
            REQUIRE(second.has_value());
            CHECK(*second == doctest::Approx(1.0));
        }

        TEST_CASE("Two Engine instances do not leak globals to each other")
        {
            auto first  = Engine::create();
            auto second = Engine::create();
            REQUIRE(first.has_value());
            REQUIRE(second.has_value());

            auto const written = first->runNumber("leaked = 9\nreturn leaked", "leak-1");
            REQUIRE(written.has_value());
            CHECK(*written == doctest::Approx(9.0));

            auto const isolated = truthOf(*second, "leaked == nil");
            REQUIRE(isolated.has_value());
            CHECK(*isolated == doctest::Approx(1.0));
        }

        TEST_CASE("deepFreeze makes nested host tables read-only")
        {
            using testing::runWithFrozenHostTable;

            SUBCASE("nested host value is readable")
            {
                auto const result = runWithFrozenHostTable("return host.nested.value", "probe-read");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
            SUBCASE("writing a nested host field is rejected")
            {
                auto const result = runWithFrozenHostTable("host.nested.value = 2\nreturn 0", "probe-nested");
                REQUIRE_FALSE(result.has_value());
                CHECK(automationErrorKind(result.error()) == AutomationErrorKind::InvalidResource);
            }
            SUBCASE("writing a table reachable only as a key is rejected")
            {
                auto const result = runWithFrozenHostTable(
                    "local key = next(host.keyed)\nkey.value = 2\nreturn 0",
                    "probe-table-key"
                );
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::InvalidResource
                );
            }
            SUBCASE("writing a top-level host field is rejected")
            {
                auto const result = runWithFrozenHostTable("host.flat = 9\nreturn 0", "probe-top");
                REQUIRE_FALSE(result.has_value());
                CHECK(automationErrorKind(result.error()) == AutomationErrorKind::InvalidResource);
            }
            SUBCASE("a value inherited through the metatable is readable")
            {
                // Anti-vacuity control for the metatable subcases: a rejected write
                // cannot be an artifact of there being no metatable at all.
                auto const result =
                    runWithFrozenHostTable("return host.inherited", "probe-meta-read");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(5.0));
            }
            SUBCASE("the metatable is not handed out at all")
            {
                // deepFreeze stamps __metatable on every metatable it walks, so
                // getmetatable hands out the label, not the table. Outer of the two
                // guards on the monkey-patch hole a values-only walk would leave open.
                auto const isLabel = runWithFrozenHostTable(
                    "return getmetatable(host) == 'probe.host' and 1 or 0",
                    "probe-meta-label-value"
                );
                REQUIRE(isLabel.has_value());
                CHECK(*isLabel == doctest::Approx(1.0));
            }
            SUBCASE("rewriting the host metatable is rejected")
            {
                // Inner guard: reached directly the metatable is still read-only, so
                // __index cannot be swapped to shadow every frozen field underneath.
                auto const result = runWithFrozenHostTable(
                    "getmetatable(host).__index = { inherited = 99 }\nreturn 0",
                    "probe-meta-write"
                );
                REQUIRE_FALSE(result.has_value());
                CHECK(automationErrorKind(result.error()) == AutomationErrorKind::InvalidResource);
            }
        }

        TEST_CASE("An external stop hard-cancels a runaway loop within the budget")
        {
            SUBCASE("plain infinite loop")
            {
                expectExternalStopCancels("while true do end", "runaway-plain");
            }
            SUBCASE("pcall-wrapped infinite loop")
            {
                expectExternalStopCancels(
                    "pcall(function() while true do end end)",
                    "runaway-pcall"
                );
            }
            SUBCASE("nested pcall infinite loop")
            {
                expectExternalStopCancels(
                    "pcall(function() pcall(function() while true do end end) end)",
                    "runaway-nested"
                );
            }
        }

        TEST_CASE("A cancelled script never executes past the break")
        {
            // Uncatchability made observable. Engine reports Cancelled the instant
            // InterruptState::broken is set -- it would say so even if a swallowed
            // break had let the script run on -- so that assertion alone cannot
            // witness it. This probe binds a host-visible mark() after the runaway
            // and asserts markCount stays 0; it goes red if lua_break regresses to a
            // pcall-catchable error, because control would then reach mark(). The
            // lever is the instruction budget, the same lua_break primitive every
            // cancel source funnels through in onInterrupt.
            using testing::probeCancellation;

            constexpr uint64 k_tripBudget = uint64{100'000};

            SUBCASE("positive control: mark() is reached on an uncancelled run")
            {
                // Anti-vacuity guard: with no break lever armed mark() is reached, so
                // a zero markCount below can only mean the break cut the script off.
                auto const probe =
                    probeCancellation("mark()\nreturn 1", "mark-control", 0);
                CHECK_FALSE(probe.cancelled);
                CHECK(probe.markCount == 1);
            }
            SUBCASE("a pcall-wrapped runaway never reaches the mark() after it")
            {
                auto const probe = probeCancellation(
                    "pcall(function() while true do end end)\nmark()\nreturn 1",
                    "mark-pcall",
                    k_tripBudget
                );
                CHECK(probe.cancelled);
                CHECK(probe.markCount == 0);
            }
            SUBCASE("a bare runaway never reaches the mark() after it")
            {
                auto const probe = probeCancellation(
                    "while true do end\nmark()\nreturn 1",
                    "mark-bare",
                    k_tripBudget
                );
                CHECK(probe.cancelled);
                CHECK(probe.markCount == 0);
            }
        }

        TEST_CASE("The instruction budget alone reports Cancelled at the engine boundary")
        {
            auto config                 = EngineConfig{};
            config.interruptBudgetTicks = 1000;                    // trips on its own
            config.maxRuntime           = std::chrono::hours{1};

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            // Only the instruction budget is armed, and the interrupt trips it even
            // inside a pcall. This is the public-API budget path; uncatchability is
            // proven by "A cancelled script never executes past the break".
            auto const result = engine->runNumber(
                "pcall(function() while true do end end)",
                "budget"
            );
            REQUIRE_FALSE(result.has_value());
            CHECK(automationErrorKind(result.error()) == AutomationErrorKind::Cancelled);
        }

        TEST_CASE("Exceeding maxRuntime hard-stops within the budget")
        {
            auto config                 = EngineConfig{};
            config.interruptBudgetTicks = 0;                       // isolate the time budget
            config.maxRuntime           = std::chrono::milliseconds{50};

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            auto const start   = std::chrono::steady_clock::now();
            auto const result  = engine->runNumber(
                "pcall(function() while true do end end)\nreturn 1",
                "timeout"
            );
            auto const elapsed = std::chrono::steady_clock::now() - start;

            CHECK(elapsed < std::chrono::milliseconds{500});
            REQUIRE_FALSE(result.has_value());
            CHECK(automationErrorKind(result.error()) == AutomationErrorKind::Cancelled);
        }

        TEST_CASE("The wall clock measures the running script, not the VM's age")
        {
            // Anchoring the deadline at construction charges the VM for the idle gap
            // between two chunks and kills the second one, so it is re-anchored per
            // run. Restore the construction anchor and the 300ms gap below goes red.
            auto config                 = EngineConfig{};
            config.interruptBudgetTicks = 0;                       // isolate the clock
            config.maxRuntime           = std::chrono::milliseconds{120};

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            auto const first = engine->runNumber("return 1", "before-the-gap");
            REQUIRE(first.has_value());

            std::this_thread::sleep_for(std::chrono::milliseconds{300});

            auto const second = engine->runNumber("return 2", "after-the-gap");
            REQUIRE(second.has_value());
            CHECK(*second == doctest::Approx(2.0));

            // The other direction on the SAME engine after the same gap: the clock is
            // re-anchored, not disarmed, so a runaway third chunk is still stopped
            // inside its own ceiling.
            auto const start   = std::chrono::steady_clock::now();
            auto const runaway = engine->runNumber(
                "local n = 0 for i = 1, 100000000 do n = n + 1 end return n",
                "runaway-after-the-gap"
            );
            auto const elapsed = std::chrono::steady_clock::now() - start;

            REQUIRE_FALSE(runaway.has_value());
            CHECK(automationErrorKind(runaway.error()) == AutomationErrorKind::Cancelled);
            CHECK(elapsed < std::chrono::milliseconds{2000});
        }

        TEST_CASE("A hard cancel names the trigger that caused it")
        {
            // All three triggers land on one lua_break, so only the message can say
            // which of them fired.
            SUBCASE("an expired ceiling names the clock, at both boundaries")
            {
                auto config                 = EngineConfig{};
                config.interruptBudgetTicks = 0;
                config.maxRuntime           = std::chrono::milliseconds{80};

                auto engine = Engine::create(config);
                REQUIRE(engine.has_value());

                auto const stopped = engine->runNumber(
                    "local n = 0 for i = 1, 100000000 do n = n + 1 end return n",
                    "on-the-clock"
                );
                REQUIRE_FALSE(stopped.has_value());
                CHECK(stopped.error().message().contains("wall-clock ceiling"));

                // The refusal a spent generation returns afterwards is the only message
                // an agent still gets once its session is over, so it must name the
                // clock too.
                auto const after = engine->runNumber("return 1", "after-the-clock");
                REQUIRE_FALSE(after.has_value());
                CHECK(after.error().message().contains("wall-clock ceiling"));
            }
            SUBCASE("a spent instruction budget names the budget instead")
            {
                // The sentence is read off what actually fired: a constant string
                // mentioning the clock passes the case above and fails here.
                auto config                 = EngineConfig{};
                config.interruptBudgetTicks = 1000;
                config.maxRuntime           = std::chrono::hours{1};

                auto engine = Engine::create(config);
                REQUIRE(engine.has_value());

                auto const stopped = engine->runNumber("while true do end", "on-budget");
                REQUIRE_FALSE(stopped.has_value());
                CHECK(stopped.error().message().contains("instruction budget"));
                CHECK_FALSE(stopped.error().message().contains("wall-clock ceiling"));
            }
        }

        TEST_CASE("A terminal engine refuses further runs with Cancelled")
        {
            auto config                 = EngineConfig{};
            config.interruptBudgetTicks = 1000;

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            auto const tripped = engine->runNumber("while true do end", "trip");
            REQUIRE_FALSE(tripped.has_value());
            CHECK(automationErrorKind(tripped.error()) == AutomationErrorKind::Cancelled);

            // The generation is spent: a trivial script is refused without resuming
            // the abandoned VM.
            auto const after = engine->runNumber("return 1", "after-terminal");
            REQUIRE_FALSE(after.has_value());
            CHECK(automationErrorKind(after.error()) == AutomationErrorKind::Cancelled);
        }

        TEST_CASE("An untriggered stop token leaves a short script unaffected")
        {
            auto stopSource     = std::stop_source{};   // never requested
            auto config         = EngineConfig{};
            config.cancellation = stopSource.get_token();

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            auto const result = engine->runNumber("return 6 * 7", "with-token");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(42.0));
        }

        TEST_CASE("A memory-quota breach is an ordinary, catchable error")
        {
            auto config             = EngineConfig{};
            config.memoryQuotaBytes = k_smallQuotaBytes;
            // Isolate the ceiling: only the allocator may stop the run.
            config.interruptBudgetTicks = 0;
            config.maxRuntime           = std::chrono::hours{1};

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            SUBCASE("an uncaught over-quota allocation is a recoverable runtime error")
            {
                // Distinct table objects kept reachable in `t`. An uncaught
                // out-of-memory is an ordinary runtime failure, NOT the uncatchable
                // Cancelled of a hard break.
                auto const result = engine->runNumber(
                    "local t = {} while true do t[#t + 1] = {} end",
                    "quota-oom"
                );
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::InvalidResource
                );
            }
            SUBCASE("the over-quota error can be caught by pcall")
            {
                // Were the breach an uncatchable break the outer script would fail
                // Cancelled; instead pcall catches it and ok is false -> 0.
                auto const result = engine->runNumber(
                    "local ok = pcall(function()\n"
                    "  local t = {} while true do t[#t + 1] = {} end\n"
                    "end)\n"
                    "return ok and 1 or 0",
                    "quota-pcall"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(0.0));
            }
        }

        TEST_CASE("The host survives a quota breach and runs a fresh engine")
        {
            auto config                 = EngineConfig{};
            config.memoryQuotaBytes     = k_smallQuotaBytes;
            config.interruptBudgetTicks = 0;
            config.maxRuntime           = std::chrono::hours{1};

            auto breached = Engine::create(config);
            REQUIRE(breached.has_value());
            auto const breach = breached->runNumber(
                "local t = {} while true do t[#t + 1] = {} end",
                "quota-oom"
            );
            REQUIRE_FALSE(breach.has_value());

            // A fresh Engine in the same process has its own ledger: the breach
            // neither corrupted nor exhausted the host.
            auto fresh = Engine::create();
            REQUIRE(fresh.has_value());
            auto const ok = fresh->runNumber("return 20 + 22", "after-oom");
            REQUIRE(ok.has_value());
            CHECK(*ok == doctest::Approx(42.0));
        }

        TEST_CASE("Deep recursion stops at the Luau stack limit as an ordinary error")
        {
            // The default 64 MiB ceiling leaves the stack-depth limit to trip
            // first, and it raises an ordinary catchable error, not a hard break.
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            SUBCASE("uncaught non-tail recursion is a recoverable runtime error")
            {
                // `1 + f(n)` is not a tail call, so every level keeps a frame.
                auto const result = engine->runNumber(
                    "local function f(n) return 1 + f(n) end\n"
                    "return f(0)",
                    "deep-recursion"
                );
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::InvalidResource
                );
            }
            SUBCASE("the stack-overflow error can be caught by pcall")
            {
                auto const result = engine->runNumber(
                    "local ok = pcall(function()\n"
                    "  local function f(n) return 1 + f(n) end\n"
                    "  return f(0)\n"
                    "end)\n"
                    "return ok and 1 or 0",
                    "deep-recursion-pcall"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(0.0));
            }
        }

        TEST_CASE("An in-quota script runs unaffected by the accounting allocator")
        {
            auto config             = EngineConfig{};
            config.memoryQuotaBytes = uint64{64} * 1024 * 1024;

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            // Thousands of entries, well within the ceiling, must be vended untouched.
            auto const result = engine->runNumber(
                "local t = {} for i = 1, 10000 do t[i] = i * 2 end return t[10000]",
                "in-quota"
            );
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(20000.0));
        }

        TEST_CASE("The memory ledger accounts allocations and returns to zero on close")
        {
            using testing::measureMemory;

            // The residual is the leak check; a non-trivial peak guards against a
            // vacuously empty ledger. Zero limitBytes disables the ceiling.
            auto const baseline = measureMemory("return 1 + 1", "ledger-baseline", 0);
            CHECK(baseline.peak > 0);
            CHECK(baseline.residual == 0);

            // Thousands of live objects drive the peak above baseline, yet closing
            // the VM still frees everything to zero.
            auto const grown = measureMemory(
                "local t = {} for i = 1, 5000 do t[i] = {} end return 0",
                "ledger-grow",
                0
            );
            CHECK(grown.peak > baseline.peak);
            CHECK(grown.residual == 0);
        }

        TEST_CASE("A full collection reclaims a spent chunk, and the ledger reports it")
        {
            auto config                 = EngineConfig{};
            config.memoryQuotaBytes     = k_smallQuotaBytes;
            config.interruptBudgetTicks = 0;
            config.maxRuntime           = std::chrono::hours{1};

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            auto const idle = engine->heapUsage();
            CHECK(idle.ceilingBytes == k_smallQuotaBytes);
            CHECK(idle.usedBytes > 0);
            CHECK(idle.headroomBytes() == idle.ceilingBytes - idle.usedBytes);

            // Four megabytes REACHABLE from the run's own thread at the instant it
            // returns, so no step of the incremental collector can have taken them
            // during the run and the ledger afterwards reads uncollected garbage. The
            // index is concatenated on because Luau interns every string, long ones
            // included: sixty-four bare copies of string.rep('x', 65536) would be one
            // 64 KiB object and this case would measure nothing.
            auto const ran = engine->runNumber(
                "local t = {} for i = 1, 64 do"
                " t[i] = string.rep('x', 65536) .. tostring(i) end"
                " return #t",
                "gc-garbage"
            );
            REQUIRE(ran.has_value());
            CHECK(*ran == doctest::Approx(64.0));

            auto const before = engine->heapUsage();
            CHECK(before.usedBytes > idle.usedBytes + (uint64{3} * 1024 * 1024));
            CHECK(before.peakBytes >= before.usedBytes);

            auto const after = engine->collectGarbage();
            CHECK(after.usedBytes < before.usedBytes);
            CHECK(after.usedBytes < idle.usedBytes + (uint64{1} * 1024 * 1024));

            // The ceiling is a VM property and the peak a record: a collection running
            // may move neither.
            CHECK(after.ceilingBytes == before.ceilingBytes);
            CHECK(after.peakBytes == before.peakBytes);
        }

        TEST_CASE("Repeated create/destroy of quota'd engines stays process-stable")
        {
            // A leak in the accounting allocator or in VM teardown would accumulate
            // across these 200 build/run/destroy generations.
            for (int i = 0; i < 200; ++i)
            {
                auto config             = EngineConfig{};
                config.memoryQuotaBytes = k_smallQuotaBytes;

                auto engine = Engine::create(config);
                REQUIRE(engine.has_value());

                auto const result = engine->runNumber(
                    "local t = {} for j = 1, 500 do t[j] = {j} end return #t",
                    "churn"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(500.0));
            }
        }
    }
}
