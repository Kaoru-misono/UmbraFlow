#include <script/engine.hpp>
#include <script/testing/cancel-probe.hpp>

#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// The adversarial suite for the script substrate: the places §11 of
// docs/plans/2026-07-29-three-layer-task-system.md says guarantees historically
// break. test-veto-suite.cpp covers the two non-yieldable frames the P0 gate named
// (table.sort, string.gsub) against a naive runaway; this adds the rest of §11's
// matrix and the ceilings against a script that actively tries to outlive them.
namespace uf::script
{
    namespace
    {
        // One of §11's non-yieldable host C frames, with `body` executed inside it. A
        // lua_break landing in one cannot unwind cleanly and Luau raises an ordinary
        // CATCHABLE error instead (ldo.cpp lua_break, guarded on nCcalls > baseCcalls);
        // the run is still Cancelled because the interrupt sets InterruptState::broken
        // BEFORE calling lua_break.
        struct NonYieldableForm final
        {
            std::string_view name;
            std::string      source;
        };

        [[nodiscard]]
        auto nonYieldableForms(std::string_view body) -> std::vector<NonYieldableForm>
        {
            auto const inner = std::string{body};
            auto forms       = std::vector<NonYieldableForm>{};

            forms.emplace_back(
                NonYieldableForm{
                    .name   = "table.sort comparator",
                    .source = "table.sort({2, 1}, function(a, b)\n" + inner
                              + "    return a < b\n"
                                "end)\n",
                }
            );
            forms.emplace_back(
                NonYieldableForm{
                    .name   = "string.gsub callback",
                    .source = "string.gsub('a', 'a', function()\n" + inner
                              + "    return ''\n"
                                "end)\n",
                }
            );
            forms.emplace_back(
                NonYieldableForm{
                    .name   = "generic-for iterator",
                    .source = "local first = true\n"
                              "for _ in function()\n"
                              "    if not first then return nil end\n"
                              "    first = false\n" + inner
                              + "    return 1\n"
                                "end do end\n",
                }
            );
            forms.emplace_back(
                NonYieldableForm{
                    .name   = "__index metamethod",
                    .source = "local probe = setmetatable({}, {\n"
                              "    __index = function()\n" + inner
                              + "        return 1\n"
                                "    end,\n"
                                "})\n"
                                "local _ = probe.attacked\n",
                }
            );
            forms.emplace_back(
                NonYieldableForm{
                    .name   = "__newindex metamethod",
                    .source = "local probe = setmetatable({}, {\n"
                              "    __newindex = function()\n" + inner
                              + "    end,\n"
                                "})\n"
                                "probe.attacked = 1\n",
                }
            );
            forms.emplace_back(
                NonYieldableForm{
                    .name   = "__tostring metamethod",
                    .source = "local probe = setmetatable({}, {\n"
                              "    __tostring = function()\n" + inner
                              + "        return 'probe'\n"
                                "    end,\n"
                                "})\n"
                                "tostring(probe)\n",
                }
            );
            forms.emplace_back(
                NonYieldableForm{
                    .name   = "xpcall error handler",
                    .source = "xpcall(function() error('trigger') end, function()\n"
                              + inner
                              + "    return 0\n"
                                "end)\n",
                }
            );
            return forms;
        }

        // Large enough that every form reaches its C frame first: a budget that fired
        // during the prologue would cancel the run without ever entering the context
        // under test, and the control case below is what would catch that.
        constexpr auto k_runawayBudgetTicks = uint64{20'000};

        TEST_CASE("A cancellation landing in any non-yieldable context still stops the run")
        {
            // The runaway sits INSIDE the C frame, so the break degrades into a
            // catchable error; mark() after the construct makes "the script did not
            // continue" an observation, not an inference from the reported kind. The
            // xpcall form is the sharp one: Luau turns an error inside an error handler
            // into LUA_ERRERR and hands the caller a plain (false, "error in error
            // handling") -- §11's silent failure -- yet the run still ends Cancelled,
            // because the boundary consults InterruptState::broken, not resume status.
            for (auto const& form : nonYieldableForms("        while true do end\n"))
            {
                INFO("non-yieldable form: ", form.name);
                auto const probe = testing::probeCancellation(
                    form.source + "mark()\nreturn 1\n",
                    "adversarial-nonyieldable",
                    k_runawayBudgetTicks
                );
                CHECK(probe.cancelled);
                CHECK(probe.markCount == 0);
            }
        }

        TEST_CASE("Control: every non-yieldable form runs and reaches mark when nothing breaks")
        {
            // Without this the case above would pass on a substrate that cancelled
            // every run, or on forms whose bodies never executed. A zero budget
            // disables every break lever and the body is bounded.
            for (auto const& form : nonYieldableForms("        local n = 1 + 1\n"))
            {
                INFO("non-yieldable form: ", form.name);
                auto const probe = testing::probeCancellation(
                    form.source + "mark()\nreturn 1\n",
                    "adversarial-nonyieldable-control",
                    0
                );
                CHECK_FALSE(probe.cancelled);
                CHECK(probe.markCount == 1);
            }
        }

        TEST_CASE("A script that catches its own ceiling breaches in a loop is still stopped")
        {
            // The evasive runaway wraps its over-quota allocation in a pcall and
            // retries forever, so the memory ceiling alone -- an ORDINARY catchable
            // error by design -- never ends it. The instruction budget and the wall
            // clock do, and that division of labour is what is under test. The breach
            // grows a TABLE rather than a string on purpose: doubling a string reaches
            // the ceiling in a couple of dozen instructions but copies the whole buffer
            // each time, so the budget that must end it would arrive only after minutes
            // of memcpy.
            constexpr std::string_view evasive =
                "local rounds = 0\n"
                "while true do\n"
                "    rounds = rounds + 1\n"
                "    pcall(function()\n"
                "        local t = {}\n"
                "        local n = 0\n"
                "        while true do n = n + 1 t[n] = n end\n"
                "    end)\n"
                "end\n"
                "return rounds\n";

            // Two rounds of a 4 MiB table cost a few hundred thousand elements, so this
            // budget is spent well inside one ctest timeout.
            constexpr auto k_quotaBytes  = uint64{4} * 1024 * 1024;
            constexpr auto k_evasiveTicks = uint64{1'000'000};

            SUBCASE("the instruction budget ends it even though every breach is caught")
            {
                auto config                 = EngineConfig{};
                config.memoryQuotaBytes     = k_quotaBytes;
                config.interruptBudgetTicks = k_evasiveTicks;
                config.maxRuntime           = std::chrono::hours{1};

                auto engine = Engine::create(config);
                REQUIRE(engine.has_value());

                auto const result =
                    engine->runNumber(evasive, "adversarial-evasive-budget");
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::Cancelled
                );
            }

            SUBCASE("the wall-clock ceiling ends it with no instruction budget armed")
            {
                // Only the deadline can stop it. A ceiling that never fired would hang
                // rather than fail, which the ctest timeout turns into a failure anyway.
                auto config                 = EngineConfig{};
                config.memoryQuotaBytes     = k_quotaBytes;
                config.interruptBudgetTicks = 0;
                config.maxRuntime           = std::chrono::milliseconds{50};

                auto engine = Engine::create(config);
                REQUIRE(engine.has_value());

                auto const start   = std::chrono::steady_clock::now();
                auto const result  = engine->runNumber(evasive, "adversarial-evasive-time");
                auto const elapsed = std::chrono::steady_clock::now() - start;

                CHECK(elapsed < std::chrono::seconds{5});
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::Cancelled
                );
            }

            SUBCASE("control: the same shape with a bounded loop completes")
            {
                // Without this the two cases above would pass on an engine that
                // cancelled every run under a quota. It also proves the breach really
                // is caught: three rounds finish only if pcall swallowed three
                // out-of-memory errors. The budget is an order of magnitude above the
                // attack's, but still present, so a blanket "any budget cancels
                // everything" regression fails here rather than passing quietly.
                auto config                 = EngineConfig{};
                config.memoryQuotaBytes     = k_quotaBytes;
                config.interruptBudgetTicks = 10 * k_evasiveTicks;
                config.maxRuntime           = std::chrono::hours{1};

                auto engine = Engine::create(config);
                REQUIRE(engine.has_value());

                auto const result = engine->runNumber(
                    "local rounds = 0\n"
                    "for _ = 1, 3 do\n"
                    "    rounds = rounds + 1\n"
                    "    pcall(function()\n"
                    "        local t = {}\n"
                    "        local n = 0\n"
                    "        while true do n = n + 1 t[n] = n end\n"
                    "    end)\n"
                    "end\n"
                    "return rounds\n",
                    "adversarial-evasive-control"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(3.0));
            }
        }

        TEST_CASE("A run cannot plant anything for the next run through any shared route")
        {
            // Each run gets a fresh shallow copy of the frozen project-environment
            // prototype, so a global it writes dies with it. What the copy SHARES by
            // reference -- the library tables and the string metatable -- is the only
            // route left for planting something, so every one must refuse the write.
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            auto const planting = engine->runNumber(
                R"lua(
                    -- Control: a plain global IS writable inside the run, so the
                    -- absence asserted next run is about lifetime, not about a
                    -- write that never happened.
                    planted = 'yes'
                    if planted ~= 'yes' then return 0 end

                    -- Every shared route refuses.
                    if pcall(function() string.planted = 'yes' end) then return 0 end
                    if pcall(rawset, string, 'planted', 'yes') then return 0 end
                    if pcall(function() math.planted = 'yes' end) then return 0 end
                    if pcall(function() table.planted = 'yes' end) then return 0 end
                    if pcall(function() getmetatable('').planted = 'yes' end) then
                        return 0
                    end
                    if pcall(function() getmetatable('').__index.planted = 'yes' end) then
                        return 0
                    end
                    if pcall(rawset, getmetatable(''), '__index', {}) then return 0 end
                    -- Freezing its own environment's shared tables changes
                    -- nothing either: they are already frozen.
                    if not table.isfrozen(string) then return 0 end
                    return 1
                )lua",
                "adversarial-plant"
            );
            REQUIRE(planting.has_value());
            CHECK(*planting == doctest::Approx(1.0));

            auto const harvest = engine->runNumber(
                R"lua(
                    if planted ~= nil then return 0 end
                    if string.planted ~= nil then return 0 end
                    if math.planted ~= nil then return 0 end
                    if table.planted ~= nil then return 0 end
                    if getmetatable('').planted ~= nil then return 0 end
                    if ('').planted ~= nil then return 0 end
                    -- Control: the environment the second run got is a real one,
                    -- with the library it is supposed to have.
                    if string.format('%d', 7) ~= '7' then return 0 end
                    return 1
                )lua",
                "adversarial-harvest"
            );
            REQUIRE(harvest.has_value());
            CHECK(*harvest == doctest::Approx(1.0));
        }

        TEST_CASE("A wall-clock ceiling wider than the monotonic clock still runs the script")
        {
            // EngineConfig::maxRuntime is an unbounded host tunable, so the
            // deadline's `now() + ceiling` is a reachable overflow and not a
            // theoretical one. Wrapping puts the deadline BEHIND the run's first
            // safepoint, which inverts the request: the longest ceiling a host
            // can name becomes the only one that cancels immediately.
            constexpr std::string_view bounded =
                "local total = 0\n"
                "for i = 1, 100000 do total = total + i end\n"
                "return total\n";
            constexpr auto k_boundedSum = 5'000'050'000.0;

            SUBCASE("the widest ceiling saturates instead of wrapping into the past")
            {
                auto config                 = EngineConfig{};
                config.interruptBudgetTicks = 0;
                config.maxRuntime           = MonotonicInstant::Duration::max();

                auto engine = Engine::create(config);
                REQUIRE(engine.has_value());

                auto const result = engine->runNumber(bounded, "adversarial-widest-ceiling");
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(k_boundedSum));
                CHECK_FALSE(engine->generationSpent());
            }

            SUBCASE("control: the deadline is the only live lever in this shape")
            {
                // Without this the case above would pass on an engine whose
                // deadline never fires at all, which is the other way to make a
                // saturated ceiling look right. Same config but for the ceiling,
                // and no instruction budget, so nothing else can end the run.
                auto config                 = EngineConfig{};
                config.interruptBudgetTicks = 0;
                config.maxRuntime           = std::chrono::milliseconds{50};

                auto engine = Engine::create(config);
                REQUIRE(engine.has_value());

                auto const result =
                    engine->runNumber("while true do end\n", "adversarial-narrow-ceiling");
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::Cancelled
                );
            }
        }
    }
}
