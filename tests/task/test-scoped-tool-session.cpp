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

#include <array>
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
        auto catalogResources(std::string_view tools = k_catalogTools)
            -> std::vector<script::PureDataProgram::Resource>
        {
            return {
                script::PureDataProgram::Resource{
                    .kind = script::PureDataProgram::ResourceKind::Json,
                    .name = std::string{scopedToolCatalogResourceName()},
                    .bytes = R"({"catalog_hash":")"
                        + digestOf(tools).hex() + R"(","tools":)"
                        + std::string{tools} + "}",
                },
            };
        }

        [[nodiscard]]
        auto session(
            std::shared_ptr<std::vector<std::string>> calls,
            MonotonicInstant::Duration maximumRuntime = std::chrono::seconds{5},
            uint64 memoryQuotaBytes = 16U * 1024U * 1024U,
            script::ToolRuntimeInvoke runtime = {},
            std::string_view catalogTools = k_catalogTools,
            std::vector<script::PureDataProgram::Module> projectModules = {},
            std::vector<script::PureDataProgram::Resource> projectResources = {}
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
                catalogResources(catalogTools),
                std::move(projectModules),
                std::move(projectResources),
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
                // `capture{}` and nothing else. A Tool's top-level arguments are
                // an object by contract, so the seam must hand the empty TABLE
                // across as the empty OBJECT; the empty-object sentinel a caller
                // used to need is gone, and this is where its absence is proved.
                CHECK_MESSAGE(
                    arguments.kind() == json::ValueKind::Object,
                    "an empty argument table must arrive as a JSON object"
                );
                CHECK(arguments.members().empty());
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
                local screen   = require("@umbraflow/screen")
                local input    = require("@umbraflow/input")
                local workflow = require("@umbraflow/workflow")

                for _ = 1, 3 do
                    local shot = screen.capture{}.screenshot_sha256
                    local seen = screen.observe{ screenshot_sha256 = shot }
                    if seen.terminal == true then
                        return "terminated"
                    end
                    input.click{ screenshot_sha256 = shot, x = 1, y = 0 }
                    workflow.wait{ duration_ms = 5 }
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

    TEST_CASE("the face renders the pinned catalog and discovery answers for it")
    {
        auto calls  = std::make_shared<std::vector<std::string>>();
        auto scoped = session(calls);
        REQUIRE(scoped.has_value());

        auto result = scoped->evaluate(
            R"LUAU(
                local screen = require("@umbraflow/screen")
                local catalog = require("@umbraflow/catalog")
                local hash = string.rep("0", 64)
                screen.observe{ screenshot_sha256 = hash }
                -- Nothing in this repository writes the name `read_lines`. It
                -- is callable because the pinned catalog names it, which is the
                -- whole property: no per-Tool source exists anywhere.
                screen.read_lines{
                    screenshot_sha256 = hash, x = 0, y = 0, width = 1, height = 1,
                }
                return #catalog.names()
                    .. ":"
                    .. catalog.describe("framework.screen.capture").description
                    .. ":"
                    .. tostring(screen.probe)
            )LUAU",
            "scoped-modules"
        );
        auto const why = result.has_value()
            ? std::string{}
            : std::string{result.error().message()};
        REQUIRE_MESSAGE(result.has_value(), why);
        REQUIRE(result->text() != nullptr);
        // Five pinned Tools, the host's own description for one of them, and
        // nothing at all for a Tool this run did not pin: the face is the
        // catalog and holds nothing the catalog did not name.
        CHECK(*result->text() == "5:Capture one screenshot:nil");
        CHECK(
            *calls
            == std::vector<std::string>{
                "framework.screen.observe",
                "framework.screen.read_lines",
            }
        );
    }

    // The one catalog shape this spelling cannot render, refused by name when
    // the session is built rather than special-cased.
    //
    // A Tool's namespace becomes its module path, and the framework's own
    // namespace is elided because `@umbraflow/` already is it. A Tool named
    // `framework.<member>` therefore has nothing left to be a module. Note what
    // is NOT refused: one Tool's name being a dotted prefix of another's is
    // fine here, because `a.b` and `a.b.c` are simply two namespaces and two
    // modules.
    TEST_CASE("a Tool with no module segment left is refused by name")
    {
        constexpr auto k_unrenderable = std::string_view{
            R"([{"description":"Capture one screenshot","input_schema":{},)"
            R"("name":"framework.capture","tool_version":"1"}])"
        };
        auto calls  = std::make_shared<std::vector<std::string>>();
        auto scoped = session(
            calls,
            std::chrono::seconds{5},
            16U * 1024U * 1024U,
            {},
            k_unrenderable
        );
        REQUIRE_FALSE(scoped.has_value());
        auto const message = std::string{scoped.error().message()};
        CHECK_MESSAGE(
            message.contains(
                "the pinned Tool catalog names framework.capture, whose "
                "namespace is the framework's own and leaves no module segment "
                "behind"
            ),
            "a Tool the module rule cannot place must be refused by name"
        );
    }

    // Two namespaces, one a dotted prefix of the other, are two modules and
    // neither shadows the other. This is the case a nested single-table face
    // could not have rendered at all.
    TEST_CASE("a Tool namespace nested inside another is its own module")
    {
        constexpr auto k_nested = std::string_view{
            R"([{"description":"Capture one screenshot","input_schema":{},)"
            R"("name":"framework.screen.capture","tool_version":"1"},)"
            R"({"description":"Capture a region","input_schema":{},)"
            R"("name":"framework.screen.region.capture","tool_version":"1"}])"
        };
        auto calls  = std::make_shared<std::vector<std::string>>();
        auto scoped = session(
            calls,
            std::chrono::seconds{5},
            16U * 1024U * 1024U,
            {},
            k_nested
        );
        REQUIRE(scoped.has_value());

        auto const result = scoped->evaluate(
            R"LUAU(
                local screen = require("@umbraflow/screen")
                local region = require("@umbraflow/screen/region")
                screen.capture{}
                region.capture{}
                return "both"
            )LUAU",
            "nested-namespaces"
        );
        auto const why = result.has_value()
            ? std::string{}
            : std::string{result.error().message()};
        REQUIRE_MESSAGE(result.has_value(), why);
        CHECK(
            *calls
            == std::vector<std::string>{
                "framework.screen.capture",
                "framework.screen.region.capture",
            }
        );
    }

    TEST_CASE("a Tool that ran and failed raises its whole envelope")
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

        SUBCASE("a caught failure carries the whole envelope")
        {
            auto const result = scoped->evaluate(
                R"LUAU(
                    local screen = require("@umbraflow/screen")
                    local ok, raised = pcall(
                        screen.observe,
                        { screenshot_sha256 = string.rep("0", 64) }
                    )
                    if ok then
                        return "READ AS SUCCESS"
                    end
                    return raised.delivery
                        .. ":" .. raised.error.code
                        .. ":" .. raised.error.message
                        .. ":" .. #raised.call_identity
                )LUAU",
                "provider-message"
            );
            auto const message = result.has_value()
                ? std::string{}
                : std::string{result.error().message()};
            REQUIRE_MESSAGE(result.has_value(), message);
            REQUIRE(result->text() != nullptr);
            // The whole point of raising rather than answering: a caller cannot
            // read a failed call as a success, and nothing about the failure is
            // discarded on the way -- delivery, the provider's verbatim message
            // and the call identity all survive the raise.
            CHECK_MESSAGE(
                *result->text()
                    == std::string{"terminal_failure:io_failure:"}
                        + std::string{k_providerMessage} + ":64",
                "a failed Tool call must raise its whole envelope and must not "
                "be readable as a success"
            );
        }

        SUBCASE("an uncaught failure reaches the host as the envelope")
        {
            auto const result = scoped->evaluate(
                R"LUAU(
                    local screen = require("@umbraflow/screen")
                    return screen.observe{
                        screenshot_sha256 = string.rep("0", 64),
                    }
                )LUAU",
                "uncaught-provider-message"
            );
            REQUIRE_FALSE(result.has_value());
            auto const message = std::string{result.error().message()};
            // THE RECORDED REASON REACHES THE READER, VERBATIM. Measured
            // 2026-08-25: an SDK wrapper replaced the provider's own sentence
            // with a constant, and three failed observations were read as "the
            // screen has no text on it". A raised envelope that a chunk did not
            // catch must still say which delivery it was and why.
            CHECK_MESSAGE(
                message.contains(k_providerMessage),
                "an uncaught Tool failure must reach the host with the "
                "provider's message verbatim"
            );
            CHECK(message.contains(R"("delivery":"terminal_failure")"));
        }
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
                local screen = require("@umbraflow/screen")
                local ok = pcall(screen.observe, {})
                return tostring(ok)
            )LUAU",
            "protocol-refusal"
        );
        auto const message = result.has_value()
            ? std::string{"a pcall observed the refusal instead of the run dying"}
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
                local screen = require("@umbraflow/screen")
                local ok, raised = pcall(screen.observe, {})
                return tostring(ok) .. ":" .. raised.delivery
            )LUAU",
            "possible-delivery"
        );
        REQUIRE(result.has_value());
        REQUIRE(result->text() != nullptr);
        // Raising does not collapse the three unresolved outcomes into one
        // failure: `possible` stays its own named value on the raised table, so
        // whether to retry an input that may have landed is still the project's
        // judgement and never the framework's.
        CHECK_MESSAGE(
            *result->text() == "false:possible",
            "an input whose landing is unknown must raise and must remain "
            "delivery possible"
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

    // The third thing an interactive chunk may require, and the defect this
    // suite exists to hold shut: the verified Project closure of the generation
    // the session pinned. A Project's own decision logic -- a card-play
    // strategy, a scoring routine -- is Luau in `tool_closure.modules`, and
    // before this it was reachable from a registered handler and unreachable
    // from a chunk, which made one program type two faces.
    TEST_CASE("a chunk requires a declared Project module and calls its pure function")
    {
        auto calls  = std::make_shared<std::vector<std::string>>();
        auto scoped = session(
            calls,
            std::chrono::seconds{5},
            16U * 1024U * 1024U,
            {},
            k_catalogTools,
            {script::PureDataProgram::Module{
                .name   = "strategy/battle",
                .source = "return table.freeze({\n"
                          "    next_card = function(hand)\n"
                          "        return hand[1]\n"
                          "    end,\n"
                          "})\n",
            }}
        );
        REQUIRE(scoped.has_value());

        auto const played = scoped->evaluate(
            "local strategy = require(\"strategy/battle\")\n"
            "return strategy.next_card({ \"ember\", \"frost\" })\n",
            "declared-strategy"
        );
        auto const playedWhy = played.has_value()
            ? std::string{}
            : std::string{played.error().message()};
        REQUIRE_MESSAGE(played.has_value(), playedWhy);
        REQUIRE(played->text() != nullptr);
        CHECK_MESSAGE(
            *played->text() == "ember",
            "a chunk must resolve the session's declared Project module under "
            "its registered name and run it"
        );
        CHECK(calls->empty());
    }

    // Capability is a property of the ISSUING RUN and never of the module, and
    // this is the case that says so with one set of bytes: the identical module
    // reaches the Tool Runtime from a chunk and is refused by name from a
    // handler. It is why admitting the Project closure into a session needs no
    // new mechanism to keep handlers leaves -- ScopedToolRun's callTool refuses
    // every name, whatever required it.
    TEST_CASE(
        "one declared Project module calls a Tool from a chunk and is refused "
        "from a handler"
    )
    {
        auto const strategy = script::PureDataProgram::Module{
            .name   = "strategy/scout",
            .source = "local screen = require(\"@umbraflow/screen\")\n"
                      "return table.freeze({\n"
                      "    look = function()\n"
                      "        return screen.capture{}\n"
                      "    end,\n"
                      "})\n",
        };

        auto calls  = std::make_shared<std::vector<std::string>>();
        auto scoped = session(
            calls,
            std::chrono::seconds{5},
            16U * 1024U * 1024U,
            {},
            k_catalogTools,
            {strategy}
        );
        REQUIRE(scoped.has_value());
        auto const fromChunk = scoped->evaluate(
            "local strategy = require(\"strategy/scout\")\n"
            "strategy.look()\n"
            "return \"looked\"\n",
            "chunk-through-strategy"
        );
        auto const chunkWhy = fromChunk.has_value()
            ? std::string{}
            : std::string{fromChunk.error().message()};
        REQUIRE_MESSAGE(fromChunk.has_value(), chunkWhy);
        CHECK_MESSAGE(
            *calls == std::vector<std::string>{"framework.screen.capture"},
            "a Project module required by a chunk must reach the session's "
            "root issuing door"
        );

        auto modules = scopedFrameworkScriptModules();
        REQUIRE(modules.has_value());
        constexpr auto k_handlerEntries = std::array{std::string_view{"play"}};
        auto handler = script::ScopedToolProgram::compile(
            "fixture.strategy",
            "main",
            {
                strategy,
                script::PureDataProgram::Module{
                    .name   = "main",
                    .source = "local strategy = require(\"strategy/scout\")\n"
                              "return {\n"
                              "    plugin_id = \"fixture.strategy\",\n"
                              "    play = function(_)\n"
                              "        return strategy.look()\n"
                              "    end,\n"
                              "}\n",
                },
            },
            k_handlerEntries,
            {},
            *modules,
            catalogResources()
        );
        REQUIRE(handler.has_value());
        auto const fromHandler = handler->invoke(
            "play",
            json::Value{},
            script::ScopedRunRequest{
                .callIdentity = digestOf("strategy-leaf"),
                .budgetOwner  = "fixture.strategy.play",
                .maximumElapsedMillis = 5'000U,
            }
        );
        REQUIRE_FALSE(fromHandler.has_value());
        CHECK_MESSAGE(
            std::string{fromHandler.error().message()}.contains(
                "Project Tool handler fixture.strategy.play may not issue Tool "
                "call framework.screen.capture"
            ),
            "the same Project module required by a handler must be refused by "
            "name at the leaf"
        );
    }

    TEST_CASE(
        "a declared Project module named as the session root is refused by name"
    )
    {
        auto const scoped = session(
            std::make_shared<std::vector<std::string>>(),
            std::chrono::seconds{5},
            16U * 1024U * 1024U,
            {},
            k_catalogTools,
            {script::PureDataProgram::Module{
                .name   = "chunk",
                .source = "return table.freeze({})\n",
            }}
        );
        REQUIRE_FALSE(scoped.has_value());
        CHECK_MESSAGE(
            std::string{scoped.error().message()}.contains(
                "a Project closure module may not be named chunk: that name is "
                "the interactive session's own root module"
            ),
            "a Project module colliding with the session's root must be "
            "refused at create() and must name the collision"
        );
    }
}
