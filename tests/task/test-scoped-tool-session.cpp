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
            R"([{"description":"Deliver one click","input_schema":{},)"
            R"("name":"framework.input.click","tool_version":"1"},)"
            R"({"description":"Capture one screenshot","input_schema":{},)"
            R"("name":"framework.screen.capture","tool_version":"1"},)"
            R"({"description":"Observe one retained screenshot","input_schema":{},)"
            R"("name":"framework.screen.observe","tool_version":"1"},)"
            R"({"description":"Read lines from one retained screenshot","input_schema":{},)"
            R"("name":"framework.screen.read_lines","tool_version":"1"},)"
            R"({"description":"Let time pass","input_schema":{},)"
            R"("name":"framework.workflow.wait","tool_version":"1"}])"
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
            uint64 memoryQuotaBytes = 16U * 1024U * 1024U,
            script::ToolRuntimeInvoke runtime = {}
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
            if (!runtime)
            {
                runtime = [calls = std::move(calls)](
                    std::string_view name,
                    json::Value const&
                ) -> Result<json::Value>
                {
                    calls->emplace_back(name);
                    return json::Value::ofObject({
                        {"call_identity", json::Value::ofString(digestOf(name).hex())},
                        {"delivery", json::Value::ofString("confirmed")},
                        {"ok", json::Value::ofBoolean(true)},
                        {"result", json::Value::ofObject({})},
                    });
                };
            }
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

    // The loop this architecture exists to run, asserted end to end.
    //
    // It was covered before the flat-call cut by a case written on the nested
    // observation body, and that case could not survive the body's deletion. It
    // is restored here in the shape the ruling gives it: a screenshot is a
    // value, so the loop CAPTURES first and every later call names that digest.
    // Nothing about the sequence is implicit any more -- no open frame, no
    // callback, no scope -- so the only thing that can hold the steps together
    // is the caller, which is exactly what this asserts.
    TEST_CASE("a flat automation loop captures observes acts waits and terminates")
    {
        auto calls        = std::make_shared<std::vector<std::string>>();
        auto observations = std::make_shared<uint32>();

        auto runtime = [calls, observations](
                           std::string_view tool,
                           json::Value const& arguments
                       ) -> Result<json::Value>
        {
            calls->emplace_back(tool);

            auto result = json::Value::ofObject({});
            if (tool == "framework.screen.capture")
            {
                result = json::Value::ofObject({
                    {"screenshot_sha256",
                     json::Value::ofString(digestOf("flat-loop-frame").hex())},
                });
            }
            else if (tool == "framework.screen.observe")
            {
                // Every later call must name the digest capture answered with.
                // A loop that observed "the current screen" instead would pass
                // this test while measuring something nobody captured.
                auto const* const named = arguments.find("screenshot_sha256");
                REQUIRE(named != nullptr);
                CHECK_MESSAGE(
                    named->string() == digestOf("flat-loop-frame").hex(),
                    "observe must name the screenshot capture answered with"
                );
                ++*observations;
                result = json::Value::ofObject({
                    {"terminal", json::Value::ofBoolean(*observations == 2U)},
                });
            }

            return json::Value::ofObject({
                {"call_identity",
                 json::Value::ofString(digestOf(
                     std::string{tool} + "#" + std::to_string(calls->size())
                 ).hex())},
                {"delivery", json::Value::ofString("confirmed")},
                {"ok", json::Value::ofBoolean(true)},
                {"result", std::move(result)},
            });
        };

        auto scoped = session({}, std::chrono::seconds{5}, 16U * 1024U * 1024U,
                              std::move(runtime));
        REQUIRE(scoped.has_value());

        auto const outcome = scoped->evaluate(
            R"LUAU(
                local tools = require("@umbraflow/tools")

                local function call(name, args)
                    local answer = tools.call(name, args)
                    if rawget(answer, "ok") ~= true then
                        error(name .. " did not confirm")
                    end
                    return rawget(answer, "result")
                end

                for _ = 1, 3 do
                    local shot = call("framework.screen.capture", canon.emptyObject)
                    local digest = rawget(shot, "screenshot_sha256")
                    local seen = call("framework.screen.observe", {
                        screenshot_sha256 = digest,
                    })
                    if rawget(seen, "terminal") == true then
                        return "terminated"
                    end
                    call("framework.input.click", {
                        screenshot_sha256 = digest, x = 1, y = 0,
                    })
                    call("framework.workflow.wait", { duration_ms = 5 })
                end
                return "exhausted"
            )LUAU",
            "flat-automation-loop"
        );

        auto const why = outcome.has_value()
            ? std::string{}
            : std::string{outcome.error().message()};
        REQUIRE_MESSAGE(outcome.has_value(), why);
        REQUIRE(outcome->text() != nullptr);
        CHECK_MESSAGE(
            *outcome->text() == "terminated",
            "the loop must end because an observation said so, not by exhaustion"
        );

        auto const expected = std::vector<std::string>{
            "framework.screen.capture",
            "framework.screen.observe",
            "framework.input.click",
            "framework.workflow.wait",
            "framework.screen.capture",
            "framework.screen.observe",
        };
        CHECK_MESSAGE(
            *calls == expected,
            "capture, observe, act, wait, then capture and observe again"
        );
    }

    TEST_CASE("interactive chunks resolve the registered-handler scoped modules")
    {
        auto calls  = std::make_shared<std::vector<std::string>>();
        auto scoped = session(calls);
        REQUIRE(scoped.has_value());

        auto result = scoped->evaluate(
            R"LUAU(
                local screen = require("@umbraflow/screen")
                local tools = require("@umbraflow/tools")
                local hash = string.rep("0", 64)
                local answer = screen.observe(hash)
                local measured = true
                tools.call("framework.screen.read_lines", {
                    screenshot_sha256 = hash,
                    x = 0,
                    y = 0,
                    width = 1,
                    height = 1,
                })
                return answer.delivery .. ":" .. tostring(answer.ok) .. ":" .. tostring(measured)
            )LUAU",
            "scoped-modules"
        );
        REQUIRE(result.has_value());
        REQUIRE(result->text() != nullptr);
        CHECK(*result->text() == "confirmed:true:true");
        CHECK(
            *calls
            == std::vector<std::string>{
                "framework.screen.observe",
                "framework.screen.read_lines",
            }
        );
    }

    TEST_CASE("screen observation carries a provider failure message verbatim")
    {
        constexpr auto k_providerMessage =
            std::string_view{"the provider could not read this retained frame"};
        auto calls = std::make_shared<std::vector<std::string>>();
        auto scoped = session(
            calls,
            std::chrono::seconds{5},
            16U * 1024U * 1024U,
            [calls, k_providerMessage](
                std::string_view name,
                json::Value const&
            ) -> Result<json::Value>
            {
                calls->emplace_back(name);
                return json::Value::ofObject({
                    {"call_identity", json::Value::ofString(digestOf(name).hex())},
                    {"delivery", json::Value::ofString("terminal_failure")},
                    {"error", json::Value::ofObject({
                        {"code", json::Value::ofString("io_failure")},
                        {"message", json::Value::ofString(std::string{k_providerMessage})},
                        {"retryable", json::Value::ofBoolean(false)},
                    })},
                    {"ok", json::Value::ofBoolean(false)},
                });
            }
        );
        REQUIRE(scoped.has_value());

        auto const result = scoped->evaluate(
            R"LUAU(
                local screen = require("@umbraflow/screen")
                local observed = screen.observation(screen.observe(string.rep("0", 64)))
                return observed.error.message
            )LUAU",
            "provider-message"
        );
        auto const message = result.has_value()
            ? std::string{}
            : std::string{result.error().message()};
        REQUIRE_MESSAGE(result.has_value(), message);
        REQUIRE(result->text() != nullptr);
        CHECK_MESSAGE(
            *result->text() == k_providerMessage,
            "screen.observation must carry the provider failure message verbatim"
        );
    }

    TEST_CASE("a Tool admission or protocol refusal still raises")
    {
        auto calls = std::make_shared<std::vector<std::string>>();
        auto scoped = session(
            calls,
            std::chrono::seconds{5},
            16U * 1024U * 1024U,
            [](std::string_view, json::Value const&) -> Result<json::Value>
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "protocol refusal sentinel"
                );
            }
        );
        REQUIRE(scoped.has_value());

        auto const result = scoped->evaluate(
            R"LUAU(
                local tools = require("@umbraflow/tools")
                local answer = tools.call("framework.screen.observe", {})
                return answer.ok
            )LUAU",
            "protocol-refusal"
        );
        auto const message = result.has_value()
            ? std::string{"returned ok: false instead of raising"}
            : std::string{result.error().message()};
        auto const raisedRefusal = !result.has_value()
            && message.contains("protocol refusal sentinel");
        CHECK_MESSAGE(
            raisedRefusal,
            "Tool admission or protocol refusal must raise instead of returning ok false"
        );
    }

    TEST_CASE("possible remains an explicit delivery value")
    {
        auto calls = std::make_shared<std::vector<std::string>>();
        auto scoped = session(
            calls,
            std::chrono::seconds{5},
            16U * 1024U * 1024U,
            [](std::string_view name, json::Value const&) -> Result<json::Value>
            {
                return json::Value::ofObject({
                    {"call_identity", json::Value::ofString(digestOf(name).hex())},
                    {"delivery", json::Value::ofString("possible")},
                    {"error", json::Value::ofObject({
                        {"code", json::Value::ofString("delivery_unknown")},
                        {"message", json::Value::ofString("the input may have landed")},
                        {"retryable", json::Value::ofBoolean(false)},
                    })},
                    {"ok", json::Value::ofBoolean(false)},
                });
            }
        );
        REQUIRE(scoped.has_value());

        auto const result = scoped->evaluate(
            R"LUAU(
                local tools = require("@umbraflow/tools")
                return tools.call("framework.screen.observe", {}).delivery
            )LUAU",
            "possible-delivery"
        );
        REQUIRE(result.has_value());
        REQUIRE(result->text() != nullptr);
        CHECK_MESSAGE(
            *result->text() == "possible",
            "an input whose landing is unknown must remain delivery possible"
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
