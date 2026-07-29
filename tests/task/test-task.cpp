#include "../annotation/test-helpers.hpp"

#include <task/capability-surface.hpp>
#include <task/framework-bundle.hpp>

#include <script/engine.hpp>
#include <script/testing/environment-probe.hpp>

#include <annotation/catalog.hpp>

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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        namespace at = annotation::test;

        constexpr auto k_anchorId  = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_dailyId   = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_battleId  = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_pageId    = "00000000-0000-0000-0000-000000000101";

        // A minimal but complete catalog: one page anchor, two action targets,
        // and one page. Built from the annotation module's own test fixtures
        // (tests/annotation/test-helpers.hpp) so the surface is driven by exactly
        // the catalog shape the engine loads at runtime.
        auto buildCatalog() -> annotation::RecognitionCatalog
        {
            auto const fingerprint = at::fingerprint();
            auto const pageId      = at::pageId(k_pageId);

            auto recognizers = std::vector<annotation::RecognizerDefinition>{};
            recognizers.push_back(at::recognizer(
                fingerprint,
                at::recognizerId(k_anchorId),
                "home_marker",
                annotation::AnnotationType::PageAnchor,
                at::pixelRect(0, 0, 1, 1),
                at::pixelRect(0, 0, 4, 4)
            ));
            recognizers.push_back(at::recognizer(
                fingerprint,
                at::recognizerId(k_dailyId),
                "daily_button",
                annotation::AnnotationType::ActionTarget,
                at::pixelRect(1, 1, 1, 1),
                at::pixelRect(0, 0, 4, 4),
                {pageId}
            ));
            recognizers.push_back(at::recognizer(
                fingerprint,
                at::recognizerId(k_battleId),
                "battle",
                annotation::AnnotationType::ActionTarget,
                at::pixelRect(2, 2, 1, 1),
                at::pixelRect(0, 0, 4, 4),
                {pageId}
            ));

            return at::catalog(
                fingerprint,
                std::move(recognizers),
                {at::page(pageId, "home", {at::recognizerId(k_anchorId)})}
            );
        }

        // Builds an engine whose only host capability is the umbra table for the
        // catalog above.
        auto engineWithUmbra(CapabilitySurface const& surface)
            -> Result<script::Engine>
        {
            auto config              = script::EngineConfig{};
            config.installHostTables = surface.installer();
            config.projectGlobals    = CapabilitySurface::projectGlobals();
            return script::Engine::create(config);
        }

        // Runs `return (<expr>) and 1 or 0`; 1.0 means the expression was truthy.
        auto truthy(script::Engine& engine, std::string_view expr) -> double
        {
            auto const source = "return (" + std::string{expr} + ") and 1 or 0";
            auto const result = engine.runNumber(source, "umbra-expr");
            REQUIRE(result.has_value());
            return *result;
        }

        // Asserts `source` fails at runtime (a rejected write, a protected
        // metatable, and so on surface as InvalidResource).
        auto expectRejected(script::Engine& engine, std::string_view source) -> void
        {
            auto const result = engine.runNumber(std::string{source}, "umbra-reject");
            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationErrorKind(result.error())
                == AutomationErrorKind::InvalidResource
            );
        }

        // Keeps the wire line of every event it is handed. A StampedTraceEvent
        // cannot be minted outside modules/trace -- its constructor is private to
        // TraceRecorder -- so recording through a real recorder is the only way to
        // read back what a trace actually says.
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
            auto sink          = std::make_unique<LineRecordingSink>();
            auto* const p_sink = sink.get();
            auto recorder      = trace::TraceRecorder{
                std::move(sink),
                TaskRunId{1},
                GenerationId{1},
            };

            for (auto const& entry : enumEntries<AutomationErrorKind>())
            {
                auto const status = recorder.emit(trace::TraceEvent{
                    .kind       = trace::TraceEventKind::RunFinished,
                    .runOutcome = trace::RunOutcome::Failed,
                    .errorKind  = entry.value,
                });
                REQUIRE(status.has_value());
            }

            auto spellings = std::vector<std::string>{};
            spellings.reserve(p_sink->lines().size());
            for (auto const& line : p_sink->lines())
            {
                spellings.emplace_back(errorKindField(line));
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

        TEST_CASE("CapabilitySurface exposes only action targets and every page")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            // Page anchors are not findable, so only the two action targets are
            // exposed; every page is exposed.
            CHECK(surface->recognizerCount() == 2);
            CHECK(surface->pageCount() == 1);
        }

        TEST_CASE("Scripts read named handles and see nil for absent or anchor names")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            constexpr std::string_view present[] = {
                "umbra.recognizers.daily_button ~= nil",
                "umbra.recognizers.battle ~= nil",
                "umbra.pages.home ~= nil",
            };
            for (std::string_view const expr : present)
            {
                INFO("expression: ", expr);
                CHECK(truthy(*engine, expr) == doctest::Approx(1.0));
            }

            constexpr std::string_view absent[] = {
                // A page anchor is page-internal evidence, never a findable handle.
                "umbra.recognizers.home_marker == nil",
                // Names that do not exist resolve to nil, not an error.
                "umbra.recognizers.does_not_exist == nil",
                "umbra.pages.does_not_exist == nil",
            };
            for (std::string_view const expr : absent)
            {
                INFO("expression: ", expr);
                CHECK(truthy(*engine, expr) == doctest::Approx(1.0));
            }
        }

        TEST_CASE("The umbra tables and handles reject every write")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            SUBCASE("a new key on the umbra root is rejected")
            {
                expectRejected(*engine, "umbra.injected = 1\nreturn 0");
            }
            SUBCASE("a new key on umbra.recognizers is rejected")
            {
                expectRejected(*engine, "umbra.recognizers.injected = 1\nreturn 0");
            }
            SUBCASE("overwriting an existing recognizer handle is rejected")
            {
                expectRejected(*engine, "umbra.recognizers.daily_button = 1\nreturn 0");
            }
            SUBCASE("a new key on umbra.pages is rejected")
            {
                expectRejected(*engine, "umbra.pages.injected = 1\nreturn 0");
            }
            SUBCASE("writing a field on a handle is rejected")
            {
                expectRejected(
                    *engine,
                    "umbra.recognizers.daily_button.x = 1\nreturn 0"
                );
            }
            SUBCASE("a new key on umbra.errors is rejected")
            {
                expectRejected(*engine, "umbra.errors.injected = 1\nreturn 0");
            }
            SUBCASE("overwriting an error kind constant is rejected")
            {
                expectRejected(*engine, "umbra.errors.timeout = 'other'\nreturn 0");
            }
        }

        TEST_CASE("umbra.errors holds exactly one constant per automation error kind")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            // None missing, and every constant is its own key, so a script writes
            // err.kind == umbra.errors.timeout and compares the exact string the
            // host raised. The table is built by iterating the reflected kinds at
            // install time, which is why a kind added to the enum appears here
            // with no edit to the surface -- and cannot compile at all until the
            // domain has given it a wire spelling.
            for (auto const& wire : expectedErrorKindSpellings())
            {
                auto const expression = "umbra.errors." + wire + " == '" + wire + "'";
                INFO("kind: ", wire);
                CHECK(truthy(*engine, expression) == doctest::Approx(1.0));
            }

            // No extras: the script counts the installed table itself, so a stray
            // key, or a duplicate spelling that collapsed two kinds onto one key,
            // fails here rather than passing every lookup above.
            auto const count = engine->runNumber(
                "local n = 0\nfor _ in pairs(umbra.errors) do n = n + 1 end\nreturn n",
                "umbra-errors-count"
            );
            REQUIRE(count.has_value());
            CHECK(
                *count
                == doctest::Approx(
                    static_cast<double>(enumEntries<AutomationErrorKind>().size())
                )
            );
        }

        TEST_CASE("A trace line and umbra.errors spell every error kind identically")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            // The invariant the two deleted copies of this mapping asserted in
            // comments and nothing checked: a trace line names a failure with
            // exactly the string the script layer sees. Both sides are read from
            // the real artifacts -- serialized umbraflow-trace/v1 lines and the
            // table installed on a live VM -- so a divergence shows up as wrong
            // output rather than as two calls to the same function.
            auto const traced = tracedErrorKindSpellings();
            CHECK(traced == expectedErrorKindSpellings());

            for (auto const& wire : traced)
            {
                auto const expression = "umbra.errors." + wire + " == '" + wire + "'";
                INFO("traced kind: ", wire);
                CHECK(truthy(*engine, expression) == doctest::Approx(1.0));
            }
        }

        TEST_CASE("Handles are opaque userdata that leak neither metatable nor fields")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            // A handle is userdata, and its metatable and tostring reveal only the
            // fixed kind label -- never the internal id or an address.
            constexpr std::string_view opaque[] = {
                "type(umbra.recognizers.daily_button) == 'userdata'",
                "type(umbra.pages.home) == 'userdata'",
                "tostring(umbra.recognizers.daily_button) == 'umbra.recognizer'",
                "tostring(umbra.pages.home) == 'umbra.page'",
                "getmetatable(umbra.recognizers.daily_button) == 'umbra.recognizer'",
                "getmetatable(umbra.pages.home) == 'umbra.page'",
                // A field read of a handle yields nil (the method table is empty
                // this wave), never any internal state.
                "umbra.recognizers.daily_button.id == nil",
                // pairs() cannot enumerate a userdata's contents.
                "pcall(function() for _ in pairs(umbra.recognizers.daily_button) do end end) == false",
            };
            for (std::string_view const expr : opaque)
            {
                INFO("expression: ", expr);
                CHECK(truthy(*engine, expr) == doctest::Approx(1.0));
            }
        }

        TEST_CASE("The handle metatable is protected against replacement")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            // setmetatable on a handle is refused: the guard cannot be lifted to
            // reopen the handle for indexing or writing.
            expectRejected(
                *engine,
                "setmetatable(umbra.recognizers.daily_button, {})\nreturn 0"
            );
        }

        TEST_CASE("Distinct handles carry distinct identity, equal handles compare equal")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto engine = engineWithUmbra(*surface);
            REQUIRE(engine.has_value());

            // Each named handle is one shared userdata object, so repeated lookups
            // are identical, while two different names are two different objects.
            CHECK(
                truthy(
                    *engine,
                    "umbra.recognizers.daily_button == umbra.recognizers.daily_button"
                )
                == doctest::Approx(1.0)
            );
            CHECK(
                truthy(
                    *engine,
                    "umbra.recognizers.daily_button ~= umbra.recognizers.battle"
                )
                == doctest::Approx(1.0)
            );
        }

        // The distinctive value modules/task/runtime/placeholder.luau exports so
        // the isolation claim below has something concrete to be about. It is
        // spelled here as well as in the .luau, and the bundle-side check makes
        // that duplication self-policing: renaming it on one side alone turns
        // the claim vacuous, and the check reddens first.
        constexpr auto k_frameworkSentinel =
            std::string_view{"uf-framework-sentinel-6b21f0"};

        // A bounded, cycle-safe reachability search for the sentinel over
        // everything a project script can name on a real task VM -- the umbra
        // root included -- following table values, table keys, and metatables.
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
                    -- The module name the loader binds the bundle's exports
                    -- under: `placeholder` on a real task VM, `probe` when the
                    -- same source is loaded through the script-layer probe. Both
                    -- are listed so one scan serves both sides.
                    umbra, placeholder, probe,
                    _G, getfenv, setfenv, newproxy, gcinfo, coroutine, debug,
                    _VERSION, assert, error, getmetatable, ipairs, next, pairs,
                    pcall, print, rawequal, rawget, rawlen, rawset, select,
                    setmetatable, tonumber, tostring, type, typeof, unpack,
                    xpcall, bit32, buffer, math, os, string, table, utf8, vector,
                }, 0)

                return found and 1 or 0
            )lua";
        }

        TEST_CASE("A task VM's project environment cannot reach the real framework bundle")
        {
            auto const catalog = buildCatalog();
            auto surface       = CapabilitySurface::create(catalog);
            REQUIRE(surface.has_value());

            auto const entries = frameworkBundleEntries();
            REQUIRE_FALSE(entries.empty());
            auto const placeholder = entries.front();

            SUBCASE("control: the bundle really carries the sentinel")
            {
                CHECK(placeholder.name == "placeholder");
                CHECK(
                    placeholder.source.find(k_frameworkSentinel)
                    != std::string_view::npos
                );
            }

            SUBCASE("control: the sentinel is reachable inside a framework environment")
            {
                // The real bundle source, loaded under a framework environment by
                // the same loader a task VM uses. Without this the case below
                // would also pass on a VM where the framework never ran.
                auto const found = script::testing::runInEnvironment(
                    placeholder.source,
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
                        .frameworkModules  = frameworkScriptModules(),
                        .installHostTables = surface->installer(),
                        .projectGlobals    = CapabilitySurface::projectGlobals(),
                    }
                );
                REQUIRE(engine.has_value());

                auto const found = engine->runNumber(sentinelScan(), "task-env-scan");
                REQUIRE(found.has_value());
                CHECK(*found == doctest::Approx(0.0));

                // The module name the loader binds in the framework environment
                // is not a project global either, named rather than searched for.
                CHECK(truthy(*engine, "placeholder == nil") == doctest::Approx(1.0));
            }
        }
    }
}
