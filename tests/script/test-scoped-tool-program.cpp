#include <script/scoped-tool-program.hpp>

#include <script/engine.hpp>
#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::script
{
    namespace
    {
        constexpr auto k_entryPoints = std::array{std::string_view{"derive"}};

        constexpr auto k_toolsFacade = std::string_view{R"LUAU(
local native = ...
return {
    call = function(name, arguments)
        return native.invoke(name, arguments)
    end,
    raw = function(...)
        return native.invoke(...)
    end,
}
)LUAU"};

        constexpr auto k_passthroughFacade = std::string_view{R"LUAU(
local _native = ...
return {}
)LUAU"};

        [[nodiscard]]
        auto parsed(std::string_view text) -> json::Value
        {
            auto value = json::parse(text);
            REQUIRE(value.has_value());
            return *std::move(value);
        }

        [[nodiscard]]
        auto digestOf(std::string_view text) -> std::string
        {
            auto const hash = sha256(std::as_bytes(std::span{text}));
            REQUIRE(hash.has_value());
            return hash->hex();
        }

        [[nodiscard]]
        auto runRequest(std::stop_token cancellation = {}) -> ScopedRunRequest
        {
            auto identity = sha256(std::as_bytes(std::span{"leaf-call"}));
            REQUIRE(identity.has_value());
            return ScopedRunRequest{
                .callIdentity = *identity,
                .budgetOwner  = "fixture.project.leaf",
                .maximumElapsedMillis = 5'000U,
                .cancellation         = cancellation,
            };
        }

        [[nodiscard]]
        auto frameworkModules() -> std::vector<FrameworkModule>
        {
            return {
                FrameworkModule{
                    .name   = "@umbraflow/audit",
                    .source = k_passthroughFacade,
                },
                FrameworkModule{
                    .name   = "@umbraflow/screen",
                    .source = k_passthroughFacade,
                },
                FrameworkModule{
                    .name   = "@umbraflow/tools",
                    .source = k_toolsFacade,
                },
                FrameworkModule{
                    .name   = "@umbraflow/workflow",
                    .source = k_passthroughFacade,
                },
            };
        }

        [[nodiscard]]
        auto pluginSource(
            std::string_view pluginId,
            std::string_view prologue,
            std::string_view body
        ) -> std::string
        {
            return std::string{prologue} + "\nreturn {\n    plugin_id = \""
                 + std::string{pluginId} + "\",\n    derive = function(input)\n"
                 + std::string{body} + "\n    end,\n}\n";
        }

        [[nodiscard]]
        auto compileScoped(
            std::string_view pluginId,
            std::string_view source,
            std::vector<FrameworkModule> const& modules,
            std::vector<PureDataProgram::Resource> resources          = {},
            std::vector<PureDataProgram::Resource> frameworkResources = {}
        ) -> Result<ScopedToolProgram>
        {
            return ScopedToolProgram::compile(
                pluginId,
                "main",
                {PureDataProgram::Module{
                    .name   = "main",
                    .source = std::string{source},
                }},
                k_entryPoints,
                std::move(resources),
                modules,
                std::move(frameworkResources)
            );
        }

        [[nodiscard]]
        auto scopedProgram(std::string_view pluginId, std::string_view source)
            -> ScopedToolProgram
        {
            auto const modules = frameworkModules();
            auto program       = compileScoped(pluginId, source, modules);
            REQUIRE(program.has_value());
            return *std::move(program);
        }

        [[nodiscard]]
        auto jsonResource(std::string name, std::string bytes)
            -> PureDataProgram::Resource
        {
            return PureDataProgram::Resource{
                .kind  = PureDataProgram::ResourceKind::Json,
                .name  = std::move(name),
                .bytes = std::move(bytes),
            };
        }
    }

    TEST_CASE("a Project Tool handler that issues a Tool call is refused by name")
    {
        auto const program = scopedProgram(
            "fixture.project.leaf",
            pluginSource(
                "fixture.project.leaf",
                "",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        local caught = pcall(function()\n"
                "            return tools.call(\"framework.screen.observe\", input)\n"
                "        end)\n"
                "        return { caught = caught }"
            )
        );
        auto const answer = program.invoke("derive", json::Value{}, runRequest());
        REQUIRE_FALSE(answer.has_value());
        auto const message = std::string{answer.error().message()};
        CHECK_MESSAGE(
            message.contains(
                "Project Tool handler fixture.project.leaf may not issue Tool call "
                "framework.screen.observe"
            ),
            "Project Tool handler fixture.project.leaf must refuse Tool call "
            "framework.screen.observe by name"
        );
    }

    TEST_CASE("a Tool call while the Project closure is admitted is refused")
    {
        auto const modules = frameworkModules();
        auto const program = compileScoped(
            "fixture.project.admission",
            pluginSource(
                "fixture.project.admission",
                "local tools = require(\"@umbraflow/tools\")\n"
                "tools.call(\"framework.screen.observe\", {})",
                "        return input"
            ),
            modules
        );
        REQUIRE_FALSE(program.has_value());
        CHECK_MESSAGE(
            std::string{program.error().message()}.contains(
                "may not call a Tool while it is admitted: "
                "framework.screen.observe"
            ),
            "Project closure admission must raise a named Tool-call refusal"
        );
    }

    TEST_CASE("malformed Tool primitive calls stay catchable inside a leaf")
    {
        auto const program = scopedProgram(
            "fixture.project.malformed",
            pluginSource(
                "fixture.project.malformed",
                "",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        local arity, arityError = pcall(function()\n"
                "            return tools.raw(\"a.one\")\n"
                "        end)\n"
                "        local named, namedError = pcall(function()\n"
                "            return tools.call(\"NotCanonical\", {})\n"
                "        end)\n"
                "        return { arity = arity, arityError = tostring(arityError),\n"
                "            named = named, namedError = tostring(namedError) }"
            )
        );
        auto const answer = program.invoke("derive", json::Value{}, runRequest());
        REQUIRE(answer.has_value());
        auto const bytes = json::canonicalBytes(*answer);
        CHECK(bytes.contains(R"("arity":false)"));
        CHECK(bytes.contains("takes a tool name and one argument value"));
        CHECK(bytes.contains(R"("named":false)"));
        CHECK(bytes.contains("rejected a non-canonical tool name"));
    }

    TEST_CASE("the pure resolver refuses each scoped module by name")
    {
        constexpr auto entries = std::array{std::string_view{"reduce"}};
        for (auto const scopedName : ScopedToolProgram::scopedModuleNames())
        {
            auto program = PureDataProgram::compile(
                "fixture.pure",
                "main",
                {PureDataProgram::Module{
                    .name = "main",
                    .source = "local _ = require(\"" + std::string{scopedName}
                        + "\")\nreturn { plugin_id = \"fixture.pure\", "
                          "reduce = function(input) return input end }",
                }},
                entries,
                {},
                {}
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.contains(
                "rejected an unknown module: " + std::string{scopedName}
            ));
        }
    }

    TEST_CASE("a scoped program admits exactly its scoped module catalog")
    {
        constexpr auto expected = std::array{
            std::string_view{"@umbraflow/audit"},
            std::string_view{"@umbraflow/screen"},
            std::string_view{"@umbraflow/tools"},
            std::string_view{"@umbraflow/workflow"},
        };
        CHECK(std::ranges::equal(ScopedToolProgram::scopedModuleNames(), expected));

        auto incomplete = frameworkModules();
        incomplete.pop_back();
        auto missing = compileScoped(
            "fixture.project.catalog",
            pluginSource("fixture.project.catalog", "", "        return input"),
            incomplete
        );
        REQUIRE_FALSE(missing.has_value());
        CHECK(std::string{missing.error().message()}.contains(
            "requires the Framework module @umbraflow/workflow"
        ));
    }

    TEST_CASE("the reserved resource namespace holds for Project Tool programs")
    {
        auto const modules = frameworkModules();
        auto const source  = pluginSource(
            "fixture.project.reserved",
            "",
            R"(        return resource.readJson("umbraflow.fixture-catalog"))"
        );
        auto shadowed = compileScoped(
            "fixture.project.reserved",
            source,
            modules,
            {jsonResource("umbraflow.fixture-catalog", R"({"pinned":"project"})")}
        );
        REQUIRE_FALSE(shadowed.has_value());

        auto supplied = compileScoped(
            "fixture.project.reserved",
            source,
            modules,
            {},
            {jsonResource("umbraflow.fixture-catalog", R"({"pinned":"framework"})")}
        );
        REQUIRE(supplied.has_value());
        auto const answer = supplied->invoke("derive", parsed("{}"), runRequest());
        REQUIRE(answer.has_value());
        CHECK(json::canonicalBytes(*answer) == R"({"pinned":"framework"})");
    }

    TEST_CASE("the scoped environment has its own pinned identity")
    {
        auto const material = scopedToolEnvironmentMaterial();
        auto const scoped   = scopedToolEnvironmentHash();
        auto const pure     = pluginEnvironmentHash();
        REQUIRE(scoped.has_value());
        REQUIRE(pure.has_value());
        CHECK(json::canonicalBytes(parsed(material)) == material);
        CHECK(scoped->hex() != pure->hex());
        CHECK(scoped->hex() == digestOf(material));

        // A published identity needs a literal to compare against. Comparing
        // the hash to a digest of the same material only proves the hash
        // function ran; it is the literal below that turns an unannounced
        // change to the environment into a red test, and so into a release
        // note. Changing it is the point at which the change becomes a break.
        CHECK(
            scoped->hex()
            == "30d312235ad2e7471cb7946e5d8e34502fa61807a4c3e79000abbff8f9026273"
        );
        CHECK(material.contains(R"("interactive_tool_calls":1024)"));
        CHECK(material.contains(
            R"("failure_behaviour":"runtime_refusal_is_terminal_uncatchable_vm_teardown_v1")"
        ));
    }
}
