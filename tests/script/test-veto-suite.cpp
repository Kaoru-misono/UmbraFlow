#include <script/engine.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <bit>
#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// The P0 "6 一票否决" gate made executable and regression-guarded on every Luau
// bump. The six vetoes are the roadmap's §五 acceptance conditions
// (docs/plans/2026-07-21-product-form-and-roadmap.md). test-script.cpp already
// exercises the bulk of #1/#2/#3; this file completes the gate with the
// remaining explicitly named cases and the documented non-yieldable-C-boundary
// exception from the hardening ledger and integration-plan §5.
//
// Coverage that structurally belongs to later phases is called out per case and
// is deliberately NOT faked here:
//   - veto #4 full observation/action trace + seed 1000x -> B2 harness (phase 3)
//   - veto #5 atomic generation SWAP / P2 live hot-reload -> modules/task (phase 2)
//   - veto #6 real host-C-binding hang                    -> modules/task (phase 2)
// The script substrate covers the parts it can prove today: run-to-run bit
// determinism (#4), compile-failure isolation at the load boundary (#5), and the
// built-in C-boundary cancellation exception that stands in for #6 until the
// first uf.* binding lands.

namespace uf::script
{
    namespace
    {
        // Create an Engine whose only break lever is an external stop token (the
        // instruction and time budgets are disabled), have a watchdog request the
        // stop shortly after the run starts, and assert the run is hard-cancelled
        // well within the 500ms exit budget. Mirrors the helper in
        // test-script.cpp; kept file-local so this veto suite is self-contained.
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

        // Reinterpret a double as its raw bit pattern so determinism can be
        // asserted as exact bit-for-bit identity without a float-equality
        // comparison.
        [[nodiscard]]
        auto bitsOf(double value) -> uint64
        {
            return std::bit_cast<uint64>(value);
        }

        // --- veto #6 (script-layer slice) + hardening-ledger C-boundary note ---

        TEST_CASE(
            "Veto #6/C-boundary: a runaway loop inside a non-yieldable C boundary "
            "is still hard-cancelled"
        )
        {
            // Documented Luau limitation (hardening-ledger "取消验证套件加'不可
            // yield 上下文'用例"; integration-plan §5, Risk #1): when the interrupt
            // fires while the script is inside a non-yieldable host C function --
            // here the built-in table.sort / string.gsub, which call back into a
            // Lua callback -- lua_break cannot unwind across the C frame and
            // instead raises a *catchable* "attempt to break across C-call
            // boundary" error rather than a clean LUA_BREAK. This is KNOWN,
            // correct behavior: do NOT "fix" it into a clean break. The engine
            // stays safe regardless because onInterrupt sets InterruptState::broken
            // BEFORE calling lua_break, so Engine::runNumber reports Cancelled
            // whether the break surfaced cleanly or as a trapped C-boundary error.
            // These cases lock that in. The veto #6 proper -- a registered host C
            // binding that hangs -- lands with the first uf.* binding in
            // modules/task (phase 2).
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
                // The C-boundary error is individually pcall-catchable, yet the
                // cancel still wins at the Engine boundary: onInterrupt set
                // InterruptState::broken before the break, so runNumber reports
                // Cancelled whether the sticky cancel re-broke before `return 1` or
                // the script momentarily ran on. This case therefore verifies
                // engine-boundary Cancelled reporting across a C frame, NOT that the
                // script's post-cancel continuation was prevented -- the pure-Lua
                // form of that stronger property is proven by the host-visible
                // mark-counter discriminator in test-script.cpp ("A cancelled script
                // never executes past the break"), and its host-C-binding form lands
                // with the first uf.* binding in phase 2.
                expectExternalStopCancels(
                    "pcall(function()\n"
                    "  table.sort({5, 4, 3, 2, 1}, function(a, b) while true do end end)\n"
                    "end)\n"
                    "return 1",
                    "veto6-sort-pcall"
                );
            }
        }

        // --- veto #2 completion: heavy string operations ---

        TEST_CASE(
            "Veto #2: an unbounded heavy string operation is a catchable per-task "
            "error, not a host crash"
        )
        {
            // Veto #2's three prongs are infinite allocation, deep recursion, and
            // HEAVY STRING OPERATIONS. test-script.cpp covers the first two (table
            // growth + non-tail recursion); this completes the third. Per the
            // hardening-ledger wording fix, the stopping mechanism is the
            // accounting allocator's hard quota, and the resulting out-of-memory is
            // an ORDINARY catchable error (never the uncatchable Cancelled of a
            // hard break); it only terminates this task and never drags down the
            // host.
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
                // allocator refuses the growth and Luau raises a catchable
                // out-of-memory error, surfacing as InvalidResource, NOT Cancelled.
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
                // Were the breach an uncatchable break, the outer script would
                // fail Cancelled. Instead pcall catches it, the script completes,
                // and ok is false -> 0.
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

        // --- veto #3 completion: filesystem / process / env / dynamic loading ---

        TEST_CASE(
            "Veto #3: filesystem, process, environment, and dynamic-loading access "
            "is denied"
        )
        {
            // test-script.cpp asserts the loaders and residual clocks are gone;
            // this makes the rest of veto #3 explicit. Luau's luaL_openlibs never
            // opens io/package, its os library never registers the process and
            // environment entries, and installSandbox removes the manual GC
            // hook. Luau's base library exposes no networking at all, so there
            // is no network vector to deny.
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            constexpr std::string_view denied[] = {
                "io == nil",             // filesystem + stdio
                "require == nil",        // dynamic module loading
                "package == nil",        // module system table
                "os.execute == nil",     // spawn a process
                "os.getenv == nil",      // read environment variables
                "os.exit == nil",        // terminate the host process
                "os.remove == nil",      // delete a file
                "os.rename == nil",      // rename a file
                "os.tmpname == nil",     // reserve a temp file name
            };
            for (std::string_view const expr : denied)
            {
                INFO("expression: ", expr);
                auto const source = "return (" + std::string{expr} + ") and 1 or 0";
                auto const result = engine->runNumber(source, "veto3-denied");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        // --- veto #4 (script-layer determinism slice) ---

        TEST_CASE("Veto #4: repeated runs of one script are bit-for-bit identical")
        {
            // Full veto #4 (same observation trace + seed -> 1000x identical action
            // trace and state hash) needs the observe/act layer and lands in the B2
            // determinism harness (phase 3). What the pure script substrate proves
            // now is its necessary foundation: the same source under the same
            // deterministic environment yields the exact same numeric result every
            // run -- no per-run thread setup, GC timing, or address noise perturbs
            // it. Kept same-machine only, never a cross-platform bit-level claim
            // (hardening-ledger determinism note: transcendental / libc-format
            // paths are not cross-CPU identical). The script uses only
            // integer-valued float arithmetic, so no transcendental path is hit.
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

        // --- veto #5 (load-boundary slice) ---

        TEST_CASE("Veto #5: a compile failure leaves the engine and its siblings usable")
        {
            // The P0 acceptance scope for veto #5 is the LOAD BOUNDARY: a new
            // generation is installed only after compile + self-check succeeds, so
            // a failed compile must not disturb any already-usable VM (roadmap §五
            // #5; script-layer plan §一.5). Each generation is a brand-new VM, so
            // mixed old/new closures are structurally impossible -- the
            // cross-engine isolation cases in test-script.cpp show that half. This
            // adds the "a failure does not poison" half. The atomic generation SWAP
            // itself (verify -> install, plus P2 live hot-reload) is a modules/task
            // concern and is deferred there.
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

                // The failed load compiled to error bytecode and never ran, so the
                // VM generation is intact and the next good source runs normally.
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
