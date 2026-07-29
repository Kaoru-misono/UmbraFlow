#include <script/engine.hpp>
#include <script/testing/environment-probe.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <string>
#include <string_view>

// The two-environment model, tested as a mechanism: the denial list a project
// script sees, the impossibility of reaching the framework environment from the
// project one, and the failure a host-table installer can now report.
//
// Every case here is written to be falsifiable. Where a claim is an absence --
// "the project cannot see X" -- the same expression is also run on the framework
// side, which must see X. An absence that both sides report would mean the test
// is asserting nothing.
namespace uf::script
{
    namespace
    {
        // A value that exists nowhere but inside the probe's framework
        // environment. Its only job is to be findable, so the isolation claim is
        // about a concrete value rather than about a name nobody defined.
        constexpr auto k_sentinel = std::string_view{"uf-probe-sentinel-91c4"};

        // A framework module that publishes the sentinel two different ways: as
        // its module exports, which the loader binds in the framework
        // environment under the module name, and as a plain global, which lands
        // in the framework environment because that is what the module's closure
        // writes its globals into. Both are routes the project side must fail to
        // follow, and neither is reachable without the environment itself.
        [[nodiscard]]
        auto frameworkSource() -> std::string
        {
            auto const sentinel = std::string{k_sentinel};
            return "frameworkSentinel = '" + sentinel + "'\n"
                   "return { sentinel = '" + sentinel + "' }\n";
        }

        // A bounded, cycle-safe reachability search for the sentinel over every
        // name a project environment is allowed to hold, plus the two framework
        // names, following table values, table KEYS, and metatables.
        //
        // It is deliberately the same source on both sides. Run under the
        // framework environment the two framework names resolve and it returns
        // 1; run under the project environment every route must dead-end and it
        // returns 0. A single expression that flips answer with the environment
        // is a discriminator; two different expressions would not be.
        [[nodiscard]]
        auto reachabilityScan() -> std::string
        {
            return "local target = '" + std::string{k_sentinel} + "'\n"
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
                    -- the framework's own two publication routes
                    probe, frameworkSentinel,
                    -- the names the denial list removes, in case one came back
                    _G, getfenv, setfenv, newproxy, gcinfo, coroutine, debug,
                    -- everything the project environment whitelist admits
                    _VERSION, assert, error, getmetatable, ipairs, next, pairs,
                    pcall, print, rawequal, rawget, rawlen, rawset, select,
                    setmetatable, tonumber, tostring, type, typeof, unpack,
                    xpcall, bit32, buffer, math, os, string, table, utf8, vector,
                }, 0)

                return found and 1 or 0
            )lua";
        }

        // Run `return (<expr>) and 1 or 0` on a plain sandboxed VM; 1.0 means the
        // expression was truthy under the project environment.
        [[nodiscard]]
        auto projectTruth(Engine& engine, std::string_view expression) -> double
        {
            auto const source = "return (" + std::string{expression} + ") and 1 or 0";
            auto const result = engine.runNumber(source, "project-env-expr");
            REQUIRE(result.has_value());
            return *result;
        }

        TEST_CASE("The project environment holds no name on the denial list")
        {
            auto engine = Engine::create();
            REQUIRE(engine.has_value());

            // The design's denial list in full, not a sample of it. The first
            // group are globals; the second are fields of libraries the project
            // environment does admit, which is why the library itself is
            // asserted present first -- `os.time == nil` would also hold if `os`
            // had silently gone missing, and that would be a different VM than
            // the one under test.
            constexpr std::string_view deniedGlobals[] = {
                "_G",
                "getfenv",
                "setfenv",
                "newproxy",
                "gcinfo",
                "coroutine",
                "debug",
            };
            for (std::string_view const name : deniedGlobals)
            {
                INFO("denied global: ", name);
                CHECK(
                    projectTruth(*engine, std::string{name} + " == nil")
                    == doctest::Approx(1.0)
                );
            }

            CHECK(projectTruth(*engine, "type(os) == 'table'") == doctest::Approx(1.0));
            CHECK(projectTruth(*engine, "type(math) == 'table'") == doctest::Approx(1.0));

            constexpr std::string_view deniedFields[] = {
                "os.time",
                "os.clock",
                "os.date",
                "math.random",
                "math.randomseed",
            };
            for (std::string_view const name : deniedFields)
            {
                INFO("denied field: ", name);
                CHECK(
                    projectTruth(*engine, std::string{name} + " == nil")
                    == doctest::Approx(1.0)
                );
            }
        }

        TEST_CASE("The project environment cannot reach the framework environment")
        {
            using testing::ProbeEnvironment;
            using testing::runInEnvironment;

            auto const framework = frameworkSource();
            auto const scan      = reachabilityScan();

            SUBCASE("control: the framework environment does hold the sentinel")
            {
                // Without this the case below proves nothing: an environment
                // where the framework never ran would pass it trivially.
                auto const found = runInEnvironment(
                    framework,
                    scan,
                    ProbeEnvironment::Framework
                );
                REQUIRE(found.has_value());
                CHECK(*found == doctest::Approx(1.0));
            }

            SUBCASE("no route out of the project environment reaches it")
            {
                auto const found = runInEnvironment(
                    framework,
                    scan,
                    ProbeEnvironment::Project
                );
                REQUIRE(found.has_value());
                CHECK(*found == doctest::Approx(0.0));
            }

            SUBCASE("neither framework name is a project global")
            {
                // The two direct routes, named rather than searched for, so a
                // failure says which one opened.
                auto const named = runInEnvironment(
                    framework,
                    "return (probe == nil and frameworkSentinel == nil) and 1 or 0",
                    ProbeEnvironment::Project
                );
                REQUIRE(named.has_value());
                CHECK(*named == doctest::Approx(1.0));
            }

            SUBCASE("control: both framework names resolve on the framework side")
            {
                auto const named = runInEnvironment(
                    framework,
                    "return (probe ~= nil and frameworkSentinel ~= nil) and 1 or 0",
                    ProbeEnvironment::Framework
                );
                REQUIRE(named.has_value());
                CHECK(*named == doctest::Approx(1.0));
            }
        }

        TEST_CASE("A failing host-table installer fails create with its own error")
        {
            using testing::probeInstallerFailure;

            // Two different failures, because a create() that always reported
            // one fixed error would pass a single-case test. The kind and the
            // message must both be the installer's own.
            auto const refused = probeInstallerFailure(
                AutomationErrorKind::InvalidResource,
                "probe installer refused to build its tables"
            );
            REQUIRE(refused.failure.has_value());
            CHECK(
                automationErrorKind(*refused.failure)
                == AutomationErrorKind::InvalidResource
            );
            CHECK(
                refused.failure->message()
                == "probe installer refused to build its tables"
            );

            auto const unsupported = probeInstallerFailure(
                AutomationErrorKind::UnsupportedCapability,
                "probe installer has no such capability"
            );
            REQUIRE(unsupported.failure.has_value());
            CHECK(
                automationErrorKind(*unsupported.failure)
                == AutomationErrorKind::UnsupportedCapability
            );
            CHECK(
                unsupported.failure->message()
                == "probe installer has no such capability"
            );

            // And the VM create() had already allocated is gone. lua_close is
            // the only thing that runs a userdata destructor, so the witness the
            // installer registered reaching exactly one is a direct observation
            // of the close rather than an inference from the returned error.
            CHECK(refused.finalizedHostObjects == 1);
            CHECK(unsupported.finalizedHostObjects == 1);
        }
    }
}
