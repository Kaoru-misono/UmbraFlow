#include <task/framework-bundle.hpp>

#include <script/engine.hpp>
#include <script/pure-data-program.hpp>
#include <script/scoped-tool-program.hpp>

#include <json/value.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The four scoped Framework SDK modules as they are actually shipped: the
// embedded Luau sources, the closure that admits them, and the environments
// that must never see them.
//
// What is proved here is what a reader cannot see from inside a VM. That the
// scoped names are absent from the pure closure, from the trusted bundle list
// and from all three project whitelists, so a reducer's require fails in the
// resolver and no authoring or runtime VM ever runs their source. That the
// modules refuse to load at all when they are not handed the private
// capability table, so there is no environment in which they silently degrade
// into something that skips the Tool Runtime. And that the facades over the one
// native primitive behave the way the scoped contract says they do.
namespace uf::task
{
    namespace
    {
        constexpr auto k_entryPoints = std::array{std::string_view{"derive"}};

        // The four file stems, which is how the trusted bundle and every
        // project whitelist would have to spell them.
        constexpr auto k_scopedStems = std::array{
            std::string_view{"audit"},
            std::string_view{"screen"},
            std::string_view{"tools"},
            std::string_view{"workflow"},
        };

        // The pinned Tool catalog the resource carries. It is the exact
        // projection ProjectGenerationRegistrar bakes into the scoped resource
        // -- argument_contract, child_effects, name and tool_version on every
        // entry, in JCS member order, sorted by name -- rather than a subset
        // shaped to what these cases happen to read. The projection has an
        // owner now, so a fixture that carried a different shape would be
        // asserting against a document the production loader never produces.
        //
        // The values are this suite's own: what the resource says is advisory
        // discovery data, and the Operator judges name, arguments and authority
        // again on every call.
        constexpr auto k_catalogTools = std::string_view{
            R"([{"argument_contract":{"maximum_duration_ms":5000})"
            R"(,"child_effects":{"maximum_child_calls":0})"
            R"(,"name":"framework.audit.record","tool_version":"1"})"
            R"(,{"argument_contract":{"maximum_duration_ms":5000})"
            R"(,"child_effects":{"maximum_child_calls":0})"
            R"(,"name":"framework.screen.capture","tool_version":"1"})"
            R"(,{"argument_contract":{"maximum_duration_ms":5000})"
            R"(,"child_effects":{"maximum_child_calls":0})"
            R"(,"name":"framework.screen.observe","tool_version":"1"})"
            R"(,{"argument_contract":{"maximum_duration_ms":5000})"
            R"(,"child_effects":{"maximum_child_calls":0})"
            R"(,"name":"framework.workflow.reconcile","tool_version":"1"})"
            R"(,{"argument_contract":{"maximum_duration_ms":5000})"
            R"(,"child_effects":{"maximum_child_calls":0})"
            R"(,"name":"framework.workflow.status","tool_version":"1"})"
            R"(,{"argument_contract":{"maximum_duration_ms":60000})"
            R"(,"child_effects":{"maximum_child_calls":0})"
            R"(,"name":"framework.workflow.wait","tool_version":"1"})"
            R"(,{"argument_contract":{"maximum_duration_ms":30000})"
            R"(,"child_effects":{"maximum_child_calls":4})"
            R"(,"name":"project.flow.run","tool_version":"2"}])"
        };

        [[nodiscard]]
        auto digestOf(std::string_view text) -> std::string
        {
            auto const hash = sha256(std::as_bytes(std::span{text}));
            REQUIRE(hash.has_value());
            return hash->hex();
        }

        [[nodiscard]]
        auto catalogDocument() -> std::string
        {
            return R"({"catalog_hash":")" + digestOf(k_catalogTools) + R"(","tools":)"
                 + std::string{k_catalogTools} + "}";
        }

        [[nodiscard]]
        auto catalogResources() -> std::vector<script::PureDataProgram::Resource>
        {
            return {
                script::PureDataProgram::Resource{
                    .kind  = script::PureDataProgram::ResourceKind::Json,
                    .name  = std::string{scopedToolCatalogResourceName()},
                    .bytes = catalogDocument(),
                },
            };
        }

        [[nodiscard]]
        auto parsed(std::string_view text) -> json::Value
        {
            auto value = json::parse(text);
            REQUIRE(value.has_value());
            return *std::move(value);
        }

        struct ScopedCall final
        {
            std::string toolName{};
            std::string arguments{};
            std::string parentPosition{};
            uint64      childIndex{0};
        };

        // The durable position a scoped run is anchored on. A run is never
        // anchored on nothing, so a case names the row its calls hang from.
        [[nodiscard]]
        auto positionOf(std::string_view text) -> ContentHash
        {
            auto const hash = sha256(std::as_bytes(std::span{text}));
            REQUIRE(hash.has_value());
            return *hash;
        }

        // A Tool Runtime that answers in the exact shape the scoped facades
        // read: the tool the seam ran, the recorded position, the delivery
        // classification, and the Tool's own result when it has one. Every
        // answer is derived from the coordinate the seam assigned, so a script
        // that returns one has proved what the seam handed the runtime.
        [[nodiscard]]
        auto scriptedRuntime(std::shared_ptr<std::vector<ScopedCall>> log)
            -> script::ToolRuntimeInvoke
        {
            return [log = std::move(log)](
                       std::string_view toolName,
                       json::Value const& arguments,
                       script::ToolCallCoordinate const& coordinate,
                       std::stop_token
                   ) -> Result<json::Value> {
                log->emplace_back(ScopedCall{
                    .toolName       = std::string{toolName},
                    .arguments      = json::canonicalBytes(arguments),
                    .parentPosition = coordinate.parentPosition.hex(),
                    .childIndex     = coordinate.childIndex,
                });

                auto const identity = digestOf(
                    std::string{toolName} + "#" + std::to_string(coordinate.childIndex)
                );
                auto members = std::vector<json::Member>{
                    json::Member{"call_identity", json::Value::ofString(identity)},
                    json::Member{"tool", json::Value::ofString(std::string{toolName})},
                };

                auto state = std::string{"confirmed"};
                if (toolName == "framework.screen.observe")
                {
                    members.emplace_back(
                        "result",
                        json::Value::ofObject({
                            {"observation_reference",
                             json::Value::ofObject({
                                 {"authorized_ui_actions",
                                  json::Value::ofArray({
                                      json::Value::ofString("activate"),
                                  })},
                                 {"frame_identity_hash",
                                  json::Value::ofString(digestOf("frame"))},
                                 {"local_semantic_targets",
                                  json::Value::ofArray({
                                      json::Value::ofString("start"),
                                      json::Value::ofString("stop"),
                                  })},
                             })},
                        })
                    );
                }
                else if (toolName == "framework.workflow.wait")
                {
                    // A cooperative human stop: the wait ran and did not
                    // complete, which is a recorded fact the script branches on
                    // rather than an error raised into it.
                    members.emplace_back(
                        "result",
                        json::Value::ofObject({
                            {"completed", json::Value::ofBoolean(false)},
                            {"duration_ms", json::Value::ofNumber(250.0)},
                        })
                    );
                }
                else if (toolName == "project.flow.run")
                {
                    state = "possible";
                }
                else if (toolName == "framework.workflow.reconcile")
                {
                    members.emplace_back(
                        "evidence",
                        json::Value::ofObject({
                            {"resolved", json::Value::ofString("proven_absent")},
                        })
                    );
                }
                members.emplace_back("state", json::Value::ofString(state));
                return json::Value::ofObject(std::move(members));
            };
        }

        [[nodiscard]]
        auto scopedProgram(
            std::string_view pluginId,
            std::string source,
            script::ToolRuntimeInvoke invokeTool
        ) -> Result<script::ScopedToolProgram>
        {
            auto framework = scopedFrameworkScriptModules();
            REQUIRE(framework.has_value());
            auto modules = std::vector<script::PureDataProgram::Module>{
                script::PureDataProgram::Module{
                    .name   = "main",
                    .source = std::move(source),
                },
            };
            return script::ScopedToolProgram::compile(
                pluginId,
                "main",
                std::move(modules),
                k_entryPoints,
                {},
                *framework,
                catalogResources(),
                std::move(invokeTool)
            );
        }
    } // namespace

    TEST_CASE("the scoped facades are absent from every environment but the scoped closure")
    {
        auto const scopedNames = script::ScopedToolProgram::scopedModuleNames();
        REQUIRE(scopedNames.size() == k_scopedStems.size());

        // The pure closure a ProjectPlugin and the Journal reducer run on. A
        // scoped name here would make E7's "fail by module name" impossible,
        // because the resolver would find the module.
        auto pure = pureFrameworkScriptModules();
        REQUIRE(pure.has_value());
        for (auto const scopedName : scopedNames)
        {
            INFO("scoped module: ", scopedName);
            CHECK(
                std::ranges::find(*pure, scopedName, &script::FrameworkModule::name)
                == pure->end()
            );
        }

        // E7's experiment, run against the closure a reducer actually gets
        // rather than against an empty one: the refusal must name the module,
        // because which modules a resolver admits is a property of the program
        // type and a pure program has to say which name its own type does not
        // carry.
        for (auto const scopedName : scopedNames)
        {
            INFO("scoped module: ", scopedName);
            auto reducer = script::PureDataProgram::compile(
                "fixture.reducer",
                "main",
                {
                    script::PureDataProgram::Module{
                        .name   = "main",
                        .source = "return {\n"
                                  "    plugin_id = \"fixture.reducer\",\n"
                                  "    derive = function(_)\n"
                                  "        return require(\""
                                + std::string{scopedName}
                                + "\")\n    end,\n}\n",
                    },
                },
                k_entryPoints,
                {},
                *pure,
                catalogResources()
            );
            REQUIRE(reducer.has_value());
            auto const refused = reducer->invoke("derive", parsed("{}"));
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                std::string{refused.error().message()}.find(
                    "rejected an unknown module: " + std::string{scopedName}
                )
                != std::string::npos
            );
        }

        // The trusted bundle list every Engine VM boots: the authoring and
        // exploration VM and the Host's runtime VM both take exactly this list,
        // so absence here is absence from both.
        auto const trusted = frameworkScriptModules();
        for (auto const stem : k_scopedStems)
        {
            INFO("scoped stem: ", stem);
            CHECK(
                std::ranges::find(trusted, stem, &script::FrameworkModule::name)
                == trusted.end()
            );
        }
        for (auto const scopedName : scopedNames)
        {
            INFO("scoped module: ", scopedName);
            CHECK(
                std::ranges::find(
                    trusted,
                    scopedName,
                    &script::FrameworkModule::resolverName
                )
                == trusted.end()
            );
        }

        // And absent from the three projections, which is the only route by
        // which anything the framework loaded becomes nameable from a project
        // script.
        auto const whitelists = std::array{
            frameworkProjectGlobals(),
            explorationProjectGlobals(),
            runtimeProjectGlobals(),
        };
        for (auto const& whitelist : whitelists)
        {
            for (auto const stem : k_scopedStems)
            {
                INFO("scoped stem: ", stem);
                CHECK_FALSE(std::ranges::contains(whitelist, std::string{stem}));
            }
        }

        // The discriminating half: a projection naming a scoped stem fails the
        // generation, and it fails because the trusted loader's module registry
        // never bound one. Asserting only that the whitelists do not name it
        // would pass just as well if the module were loaded and simply not
        // published.
        for (auto const stem : k_scopedStems)
        {
            INFO("scoped stem: ", stem);
            auto engine = script::Engine::create(
                script::EngineConfig{
                    .frameworkModules        = frameworkScriptModules(),
                    .frameworkProjectGlobals = {std::string{stem}},
                }
            );
            REQUIRE_FALSE(engine.has_value());
            CHECK(
                std::string{engine.error().message()}.find(
                    "framework module registry): " + std::string{stem}
                )
                != std::string::npos
            );
        }
    }

    TEST_CASE("the scoped closure is the pure closure plus exactly the four facades")
    {
        auto pure   = pureFrameworkScriptModules();
        auto scoped = scopedFrameworkScriptModules();
        REQUIRE(pure.has_value());
        REQUIRE(scoped.has_value());
        REQUIRE(scoped->size() == pure->size() + 4U);

        for (auto index = std::size_t{}; index < pure->size(); ++index)
        {
            CHECK(scoped->at(index).name == pure->at(index).name);
        }

        // Declared depths continue the pure tier: `tools` owns the one call
        // primitive and the other three reach the Tool Runtime through it.
        constexpr auto expected = std::array{
            std::pair{std::string_view{"@umbraflow/tools"}, std::size_t{3U}},
            std::pair{std::string_view{"@umbraflow/audit"}, std::size_t{4U}},
            std::pair{std::string_view{"@umbraflow/screen"}, std::size_t{4U}},
            std::pair{std::string_view{"@umbraflow/workflow"}, std::size_t{4U}},
        };
        for (auto index = std::size_t{}; index < expected.size(); ++index)
        {
            auto const& module = scoped->at(pure->size() + index);
            INFO("scoped module: ", module.name);
            CHECK(module.name == expected[index].first);
            CHECK(module.dependencyDepth == expected[index].second);
            CHECK(module.projectVisible);
        }

        // The C++ catalog-resource name and the spelling inside the embedded
        // module cannot drift, because a Luau source cannot read a C++
        // constant and the module would otherwise read a resource nobody bakes.
        auto const entries = frameworkBundleEntries();
        auto const toolsEntry =
            std::ranges::find(entries, "tools", &FrameworkBundleEntry::name);
        REQUIRE(toolsEntry != entries.end());
        CHECK(
            toolsEntry->source.find(scopedToolCatalogResourceName())
            != std::string_view::npos
        );
    }

    TEST_CASE("a scoped facade refuses to load without the Tool Runtime capability table")
    {
        // The same embedded source, admitted by a program type that publishes
        // no native seam. Nothing hands it the capability table, so its chunk
        // argument is absent and it refuses -- rather than degrading into a
        // module that answers without ever reaching a Tool.
        constexpr auto borrowed = std::array{
            std::pair{std::string_view{"collections"}, std::string_view{"@umbraflow/collections"}},
            std::pair{std::string_view{"result"}, std::string_view{"@umbraflow/result"}},
            std::pair{std::string_view{"tools"}, std::string_view{"@umbraflow/tools"}},
        };
        auto const entries = frameworkBundleEntries();
        auto framework = std::vector<script::FrameworkModule>{};
        for (auto const& [stem, reserved] : borrowed)
        {
            auto const found =
                std::ranges::find(entries, stem, &FrameworkBundleEntry::name);
            REQUIRE(found != entries.end());
            framework.emplace_back(script::FrameworkModule{
                .name           = reserved,
                .source         = found->source,
                .projectVisible = true,
            });
        }

        auto program = script::PureDataProgram::compile(
            "fixture.scoped.unarmed",
            "main",
            {
                script::PureDataProgram::Module{
                    .name   = "main",
                    .source = R"LUAU(
return {
    plugin_id = "fixture.scoped.unarmed",
    derive = function(_)
        return require("@umbraflow/tools")
    end,
}
)LUAU",
                },
            },
            k_entryPoints,
            {},
            framework,
            catalogResources()
        );
        REQUIRE(program.has_value());

        auto const answer = program->invoke("derive", parsed("{}"));
        REQUIRE_FALSE(answer.has_value());
        CHECK(
            std::string{answer.error().message()}.find(
                "tools requires the scoped Tool Runtime capability table"
            )
            != std::string::npos
        );
    }

    TEST_CASE("the shipped scoped facades drive one run over the one native primitive")
    {
        auto const log = std::make_shared<std::vector<ScopedCall>>();
        auto program = scopedProgram(
            "fixture.scoped.sdk",
            R"LUAU(
local tools = require("@umbraflow/tools")
local screen = require("@umbraflow/screen")
local workflow = require("@umbraflow/workflow")
local audit = require("@umbraflow/audit")
local outcome = require("@umbraflow/result")

return {
    plugin_id = "fixture.scoped.sdk",
    derive = function(_)
        local observed = screen.observe()
        local handle = outcome.unwrap_or(screen.observation(observed), nil)
        local targets = screen.targets(handle)
        local actions = screen.actions(handle)
        local reference = screen.use(handle)
        local reused = pcall(function() return screen.use(handle) end)

        local waited = workflow.wait(250)
        local recorded = audit.record({ note = "kept" })

        local uncertain = workflow.child_flow("project.flow.run", canon.emptyObject)
        local reconciled = workflow.reconcile(uncertain)

        local noArm = pcall(function()
            return workflow.recover(uncertain, {
                confirmed = function() return "wrong" end,
            })
        end)
        local unknownTool = pcall(function()
            return tools.call("nowhere.at.all", canon.emptyObject)
        end)
        local emptyRecord = pcall(function() return audit.record({}) end)
        local overLongWait = pcall(function() return workflow.wait(60001) end)
        local leafFlow = pcall(function()
            return workflow.child_flow("framework.workflow.status", canon.emptyObject)
        end)
        local settledReconcile = pcall(function() return workflow.reconcile(waited) end)

        return {
            actions = actions,
            audit_recorded = audit.recorded(recorded),
            catalog_hash = tools.catalog_hash,
            delivered = workflow.delivered(waited),
            empty_record = emptyRecord,
            evidence = outcome.unwrap_or(tools.evidence(reconciled), "none"),
            frame = reference.frame_identity_hash,
            knows = tools.knows("framework.screen.observe")
                and not tools.knows("nowhere.at.all"),
            leaf_flow = leafFlow,
            names = tools.names(),
            no_arm = noArm,
            no_result = outcome.unwrap_or(tools.result(recorded), "absent"),
            over_long_wait = overLongWait,
            possible = tools.state(uncertain),
            reconciled = tools.state(reconciled),
            recovered = workflow.recover(uncertain, {
                possible = function(answer) return tools.call_identity(answer) end,
            }),
            reused = reused,
            settled = workflow.settled(uncertain),
            settled_reconcile = settledReconcile,
            states = tools.states,
            stopped = workflow.stopped(waited),
            targets = targets,
            uncertain = workflow.uncertain(uncertain),
            unknown_tool = unknownTool,
            wait_ceiling = tools.describe("framework.workflow.wait")
                .argument_contract.maximum_duration_ms,
        }
    end,
}
)LUAU",
            scriptedRuntime(log)
        );
        REQUIRE(program.has_value());

        auto const answer = program->invoke(
            "derive",
            parsed("{}"),
            script::ScopedRunRequest{.parentPosition = positionOf("root-run")}
        );
        REQUIRE(answer.has_value());
        auto const bytes = json::canonicalBytes(*answer);

        // Exactly five Tool calls reached the seam, numbered from 1 in the
        // order the script issued them, every one of them anchored on the
        // coordinate of the root-positioned call this run implements rather
        // than on an absence. Everything the facades refused was refused
        // BEFORE a call was spent on it.
        REQUIRE(log->size() == 5U);
        constexpr auto issued = std::array{
            std::string_view{"framework.screen.observe"},
            std::string_view{"framework.workflow.wait"},
            std::string_view{"framework.audit.record"},
            std::string_view{"project.flow.run"},
            std::string_view{"framework.workflow.reconcile"},
        };
        for (auto index = std::size_t{}; index < issued.size(); ++index)
        {
            INFO("call: ", issued[index]);
            CHECK((*log)[index].toolName == issued[index]);
            CHECK((*log)[index].childIndex == index + 1U);
            CHECK((*log)[index].parentPosition == positionOf("root-run").hex());
        }

        // The facades own the argument shapes, including the empty JSON OBJECT
        // that a Luau table literal cannot spell.
        CHECK((*log)[0].arguments == "{}");
        CHECK((*log)[1].arguments == R"({"duration_ms":250})");
        CHECK((*log)[2].arguments == R"({"record":{"note":"kept"}})");
        CHECK((*log)[3].arguments == "{}");
        CHECK(
            (*log)[4].arguments
            == R"({"call_identity":")" + digestOf("project.flow.run#4") + R"("})"
        );

        // A possible outcome surfaces as itself, is not settled, and reaches a
        // handler only through an arm the script wrote for it by name.
        CHECK(bytes.find(R"("possible":"possible")") != std::string::npos);
        CHECK(bytes.find(R"("uncertain":true)") != std::string::npos);
        CHECK(bytes.find(R"("settled":false)") != std::string::npos);
        CHECK(bytes.find(R"("no_arm":false)") != std::string::npos);
        CHECK(
            bytes.find(R"("recovered":")" + digestOf("project.flow.run#4") + R"(")")
            != std::string::npos
        );

        // Reconciliation replaces the uncertain classification, and refuses a
        // settled one.
        CHECK(bytes.find(R"("reconciled":"confirmed")") != std::string::npos);
        CHECK(bytes.find(R"("settled_reconcile":false)") != std::string::npos);
        CHECK(
            bytes.find(R"("evidence":{"resolved":"proven_absent"})")
            != std::string::npos
        );

        // Discovery and description are frozen data read from the pinned
        // catalog resource, and they cost no Tool call.
        CHECK(
            bytes.find(R"("catalog_hash":")" + digestOf(k_catalogTools) + R"(")")
            != std::string::npos
        );
        CHECK(bytes.find(R"("wait_ceiling":60000)") != std::string::npos);
        CHECK(bytes.find(R"("knows":true)") != std::string::npos);
        CHECK(bytes.find(R"("unknown_tool":false)") != std::string::npos);
        CHECK(
            bytes.find(
                R"("names":["framework.audit.record","framework.screen.capture",)"
                R"("framework.screen.observe","framework.workflow.reconcile",)"
                R"("framework.workflow.status","framework.workflow.wait",)"
                R"("project.flow.run"])"
            )
            != std::string::npos
        );
        CHECK(
            bytes.find(
                R"("states":["proposed","admitted","dispatching","confirmed",)"
                R"("proven_absent","possible","rejected","terminal_failure",)"
                R"("terminally_unresolved"])"
            )
            != std::string::npos
        );

        // Snapshot handles are plain JSON data with single-use ergonomics.
        CHECK(bytes.find(R"("actions":["activate"])") != std::string::npos);
        CHECK(bytes.find(R"("targets":["start","stop"])") != std::string::npos);
        CHECK(
            bytes.find(R"("frame":")" + digestOf("frame") + R"(")")
            != std::string::npos
        );
        CHECK(bytes.find(R"("reused":false)") != std::string::npos);

        // The human stop arrives as the wait Tool's own recorded outcome.
        CHECK(bytes.find(R"("stopped":true)") != std::string::npos);
        CHECK(bytes.find(R"("delivered":true)") != std::string::npos);

        // Audit is an ordinary Tool call whose record is its durable outcome,
        // and its arguments carry no attribution member at all.
        CHECK(bytes.find(R"("audit_recorded":true)") != std::string::npos);
        CHECK(bytes.find(R"("no_result":"absent")") != std::string::npos);
        CHECK(bytes.find(R"("empty_record":false)") != std::string::npos);

        // Bounds come from the catalog, and a leaf Tool is not a child flow.
        CHECK(bytes.find(R"("over_long_wait":false)") != std::string::npos);
        CHECK(bytes.find(R"("leaf_flow":false)") != std::string::npos);
    }

    TEST_CASE("a handler run numbers its own context and a Tool refusal is terminal")
    {
        auto const log = std::make_shared<std::vector<ScopedCall>>();
        auto program = scopedProgram(
            "fixture.scoped.handler",
            R"LUAU(
local screen = require("@umbraflow/screen")
local audit = require("@umbraflow/audit")

return {
    plugin_id = "fixture.scoped.handler",
    derive = function(_)
        screen.observe()
        return { recorded = audit.recorded(audit.record({ note = "handler" })) }
    end,
}
)LUAU",
            scriptedRuntime(log)
        );
        REQUIRE(program.has_value());

        auto const answer = program->invoke(
            "derive",
            parsed("{}"),
            script::ScopedRunRequest{.parentPosition = positionOf("handler-run")}
        );
        REQUIRE(answer.has_value());
        CHECK(json::canonicalBytes(*answer) == R"({"recorded":true})");

        REQUIRE(log->size() == 2U);
        for (auto index = std::size_t{}; index < log->size(); ++index)
        {
            CHECK(
                (*log)[index].parentPosition == positionOf("handler-run").hex()
            );
            CHECK((*log)[index].childIndex == index + 1U);
        }

        // A Tool Runtime refusal is a fact about the run, not a value: it tears
        // the VM down past every pcall the facades or the script can install.
        auto refusing = scopedProgram(
            "fixture.scoped.terminal",
            R"LUAU(
local audit = require("@umbraflow/audit")

return {
    plugin_id = "fixture.scoped.terminal",
    derive = function(_)
        local caught = pcall(function()
            return audit.record({ note = "swallowed" })
        end)
        return { caught = caught }
    end,
}
)LUAU",
            [](std::string_view,
               json::Value const&,
               script::ToolCallCoordinate const&,
               std::stop_token) -> Result<json::Value> {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "the recorded call at this coordinate names another tool"
                );
            }
        );
        REQUIRE(refusing.has_value());
        auto const torn =
            refusing->invoke(
                "derive",
                parsed("{}"),
                script::ScopedRunRequest{.parentPosition = positionOf("torn-run")}
            );
        REQUIRE_FALSE(torn.has_value());
        CHECK(
            std::string{torn.error().message()}.find(
                "the recorded call at this coordinate names another tool"
            )
            != std::string::npos
        );
    }
}
