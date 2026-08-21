#include <script/engine.hpp>
#include <script/testing/environment-probe.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>

// The two-environment model tested as a mechanism. Where a claim is an absence --
// "the project cannot see X" -- the same expression is also run on the framework
// side, which must see X; an absence both sides reported would mean the case is
// asserting nothing.
namespace uf::script
{
    namespace
    {
        // Exists nowhere but inside the probe's framework environment, so the isolation
        // claim is about a concrete value rather than a name nobody defined.
        constexpr auto k_sentinel = std::string_view{"uf-probe-sentinel-91c4"};

        // Publishes the sentinel twice: as module exports, which the loader binds under
        // the module name, and as a plain global, which lands in the framework
        // environment because that is where the module's closure writes globals. Both
        // are routes the project side must fail to follow.
        [[nodiscard]]
        auto frameworkSource() -> std::string
        {
            auto const sentinel = std::string{k_sentinel};
            return "frameworkSentinel = '" + sentinel + "'\n"
                   "return { sentinel = '" + sentinel + "' }\n";
        }

        // A bounded, cycle-safe search for the sentinel over every name a project
        // environment may hold plus the two framework names, following table values,
        // table KEYS, and metatables. Deliberately the same source on both sides: one
        // expression that flips answer with the environment is a discriminator, two
        // different expressions would not be.
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

            // The denial list in full, not a sample. The library itself is asserted
            // present first because `os.time == nil` would also hold if `os` had
            // silently gone missing, which would be a different VM than the one tested.
            constexpr auto deniedGlobals = std::to_array<std::string_view>({
                "_G",
                "getfenv",
                "setfenv",
                "newproxy",
                "gcinfo",
                "coroutine",
                "debug",
            });
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

            constexpr auto deniedFields = std::to_array<std::string_view>({
                "os.time",
                "os.clock",
                "os.date",
                "math.random",
                "math.randomseed",
            });
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
                // Without this, an environment where the framework never ran passes
                // the case below trivially.
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
                // The two direct routes named rather than searched, so a failure says
                // which one opened.
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

        TEST_CASE("Trusted Framework require resolves only exact earlier modules")
        {
            auto engine = Engine::create(
                EngineConfig{
                    .frameworkModules = {
                        FrameworkModule{
                            .name         = "unicodeData",
                            .source       = "return { nested = { value = 41 } }",
                            .resolverName = "@umbraflow/internal/unicode-data",
                        },
                        FrameworkModule{
                            .name = "consumer",
                            .source = R"luau(
local exactRequire = require
local dependency = exactRequire("@umbraflow/internal/unicode-data")
return {
    verify = function()
        local replaced = pcall(function()
            dependency.nested.value = 0
        end)
        return not replaced
            and dependency.nested.value == 41
            and type(exactRequire) == "function"
    end,
}
)luau",
                            .resolverName = "@umbraflow/consumer",
                        },
                    },
                    .frameworkProjectGlobals = {"consumer"},
                }
            );
            REQUIRE(engine.has_value());

            auto const result = engine->runNumber(
                R"luau(
return require == nil
    and unicodeData == nil
    and consumer.verify()
    and 1
    or 0
)luau",
                "trusted-framework-require"
            );
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("Trusted Framework require rejects an open dependency graph")
        {
            SUBCASE("an unknown exact alias is refused")
            {
                auto const engine = Engine::create(
                    EngineConfig{
                        .frameworkModules = {
                            FrameworkModule{
                                .name = "consumer",
                                .source = R"luau(
return require("@umbraflow/missing")
)luau",
                            },
                        },
                    }
                );
                REQUIRE_FALSE(engine.has_value());
                CHECK(engine.error().message().contains("unknown or forward"));
            }

            SUBCASE("a numeric request is refused as the wrong argument type")
            {
                auto const engine = Engine::create(
                    EngineConfig{
                        .frameworkModules = {
                            FrameworkModule{
                                .name   = "consumer",
                                .source = "return require(1)",
                            },
                        },
                    }
                );
                REQUIRE_FALSE(engine.has_value());
                CHECK(engine.error().message().contains("module-name string"));
                CHECK_FALSE(
                    engine.error().message().contains("unknown or forward")
                );
            }

            SUBCASE("a forward dependency is refused")
            {
                auto const engine = Engine::create(
                    EngineConfig{
                        .frameworkModules = {
                            FrameworkModule{
                                .name = "consumer",
                                .source = R"luau(
return require("@umbraflow/dependency")
)luau",
                            },
                            FrameworkModule{
                                .name         = "dependency",
                                .source       = "return {}",
                                .resolverName = "@umbraflow/dependency",
                            },
                        },
                    }
                );
                REQUIRE_FALSE(engine.has_value());
                CHECK(engine.error().message().contains("unknown or forward"));
            }

            SUBCASE("a cycle is refused at its first forward edge")
            {
                auto const engine = Engine::create(
                    EngineConfig{
                        .frameworkModules = {
                            FrameworkModule{
                                .name = "first",
                                .source = R"luau(
return require("@umbraflow/second")
)luau",
                                .resolverName = "@umbraflow/first",
                            },
                            FrameworkModule{
                                .name = "second",
                                .source = R"luau(
return require("@umbraflow/first")
)luau",
                                .resolverName = "@umbraflow/second",
                            },
                        },
                    }
                );
                REQUIRE_FALSE(engine.has_value());
                CHECK(engine.error().message().contains("unknown or forward"));
            }

            SUBCASE("duplicate aliases are rejected before source runs")
            {
                auto const engine = Engine::create(
                    EngineConfig{
                        .frameworkModules = {
                            FrameworkModule{
                                .name         = "first",
                                .source       = "error('must not run')",
                                .resolverName = "@umbraflow/shared",
                            },
                            FrameworkModule{
                                .name         = "second",
                                .source       = "return {}",
                                .resolverName = "@umbraflow/shared",
                            },
                        },
                    }
                );
                REQUIRE_FALSE(engine.has_value());
                CHECK(engine.error().message().contains("duplicate"));
                CHECK_FALSE(engine.error().message().contains("must not run"));
            }

            SUBCASE("a non-canonical alias is refused")
            {
                auto const engine = Engine::create(
                    EngineConfig{
                        .frameworkModules = {
                            FrameworkModule{
                                .name         = "dependency",
                                .source       = "return {}",
                                .resolverName = "dependency",
                            },
                        },
                    }
                );
                REQUIRE_FALSE(engine.has_value());
                CHECK(engine.error().message().contains("non-canonical"));
            }
        }

        TEST_CASE("A framework module cannot capture a dangerous global at load time")
        {
            using testing::ProbeEnvironment;
            using testing::runInEnvironment;

            // The framework environment chains __index to the main globals, so a module
            // that binds a name at LOAD time keeps it for the whole generation whatever
            // the boot removes afterwards. The boot therefore strips the denial list
            // before any Lua runs, the framework included; move the strip back after
            // the framework load and this goes red. The admitted name is asserted too,
            // so a strip that took the whole standard library with it also fails.
            constexpr auto framework = std::string_view{
                "capturedGlobalEnv  = _G\n"
                "capturedGetfenv    = getfenv\n"
                "capturedSetfenv    = setfenv\n"
                "capturedNewproxy   = newproxy\n"
                "capturedGcinfo     = gcinfo\n"
                "capturedCoroutine  = coroutine\n"
                "capturedDebug      = debug\n"
                "capturedOsTime     = os.time\n"
                "capturedOsClock    = os.clock\n"
                "capturedOsDate     = os.date\n"
                "capturedRandom     = math.random\n"
                "capturedRandomseed = math.randomseed\n"
                "capturedPcall      = pcall\n"
                "return {}\n"
            };

            SUBCASE("every dangerous name was already gone when the module ran")
            {
                auto const captured = runInEnvironment(
                    framework,
                    "return ("
                    "capturedGlobalEnv == nil and capturedGetfenv == nil"
                    " and capturedSetfenv == nil and capturedNewproxy == nil"
                    " and capturedGcinfo == nil and capturedCoroutine == nil"
                    " and capturedDebug == nil and capturedOsTime == nil"
                    " and capturedOsClock == nil and capturedOsDate == nil"
                    " and capturedRandom == nil and capturedRandomseed == nil"
                    ") and 1 or 0",
                    ProbeEnvironment::Framework
                );
                REQUIRE(captured.has_value());
                CHECK(*captured == doctest::Approx(1.0));
            }

            SUBCASE("control: an admitted name was capturable at the same moment")
            {
                // Without this the case above would pass on a boot that never ran
                // the module at all, or one whose load-time environment was empty.
                auto const admitted = runInEnvironment(
                    framework,
                    "return (capturedPcall ~= nil and capturedPcall == pcall) and 1 or 0",
                    ProbeEnvironment::Framework
                );
                REQUIRE(admitted.has_value());
                CHECK(*admitted == doctest::Approx(1.0));
            }
        }

        TEST_CASE("A failing host-table installer fails create with its own error")
        {
            using testing::probeInstallerFailure;

            // Two different failures, because a create() reporting one fixed error would
            // pass a single-case test. Kind and message must both be the installer's own.
            auto const refused = probeInstallerFailure(
                AutomationErrorKind::InvalidResource,
                "probe installer refused to build its tables"
            );
            REQUIRE(refused.failure.has_value());
            CHECK(
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
                automationErrorKind(*refused.failure)
                == AutomationErrorKind::InvalidResource
            );
            CHECK(
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
                refused.failure->message()
                == "probe installer refused to build its tables"
            );

            auto const unsupported = probeInstallerFailure(
                AutomationErrorKind::UnsupportedCapability,
                "probe installer has no such capability"
            );
            REQUIRE(unsupported.failure.has_value());
            CHECK(
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
                automationErrorKind(*unsupported.failure)
                == AutomationErrorKind::UnsupportedCapability
            );
            CHECK(
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
                unsupported.failure->message()
                == "probe installer has no such capability"
            );

            // The VM create() had already allocated is gone. lua_close is the only thing
            // that runs a userdata destructor, so the installer's witness reaching
            // exactly one is a direct observation of the close, not an inference.
            CHECK(refused.finalizedHostObjects == 1);
            CHECK(unsupported.finalizedHostObjects == 1);
        }
    }
}
