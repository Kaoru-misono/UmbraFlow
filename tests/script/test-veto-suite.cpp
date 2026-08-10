#include <script/engine.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// The P0 six-veto gate made executable and regression-guarded on every Luau bump;
// the vetoes are the roadmap's §5 acceptance conditions
// (docs/plans/2026-07-21-product-form-and-roadmap.md). test-script.cpp already
// exercises the bulk of #1/#2/#3, so this file adds the remaining named cases plus
// the documented non-yieldable-C-boundary exception. Coverage that structurally
// belongs above the substrate is deliberately NOT faked here: veto #4's full
// observation/action trace and 1000x seed replay go to the B2 harness, veto #5's
// atomic generation swap and P2 hot-reload to modules/task, and veto #6's blocked
// host C binding to tests/task/test-veto-blocking.cpp.

namespace uf::script
{
    namespace
    {
        // Isolate an external stop token as the only break lever, then assert the run
        // is hard-cancelled well inside the 500ms exit budget. Duplicated from
        // test-script.cpp so this veto suite stays self-contained.
        auto expectExternalStopCancels(
            std::string_view source,
            std::string_view chunkName
        ) -> void
        {
            auto stopSource     = std::stop_source{};
            auto config         = EngineConfig{};
            config.cancellation = stopSource.get_token();
            config.interruptBudgetTicks = 0;                       // isolate the token
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

        // Determinism is asserted as exact bit-for-bit identity, never as a
        // float-equality comparison.
        [[nodiscard]]
        auto bitsOf(double value) -> uint64
        {
            return std::bit_cast<uint64>(value);
        }

        TEST_CASE(
            "Veto #6/C-boundary: a runaway loop inside a non-yieldable C boundary "
            "is still hard-cancelled"
        )
        {
            // Documented Luau limitation (integration-plan §5, Risk #1): an interrupt
            // that fires while the script is inside a non-yieldable host C function --
            // here table.sort / string.gsub calling back into Lua -- cannot unwind
            // across the C frame, so lua_break raises a catchable "attempt to break
            // across C-call boundary" error instead of a clean LUA_BREAK. That is
            // known-correct; do NOT "fix" it into a clean break. runNumber still
            // reports Cancelled because onInterrupt sets InterruptState::broken BEFORE
            // calling lua_break. Veto #6 proper -- a hanging host C binding -- lives in
            // tests/task/test-veto-blocking.cpp.
            SUBCASE("table.sort comparator infinite loop")
            {
                expectExternalStopCancels(
                    "table.sort({5, 4, 3, 2, 1}, function(a, b) while true do end end)",
                    "veto6-sort"
                );
            }
            SUBCASE("string.gsub callback infinite loop")
            {
                expectExternalStopCancels(
                    "string.gsub('aaaa', 'a', function() while true do end end)",
                    "veto6-gsub"
                );
            }
            SUBCASE("pcall around the C boundary still reports Cancelled at the engine boundary")
            {
                // The C-boundary error is individually pcall-catchable, yet the cancel
                // still wins at the Engine boundary. This case therefore verifies
                // Cancelled reporting across a C frame, NOT that the script's
                // post-cancel continuation was prevented; that stronger property is the
                // mark-counter discriminator in test-script.cpp ("A cancelled script
                // never executes past the break") and in tests/task/test-veto-blocking.cpp.
                expectExternalStopCancels(
                    "pcall(function()\n"
                    "  table.sort({5, 4, 3, 2, 1}, function(a, b) while true do end end)\n"
                    "end)\n"
                    "return 1",
                    "veto6-sort-pcall"
                );
            }
        }

        TEST_CASE(
            "Veto #2: an unbounded heavy string operation is a catchable per-task "
            "error, not a host crash"
        )
        {
            // Veto #2's third prong; test-script.cpp covers the other two (table
            // growth, non-tail recursion). The stopping mechanism is the accounting
            // allocator's hard quota, and the out-of-memory it raises is an ORDINARY
            // catchable error, never the uncatchable Cancelled of a hard break.
            constexpr uint64 smallQuota = uint64{16} * 1024 * 1024;

            auto config             = EngineConfig{};
            config.memoryQuotaBytes = smallQuota;
            config.interruptBudgetTicks = 0;                       // isolate the ceiling
            config.maxRuntime           = std::chrono::hours{1};

            auto engine = Engine::create(config);
            REQUIRE(engine.has_value());

            SUBCASE("uncaught unbounded string growth is a recoverable runtime error")
            {
                // Repeated doubling drives the live string past the ceiling; the
                // refusal surfaces as InvalidResource, NOT Cancelled.
                auto const result = engine->runNumber(
                    "local s = 'x' while true do s = s .. s end return #s",
                    "veto2-string"
                );
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::InvalidResource
                );
            }
            SUBCASE("the out-of-memory from string growth can be caught by pcall")
            {
                // Were the breach an uncatchable break the outer script would fail
                // Cancelled; instead pcall catches it and ok is false -> 0.
                auto const result = engine->runNumber(
                    "local ok = pcall(function()\n"
                    "  local s = 'x' while true do s = s .. s end\n"
                    "end)\n"
                    "return ok and 1 or 0",
                    "veto2-string-pcall"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(0.0));
            }
        }

        TEST_CASE(
            "Veto #3: filesystem, process, environment, and dynamic-loading access "
            "is denied"
        )
        {
            // luaL_openlibs never opens io/package and its os library never registers
            // the process and environment entries; installSandbox removes the manual GC
            // hook. Luau's base library has no networking at all, so there is none to
            // deny.
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            constexpr auto denied = std::to_array<std::string_view>({
                "io == nil",             // filesystem + stdio
                "require == nil",        // dynamic module loading
                "package == nil",        // module system table
                "os.execute == nil",     // spawn a process
                "os.getenv == nil",      // read environment variables
                "os.exit == nil",        // terminate the host process
                "os.remove == nil",      // delete a file
                "os.rename == nil",      // rename a file
                "os.tmpname == nil",     // reserve a temp file name
            });
            for (std::string_view const expr : denied)
            {
                INFO("expression: ", expr);
                auto const source = "return (" + std::string{expr} + ") and 1 or 0";
                auto const result = engine->runNumber(source, "veto3-denied");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("Veto #4: repeated runs of one script are bit-for-bit identical")
        {
            // The substrate's necessary foundation for veto #4: no per-run thread
            // setup, GC timing, or address noise may perturb the result. Same-machine
            // only, never a cross-platform bit-level claim -- transcendental and
            // libc-format paths are not cross-CPU identical, which is why the source
            // below sticks to integer-valued float arithmetic.
            constexpr std::string_view source =
                "local acc = 0.0\n"
                "for i = 1, 256 do acc = acc + (i % 7) * 0.25 end\n"
                "return acc";

            SUBCASE("1000 runs on one engine are identical")
            {
                auto engine = Engine::create();
                REQUIRE(engine.has_value());

                auto const first = engine->runNumber(source, "veto4-first");
                REQUIRE(first.has_value());
                auto const expected = bitsOf(*first);

                for (int i = 0; i < 1000; ++i)
                {
                    auto const again = engine->runNumber(source, "veto4-again");
                    REQUIRE(again.has_value());
                    CHECK(bitsOf(*again) == expected);
                }
            }

            SUBCASE("fresh engine generations agree bit-for-bit")
            {
                auto first = Engine::create();
                REQUIRE(first.has_value());
                auto const baseline = first->runNumber(source, "veto4-gen-first");
                REQUIRE(baseline.has_value());
                auto const expected = bitsOf(*baseline);

                for (int i = 0; i < 50; ++i)
                {
                    auto engine = Engine::create();
                    REQUIRE(engine.has_value());
                    auto const value = engine->runNumber(source, "veto4-gen");
                    REQUIRE(value.has_value());
                    CHECK(bitsOf(*value) == expected);
                }
            }
        }

        TEST_CASE("Veto #5: a compile failure leaves the engine and its siblings usable")
        {
            // Veto #5's load-boundary slice: a generation is installed only after
            // compile and self-check succeed, so a failed compile must not disturb an
            // already-usable VM. The atomic generation swap itself (verify -> install,
            // plus P2 live hot-reload) is a modules/task concern and is deferred there.
            SUBCASE("the same engine still runs after a compile failure")
            {
                auto engine = Engine::create();
                REQUIRE(engine.has_value());

                auto const bad = engine->runNumber("return 1 +", "veto5-bad");
                REQUIRE_FALSE(bad.has_value());
                CHECK(
                    automationErrorKind(bad.error())
                    == AutomationErrorKind::InvalidResource
                );

                // The failed load compiled to error bytecode and never ran, so the VM
                // generation is intact.
                auto const good = engine->runNumber("return 42", "veto5-good");
                REQUIRE(good.has_value());
                CHECK(*good == doctest::Approx(42.0));
            }

            SUBCASE("a sibling engine is unaffected by another's compile failure")
            {
                auto broken  = Engine::create();
                auto healthy = Engine::create();
                REQUIRE(broken.has_value());
                REQUIRE(healthy.has_value());

                auto const bad = broken->runNumber("this is not lua", "veto5-sibling-bad");
                REQUIRE_FALSE(bad.has_value());

                auto const good = healthy->runNumber("return 7 * 6", "veto5-sibling-good");
                REQUIRE(good.has_value());
                CHECK(*good == doctest::Approx(42.0));
            }
        }
    }
}
