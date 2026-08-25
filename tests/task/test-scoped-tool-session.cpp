#include <task/framework-bundle.hpp>

// Interactive chunks are scoped Tool programs; this suite protects that shared
// environment rather than an exploration-only verb surface.

#include <script/pure-data-program.hpp>
#include <script/scoped-tool-program.hpp>
#include <script/tool-runtime.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        constexpr auto k_catalogTools = std::string_view{
            R"([{"argument_contract":{},"body":true)"
            R"(,"child_effects":{"maximum_child_calls":1})"
            R"(,"name":"framework.screen.observe","tool_version":"1"})"
            R"(,{"argument_contract":{},"body":false)"
            R"(,"child_effects":{"maximum_child_calls":0})"
            R"(,"name":"framework.screen.read_lines","tool_version":"1"}])"
        };

        [[nodiscard]] auto digestOf(std::string_view text) -> ContentHash
        {
            auto digest = sha256(std::as_bytes(std::span{text}));
            REQUIRE(digest.has_value());
            return *digest;
        }

        [[nodiscard]]
        auto catalogResources()
            -> std::vector<script::PureDataProgram::Resource>
        {
            return {
                script::PureDataProgram::Resource{
                    .kind = script::PureDataProgram::ResourceKind::Json,
                    .name = std::string{scopedToolCatalogResourceName()},
                    .bytes = R"({"catalog_hash":")"
                        + digestOf(k_catalogTools).hex() + R"(","tools":)"
                        + std::string{k_catalogTools} + "}",
                },
            };
        }

        [[nodiscard]]
        auto session(
            std::shared_ptr<std::vector<std::string>> calls,
            MonotonicInstant::Duration maximumRuntime = std::chrono::seconds{5},
            uint64 memoryQuotaBytes = 16U * 1024U * 1024U
        )
            -> Result<script::ScopedToolSession>
        {
            auto modules = scopedFrameworkScriptModules();
            REQUIRE(modules.has_value());
            modules->emplace_back(script::FrameworkModule{
                .name = "@umbraflow/session-probe",
                .source = "local count = 0\n"
                          "return table.freeze({ next = function()\n"
                          "    count += 1\n"
                          "    return count\n"
                          "end })\n",
                .dependencyDepth = 0U,
                .projectVisible  = true,
            });
            auto runtime = script::ToolRuntimeInvoke{
                [calls = std::move(calls)](
                    std::string_view name,
                    json::Value const&,
                    script::ToolCallBody body
                ) -> Result<json::Value>
                {
                    calls->emplace_back(name);
                    if (body)
                    {
                        UF_TRY(body(digestOf(name)));
                    }
                    return json::Value::ofObject({
                        {"call_identity", json::Value::ofString(digestOf(name).hex())},
                        {"state", json::Value::ofString("confirmed")},
                        {"tool", json::Value::ofString(std::string{name})},
                    });
                }
            };
            return script::ScopedToolSession::create(
                std::move(*modules),
                catalogResources(),
                std::move(runtime),
                {},
                maximumRuntime,
                memoryQuotaBytes
            );
        }
    } // namespace

    TEST_CASE("interactive chunks resolve the registered-handler scoped modules")
    {
        auto calls  = std::make_shared<std::vector<std::string>>();
        auto scoped = session(calls);
        REQUIRE(scoped.has_value());

        auto result = scoped->evaluate(
            R"LUAU(
                local screen = require("@umbraflow/screen")
                local tools = require("@umbraflow/tools")
                local measured = false
                local answer = screen.observe(function()
                    measured = true
                    tools.call("framework.screen.read_lines", {
                        x = 0,
                        y = 0,
                        width = 1,
                        height = 1,
                    })
                end)
                return tools.state(answer) .. ":" .. tostring(measured)
            )LUAU",
            "scoped-modules"
        );
        REQUIRE(result.has_value());
        REQUIRE(result->text() != nullptr);
        CHECK(*result->text() == "confirmed:true");
        CHECK(
            *calls
            == std::vector<std::string>{
                "framework.screen.observe",
                "framework.screen.read_lines",
            }
        );
    }

    TEST_CASE("interactive chunks share host state and never Framework module state")
    {
        auto calls  = std::make_shared<std::vector<std::string>>();
        auto scoped = session(calls);
        REQUIRE(scoped.has_value());

        auto first = scoped->evaluate(
            "local probe = require(\"@umbraflow/session-probe\")\n"
            "return probe.next()",
            "first"
        );
        REQUIRE(first.has_value());
        auto second = scoped->evaluate(
            "local probe = require(\"@umbraflow/session-probe\")\n"
            "return probe.next()",
            "second"
        );
        REQUIRE(second.has_value());
        CHECK(first->number() == std::optional<double>{1.0});
        CHECK(second->number() == std::optional<double>{1.0});
        CHECK(calls->empty());
    }

    TEST_CASE("interactive chunk failures name the session-owned execution limit")
    {
        SUBCASE("elapsed time")
        {
            auto calls  = std::make_shared<std::vector<std::string>>();
            auto scoped = session(calls, std::chrono::milliseconds{100});
            REQUIRE(scoped.has_value());

            auto result = scoped->evaluate("while true do end", "deadline-owner");
            REQUIRE_FALSE(result.has_value());
            auto const message = std::string_view{result.error().message()};
            CHECK(message.contains("interactive chunk deadline-owner"));
            CHECK(message.contains("session-supplied maximum_elapsed_ms=100"));
            CHECK(scoped->generationSpent());
        }

        SUBCASE("memory")
        {
            constexpr auto k_ceiling = uint64{4U * 1024U * 1024U};
            auto calls  = std::make_shared<std::vector<std::string>>();
            auto scoped = session(calls, std::chrono::seconds{5}, k_ceiling);
            REQUIRE(scoped.has_value());

            auto result = scoped->evaluate(
                "return table.create(1000000, 0)",
                "memory-owner"
            );
            REQUIRE_FALSE(result.has_value());
            auto const message = std::string_view{result.error().message()};
            CHECK(message.contains("interactive chunk memory-owner"));
            CHECK(
                message.contains(
                    "session-supplied Luau memory ceiling of 4194304 bytes"
                )
            );
            CHECK_FALSE(scoped->generationSpent());
        }
    }
}
