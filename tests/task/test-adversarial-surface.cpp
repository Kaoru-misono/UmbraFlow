#include "binding-fixture.hpp"

#include <task/capability-surface.hpp>
#include <task/task-context.hpp>

#include <script/engine.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>

#include <engine/session.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The adversarial suite for stage 2's four guarantees, written from the
// attacker's side: environment isolation, the private capability surface, the
// terminal cancellation latch, and the unforgeable Tier B carrier.
//
// It is deliberately separate from test-task-binding.cpp, which asserts what the
// binding DOES. Everything here asserts what a hostile project script cannot
// make it do, and every absence is paired with a control that would fail if the
// probe never ran -- an attack that fails for the wrong reason proves nothing.
namespace uf::task
{
    namespace
    {
        // The framework-only value ctx.luau assigns as a framework GLOBAL rather
        // than exporting. Reaching it from a project script would mean the
        // project environment found a route into the framework environment, so
        // it is the target of every reachability scan below. Spelled here exactly
        // as modules/task/runtime/ctx.luau spells it; the control cases prove the
        // scanner can find the string when it is genuinely present, so a typo
        // surfaces as a failing control rather than as a silent pass.
        constexpr auto k_frameworkSentinel =
            std::string_view{"uf-framework-sentinel-6b21f0"};

        // Requests the stop as it fails a capture with a kind that is NOT
        // Cancelled, so the primitive mints a Tier B carrier while a hard cancel
        // is already pending. That is the one arrangement in which a script could
        // hope to trade the host's terminal control for a recoverable failure it
        // is allowed to catch and continue past.
        class StallAndStopFrameSource final : public engine::IFrameSource
        {
            std::stop_source m_stop;

        public:
            explicit StallAndStopFrameSource(std::stop_source stop) noexcept
                : m_stop{std::move(stop)}
            {
            }

            [[nodiscard]]
            auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
            {
                m_stop.request_stop();
                return fail(
                    AutomationErrorKind::CaptureStalled,
                    "capture stalled while a stop was requested"
                );
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }
        };

        // Serves a good frame every time and requests the stop on the Nth
        // capture, so a script can mint a Tier B carrier first and only then
        // arrange for the hard cancel -- which is the only way to hold a
        // catchable automation error and a pending cancellation at the same time.
        //
        // `stopAt == 0` never requests the stop, which is how the control run
        // shares one source with the attack instead of needing a second class.
        class StopOnNthCaptureFrameSource final : public engine::IFrameSource
        {
            Frame            m_frame;
            std::stop_source m_stop;
            std::size_t      m_stopAt;
            std::size_t      m_captureCount{0};

        public:
            StopOnNthCaptureFrameSource(
                Frame frame,
                std::stop_source stop,
                std::size_t stopAt
            ) noexcept
                : m_frame{std::move(frame)}
                , m_stop{std::move(stop)}
                , m_stopAt{stopAt}
            {
            }

            [[nodiscard]]
            auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
            {
                ++m_captureCount;
                if (m_stopAt != 0 && m_captureCount >= m_stopAt)
                {
                    m_stop.request_stop();
                }
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }
        };

        // Fails every capture with a Tier B kind and never requests a stop. It is
        // the control for the source above: the same recoverable failure, with
        // nothing cancelled, so a run that reports Cancelled there and
        // CaptureStalled here is discriminating on the cancel rather than on the
        // failure.
        class StallOnlyFrameSource final : public engine::IFrameSource
        {
        public:
            [[nodiscard]]
            auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
            {
                return fail(
                    AutomationErrorKind::CaptureStalled,
                    "capture stalled with nothing cancelled"
                );
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }
        };

        // One attack: a label naming it and a Luau expression that must be truthy
        // on a real task VM. Each probe runs on its own VM, so a probe that
        // corrupts its environment cannot make the next one pass or fail; the
        // label is what a failure reports, which is why the attacks are written
        // one per expression rather than folded into one long script.
        struct Attack final
        {
            std::string_view label;
            std::string_view expression;
        };

        // Runs every attack against `context`'s bound session and requires each
        // to hold. Every expression must leave the cycle ledger as it found it,
        // because one context serves them all.
        auto expectEveryAttackHolds(
            TaskContext& context,
            Built& built,
            std::span<Attack const> attacks
        ) -> void
        {
            for (auto const& attack : attacks)
            {
                INFO("attack: ", attack.label);
                auto const source =
                    "return (" + std::string{attack.expression} + ") and 1 or 0";
                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            }
        }

        // One of §11's non-yieldable host C frames, with `body` executed exactly
        // once inside it.
        //
        // These are where guarantees historically break: a primitive called from
        // one of them cannot yield, and a lua_break landing in one degrades from
        // a clean LUA_BREAK into an ordinary catchable error. The design answers
        // both by keeping every primitive a direct, non-yielding C call and by
        // latching terminal state in C++ rather than relying on the break, so the
        // matrix below has to hold for every frame in the list.
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
                    .source = "local done = false\n"
                              "table.sort({2, 1}, function(a, b)\n"
                              "    if not done then done = true\n" + inner
                              + "    end\n"
                                "    return a < b\n"
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

        TEST_CASE("ctx is the whole published framework surface and yields nothing more")
        {
            auto built = buildBinding(resolvingFrames(FrameId{200}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // ctx is the one framework object a project script can name, so it is
            // the natural place to start an escalation. Each attack below is a
            // route from ctx back into the framework: replace what a method does,
            // read a method's captured surface, or get a second, writable ctx
            // whose methods still answer.
            constexpr Attack attacks[] = {
                // Controls first: an attack list against an object that answered
                // nothing would pass every refusal below.
                {"control: ctx is a table", "type(ctx) == 'table'"},
                {"control: a method is callable",
                 "(function() local c = ctx:cycle_open() ctx:cycle_close(c)"
                 " return true end)()"},
                {"ctx is frozen", "table.isfrozen(ctx)"},
                {"a method cannot be replaced",
                 "not pcall(function() ctx.cycle_open = print end)"},
                {"a method cannot be replaced through rawset",
                 "not pcall(rawset, ctx, 'cycle_open', print)"},
                {"a new method cannot be added",
                 "not pcall(function() ctx.escalate = print end)"},
                {"ctx wears no metatable to subvert", "getmetatable(ctx) == nil"},
                {"a metatable cannot be attached to ctx",
                 "not pcall(setmetatable, ctx, { __index = print })"},
                // A clone of ctx IS obtainable -- ctx has no protected metatable,
                // so table.clone copies it. That is not an escalation and the
                // next two attacks say why: the copy holds the same closures,
                // which reach the same guarded primitives, and holds no name for
                // anything else.
                {"control: ctx can be cloned", "type(table.clone(ctx)) == 'table'"},
                {"a clone confers no new name",
                 "(function()\n"
                 "    local copy = table.clone(ctx)\n"
                 "    for key, value in pairs(copy) do\n"
                 "        if rawget(ctx, key) ~= value then return false end\n"
                 "    end\n"
                 "    for key in pairs(ctx) do\n"
                 "        if rawget(copy, key) == nil then return false end\n"
                 "    end\n"
                 "    return true\n"
                 "end)()"},
                {"a clone's method is still the guarded primitive",
                 "(function()\n"
                 "    local copy = table.clone(ctx)\n"
                 "    local c = copy.cycle_open(copy)\n"
                 "    copy.cycle_close(copy, c)\n"
                 "    return not pcall(function() return copy.cycle_close(copy, {}) end)\n"
                 "end)()"},
                // Calling a method with a foreign self is allowed -- the thin ctx
                // methods ignore self entirely -- and buys nothing: the primitive
                // behind it still validates its handle arguments.
                {"a foreign self cannot smuggle a handle",
                 "not pcall(function() return ctx.cycle_close({}, {ordinal = 1}) end)"},
                {"the framework's own globals are not exported on ctx",
                 "rawget(ctx, 'frameworkSentinel') == nil"
                 " and rawget(ctx, 'native') == nil"
                 " and rawget(ctx, 'error_tag') == nil"},
            };

            expectEveryAttackHolds(context, built, attacks);
        }

        // A bounded, cycle-safe search for `target` that follows every route the
        // brief names out of a value: table values, table KEYS, metatables, a
        // userdata's printed form and its readable fields, and strings by
        // SUBSTRING rather than by equality -- error() prefixes a source position
        // onto a raised string, so an exact-match scan would miss a leak that
        // travelled through a message.
        //
        // It is one definition shared by the control and the attack below, which
        // is what makes the pair a discriminator: the same scanner finds a
        // planted value and fails to find the framework's.
        constexpr auto k_reachabilityScan = std::string_view{R"lua(
            local function scan(value, depth, seen)
                if depth > 6 then return false end
                local kind = type(value)
                if kind == 'string' then
                    return string.find(value, target, 1, true) ~= nil
                end
                if kind == 'userdata' then
                    if scan(tostring(value), depth + 1, seen) then return true end
                    for _, field in ipairs({'kind', 'message', 'retryable', 'is'}) do
                        local read, held = pcall(function() return value[field] end)
                        if read and scan(held, depth + 1, seen) then return true end
                    end
                    return scan(getmetatable(value), depth + 1, seen)
                end
                if kind ~= 'table' or seen[value] then return false end
                seen[value] = true
                for key, entry in pairs(value) do
                    if scan(key, depth + 1, seen) then return true end
                    if scan(entry, depth + 1, seen) then return true end
                end
                local first = next(value)
                if first ~= nil and scan(rawget(value, first), depth + 1, seen) then
                    return true
                end
                return scan(getmetatable(value), depth + 1, seen)
            end
        )lua"};

        TEST_CASE("No route from a project script reaches a framework-only value")
        {
            auto built = buildBinding(resolvingFrames(FrameId{201}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const prelude = "local target = '" + std::string{k_frameworkSentinel}
                                 + "'\n" + std::string{k_reachabilityScan};

            SUBCASE("control: the scanner finds the value through each of those routes")
            {
                // Without this the case below proves nothing: a scanner that
                // never looked would report the same absence.
                auto const source = prelude + R"lua(
                    if not scan({ nest = { { target } } }, 0, {}) then return 0 end
                    if not scan({ [target] = true }, 0, {}) then return 0 end
                    if not scan(setmetatable({}, { __metatable = target }), 0, {}) then
                        return 0
                    end
                    local _, planted = pcall(function() error(target) end)
                    if not scan({ planted }, 0, {}) then return 0 end
                    if not scan({ tostring(target) }, 0, {}) then return 0 end
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            }

            SUBCASE("no root a project can name carries it, and no error path leaks it")
            {
                // The roots are everything a project script has: ctx, the uf
                // tables, live handles, the whitelisted libraries and every base
                // function that takes or returns a function or a table. The
                // second half adds the error paths, because a message, a
                // traceback or a __tostring is a route out of the framework just
                // as much as a field is.
                auto const source = prelude + R"lua(
                    local cycle = ctx:cycle_open()
                    local page = ctx:cycle_page(cycle)
                    local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)

                    local roots = {
                        -- The framework global BY NAME, first. A project
                        -- environment that grew an __index chain would resolve
                        -- it here, and no walk from any value would: the chain
                        -- hangs off the environment table itself, which a
                        -- project script has no name for.
                        frameworkSentinel, frameworkTaskRegistry,
                        ctx, task, uf, uf.pages, uf.recognizers, uf.errors,
                        table.clone(ctx), table.clone(task), table.clone(uf),
                        cycle, page, hit,
                        uf.pages.page_a, uf.recognizers.action_target,
                        -- library indirection, including the string metatable a
                        -- sandboxed VM leaves readable
                        ('').format, getmetatable(''), getmetatable('').__index,
                        string, table, math, os, bit32, buffer, utf8, vector,
                        -- base functions that take or return a function or table
                        getmetatable, setmetatable, rawget, rawset, rawequal,
                        rawlen, select, unpack, next, pairs, ipairs, tostring,
                        tonumber, type, typeof, pcall, xpcall, assert, error,
                        print, _VERSION,
                        -- the denial list, in case one of them came back
                        _G, getfenv, setfenv, newproxy, gcinfo, coroutine, debug,
                        -- what select and unpack hand back out of a project table
                        select(2, ctx, uf), { unpack({ctx, uf}) },
                    }
                    if scan(roots, 0, {}) then
                        ctx:cycle_close(cycle)
                        return 0
                    end

                    local raised = {}
                    local function keep(fn)
                        local ok, err = pcall(fn)
                        raised[#raised + 1] = err
                        raised[#raised + 1] = tostring(err)
                    end
                    keep(function() ctx:cycle_close(nil) end)
                    keep(function() ctx:cycle_find(cycle, uf.pages.page_a) end)
                    keep(function() ctx:random(5, 1) end)
                    keep(function() return ctx:try(nil) end)
                    keep(function() return uf.missing.field end)
                    ctx:cycle_close(cycle)
                    keep(function() ctx:cycle_click(cycle, hit) end)
                    if scan(raised, 0, {}) then return 0 end
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK(built.clicks->clickCount() == 0);
            }
        }

        TEST_CASE("The standard library a project shares with the framework is immutable")
        {
            auto built = buildBinding(resolvingFrames(FrameId{202}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The project environment whitelists library tables BY REFERENCE from
            // the main globals, so `string` in a project script and `string` in
            // the framework are the same object. A writable one would be the
            // shortest path to the framework there is: redefine string.format and
            // trusted code calls the redefinition. luaL_sandbox freezes them, and
            // these attacks are what holds that to it -- including the string
            // metatable, which the sandbox freezes without protecting, so it
            // stays readable and must stay unwritable.
            constexpr Attack attacks[] = {
                {"control: the libraries are readable",
                 "type(string.format) == 'function' and type(table.concat) == 'function'"
                 " and type(math.floor) == 'function' and type(os.difftime) == 'function'"},
                {"string is frozen", "table.isfrozen(string)"},
                {"table is frozen", "table.isfrozen(table)"},
                {"math is frozen", "table.isfrozen(math)"},
                {"os is frozen", "table.isfrozen(os)"},
                {"bit32 is frozen", "table.isfrozen(bit32)"},
                {"buffer is frozen", "table.isfrozen(buffer)"},
                {"utf8 is frozen", "table.isfrozen(utf8)"},
                {"vector is frozen", "table.isfrozen(vector)"},
                {"an existing library entry cannot be replaced",
                 "not pcall(function() string.format = print end)"},
                {"a new library entry cannot be added",
                 "not pcall(function() string.escalate = print end)"},
                {"rawset cannot reach a library entry",
                 "not pcall(rawset, string, 'format', print)"},
                {"control: the string metatable is readable and is the string table",
                 "getmetatable('').__index == string and ('').format == string.format"},
                {"the string metatable is frozen", "table.isfrozen(getmetatable(''))"},
                {"the string metatable cannot be rewritten",
                 "not pcall(function() getmetatable('').__index = {} end)"},
                {"a clone of the string metatable cannot be attached to anything",
                 "not pcall(setmetatable, '', table.clone(getmetatable('')))"},
                // Shadowing the NAME is allowed and is not a hole: the project
                // environment is a fresh writable copy per run, so the binding
                // dies with the run and never reaches the framework's own.
                {"control: shadowing a library name only rebinds this run's copy",
                 "(function()\n"
                 "    local real = string\n"
                 "    string = { format = function() return 'forged' end }\n"
                 "    local shadowed = string.format('x')\n"
                 "    string = real\n"
                 "    return shadowed == 'forged' and string.format('%d', 7) == '7'\n"
                 "end)()"},
            };

            expectEveryAttackHolds(context, built, attacks);
        }

        TEST_CASE("A Tier B carrier cannot be forged from any value a script can hold")
        {
            auto built = buildBinding(resolvingFrames(FrameId{203}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The carrier is host-minted userdata under a host tag. ctx:try keys
            // on `type(err) == 'userdata'` plus the label; C++ keys on the tag
            // alone. The table-shaped forgeries are already refused by
            // test-task-binding.cpp; what this adds is the routes that produce a
            // value of the RIGHT type -- the other host userdata a script legally
            // holds, and Luau's own non-table opaque values.
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local page = ctx:cycle_page(cycle)
                local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                ctx:cycle_click(cycle, hit)

                -- Control: the genuine carrier IS classified by this exact path.
                local ok, real = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok ~= false or real == nil then return 0 end
                if type(real) ~= 'userdata' then return 0 end

                -- Every other userdata a project script can name is userdata too,
                -- and each wears a label of its own. try must refuse all of them.
                local impostors = {
                    uf.pages.page_a,
                    uf.recognizers.action_target,
                    cycle,
                    page,
                    hit,
                }
                for _, impostor in ipairs(impostors) do
                    if type(impostor) ~= 'userdata' then return 0 end
                    -- Control: the impostor is genuinely of the right TYPE, so a
                    -- refusal below is about identity and not about type.
                    if getmetatable(impostor) == getmetatable(real) then return 0 end
                    local through, back = pcall(function()
                        ctx:try(function() error(impostor) end)
                    end)
                    if through then return 0 end
                    if back ~= impostor then return 0 end
                end

                -- Luau's other opaque values are not userdata at all, so they
                -- fail the first half of the test rather than the second.
                if type(buffer.create(8)) == 'userdata' then return 0 end
                if type(vector.create(1, 2, 3)) == 'userdata' then return 0 end

                -- newproxy is the one base-library way to mint a userdata and is
                -- absent from the project environment; so is every other route.
                if newproxy ~= nil then return 0 end
                if pcall(table.clone, real) then return 0 end
                -- table.clone of a HANDLE fails because a userdata is not a
                -- table -- a different reason from the carrier's protected
                -- metatable, and the control below shows the verb itself works.
                if pcall(table.clone, cycle) then return 0 end
                if type(table.clone({a = 1})) ~= 'table' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("The host classifies an escaping impostor as the script's own failure")
        {
            // The C++ half: a handle raised uncaught must not choose the kind its
            // run is reported under. The control is the genuine carrier, which
            // must choose it -- otherwise a classifier that named nothing would
            // pass this vacuously.
            SUBCASE("a host handle names no automation kind")
            {
                auto built = buildBinding(resolvingFrames(FrameId{204}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                auto const result = runBoundResult(
                    context,
                    built,
                    "error(uf.pages.page_a)"
                );
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::InvalidResource
                );
            }

            SUBCASE("control: the genuine carrier does name its kind")
            {
                auto built = buildBinding(resolvingFrames(FrameId{205}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    local cycle = ctx:cycle_open()
                    local page = ctx:cycle_page(cycle)
                    local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                    ctx:cycle_click(cycle, hit)
                    local ok, real = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                    if ok ~= false or real == nil then return 1 end
                    error(real)
                )lua";

                auto const result = runBoundResult(context, built, source);
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::StaleObservation
                );
            }
        }

        TEST_CASE("Every host object a project holds refuses mutation on every route")
        {
            auto built = buildBinding(resolvingFrames(FrameId{206}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Handles and the uf data tables are identities the host hands out.
            // A script that could write one could redirect a click; a script that
            // could add one could name a recognizer the catalog never authorized.
            constexpr Attack attacks[] = {
                {"control: the data tables are readable",
                 "uf.pages.page_a ~= nil and uf.recognizers.action_target ~= nil"
                 " and uf.errors.timeout == 'timeout'"},
                {"uf is frozen", "table.isfrozen(uf)"},
                {"uf.pages is frozen", "table.isfrozen(uf.pages)"},
                {"uf.recognizers is frozen", "table.isfrozen(uf.recognizers)"},
                {"uf.errors is frozen", "table.isfrozen(uf.errors)"},
                {"a root field cannot be replaced",
                 "not pcall(function() uf.pages = {} end)"},
                {"a root field cannot be added",
                 "not pcall(function() uf.escalate = print end)"},
                {"rawset cannot reach the root",
                 "not pcall(rawset, uf, 'pages', {})"},
                {"a page cannot be added",
                 "not pcall(function() uf.pages.forged = uf.pages.page_a end)"},
                {"a recognizer cannot be replaced",
                 "not pcall(rawset, uf.recognizers, 'action_target', uf.pages.page_a)"},
                {"an error constant cannot be replaced",
                 "not pcall(function() uf.errors.timeout = 'cancelled' end)"},
                {"a handle refuses a field write",
                 "not pcall(function() uf.pages.page_a.pageId = 1 end)"},
                {"a handle refuses rawset",
                 "not pcall(rawset, uf.pages.page_a, 'pageId', 1)"},
                {"a handle refuses a new metatable",
                 "not pcall(setmetatable, uf.pages.page_a, {})"},
                {"a handle hands out a label, not its metatable",
                 "getmetatable(uf.pages.page_a) == 'uf.page'"
                 " and getmetatable(uf.recognizers.action_target) == 'uf.recognizer'"},
                {"a handle's printed form is its label and nothing else",
                 "tostring(uf.pages.page_a) == 'uf.page'"
                 " and tostring(uf.recognizers.action_target) == 'uf.recognizer'"},
                {"a live ticket and hit are just as opaque",
                 "(function()\n"
                 "    local cycle = ctx:cycle_open()\n"
                 "    local page = ctx:cycle_page(cycle)\n"
                 "    local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)\n"
                 "    local sealed =\n"
                 "        tostring(cycle) == 'uf.cycle'\n"
                 "        and tostring(hit) == 'uf.hit'\n"
                 "        and tostring(page) == 'uf.resolved_page'\n"
                 "        and getmetatable(cycle) == 'uf.cycle'\n"
                 "        and not pcall(function() cycle.ordinal = 99 end)\n"
                 "        and not pcall(rawset, hit, 'cycleOrdinal', 99)\n"
                 "        and not pcall(setmetatable, page, {})\n"
                 "        -- Control: the page still answers its one real method.\n"
                 "        and page:is(uf.pages.page_a)\n"
                 "    ctx:cycle_close(cycle)\n"
                 "    return sealed\n"
                 "end)()"},
                // A clone of uf IS obtainable, because uf carries no protected
                // metatable. It confers nothing: the sub-tables it copies are the
                // same frozen ones, and a handle is an identity, not a capability.
                {"a clone of uf shares the same frozen tables",
                 "(function()\n"
                 "    local copy = table.clone(uf)\n"
                 "    return copy.pages == uf.pages\n"
                 "        and copy.recognizers == uf.recognizers\n"
                 "        and table.isfrozen(copy.pages)\n"
                 "        and not pcall(function() copy.pages.forged = 1 end)\n"
                 "end)()"},
            };

            expectEveryAttackHolds(context, built, attacks);
        }

        TEST_CASE("A primitive called from a non-yieldable context still obeys the protocol")
        {
            // §11's matrix, driven forwards: a primitive is a direct, non-yielding
            // C call, so calling one from inside a host C frame must simply work.
            // If any of these ever stopped working it would mean a primitive had
            // acquired a yield -- the exact regression the design's first
            // invariant (no primitive calls back into Lua) exists to prevent.
            constexpr std::string_view body =
                "        local c = ctx:cycle_open()\n"
                "        local p = ctx:cycle_page(c)\n"
                "        if p == nil or not p:is(uf.pages.page_a) then\n"
                "            opened = -100\n"
                "        else\n"
                "            opened = opened + 1\n"
                "        end\n"
                "        ctx:cycle_close(c)\n";

            for (auto const& form : nonYieldableForms(body))
            {
                INFO("non-yieldable form: ", form.name);
                auto frameSource = std::make_unique<FakeFrameSource>(
                    resolvingFrames(FrameId{210})
                );
                auto* const p_frames = frameSource.get();
                auto built           = buildBindingWith(
                    std::move(frameSource),
                    std::stop_token{},
                    std::make_unique<DiscardingTraceSink>()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                auto const source = "opened = 0\n" + form.source + "return opened\n";
                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK(p_frames->captureCount() == 1U);
                CHECK(built.clicks->clickCount() == 0);
            }
        }

        TEST_CASE("The terminal latch refuses a primitive called from any context")
        {
            // The same matrix, with the cancellation both RAISED and CAUGHT
            // inside the non-yieldable frame. The first cycle_open is cancelled
            // by the frame source and NO stop token is armed anywhere, so the VM
            // interrupt never fires: the only thing that can refuse the second
            // call is the latch the first one set, checked at the C guard entry
            // before the engine is touched.
            //
            // captureCount staying at one is what makes "before the engine" an
            // observation rather than an inference.
            constexpr std::string_view body =
                "        local first = pcall(function() return ctx:cycle_open() end)\n"
                "        local again = pcall(function() return ctx:cycle_open() end)\n"
                "        if not first and not again then refused = refused + 1 end\n";

            for (auto const& form : nonYieldableForms(body))
            {
                INFO("non-yieldable form: ", form.name);
                auto frameSource = std::make_unique<CancelOnceFrameSource>(
                    grayFrame(
                        anno::test::fingerprint(3, 1, 96, 96),
                        resolvingPixels(),
                        FrameId{211}
                    )
                );
                auto* const p_frames = frameSource.get();
                auto built           = buildBindingWith(
                    std::move(frameSource),
                    std::stop_token{},
                    std::make_unique<DiscardingTraceSink>()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                auto const source =
                    "refused = 0\n" + form.source + "return refused\n";
                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK(p_frames->captureCount() == 1U);
                CHECK(built.clicks->clickCount() == 0);
            }
        }

        TEST_CASE("No nest of catchers converts a swallowed cancellation into control")
        {
            auto frameSource = std::make_unique<CancelOnceFrameSource>(
                grayFrame(
                    anno::test::fingerprint(3, 1, 96, 96),
                    resolvingPixels(),
                    FrameId{212}
                )
            );
            auto* const p_frames = frameSource.get();
            auto built           = buildBindingWith(
                std::move(frameSource),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The Tier C sentinel is a plain string a project pcall MAY catch,
            // and the design accepts that: control is not what the sentinel
            // protects, the latch is. Here the sentinel is caught through every
            // nesting a script has -- bare pcall, ctx:try, pcall inside try, try
            // inside pcall -- and the run keeps going. Every later primitive is
            // still refused, and the frame source is never reached again.
            constexpr std::string_view source = R"lua(
                local function open() return ctx:cycle_open() end

                -- 1. bare pcall
                local caught, sentinel = pcall(open)
                if caught then return 0 end
                if type(sentinel) ~= 'string' then return 0 end

                -- 2. ctx:try re-raises it, so the outer pcall catches
                local through, again = pcall(function() return ctx:try(open) end)
                if through then return 0 end
                if again ~= sentinel then return 0 end

                -- 3. a pcall INSIDE try swallows it completely: try sees its
                --    function return normally and reports success.
                local ok, err = ctx:try(function() return pcall(open) end)
                if ok ~= true or err ~= nil then return 0 end

                -- 4. and a try inside a pcall inside a pcall
                local outer = pcall(function()
                    return pcall(function() return ctx:try(open) end)
                end)
                if not outer then return 0 end

                -- Every one of those was swallowed, and none of it bought a
                -- single further primitive: they all still refuse.
                if pcall(open) then return 0 end
                if pcall(function() return ctx:cycle_close(nil) end) then return 0 end
                local waited = pcall(function()
                    ctx:wait_for_page(uf.pages.page_a, nil, function() end)
                end)
                if waited then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            // One capture: the cancelled one. Every refusal after it stopped at
            // the guard, so none of them cost a frame.
            CHECK(p_frames->captureCount() == 1U);
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("A hard cancel cannot be traded for a recoverable Tier B failure")
        {
            // The failure-priority rule of §9: a cancel wins, fail closed. The
            // attack is the only shape that can even try -- a primitive that
            // mints a Tier B carrier while a hard cancel is pending -- and the
            // script catches that carrier and keeps running, which is exactly
            // what `retryable` invites an author to do.
            //
            // mark() is the discriminator: a host-visible witness that only runs
            // if the script really did continue past the cancel.
            auto const stalledRun = [](bool requestStop) -> DiscriminatorRun
            {
                auto stop        = std::stop_source{};
                auto frameSource = requestStop
                    ? std::unique_ptr<engine::IFrameSource>{
                          std::make_unique<StallAndStopFrameSource>(stop)
                      }
                    : std::unique_ptr<engine::IFrameSource>{
                          std::make_unique<StallOnlyFrameSource>()
                      };
                auto built = buildBindingWith(
                    std::move(frameSource),
                    requestStop ? stop.get_token() : std::stop_token{},
                    std::make_unique<DiscardingTraceSink>()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{
                        .cancellation = requestStop ? stop.get_token() : std::stop_token{},
                    },
                };

                constexpr std::string_view source = R"lua(
                    -- The capture fails with a RECOVERABLE kind, so this is a
                    -- Tier B carrier and not the Tier C sentinel.
                    local ok, err = ctx:try(function() return ctx:cycle_open() end)
                    if ok ~= false then return 0 end
                    if type(err) ~= 'userdata' then return 0 end
                    if err.kind ~= 'capture_stalled' then return 0 end
                    if err.retryable ~= true then return 0 end
                    -- An author told the failure is retryable does exactly this.
                    mark()
                    return 1
                )lua";

                return runWithMark(
                    context,
                    built,
                    requestStop ? stop.get_token() : std::stop_token{},
                    source
                );
            };

            SUBCASE("control: with nothing cancelled the Tier B failure is the run's own")
            {
                // Without this the case below would also pass on a binding that
                // reported Cancelled for every stalled capture.
                auto const run = stalledRun(false);
                REQUIRE(run.result.has_value());
                CHECK(*run.result == doctest::Approx(1.0));
                CHECK(run.markCount == 1);
            }

            SUBCASE("a stop requested during the raise still ends the run cancelled")
            {
                auto const run = stalledRun(true);
                REQUIRE_FALSE(run.result.has_value());
                CHECK(
                    automationErrorKind(run.result.error())
                    == AutomationErrorKind::Cancelled
                );
                // The carrier was catchable and was caught, and it still bought
                // no statement: the interrupt breaks the thread at the next
                // safepoint, which is the call to mark() itself.
                CHECK(run.markCount == 0);
            }
        }

        TEST_CASE("A cancel that lands with a Tier B carrier in hand is still the cancel")
        {
            // The sharpest form of §9's failure priority, and the only shape in
            // which a script can hold a catchable automation error while a hard
            // cancel is already pending.
            //
            // The script mints a real Tier B carrier first, then walks into a
            // non-yieldable C frame (§11's table.sort), arms the stop from inside
            // it, swallows the C-boundary error the break degrades into, and
            // re-raises its carrier -- an attempt to have the run reported under
            // the kind IT chose rather than as cancelled. What actually happens
            // is that the interrupt fires again at the very call op that would
            // raise the carrier, so the carrier never becomes the pending error
            // and the run is reported Cancelled either way.
            auto const carrierRun = [](bool armStop) -> DiscriminatorRun
            {
                auto stop  = std::stop_source{};
                auto frame = grayFrame(
                    anno::test::fingerprint(3, 1, 96, 96),
                    resolvingPixels(),
                    FrameId{214}
                );
                auto built = buildBindingWith(
                    std::make_unique<StopOnNthCaptureFrameSource>(
                        std::move(frame),
                        stop,
                        // Capture 1 mints the carrier; capture 2 happens inside
                        // the comparator, which is where the stop is armed. A
                        // stop that is never requested is the control.
                        armStop ? std::size_t{2} : std::size_t{0}
                    ),
                    stop.get_token(),
                    std::make_unique<DiscardingTraceSink>()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.cancellation = stop.get_token()},
                };

                constexpr std::string_view source = R"lua(
                    local cycle = ctx:cycle_open()
                    local page = ctx:cycle_page(cycle)
                    local hit = ctx:cycle_find(cycle, uf.recognizers.action_target)
                    ctx:cycle_click(cycle, hit)

                    -- A genuine host-minted carrier, in hand before anything is
                    -- cancelled.
                    local held, carrier = ctx:try(function()
                        ctx:cycle_click(cycle, hit)
                    end)
                    if held ~= false or type(carrier) ~= 'userdata' then return 0 end

                    table.sort({2, 1}, function(a, b)
                        -- Arms the stop from inside a host C frame, where a
                        -- break cannot unwind cleanly and becomes a catchable
                        -- error instead. Swallow it and carry on.
                        pcall(function()
                            local c = ctx:cycle_open()
                            ctx:cycle_close(c)
                        end)
                        -- The bid: make the run report the carrier's kind.
                        error(carrier)
                    end)

                    mark()
                    return 1
                )lua";

                return runWithMark(context, built, stop.get_token(), source);
            };

            SUBCASE("control: with no stop armed the carrier does name the run's kind")
            {
                // Without this the case below would pass on a binding that
                // reported Cancelled for every raise out of a sort comparator.
                auto const run = carrierRun(false);
                REQUIRE_FALSE(run.result.has_value());
                CHECK(
                    automationErrorKind(run.result.error())
                    == AutomationErrorKind::StaleObservation
                );
                CHECK(run.markCount == 0);
            }

            SUBCASE("the cancel wins and the carrier's kind never reaches the report")
            {
                auto const run = carrierRun(true);
                REQUIRE_FALSE(run.result.has_value());
                CHECK(
                    automationErrorKind(run.result.error())
                    == AutomationErrorKind::Cancelled
                );
                CHECK(run.markCount == 0);
            }
        }

        TEST_CASE("A task VM stops a runaway that never calls a primitive")
        {
            // The interrupt-driven half of cancellation, on a REAL task VM rather
            // than a bare one: a script that touches no primitive is stopped by
            // the instruction budget alone, so the latch is not the only thing
            // standing between a hostile script and the host.
            auto built = buildBinding(resolvingFrames(FrameId{213}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const runWithBudget = [&](uint64 budgetTicks) -> Result<double>
            {
                auto config                 = taskVmConfig(built.surface, context);
                config.interruptBudgetTicks = budgetTicks;
                config.maxRuntime           = std::chrono::hours{1};
                auto engine                 = script::Engine::create(config);
                REQUIRE(engine.has_value());
                return engine->runNumber(
                    "local n = 0 while true do n = n + 1 end return n",
                    "task-runaway"
                );
            };

            SUBCASE("the budget stops it")
            {
                auto const result = runWithBudget(200'000);
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::Cancelled
                );
            }

            SUBCASE("control: the same VM runs a bounded script to completion")
            {
                // Without this the case above would pass on a VM that failed
                // every run for some unrelated reason.
                auto config                 = taskVmConfig(built.surface, context);
                config.interruptBudgetTicks = 200'000;
                config.maxRuntime = std::chrono::hours{1};
                auto engine       = script::Engine::create(config);
                REQUIRE(engine.has_value());
                auto const result = engine->runNumber(
                    "local n = 0 for i = 1, 100 do n = n + i end return n",
                    "task-bounded"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(5050.0));
            }
        }
    }
}
