#include <task/script-bindings.hpp>
#include <task/framework-bundle.hpp>

#include <script/engine.hpp>
#include <script/testing/environment-probe.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/enum-reflection.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        // An engine whose only host capability is the frozen `uf` root. It
        // carries `uf.errors` and nothing else since the script-owned page model
        // retired the element and page name tables
        // (docs/plans/2026-07-31-script-owned-page-model.md 9).
        auto engineWithUfRoot() -> Result<script::Engine>
        {
            auto config              = script::EngineConfig{};
            config.installHostTables = scriptHostTableInstaller();
            config.projectGlobals    = scriptProjectGlobals();
            return script::Engine::create(config);
        }

        // Runs `return (<expr>) and 1 or 0`; 1.0 means the expression was truthy.
        auto truthy(script::Engine& engine, std::string_view expr) -> double
        {
            auto const source = "return (" + std::string{expr} + ") and 1 or 0";
            auto const result = engine.runNumber(source, "uf-expr");
            REQUIRE(result.has_value());
            return *result;
        }

        // Asserts `source` fails at runtime (a rejected write, a protected
        // metatable, and so on surface as InvalidResource).
        auto expectRejected(script::Engine& engine, std::string_view source) -> void
        {
            auto const result = engine.runNumber(std::string{source}, "uf-reject");
            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationErrorKind(result.error())
                == AutomationErrorKind::InvalidResource
            );
        }

        // Keeps the wire line of every event it is handed. A StampedTraceEvent
        // cannot be minted outside modules/trace, so recording through a real
        // recorder is the only way to read back what a trace actually says.
        class LineRecordingSink final : public trace::ITraceSink
        {
            std::vector<std::string> m_lines{};

        public:
            [[nodiscard]]
            auto emit(trace::StampedTraceEvent const& event) -> Status override
            {
                m_lines.emplace_back(trace::serializeTraceEvent(event));
                return ok();
            }

            [[nodiscard]]
            auto lines() const noexcept UF_LIFETIME_BOUND
                -> std::vector<std::string> const&
            {
                return m_lines;
            }
        };

        // The value of a line's "errorKind" member. A wire spelling is lower-case
        // ASCII with underscores, so nothing in it is escaped and the value ends
        // at the next quote.
        auto errorKindField(std::string const& line) -> std::string
        {
            constexpr auto member = std::string_view{R"("errorKind":")"};
            auto const start      = line.find(member);
            REQUIRE(start != std::string::npos);
            auto const from = start + member.size();
            auto const end  = line.find('"', from);
            REQUIRE(end != std::string::npos);
            return line.substr(from, end - from);
        }

        // One serialized run.finished line per error kind, in reflected enum
        // order, reduced to the errorKind each one wrote.
        auto tracedErrorKindSpellings() -> std::vector<std::string>
        {
            auto spellings = std::vector<std::string>{};
            spellings.reserve(enumEntries<AutomationErrorKind>().size());

            for (auto const& entry : enumEntries<AutomationErrorKind>())
            {
                // One recorder per line: a run's bracket closes at its first
                // run.finished and the stream validator accepts nothing after
                // it, so one recorder would be a dozen runs in one bracket.
                auto sink          = std::make_unique<LineRecordingSink>();
                auto* const p_sink = sink.get();
                auto recorder      = trace::TraceRecorder{
                    std::move(sink),
                    TaskRunId{1},
                    GenerationId{1},
                    trace::FrontEnd::Task,
                };

                auto const status = recorder.emit(trace::TraceEvent{
                    .kind       = trace::TraceEventKind::RunFinished,
                    .runOutcome = trace::RunOutcome::Failed,
                    .errorKind  = entry.value,
                });
                REQUIRE(status.has_value());
                REQUIRE(p_sink->lines().size() == 1U);
                spellings.emplace_back(errorKindField(p_sink->lines().front()));
            }
            return spellings;
        }

        // Every kind's wire spelling in reflected enum order, from the one domain
        // function both the trace and the script table are now built on.
        auto expectedErrorKindSpellings() -> std::vector<std::string>
        {
            auto spellings = std::vector<std::string>{};
            spellings.reserve(enumEntries<AutomationErrorKind>().size());
            for (auto const& entry : enumEntries<AutomationErrorKind>())
            {
                spellings.emplace_back(automationErrorWireName(entry.value));
            }
            return spellings;
        }

        TEST_CASE("The uf root and its error table reject every write")
        {
            auto engine = engineWithUfRoot();
            REQUIRE(engine.has_value());

            SUBCASE("a new key on the uf root is rejected")
            {
                expectRejected(*engine, "uf.injected = 1\nreturn 0");
            }
            SUBCASE("a new key on uf.errors is rejected")
            {
                expectRejected(*engine, "uf.errors.injected = 1\nreturn 0");
            }
            SUBCASE("overwriting an error kind constant is rejected")
            {
                expectRejected(*engine, "uf.errors.timeout = 'other'\nreturn 0");
            }
        }

        TEST_CASE("The retired element and page tables are absent rather than empty")
        {
            auto engine = engineWithUfRoot();
            REQUIRE(engine.has_value());

            // An empty table would let `uf.elements.whatever` read as nil and a
            // script go on believing the surface exists. The tables are gone, so
            // the ROOT itself answers nil for them.
            CHECK(truthy(*engine, "uf.elements == nil") == doctest::Approx(1.0));
            CHECK(truthy(*engine, "uf.pages == nil") == doctest::Approx(1.0));
        }

        TEST_CASE("uf.errors holds exactly one constant per automation error kind")
        {
            auto engine = engineWithUfRoot();
            REQUIRE(engine.has_value());

            // None missing, and every constant is its own key, so a script
            // compares the exact string the host raised. The table is built by
            // iterating the reflected kinds at install time, so a kind added to
            // the enum appears here with no edit to the surface -- and cannot
            // compile at all until the domain has given it a wire spelling.
            for (auto const& wire : expectedErrorKindSpellings())
            {
                auto const expression = "uf.errors." + wire + " == '" + wire + "'";
                INFO("kind: ", wire);
                CHECK(truthy(*engine, expression) == doctest::Approx(1.0));
            }

            // No extras: the script counts the installed table itself, so a stray
            // key, or a duplicate spelling that collapsed two kinds onto one key,
            // fails here rather than passing every lookup above.
            auto const count = engine->runNumber(
                "local n = 0\nfor _ in pairs(uf.errors) do n = n + 1 end\nreturn n",
                "uf-errors-count"
            );
            REQUIRE(count.has_value());
            CHECK(
                *count
                == doctest::Approx(
                    static_cast<double>(enumEntries<AutomationErrorKind>().size())
                )
            );
        }

        TEST_CASE("A trace line and uf.errors spell every error kind identically")
        {
            auto engine = engineWithUfRoot();
            REQUIRE(engine.has_value());

            // A trace line names a failure with exactly the string the script
            // layer sees. Both sides are read from the real artifacts --
            // serialized umbraflow-trace/v3 lines and the table installed on a
            // live VM -- so a divergence shows up as wrong output rather than as
            // two calls to the same function.
            auto const traced = tracedErrorKindSpellings();
            CHECK(traced == expectedErrorKindSpellings());

            for (auto const& wire : traced)
            {
                auto const expression = "uf.errors." + wire + " == '" + wire + "'";
                INFO("traced kind: ", wire);
                CHECK(truthy(*engine, expression) == doctest::Approx(1.0));
            }
        }

        // The distinctive value modules/task/runtime/ctx.luau assigns as a
        // FRAMEWORK global -- not as an export, because the exports are published
        // into the project environment and could not carry an isolation claim.
        // Renaming it on one side alone turns the claim vacuous, and the
        // bundle-side control below reddens first.
        constexpr auto k_frameworkSentinel =
            std::string_view{"uf-framework-sentinel-6b21f0"};

        // A bounded, cycle-safe reachability search for the sentinel over
        // everything a project script can name on a real task VM, following
        // table values, table keys, and metatables.
        [[nodiscard]]
        auto sentinelScan() -> std::string
        {
            return "local target = '" + std::string{k_frameworkSentinel} + "'\n"
                   R"lua(
                local found = false
                local seen = {}

                local function scan(value, depth)
                    if found or depth > 8 then return end
                    local kind = type(value)
                    if kind == 'string' then
                        if value == target then found = true end
                        return
                    end
                    if kind ~= 'table' or seen[value] then return end
                    seen[value] = true
                    for key, entry in pairs(value) do
                        scan(key, depth + 1)
                        scan(entry, depth + 1)
                    end
                    scan(getmetatable(value), depth + 1)
                end

                scan({
                    -- The framework's own globals, and the module names the
                    -- loader binds the bundle's exports under: `ctx` and `task`
                    -- on a real task VM, `probe` when the same source is loaded
                    -- through the script-layer probe. All of them are listed so
                    -- one scan serves both sides.
                    frameworkSentinel,
                    uf, ctx, task, probe,
                    _G, getfenv, setfenv, newproxy, gcinfo, coroutine, debug,
                    _VERSION, assert, error, getmetatable, ipairs, next, pairs,
                    pcall, print, rawequal, rawget, rawlen, rawset, select,
                    setmetatable, tonumber, tostring, type, typeof, unpack,
                    xpcall, bit32, buffer, math, os, string, table, utf8, vector,
                }, 0)

                return found and 1 or 0
            )lua";
        }

        TEST_CASE("A task VM's project environment reaches ctx and nothing else framework-side")
        {
            auto const entries = frameworkBundleEntries();
            REQUIRE_FALSE(entries.empty());
            auto const context = entries.front();

            SUBCASE("control: the bundle really carries the sentinel")
            {
                CHECK(context.name == "ctx");
                CHECK(
                    context.source.find(k_frameworkSentinel)
                    != std::string_view::npos
                );
            }

            SUBCASE("control: the sentinel is reachable inside a framework environment")
            {
                // The real bundle source, loaded under a framework environment by
                // the same loader a task VM uses. Without this the case below
                // would also pass on a VM where the framework never ran.
                auto const found = script::testing::runInEnvironment(
                    context.source,
                    sentinelScan(),
                    script::testing::ProbeEnvironment::Framework
                );
                REQUIRE(found.has_value());
                CHECK(*found == doctest::Approx(1.0));
            }

            SUBCASE("no route out of a real task's project environment reaches it")
            {
                auto engine = script::Engine::create(
                    script::EngineConfig{
                        .frameworkModules        = frameworkScriptModules(),
                        .installHostTables       = scriptHostTableInstaller(),
                        .projectGlobals          = scriptProjectGlobals(),
                        .frameworkProjectGlobals = frameworkProjectGlobals(),
                    }
                );
                REQUIRE(engine.has_value());

                auto const found = engine->runNumber(sentinelScan(), "task-env-scan");
                REQUIRE(found.has_value());
                CHECK(*found == doctest::Approx(0.0));

                // What the project DOES see is exactly the two published exports,
                // named rather than searched for, and not the framework-only
                // global beside them.
                CHECK(truthy(*engine, "type(ctx) == 'table'") == doctest::Approx(1.0));
                CHECK(truthy(*engine, "type(task) == 'table'") == doctest::Approx(1.0));
                CHECK(
                    truthy(*engine, "frameworkSentinel == nil") == doctest::Approx(1.0)
                );
            }
        }
    }
}
