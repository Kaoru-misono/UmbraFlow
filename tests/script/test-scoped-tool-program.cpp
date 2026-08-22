#include <script/scoped-tool-program.hpp>

#include <script/engine.hpp>
#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The scoped program type: the same closed graph, the same fresh quota-bound VM
// and the same deep-freeze discipline as the pure one, plus exactly one native
// seam. What is proved here is what a reader cannot see from inside a VM -- that
// the pure resolver still carries no scoped name, that only the catalog's
// Framework modules are handed the capability table, that the call ordinals come
// from the seam alone, and that a Tool Runtime refusal is teardown rather than a
// value a script can catch.
namespace uf::script
{
    namespace
    {
        constexpr auto k_entryPoints = std::array{std::string_view{"derive"}};

        // Test-local Luau facades over the one native primitive. The real ones
        // live in the Framework bundle; these carry only what these cases drive.
        constexpr auto k_toolsFacade = std::string_view{R"LUAU(
local native = ...
local invoke = native.invoke
return {
    call = function(name, arguments)
        return invoke(name, arguments)
    end,
    raw = function(...)
        return invoke(...)
    end,
}
)LUAU"};

        constexpr auto k_screenFacade = std::string_view{R"LUAU(
local native = ...
return {
    observe = function(request)
        return native.invoke("framework.screen.observe", request)
    end,
}
)LUAU"};

        constexpr auto k_workflowFacade = std::string_view{R"LUAU(
local native = ...
return {
    wait = function(request)
        return native.invoke("framework.workflow.wait", request)
    end,
}
)LUAU"};

        constexpr auto k_auditFacade = std::string_view{R"LUAU(
local native = ...
return {
    record = function(record)
        return native.invoke("framework.audit.record", record)
    end,
}
)LUAU"};

        struct ToolCallRecord final
        {
            std::string toolName{};
            std::string arguments{};
            uint64      parentPosition{};
            uint64      childIndex{0};
        };

        // The coordinate a root run is started under. It stands for the
        // position of the root-positioned call the run implements: a run is
        // never anchored on nothing, so a case that wants a root run names one
        // rather than leaving the request's parent unstated.
        constexpr auto k_rootRunPosition = uint64{1};

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
        auto jsonResource(std::string name, std::string bytes)
            -> PureDataProgram::Resource
        {
            return PureDataProgram::Resource{
                .kind  = PureDataProgram::ResourceKind::Json,
                .name  = std::move(name),
                .bytes = std::move(bytes),
            };
        }

        [[nodiscard]]
        auto scopedFrameworkModules() -> std::vector<FrameworkModule>
        {
            return {
                FrameworkModule{.name = "@umbraflow/audit", .source = k_auditFacade},
                FrameworkModule{.name = "@umbraflow/screen", .source = k_screenFacade},
                FrameworkModule{.name = "@umbraflow/tools", .source = k_toolsFacade},
                FrameworkModule{
                    .name   = "@umbraflow/workflow",
                    .source = k_workflowFacade,
                },
            };
        }

        [[nodiscard]]
        auto pluginSource(
            std::string_view pluginId,
            std::string_view prologue,
            std::string_view deriveBody
        ) -> std::string
        {
            return std::string{prologue} + "\nreturn {\n    plugin_id = \""
                 + std::string{pluginId} + "\",\n    derive = function(input)\n"
                 + std::string{deriveBody} + "\n    end,\n}\n";
        }

        // Answers every call with the coordinate the seam assigned, so a script
        // that returns the answer proves what the seam handed the runtime.
        [[nodiscard]]
        auto recordingRuntime(
            std::shared_ptr<std::vector<ToolCallRecord>> log
        ) -> ToolRuntimeInvoke
        {
            return [log = std::move(log)](
                       std::string_view toolName,
                       json::Value const& arguments,
                       ToolCallCoordinate const& coordinate,
                       std::stop_token
                   ) -> Result<json::Value> {
                log->emplace_back(ToolCallRecord{
                    .toolName       = std::string{toolName},
                    .arguments      = json::canonicalBytes(arguments),
                    .parentPosition = coordinate.parentPosition,
                    .childIndex     = coordinate.childIndex,
                });
                return json::Value::ofObject({
                    json::Member{"arguments", arguments},
                    json::Member{
                        "child",
                        json::Value::ofNumber(static_cast<double>(coordinate.childIndex)),
                    },
                    json::Member{"tool", json::Value::ofString(std::string{toolName})},
                });
            };
        }

        // Refuses once the issuing context reaches `refuseAt`, which is how a
        // replay divergence or a spent budget reaches a run.
        [[nodiscard]]
        auto runtimeRefusingAt(uint64 refuseAt) -> ToolRuntimeInvoke
        {
            return [refuseAt](
                       std::string_view,
                       json::Value const&,
                       ToolCallCoordinate const& coordinate,
                       std::stop_token
                   ) -> Result<json::Value> {
                if (coordinate.childIndex >= refuseAt)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "the recorded call at this coordinate names another tool"
                    );
                }
                return json::Value::ofBoolean(true);
            };
        }

        [[nodiscard]]
        auto compileScoped(
            std::string_view pluginId,
            std::string_view source,
            ToolRuntimeInvoke invokeTool,
            std::vector<FrameworkModule> const& frameworkModules,
            std::vector<PureDataProgram::Resource> resources          = {},
            std::vector<PureDataProgram::Resource> frameworkResources = {}
        ) -> Result<ScopedToolProgram>
        {
            auto modules = std::vector<PureDataProgram::Module>{
                PureDataProgram::Module{.name = "main", .source = std::string{source}},
            };
            return ScopedToolProgram::compile(
                pluginId,
                "main",
                std::move(modules),
                k_entryPoints,
                std::move(resources),
                frameworkModules,
                std::move(frameworkResources),
                std::move(invokeTool)
            );
        }

        [[nodiscard]]
        auto scopedProgram(
            std::string_view pluginId,
            std::string_view source,
            ToolRuntimeInvoke invokeTool
        ) -> ScopedToolProgram
        {
            auto const modules = scopedFrameworkModules();
            auto program = compileScoped(pluginId, source, std::move(invokeTool), modules);
            REQUIRE(program.has_value());
            return *std::move(program);
        }
    } // namespace

    TEST_CASE("a scoped program reaches the Tool Runtime through one native primitive")
    {
        auto const log     = std::make_shared<std::vector<ToolCallRecord>>();
        auto const program = scopedProgram(
            "fixture.scoped.reach",
            pluginSource(
                "fixture.scoped.reach",
                "",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        return tools.call(\"framework.audit.record\", input)"
            ),
            recordingRuntime(log)
        );

        auto const answer = program.invoke(
            "derive",
            parsed(R"({"note":"kept"})"),
            ScopedRunRequest{.parentPosition = k_rootRunPosition}
        );
        REQUIRE(answer.has_value());
        CHECK(
            json::canonicalBytes(*answer)
            == R"({"arguments":{"note":"kept"},"child":1,"tool":"framework.audit.record"})"
        );

        REQUIRE(log->size() == 1U);
        CHECK((*log)[0].toolName == "framework.audit.record");
        CHECK((*log)[0].arguments == R"({"note":"kept"})");
        CHECK((*log)[0].parentPosition == k_rootRunPosition);
        CHECK((*log)[0].childIndex == 1U);

        auto const unregistered =
            program.invoke(
            "reduce",
            json::Value{},
            ScopedRunRequest{.parentPosition = k_rootRunPosition}
        );
        REQUIRE_FALSE(unregistered.has_value());
        CHECK(
            std::string{unregistered.error().message()}
                .find("scoped tool entry point is not registered")
            != std::string::npos
        );
        CHECK(log->size() == 1U);
    }

    TEST_CASE("the seam numbers each issuing context and script cannot influence it")
    {
        auto const log     = std::make_shared<std::vector<ToolCallRecord>>();
        auto const program = scopedProgram(
            "fixture.scoped.ordinals",
            pluginSource(
                "fixture.scoped.ordinals",
                "",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        tools.call(\"a.one\", input)\n"
                "        tools.call(\"a.two\", input)\n"
                "        return tools.call(\"a.three\", input)"
            ),
            recordingRuntime(log)
        );

        auto const root = program.invoke(
            "derive",
            json::Value{},
            ScopedRunRequest{.parentPosition = k_rootRunPosition}
        );
        REQUIRE(root.has_value());

        // A second invoke is a second issuing context, so its numbering starts
        // again at 1 under its own parent rather than continuing the first.
        auto const child = program.invoke(
            "derive",
            json::Value{},
            ScopedRunRequest{.parentPosition = uint64{7}}
        );
        REQUIRE(child.has_value());

        REQUIRE(log->size() == 6U);
        auto const expected = std::array{
            std::string_view{"a.one"},
            std::string_view{"a.two"},
            std::string_view{"a.three"},
        };
        for (auto index = std::size_t{0}; index < 3U; ++index)
        {
            CHECK((*log)[index].toolName == expected[index]);
            CHECK((*log)[index].childIndex == index + 1U);
            CHECK((*log)[index].parentPosition == k_rootRunPosition);

            CHECK((*log)[index + 3U].toolName == expected[index]);
            CHECK((*log)[index + 3U].childIndex == index + 1U);
            CHECK((*log)[index + 3U].parentPosition == 7U);
        }
    }

    TEST_CASE("a Tool Runtime refusal tears the run down past every pcall")
    {
        auto const program = scopedProgram(
            "fixture.scoped.refusal",
            pluginSource(
                "fixture.scoped.refusal",
                "",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        tools.call(\"a.one\", input)\n"
                "        local caught = pcall(function()\n"
                "            return tools.call(\"a.two\", input)\n"
                "        end)\n"
                "        return { caught = caught }"
            ),
            runtimeRefusingAt(2U)
        );

        auto const answer = program.invoke(
            "derive",
            json::Value{},
            ScopedRunRequest{.parentPosition = k_rootRunPosition}
        );
        REQUIRE_FALSE(answer.has_value());
        CHECK(
            std::string{answer.error().message()}
                .find("the recorded call at this coordinate names another tool")
            != std::string::npos
        );
    }

    TEST_CASE("the pure resolver refuses each scoped module by name")
    {
        for (auto const scopedName : ScopedToolProgram::scopedModuleNames())
        {
            auto modules = std::vector<PureDataProgram::Module>{
                PureDataProgram::Module{
                    .name   = "main",
                    .source = pluginSource(
                        "fixture.reducer",
                        "",
                        "        return require(\"" + std::string{scopedName} + "\")"
                    ),
                },
            };
            auto const program = PureDataProgram::compile(
                "fixture.reducer",
                "main",
                std::move(modules),
                k_entryPoints,
                {}
            );
            REQUIRE(program.has_value());

            auto const answer = program->invoke("derive", json::Value{});
            REQUIRE_FALSE(answer.has_value());
            CHECK(
                std::string{answer.error().message()}.find(
                    "rejected an unknown module: " + std::string{scopedName}
                )
                != std::string::npos
            );
        }
    }

    TEST_CASE("a scoped program admits exactly its own scoped module catalog")
    {
        constexpr auto expected = std::array{
            std::string_view{"@umbraflow/audit"},
            std::string_view{"@umbraflow/screen"},
            std::string_view{"@umbraflow/tools"},
            std::string_view{"@umbraflow/workflow"},
        };
        auto const published = ScopedToolProgram::scopedModuleNames();
        REQUIRE(published.size() == expected.size());
        for (auto index = std::size_t{0}; index < expected.size(); ++index)
        {
            CHECK(published[index] == expected[index]);
        }

        auto const source = pluginSource("fixture.scoped.catalog", "", "        return input");

        auto incomplete = scopedFrameworkModules();
        incomplete.pop_back();
        auto const missing = compileScoped(
            "fixture.scoped.catalog",
            source,
            recordingRuntime(std::make_shared<std::vector<ToolCallRecord>>()),
            incomplete
        );
        REQUIRE_FALSE(missing.has_value());
        CHECK(
            std::string{missing.error().message()}
                .find("requires the Framework module @umbraflow/workflow")
            != std::string::npos
        );

        auto hidden = scopedFrameworkModules();
        hidden.front().projectVisible = false;
        auto const invisible = compileScoped(
            "fixture.scoped.catalog",
            source,
            recordingRuntime(std::make_shared<std::vector<ToolCallRecord>>()),
            hidden
        );
        REQUIRE_FALSE(invisible.has_value());
        CHECK(
            std::string{invisible.error().message()}
                .find("project-visible Framework module: @umbraflow/audit")
            != std::string::npos
        );

        auto const complete = scopedFrameworkModules();
        auto const seamless =
            compileScoped("fixture.scoped.catalog", source, ToolRuntimeInvoke{}, complete);
        REQUIRE_FALSE(seamless.has_value());
        CHECK(
            std::string{seamless.error().message()}.find("requires a Tool Runtime")
            != std::string::npos
        );
    }

    TEST_CASE("only the catalog's Framework modules are handed the capability table")
    {
        auto const log     = std::make_shared<std::vector<ToolCallRecord>>();
        auto const program = scopedProgram(
            "fixture.scoped.chunk",
            pluginSource(
                "fixture.scoped.chunk",
                "local chunkArguments = select(\"#\", ...)",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        local wrote = pcall(function() tools.call = nil end)\n"
                "        return {\n"
                "            chunkArguments = chunkArguments,\n"
                "            frozen = wrote == false,\n"
                "        }"
            ),
            recordingRuntime(log)
        );

        auto const answer = program.invoke(
            "derive",
            json::Value{},
            ScopedRunRequest{.parentPosition = k_rootRunPosition}
        );
        REQUIRE(answer.has_value());
        CHECK(json::canonicalBytes(*answer) == R"({"chunkArguments":0,"frozen":true})");
        CHECK(log->empty());
    }

    TEST_CASE("a Tool call while the closure is admitted is refused")
    {
        auto const modules = scopedFrameworkModules();
        auto const program = compileScoped(
            "fixture.scoped.admission",
            pluginSource(
                "fixture.scoped.admission",
                "local tools = require(\"@umbraflow/tools\")\n"
                "tools.call(\"a.one\", {})",
                "        return input"
            ),
            recordingRuntime(std::make_shared<std::vector<ToolCallRecord>>()),
            modules
        );
        REQUIRE_FALSE(program.has_value());
        CHECK(
            std::string{program.error().message()}
                .find("may not call a Tool while it is admitted")
            != std::string::npos
        );
    }

    TEST_CASE("the Tool primitive refuses a malformed call without ending the run")
    {
        auto const log     = std::make_shared<std::vector<ToolCallRecord>>();
        auto const program = scopedProgram(
            "fixture.scoped.malformed",
            pluginSource(
                "fixture.scoped.malformed",
                "",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        local arity, arityError = pcall(function()\n"
                "            return tools.raw(\"a.one\")\n"
                "        end)\n"
                "        local named, namedError = pcall(function()\n"
                "            return tools.call(\"NotCanonical\", {})\n"
                "        end)\n"
                "        local shaped, shapedError = pcall(function()\n"
                "            return tools.call(\"a.one\", tools.call)\n"
                "        end)\n"
                "        return {\n"
                "            arity = arity,\n"
                "            arityError = tostring(arityError),\n"
                "            named = named,\n"
                "            namedError = tostring(namedError),\n"
                "            shaped = shaped,\n"
                "            shapedError = tostring(shapedError),\n"
                "        }"
            ),
            recordingRuntime(log)
        );

        auto const answer = program.invoke(
            "derive",
            json::Value{},
            ScopedRunRequest{.parentPosition = k_rootRunPosition}
        );
        REQUIRE(answer.has_value());
        auto const bytes = json::canonicalBytes(*answer);
        CHECK(bytes.find(R"("arity":false)") != std::string::npos);
        CHECK(bytes.find("takes a tool name and one argument value") != std::string::npos);
        CHECK(bytes.find(R"("named":false)") != std::string::npos);
        CHECK(bytes.find("rejected a non-canonical tool name") != std::string::npos);
        CHECK(bytes.find(R"("shaped":false)") != std::string::npos);
        CHECK(bytes.find("a type no JSON document has") != std::string::npos);

        // None of the three reached the Tool Runtime, so none of them spent a
        // child index either.
        CHECK(log->empty());
    }

    TEST_CASE("a scoped run spinning in pure computation is torn down by its stop token")
    {
        auto const stopSource = std::make_shared<std::stop_source>();
        auto const program    = scopedProgram(
            "fixture.scoped.spin",
            pluginSource(
                "fixture.scoped.spin",
                "",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        tools.call(\"a.one\", input)\n"
                "        local total = 0\n"
                "        while true do\n"
                "            total = total + 1\n"
                "        end\n"
                "        return { total = total }"
            ),
            [stopSource](
                std::string_view,
                json::Value const&,
                ToolCallCoordinate const&,
                std::stop_token
            ) -> Result<json::Value> {
                stopSource->request_stop();
                return json::Value::ofBoolean(true);
            }
        );

        auto const answer = program.invoke(
            "derive",
            json::Value{},
            ScopedRunRequest{
                .parentPosition = k_rootRunPosition,
                .cancellation   = stopSource->get_token(),
            }
        );
        REQUIRE_FALSE(answer.has_value());
        CHECK(automationErrorKind(answer.error()) == AutomationErrorKind::Cancelled);
        CHECK(
            std::string{answer.error().message()}.find("the host requested cancellation")
            != std::string::npos
        );
    }

    TEST_CASE("one issuing context cannot exceed its fixed Tool call ceiling")
    {
        auto const program = scopedProgram(
            "fixture.scoped.ceiling",
            pluginSource(
                "fixture.scoped.ceiling",
                "",
                "        local tools = require(\"@umbraflow/tools\")\n"
                "        local total = 0\n"
                "        for index = 1, 4096 do\n"
                "            if tools.call(\"a.one\", input) then\n"
                "                total = total + 1\n"
                "            end\n"
                "        end\n"
                "        return { total = total }"
            ),
            runtimeRefusingAt(ScopedToolProgram::k_maximumToolCallsPerContext + 1U)
        );

        auto const answer = program.invoke(
            "derive",
            json::Value{},
            ScopedRunRequest{.parentPosition = k_rootRunPosition}
        );
        REQUIRE_FALSE(answer.has_value());
        CHECK(
            std::string{answer.error().message()}
                .find("exceeded its fixed scoped Tool call ceiling")
            != std::string::npos
        );
    }

    // The reservation is a property of the shared runtime, so it must hold for
    // BOTH program types: the scoped one is the type whose Framework modules
    // actually read a pinned resource, and a reservation that covered only the
    // pure type would leave the case that motivates it uncovered.
    TEST_CASE("the reserved resource namespace holds for the scoped program type")
    {
        auto const modules = scopedFrameworkModules();
        auto const source  = pluginSource(
            "fixture.scoped.reserved",
            "",
            R"(        return resource.readJson("umbraflow.fixture-catalog"))"
        );

        auto const shadowed = compileScoped(
            "fixture.scoped.reserved",
            source,
            recordingRuntime(std::make_shared<std::vector<ToolCallRecord>>()),
            modules,
            {jsonResource("umbraflow.fixture-catalog", R"({"pinned":"project"})")}
        );
        REQUIRE_FALSE(shadowed.has_value());
        CHECK(
            std::string{shadowed.error().message()}
            == "pure data resource name is reserved for the Framework: "
               "umbraflow.fixture-catalog"
        );

        auto const supplied = compileScoped(
            "fixture.scoped.reserved",
            source,
            recordingRuntime(std::make_shared<std::vector<ToolCallRecord>>()),
            modules,
            {},
            {jsonResource("umbraflow.fixture-catalog", R"({"pinned":"framework"})")}
        );
        REQUIRE(supplied.has_value());
        auto const answer = supplied->invoke(
            "derive",
            parsed("{}"),
            ScopedRunRequest{.parentPosition = k_rootRunPosition}
        );
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

        // What the digest covers, named. A preimage over the catalog alone would
        // leave a build that changed what invoke ANSWERS WITH unmoved.
        CHECK(
            material.find(
                R"("scoped_modules":["@umbraflow/audit","@umbraflow/screen",)"
                R"("@umbraflow/tools","@umbraflow/workflow"])"
            )
            != std::string::npos
        );
        CHECK(
            material.find(
                R"("failure_behaviour":"runtime_refusal_is_terminal_uncatchable_vm_teardown_v1")"
            )
            != std::string::npos
        );
        CHECK(material.find(R"("invoke_arity":2)") != std::string::npos);
        CHECK(
            material.find(R"("tool_calls_per_context":1024)") != std::string::npos
        );

        // Moving one scoped module name moves the digest, and so does moving the
        // facade contract; the two are covered independently.
        auto movedName = material;
        auto const namePosition = movedName.find("@umbraflow/workflow");
        REQUIRE(namePosition != std::string::npos);
        movedName.replace(namePosition, std::string_view{"@umbraflow/workflow"}.size(),
                          "@umbraflow/workflox");
        CHECK(digestOf(movedName) != scoped->hex());

        auto movedContract = material;
        auto const contractPosition =
            movedContract.find("one_frozen_decoded_json_value");
        REQUIRE(contractPosition != std::string::npos);
        movedContract.replace(
            contractPosition,
            std::string_view{"one_frozen_decoded_json_value"}.size(),
            "one_mutable_decoded_json_valve"
        );
        CHECK(digestOf(movedContract) != scoped->hex());
    }
} // namespace uf::script
