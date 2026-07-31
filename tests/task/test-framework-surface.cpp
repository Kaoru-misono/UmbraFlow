#include "binding-fixture.hpp"

#include <task/capability-surface.hpp>
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
// framework through fake ENGINE ports. There every assertion travels through
// real recognition and the real cycle ledger, so a case can only say how many
// frames were served and whether a cycle stayed open; it cannot say that the
// wait re-observed before testing the target, that a retry paused between two
// attempts and not after the last, or that an interrupt handler was handed the
// cycle the loop had already opened. Here those are the assertion.
//
// Nothing below observes, recognizes or clicks anything: the session's frame
// source holds no frames at all, so any primitive that reached the real engine
// would fail CaptureUnavailable rather than quietly succeed. The one thing the
// fake does NOT fake is `raise` -- a Tier B carrier is host-minted userdata that
// no Luau chunk can forge, and it is what ctx:try and ctx:retry decide on -- so
// the fake forwards that one primitive to the real surface it wraps.
namespace uf::task
{
    namespace
    {
        // The scripted private capability surface (design section 5's twelve
        // primitives), handed the REAL surface as its chunk argument.
        //
        // Every primitive records what it was asked and answers from a plan the
        // project script installs, so "cycle_page resolves on the third call" or
        // "wait reports its deadline expired" is one line of test setup rather
        // than a sequence of fake frames chosen to make recognition say so.
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

            function fake.cycle_page(ticket)
                local scripted = answer("cycle_page")
                local where    = "cycle_page(" .. ticket.ordinal .. ")"
                if type(scripted) == "string" then
                    raiseScripted(scripted, where)
                end
                if scripted == nil then
                    record(where .. "->nil")
                    return nil
                end
                record(where .. "->page")

                -- The resolved page answers `is` by identity against the very
                -- uf.pages.<name> handle the plan named, so page selection stays
                -- the framework's decision and none of it is recognition.
                local page = {}
                function page:is(other)
                    return other == scripted
                end
                return page
            end

            function fake.cycle_find(ticket, element)
                local scripted = answer("cycle_find")
                local where    = "cycle_find(" .. ticket.ordinal .. ")"
                if type(scripted) == "string" then
                    raiseScripted(scripted, where)
                end
                if scripted ~= true then
                    record(where .. "->nil")
                    return nil
                end
                record(where .. "->hit")
                return { ordinal = ticket.ordinal }
            end

            -- Both ordinals are recorded, so a click is readable as "this hit
            -- came from this cycle" rather than only as "a click happened".
            function fake.cycle_click(ticket, hit)
                answer("cycle_click")
                record("cycle_click(" .. ticket.ordinal .. "," .. hit.ordinal .. ")")
            end

            function fake.deadline(milliseconds)
                answer("deadline")
                record("deadline(" .. milliseconds .. ")")
                return { milliseconds = milliseconds }
            end

            -- A plan entry of false is the budget running out. The deadline's
            -- own millisecond count is echoed back into the log, which is what
            -- makes the framework's default timeout observable at all.
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

        // A session over the two-page runtime whose frame source holds NO
        // frames. Nothing here should ever observe, and a capture that happened
        // anyway fails loudly instead of serving a frame a case might have been
        // passing on.
        [[nodiscard]]
        auto buildUnbound() -> Built
        {
            return buildBindingOver(
                interruptRuntime(),
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
            Built& built,
            std::string_view source
        ) -> double
        {
            auto config = taskVmConfig(built.surface, context);
            config.installPrivateCapabilities =
                script::testing::scriptedPrivateCapabilities(
                    CapabilitySurface::privateCapabilities(context),
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

        TEST_CASE("ctx:wait_for_page opens one cycle per poll and uses the one that resolved")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Three polls: the target resolves on the third, and only then does
            // the block run. The expected log is the whole claim -- one cycle per
            // turn, each closed before the wait, the deadline minted once and the
            // poll interval carried into every wait -- and none of it is visible
            // from outside the surface.
            constexpr std::string_view source = R"lua(
                probe.plan('cycle_page', { nil, nil, uf.pages.page_a })
                probe.plan('cycle_find', { true })

                local ran = 0
                task.define {
                    run = function(ctx)
                        ctx:wait_for_page(
                            uf.pages.page_a,
                            { timeout_ms = 60000, poll_ms = 250 },
                            function(cycle)
                                -- Asked twice on purpose: the resolution is
                                -- memoized per cycle, so one cycle_page must
                                -- still be all the log shows for this turn.
                                if cycle:page() == nil then return end
                                if cycle:page() == nil then return end
                                local hit = cycle:find(uf.elements.action_target)
                                if hit == nil then return end
                                cycle:click(hit)
                                ran = 1
                            end
                        )
                    end,
                }
                if ran ~= 1 then return 0 end

                -- No cycle_close(3): a delivered click CONSUMES the cycle, so
                -- the framework must not also close it.
                local expected = table.concat({
                    'deadline(60000)',
                    'cycle_open->1',
                    'cycle_page(1)->nil',
                    'cycle_close(1)',
                    'wait(60000,250)->true',
                    'cycle_open->2',
                    'cycle_page(2)->nil',
                    'cycle_close(2)',
                    'wait(60000,250)->true',
                    'cycle_open->3',
                    'cycle_page(3)->page',
                    'cycle_find(3)->hit',
                    'cycle_click(3,3)',
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

        TEST_CASE("ctx:wait_for_page closes the cycle it opened when the block raises")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Two raises out of the block, one of each shape a project can
            // produce: its own Luau error, and a Tier B failure from a primitive.
            // Both must leave a cycle_close behind, and the raise must survive
            // the close rather than being replaced by it.
            constexpr std::string_view source = R"lua(
                probe.plan('cycle_page', { uf.pages.page_a, uf.pages.page_a })
                probe.plan('cycle_find', { 'invalid_resource' })

                local ok, err = pcall(function()
                    ctx:wait_for_page(
                        uf.pages.page_a,
                        { timeout_ms = 1000, poll_ms = 10 },
                        function() error('boom', 0) end
                    )
                end)
                if ok or err ~= 'boom' then return 0 end

                local caught, tierB = ctx:try(function()
                    ctx:wait_for_page(
                        uf.pages.page_a,
                        { timeout_ms = 2000, poll_ms = 20 },
                        function(cycle)
                            cycle:find(uf.elements.action_target)
                        end
                    )
                end)
                if caught ~= false then return 0 end
                if tierB.kind ~= uf.errors.invalid_resource then return 0 end

                local expected = table.concat({
                    'deadline(1000)',
                    'cycle_open->1',
                    'cycle_page(1)->page',
                    'cycle_close(1)',
                    'deadline(2000)',
                    'cycle_open->2',
                    'cycle_page(2)->page',
                    'cycle_find(2)!invalid_resource',
                    'cycle_close(2)',
                }, '|')
                if probe.calls() ~= expected then return 0 end
                return 1
            )lua";

            CHECK(runOnFakeSurface(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("the wait ends on its own budget, and the framework owns the defaults")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // `wait` reporting false is the only thing that ends a wait, and the
            // timeout it turns into is minted by the host rather than raised as a
            // Luau string. The second half is what no other test can see: with no
            // options at all the deadline and the poll interval are the
            // framework's own constants, and they reach the surface as arguments.
            constexpr std::string_view source = R"lua(
                probe.plan('wait', { false, false })

                local ok, err = ctx:try(function()
                    ctx:wait_for_page(
                        uf.pages.page_a,
                        { timeout_ms = 4000, poll_ms = 700 },
                        function() end
                    )
                end)
                if ok ~= false then return 0 end
                if err.kind ~= uf.errors.timeout then return 0 end
                if err.retryable ~= false then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                local plain = ctx:try(function()
                    ctx:wait_for_page(uf.pages.page_a, nil, function() end)
                end)
                if plain ~= false then return 0 end

                local expected = table.concat({
                    'deadline(4000)',
                    'cycle_open->1',
                    'cycle_page(1)->nil',
                    'cycle_close(1)',
                    'wait(4000,700)->false',
                    'raise:timeout',
                    'deadline(600000)',
                    'cycle_open->2',
                    'cycle_page(2)->nil',
                    'cycle_close(2)',
                    'wait(600000,500)->false',
                    'raise:timeout',
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
                    'cycle_find',
                    { 'stale_observation', 'stale_observation', 'stale_observation' }
                )

                local tries = 0
                local ok, err = ctx:try(function()
                    ctx:retry({ attempts = 3, backoff_ms = 250 }, function()
                        tries += 1
                        ctx:cycle(function(cycle)
                            cycle:find(uf.elements.action_target)
                        end)
                    end)
                end)
                if ok ~= false then return 0 end
                if tries ~= 3 then return 0 end
                if err.kind ~= uf.errors.stale_observation then return 0 end

                local expected = table.concat({
                    'emit:retry_attempt:1:3',
                    'cycle_open->1',
                    'cycle_find(1)!stale_observation',
                    'cycle_close(1)',
                    'settle(250)',
                    'emit:retry_backoff:250',
                    'emit:retry_attempt:2:3',
                    'cycle_open->2',
                    'cycle_find(2)!stale_observation',
                    'cycle_close(2)',
                    'settle(250)',
                    'emit:retry_backoff:250',
                    'emit:retry_attempt:3:3',
                    'cycle_open->3',
                    'cycle_find(3)!stale_observation',
                    'cycle_close(3)',
                }, '|')
                if probe.calls() ~= expected then return 0 end
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
                    probe.plan('cycle_find', { kind, kind, kind, kind })

                    local tries = 0
                    local ok = ctx:try(function()
                        ctx:retry(policy, function()
                            tries += 1
                            ctx:cycle(function(cycle)
                                cycle:find(uf.elements.action_target)
                            end)
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
                    'cycle_find(1)!stale_observation',
                    'cycle_close(1)',
                }, '|')
                if probe.calls() ~= expected then return 0 end
                return 1
            )lua";

            CHECK(runOnFakeSurface(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("an interrupt handler is handed the cycle the wait already opened")
        {
            auto built = buildUnbound();
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The capability the three-layer design exists for, asserted as the
            // call sequence rather than as a frame count. Between the popup
            // resolving and the handler's find there is NO cycle_open, and the
            // ordinals say the handler acted on cycle 1 -- which is the whole of
            // "the handler is offered the current cycle". The handler's click
            // consumes that cycle, so no close follows it, and the wait then
            // polls once more and finds its target on cycle 2.
            constexpr std::string_view source = R"lua(
                probe.plan('cycle_page', { uf.pages.popup, uf.pages.page_a })
                probe.plan('cycle_find', { true, true })

                local handled = 0
                local clicked = 0

                local popup = task.interrupt {
                    id = 'popup',
                    when = uf.pages.popup,
                    max_hits = 3,
                    handle = function(ctx, cycle)
                        handled += 1
                        local close = cycle:find(uf.elements.close_dialog)
                        if close ~= nil then
                            cycle:click(close)
                        end
                    end,
                }

                task.define {
                    interrupts = { popup },
                    run = function(ctx)
                        ctx:wait_for_page(
                            uf.pages.page_a,
                            { timeout_ms = 30000, poll_ms = 100 },
                            function(cycle)
                                local hit = cycle:find(uf.elements.action_target)
                                if hit ~= nil then
                                    cycle:click(hit)
                                    clicked += 1
                                end
                            end
                        )
                    end,
                }

                if handled ~= 1 then return 0 end
                if clicked ~= 1 then return 0 end

                local expected = table.concat({
                    'deadline(30000)',
                    'cycle_open->1',
                    'cycle_page(1)->page',
                    'emit:interrupt_matched:popup',
                    'cycle_find(1)->hit',
                    'cycle_click(1,1)',
                    'emit:interrupt_handled:popup',
                    'wait(30000,100)->true',
                    'cycle_open->2',
                    'cycle_page(2)->page',
                    'cycle_find(2)->hit',
                    'cycle_click(2,2)',
                }, '|')
                if probe.calls() ~= expected then return 0 end

                -- The closing event was emitted only after the framework asked
                -- whether the generation was still live, which is what keeps a
                -- cancelled handler from raising over its own cause.
                if probe.terminal_calls() < 1 then return 0 end
                return 1
            )lua";

            CHECK(runOnFakeSurface(context, built, source) == doctest::Approx(1.0));
        }
    }
}
