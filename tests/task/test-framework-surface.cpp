#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <domain/frame.hpp>

#include <script/engine.hpp>
#include <script/testing/capability-probe.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// The trusted Luau framework as a UNIT: ctx runs against a scripted stand-in for
// the private capability surface, so what a case asserts is the sequence of
// primitive calls the framework produced and the arguments it produced them
// with.
//
// That is the difference from test-framework-context.cpp, which drives the same
// framework through fake ENGINE ports. There every assertion travels through the
// real cycle ledger, so a case can only say how many frames were served and
// whether a cycle stayed open; it cannot say that a retry paused between two
// attempts and not after the last, or that the block's own failure outranks the
// close that follows it. Here those are the assertion.
//
// WHAT LEFT THIS FILE WITH THE PAGE MODEL. `ctx:wait_for_page` and the interrupt
// registry were most of what it proved, and both retired with the C++ page model
// (docs/plans/2026-07-31-script-owned-page-model.md 9): the wait is
// `observe.wait_until` now and an interrupt page is a flag the walk consults, and
// both are exercised against a real session in test-script-owned-model.cpp. What
// stayed is the policy that never mentioned a page -- the scoped cycle and the
// retry loop -- and it is asserted here exactly as it was.
//
// Nothing below observes, matches or clicks anything: the session's frame source
// holds no frames at all, so any primitive that reached the real engine would
// fail CaptureUnavailable rather than quietly succeed. The one thing the fake
// does NOT fake is `raise` -- a Tier B carrier is host-minted userdata that no
// Luau chunk can forge, and it is what ctx:try and ctx:retry decide on -- so the
// fake forwards that one primitive to the real surface it wraps.
namespace uf::task
{
    namespace
    {
        // The scripted private capability surface, handed the REAL surface as its
        // chunk argument.
        //
        // Every primitive records what it was asked and answers from a plan the
        // project script installs, so "the third match hits" or "wait reports its
        // deadline expired" is one line of test setup rather than a sequence of
        // fake frames chosen to make a search say so.
        constexpr auto k_fakeSurfaceSource = std::string_view{R"lua(
            local real = ...

            local log           = {}
            local plan          = {}
            local counts        = {}
            local tickets       = 0
            local terminalCalls = 0

            local function record(entry)
                log[#log + 1] = entry
            end

            -- The scripted answer for this call of `verb`, or nil when the plan
            -- says nothing about it. The counter advances whether or not a plan
            -- exists, so the nth call of a verb is the nth entry either way.
            local function answer(verb)
                local index  = (counts[verb] or 0) + 1
                counts[verb] = index
                local list   = plan[verb]
                if list == nil then
                    return nil
                end
                return list[index]
            end

            -- A plan entry that is a STRING names an error kind: the primitive
            -- fails the way a real one does, through the host's own mint. Only
            -- the host can produce a Tier B carrier, which is why `raise` below
            -- forwards rather than fakes.
            local function raiseScripted(kind, where)
                record(where .. "!" .. kind)
                real.raise(kind, "scripted " .. kind)
            end

            local fake = {}

            function fake.cycle_open()
                answer("cycle_open")
                tickets += 1
                record("cycle_open->" .. tickets)
                return { ordinal = tickets }
            end

            function fake.cycle_close(ticket)
                answer("cycle_close")
                record("cycle_close(" .. ticket.ordinal .. ")")
            end

            function fake.template_load(blob)
                answer("template_load")
                record("template_load(" .. #blob .. ")")
                return { bytes = #blob }
            end

            function fake.cycle_match(ticket, template, x, y, width, height)
                local scripted = answer("cycle_match")
                local where    = "cycle_match(" .. ticket.ordinal .. ")"
                if type(scripted) == "string" then
                    raiseScripted(scripted, where)
                end
                if scripted ~= true then
                    record(where .. "->nil")
                    return nil
                end
                record(where .. "->match")
                return { ordinal = ticket.ordinal }
            end

            -- Both ordinals are recorded, so a click is readable as "this match
            -- came from this cycle" rather than only as "a click happened".
            function fake.cycle_click(ticket, match)
                answer("cycle_click")
                record(
                    "cycle_click(" .. ticket.ordinal .. "," .. match.ordinal .. ")"
                )
            end

            function fake.key(ticket, name)
                local scripted = answer("key")
                local where    = "key(" .. ticket.ordinal .. "," .. name .. ")"
                if type(scripted) == "string" then
                    raiseScripted(scripted, where)
                end
                record(where)
            end

            function fake.deadline(milliseconds)
                answer("deadline")
                record("deadline(" .. milliseconds .. ")")
                return { milliseconds = milliseconds }
            end

            -- A plan entry of false is the budget running out. The deadline's
            -- own millisecond count is echoed back into the log, which is what
            -- makes a caller's timeout observable at all.
            function fake.wait(deadline, interval)
                local remains = answer("wait") ~= false
                record(
                    "wait(" .. deadline.milliseconds .. "," .. interval .. ")->"
                    .. tostring(remains)
                )
                return remains
            end

            function fake.settle(milliseconds)
                answer("settle")
                record("settle(" .. milliseconds .. ")")
            end

            function fake.emit(name, first, second)
                answer("emit")
                local entry = "emit:" .. name
                if first ~= nil then
                    entry = entry .. ":" .. tostring(first)
                end
                if second ~= nil then
                    entry = entry .. ":" .. tostring(second)
                end
                record(entry)
            end

            -- Deliberately unrecorded: the framework asks it on every cleanup
            -- path, so logging it would bury the effect sequence the log exists
            -- for. The count is exposed instead, so "the framework asked before
            -- it emitted a closing event" stays observable.
            function fake.terminal()
                terminalCalls += 1
                return false
            end

            function fake.raise(kind, message)
                record("raise:" .. kind)
                return real.raise(kind, message)
            end

            function fake.random(low, high)
                return real.random(low, high)
            end

            fake.error_tag = real.error_tag

            -- The control channel the `probe` framework module republishes into
            -- the project environment. It lives on the surface because the
            -- surface is the one table both that module and ctx are handed.
            fake.__probe = {
                calls = function()
                    return table.concat(log, "|")
                end,
                plan = function(verb, answers)
                    plan[verb] = answers
                end,
                terminal_calls = function()
                    return terminalCalls
                end,
                reset = function()
                    log           = {}
                    plan          = {}
                    counts        = {}
                    tickets       = 0
                    terminalCalls = 0
                end,
            }

            return fake
        )lua"};

        // A framework module that exists only to carry the fake's control
        // channel across into the project environment. It receives the same one
        // table ctx does, which is the whole reason it can: a private capability
        // surface is reachable from nowhere else.
        constexpr auto k_probeSource = std::string_view{R"lua(
            local native = ...
            return native.__probe
        )lua"};

        // A session whose frame source holds NO frames. Nothing here should ever
        // observe, and a capture that happened anyway fails loudly instead of
        // serving a frame a case might have been passing on.
        [[nodiscard]]
        auto buildUnbound() -> Built
        {
            return buildBindingWith(
                std::make_unique<FakeFrameSource>(std::vector<Frame>{}),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
        }

        // Runs `source` on a task VM whose framework bundle is the real one and
        // whose private capability surface is the fake above.
        [[nodiscard]]
        auto runOnFakeSurface(
            TaskContext& context,
            Built& /*built*/,
            std::string_view source
        ) -> double
        {
            auto config = taskVmConfig(context);
            config.installPrivateCapabilities =
                script::testing::scriptedPrivateCapabilities(
                    scriptPrivateCapabilities(context, ScriptTrustMode::Run),
                    std::string{k_fakeSurfaceSource},
                    "fake-capability-surface"
                );
            config.frameworkModules.emplace_back(
                script::FrameworkModule{.name = "probe", .source = k_probeSource}
            );
            config.frameworkProjectGlobals.emplace_back("probe");

            auto engine = script::Engine::create(config);
            REQUIRE(engine.has_value());
            auto const result = engine->runNumber(source, "framework-surface");
            REQUIRE(result.has_value());
            return *result;
        }

        TEST_CASE("ctx:cycle closes the cycle it opened when the block raises")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Two raises out of the block, one of each shape a project can
            // produce: its own Luau error, and a Tier B failure from a primitive.
            // Both must leave a cycle_close behind, and the raise must survive
            // the close rather than being replaced by it.
            constexpr std::string_view source = R"lua(
                probe.plan('key', { 'invalid_resource' })

                local ok, err = pcall(function()
                    ctx:cycle(function() error('boom', 0) end)
                end)
                if ok or err ~= 'boom' then return 0 end

                local caught, tierB = ctx:try(function()
                    ctx:cycle(function(cycle) cycle:key('E') end)
                end)
                if caught ~= false then return 0 end
                if tierB.kind ~= uf.errors.invalid_resource then return 0 end

                local expected = table.concat({
                    'cycle_open->1',
                    'cycle_close(1)',
                    'cycle_open->2',
                    'key(2,E)!invalid_resource',
                    'cycle_close(2)',
                }, '|')
                if probe.calls() ~= expected then return 0 end
                return 1
            )lua";

            CHECK(runOnFakeSurface(context, built, source) == doctest::Approx(1.0));

            // The control on the whole file's premise: the framework's policy ran
            // to completion without the host holding anything, so nothing below
            // the surface was involved.
            CHECK_FALSE(context.hasOpenCycle());
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("a delivered input consumes the cycle and the framework does not close it")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // No cycle_close after either delivery. The ledger spends the cycle
            // on a click and on a keystroke alike, so a framework that also
            // closed would be closing a cycle that no longer exists -- and the
            // surface is the only place both halves of that are visible at once.
            constexpr std::string_view source = R"lua(
                ctx:cycle(function(cycle)
                    cycle:click({ ordinal = 1 })
                end)

                ctx:cycle(function(cycle)
                    cycle:key('E')
                end)

                local expected = table.concat({
                    'cycle_open->1',
                    'cycle_click(1,1)',
                    'cycle_open->2',
                    'key(2,E)',
                }, '|')
                if probe.calls() ~= expected then return 0 end
                return 1
            )lua";

            CHECK(runOnFakeSurface(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("ctx:retry pauses between attempts and not after the last one")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Three attempts, two pauses. The placement is the point: a backoff
            // after the final attempt would be a pause nobody waits through, and
            // the surface is the only place the pause and the attempt that
            // follows it can be seen in one order.
            constexpr std::string_view source = R"lua(
                probe.plan(
                    'key',
                    { 'stale_observation', 'stale_observation', 'stale_observation' }
                )

                local tries = 0
                local ok, err = ctx:try(function()
                    ctx:retry({ attempts = 3, backoff_ms = 250 }, function()
                        tries += 1
                        ctx:cycle(function(cycle) cycle:key('E') end)
                    end)
                end)
                if ok ~= false then return 0 end
                if tries ~= 3 then return 0 end
                if err.kind ~= uf.errors.stale_observation then return 0 end

                local expected = table.concat({
                    'emit:retry_attempt:1:3',
                    'cycle_open->1',
                    'key(1,E)!stale_observation',
                    'cycle_close(1)',
                    'settle(250)',
                    'emit:retry_backoff:250',
                    'emit:retry_attempt:2:3',
                    'cycle_open->2',
                    'key(2,E)!stale_observation',
                    'cycle_close(2)',
                    'settle(250)',
                    'emit:retry_backoff:250',
                    'emit:retry_attempt:3:3',
                    'cycle_open->3',
                    'key(3,E)!stale_observation',
                    'cycle_close(3)',
                }, '|')
                if probe.calls() ~= expected then return 0 end
                return 1
            )lua";

            CHECK(runOnFakeSurface(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("ctx:retry takes its default attempt count from the framework")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The only remaining framework-owned default, and the surface is the
            // one place it is observable: a policy naming no total still announces
            // "attempt N of 3" on the wire, so the number is the framework's and
            // not the host's.
            constexpr std::string_view source = R"lua(
                probe.plan(
                    'key',
                    { 'stale_observation', 'stale_observation', 'stale_observation' }
                )

                local ok = ctx:try(function()
                    ctx:retry({}, function()
                        ctx:cycle(function(cycle) cycle:key('E') end)
                    end)
                end)
                if ok ~= false then return 0 end

                local calls = probe.calls()
                if string.find(calls, 'emit:retry_attempt:1:3', 1, true) == nil then
                    return 0
                end
                if string.find(calls, 'emit:retry_attempt:3:3', 1, true) == nil then
                    return 0
                end
                if string.find(calls, 'emit:retry_attempt:4:', 1, true) ~= nil then
                    return 0
                end
                return 1
            )lua";

            CHECK(runOnFakeSurface(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("ctx:retry follows the on list over retryable, and retryable without one")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Four directions over two kinds whose retryable flags differ:
            // `timeout` is Abort and therefore retryable = false, while
            // `stale_observation` is Retry and therefore true.
            //
            //   1. `on` names timeout            -> retried, though not retryable
            //   2. no `on`, timeout              -> not retried, retryable rules
            //   3. no `on`, stale_observation    -> retried, retryable rules
            //   4. `on` names timeout only, and
            //      the failure is retryable      -> NOT retried
            //
            // The fourth is what pins the semantics: with `on` present,
            // `retryable` is not consulted at all.
            constexpr std::string_view source = R"lua(
                local function count(policy, kind)
                    probe.reset()
                    probe.plan('key', { kind, kind, kind, kind })

                    local tries = 0
                    local ok = ctx:try(function()
                        ctx:retry(policy, function()
                            tries += 1
                            ctx:cycle(function(cycle) cycle:key('E') end)
                        end)
                    end)
                    if ok ~= false then return -1 end
                    return tries
                end

                local overridden = count(
                    { attempts = 3, on = { uf.errors.timeout } },
                    'timeout'
                )
                if overridden ~= 3 then return 0 end

                local defaulted = count({ attempts = 3 }, 'timeout')
                if defaulted ~= 1 then return 0 end

                local retryable = count({ attempts = 2 }, 'stale_observation')
                if retryable ~= 2 then return 0 end

                local excluded = count(
                    { attempts = 3, on = { uf.errors.timeout } },
                    'stale_observation'
                )
                if excluded ~= 1 then return 0 end

                -- The control against a vacuous run: the last direction really
                -- did open and close exactly one cycle, so "one attempt" is one
                -- attempt that happened rather than none at all.
                local expected = table.concat({
                    'emit:retry_attempt:1:3',
                    'cycle_open->1',
                    'key(1,E)!stale_observation',
                    'cycle_close(1)',
                }, '|')
                if probe.calls() ~= expected then return 0 end
                return 1
            )lua";

            CHECK(runOnFakeSurface(context, built, source) == doctest::Approx(1.0));
        }
    }
}
