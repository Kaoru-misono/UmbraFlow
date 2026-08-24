// The authoring path for a Project Tool program, end to end and with nothing
// hand-built in the middle.
//
// Every other case that has ever reached a Project Tool registrar built its
// registration in C++: opaque catalog bytes, a claims struct written out by
// hand, a binding table handed straight to the registrar. Such a case can prove
// that the loader refuses a disagreement, and it can prove nothing at all about
// whether any authoring path can produce a registration the loader accepts --
// which is the property that decides whether the loader is reachable by a real
// project or only by its own tests.
//
// So these cases start from bytes a project author writes: one
// umbraflow-project.json naming a deployment, declaring its Tools inline and
// joining them to closure entries through tool_bindings, and a Luau closure on
// disk. loadProductionProject derives the registration from those bytes, and
// the derived registration is what the registrar is handed. Nothing between the
// author and the compiled program is written by a test.
//
// All of it stays production-unreachable: ProductLifecycle registers no Project
// Tool program, and these cases are the only callers.

#include "umbraflow/project-schemas.hpp"

#include <deployment/project-directory.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <operator/project-plugin.hpp>
#include <operator/project-generation.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-invocation.hpp>

#include <script/scoped-tool-program.hpp>

#include <json/value.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::deployment
{
    namespace
    {
        namespace umbraflow = operator_runtime::test_support;

        using operator_runtime::ProjectToolBinding;
        using operator_runtime::ProjectGenerationRegistrar;
        using operator_runtime::Risk;
        using operator_runtime::ToolMutability;
        using operator_runtime::ToolSurface;

        constexpr auto k_pluginId     = std::string_view{"chaos.project"};
        constexpr auto k_dismissTool  = std::string_view{"chaos.project.dismiss"};
        constexpr auto k_sweepTool    = std::string_view{"chaos.project.sweep"};
        constexpr auto k_dismissEntry = std::string_view{"dismiss"};
        constexpr auto k_sweepEntry   = std::string_view{"sweep"};

        // What the deployment's tool closure exports, stated as the fact about
        // the CODE that it is. The loader cannot derive it -- refusing a
        // disagreement between the contract and the code is its whole job, so
        // it may not compute one side from the other -- and nothing short of
        // running the module can read a Luau table's keys. It is checked twice
        // and trusted neither time: the registrar joins it against the binding
        // table, and the bridge then admits a module whose exported set is
        // exactly `plugin_id` plus these entries.
        //
        // It is authored INTO the root document now, so a case that wants a
        // different statement writes a different document.
        [[nodiscard]] auto exportedEntryPoints() -> std::vector<std::string>
        {
            return {std::string{k_dismissEntry}, std::string{k_sweepEntry}};
        }

        [[nodiscard]] auto entryPointsJson(
            std::vector<std::string> const& entryPoints
        ) -> std::string
        {
            auto rendered = std::string{"["};
            for (auto index = std::size_t{0}; index < entryPoints.size(); ++index)
            {
                rendered += index == 0U ? "" : ",";
                rendered += "\"" + entryPoints.at(index) + "\"";
            }
            rendered += "]";
            return rendered;
        }

        // Two entries, one closure. `dismiss` reaches the Tool Runtime through
        // the scoped facade -- which is loadable only because the registrar
        // baked the pinned catalog resource -- and `sweep` answers from the
        // frozen discovery table without spending a call. Nothing judges what
        // either of them returns: the framework records an answer's bytes and
        // never reads their meaning.
        constexpr auto k_pluginSource = std::string_view{R"LUAU(
local tools = require("@umbraflow/tools")

return {
    plugin_id = "chaos.project",
    dismiss = function(input)
        local answer = tools.call("chaos.project.sweep", input)
        return { outcome = tools.state(answer) }
    end,
    sweep = function(_input)
        if tools.knows("chaos.project.dismiss") then
            return { outcome = "swept" }
        end
        return { outcome = "blind" }
    end,
}
)LUAU"};

        [[nodiscard]] auto hashOf(std::string_view bytes) -> ContentHash
        {
            auto const digest = sha256(std::as_bytes(std::span{bytes}));
            REQUIRE(digest.has_value());
            return *digest;
        }

        auto write(std::filesystem::path const& path, std::string_view bytes) -> void
        {
            std::filesystem::create_directories(path.parent_path());
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(stream.good());
        }

        // One Tool of this deployment's declaration, as the author writes it
        // into `tools`. Every member is stated: a Tool the framework admits is
        // one whose whole contract was written down, and there is no optional
        // member for an absence to be read as.
        //
        // `childEffects` is spliced in whole so that a case can write the
        // contradictory declaration the reader must refuse -- which a typed
        // builder could not express.
        [[nodiscard]]
        auto declaredTool(
            std::string_view name,
            std::string_view childEffects
        ) -> std::string
        {
            auto tool = std::string{R"json({"argument_schema":)json"};
            tool += umbraflow::k_toolArgumentSchema;
            tool += R"json(,"body":false)json";
            tool += R"json(,"child_effects":)json";
            tool += childEffects;
            tool += R"json(,"effect_bounds":[],"idempotency":"read_safe",)json";
            tool += R"json("mutability":"read_only","name":")json";
            tool += name;
            tool += R"json(","required_capabilities":[],"surface":"semantic",)json";
            tool += R"json("timeout_policy":{"maximum_elapsed_ms":30000,)json"
                R"json("on_timeout":"stop"},"ui_action_bounds":[],)json";
            tool += R"json("version":"1","workflow_limits":)json"
                R"json({"maximum_dispatches":4,"maximum_elapsed_ms":60000,)json"
                R"json("maximum_observations":16,"maximum_steps":4,)json"
                R"json("maximum_waits":4}})json";
            return tool;
        }

        // The empty declaration: no name, no call, and the most restricted
        // ceiling of each kind. It is what a Tool that issues no child call
        // states, and it is written out rather than left out.
        constexpr auto k_noChildCalls = std::string_view{
            R"json({"child_tool_names":[],"maximum_child_calls":0,)json"
            R"json("maximum_child_mutability":"read_only",)json"
            R"json("maximum_child_risk":"read_only",)json"
            R"json("maximum_child_surface":"semantic"})json"
        };

        // The Tools this deployment declares, inline, as the array the block's
        // `tools` member carries. `chaos.project.dismiss` declares a real child
        // effect set, because the whole point of giving child_effects a wire
        // spelling is that a project can state one; `chaos.project.sweep`
        // declares the empty one.
        [[nodiscard]]
        auto toolsJson(uint32 maximumChildCalls = 2U) -> std::string
        {
            auto delegating = std::string{R"json({"child_tool_names":[")json"};
            delegating += k_sweepTool;
            delegating += R"json("],"maximum_child_calls":)json";
            delegating += std::to_string(maximumChildCalls);
            delegating += R"json(,"maximum_child_mutability":"read_only",)json"
                R"json("maximum_child_risk":"low",)json"
                R"json("maximum_child_surface":"semantic"})json";

            auto rendered = std::string{"["};
            rendered += declaredTool(k_dismissTool, delegating);
            rendered += ",";
            rendered += declaredTool(k_sweepTool, k_noChildCalls);
            rendered += "]";
            return rendered;
        }

        // One tool_bindings array, as a project author spells it.
        [[nodiscard]]
        auto bindingsJson(std::span<ProjectToolBinding const> bindings)
            -> std::string
        {
            auto rendered = std::string{"["};
            for (auto index = std::size_t{0}; index < bindings.size(); ++index)
            {
                rendered += index == 0U ? "" : ",";
                rendered += R"({"entry_point":")";
                rendered += bindings[index].entryPoint;
                rendered += R"(","tool_name":")";
                rendered += bindings[index].toolName;
                rendered += R"("})";
            }
            rendered += "]";
            return rendered;
        }

        [[nodiscard]] auto acceptedBindings() -> std::vector<ProjectToolBinding>
        {
            return {
                ProjectToolBinding{
                    .toolName   = std::string{k_dismissTool},
                    .entryPoint = std::string{k_dismissEntry},
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_sweepTool},
                    .entryPoint = std::string{k_sweepEntry},
                },
            };
        }

        // One project directory on disk, written the way a project author
        // writes one and read back by the production loader. Every case
        // constructs its own; nothing is shared between them.
        class AuthoredProject final
        {
            std::filesystem::path m_root{};

        public:
            AuthoredProject(
                std::string_view tools,
                std::string_view toolBindings,
                std::string_view pluginSource,
                std::vector<std::string> declaredEntryPoints
                    = exportedEntryPoints()
            )
            {
                m_root = std::filesystem::temp_directory_path()
                    / std::filesystem::path{
                        "uf-tool-authoring-"
                        + std::to_string(std::random_device{}())
                    };
                std::filesystem::remove_all(m_root);
                std::filesystem::create_directories(m_root);

                write(m_root / "plugin/main.luau", pluginSource);
                write(m_root / "runtime/artifact/runtime-model.toml", "[[page]]\n");
                write(
                    m_root / "umbraflow-project.json",
                    manifest(tools, toolBindings, declaredEntryPoints)
                );
            }

            AuthoredProject(AuthoredProject const&)                    = delete;
            AuthoredProject(AuthoredProject&&)                         = delete;
            auto operator=(AuthoredProject const&) -> AuthoredProject& = delete;
            auto operator=(AuthoredProject&&) -> AuthoredProject&      = delete;

            ~AuthoredProject()
            {
                auto discarded = std::error_code{};
                std::filesystem::remove_all(m_root, discarded);
            }

            [[nodiscard]] auto load() const -> Result<LoadedProject>
            {
                return loadProductionProject(m_root, {});
            }

            // The accepted directory with one document rewritten. A case that
            // breaks the root document has to start from the one this loader
            // accepts, or its refusal proves nothing about the rule it named.
            auto rewriteManifest(std::string_view document) const -> void
            {
                write(m_root / "umbraflow-project.json", document);
            }

            [[nodiscard]]
            static auto manifest(
                std::string_view tools,
                std::string_view toolBindings,
                std::vector<std::string> const& declaredEntryPoints
                    = exportedEntryPoints()
            ) -> std::string
            {
                auto document =
                    std::string{R"json({"schema":"umbraflow-project/v3",)json"};
                document += R"json("runtime_artifact":"runtime/artifact",)json";
                document += R"json("primary_deployment":"main",)json";
                document += R"json("template_cuts":[],"deployments":[{)json";
                document += R"json("name":"main","plugin_id":")json";
                document += k_pluginId;
                document += R"json(","tool_closure":{"entry":"main",)json"
                    R"json("exported_entry_points":)json";
                document += entryPointsJson(declaredEntryPoints);
                document += R"json(,"modules":)json"
                    R"json([{"name":"main","path":"plugin/main.luau"}]},)json";
                document += R"json("plugin_authoring":"hand-written",)json";
                document += R"json("plugin_justification":"This deployment's )json"
                    R"json(entries are Tool handlers reached through the scoped )json"
                    R"json(Tool Runtime, which umbraflow-declarative-workflow-)json"
                    R"json(tool/v1 has no member for at all.",)json";
                document += R"json("tools":)json";
                document += tools;
                document += R"json(,"observed_instance_identity_schemas":[],)json";
                document += R"json("tool_bindings":)json";
                document += toolBindings;
                document += R"json(,"resources":[]}]})json";
                return document;
            }
        };

        // The one Tool Runtime seam a compiled program reaches, in the shape
        // @umbraflow/tools requires an answer to have. It records what it was
        // asked so a case can prove the call left the VM, and it carries no run
        // state: every run-scoped value arrives in the ScopedRunRequest.
        struct RecordedCall final
        {
            std::string toolName{};
            std::string parentPosition{};
            uint64      childIndex{0};
        };

        [[nodiscard]]
        auto recordingRuntime(std::shared_ptr<std::vector<RecordedCall>> p_calls)
            -> script::ToolRuntimeDispatch
        {
            return [p_calls = std::move(p_calls)](
                       std::string_view toolName,
                       json::Value const&,
                       script::ToolCallCoordinate const& coordinate,
                       std::stop_token,
                       script::ToolCallBody
                   ) -> Result<json::Value>
            {
                p_calls->emplace_back(RecordedCall{
                    .toolName       = std::string{toolName},
                    .parentPosition = coordinate.parentPosition.hex(),
                    .childIndex     = coordinate.childIndex,
                });
                return json::Value::ofObject({
                    {"call_identity",
                     json::Value::ofString(hashOf("recorded-call").hex())},
                    {"state", json::Value::ofString("confirmed")},
                    {"tool", json::Value::ofString(std::string{toolName})},
                });
            };
        }

        [[nodiscard]] auto runPosition() -> ContentHash
        {
            return hashOf("authored-run-position");
        }

        template <typename Value>
        [[nodiscard]] auto why(Result<Value> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<it was accepted>"}
                : std::string{outcome.error().message()};
        }

        // The registrar call every case below makes, over one loaded
        // deployment. Every declared entry set comes from the loaded
        // generation rather than from this call, which is the whole of what
        // moved when the declaration became an authored member.
        [[nodiscard]]
        auto registerLoaded(
            ProjectGenerationRegistrar& registrar,
            LoadedDeployment const& deployment,
            std::shared_ptr<std::vector<RecordedCall>> p_calls
        ) -> Result<operator_runtime::ProjectGenerationHandle>
        {
            return registrar.registerGeneration(
                deployment.generation,
                deployment.toolCatalogSchemaOwner,
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = deployment.toolClosure.entryModule,
                    .modules     = deployment.toolClosure.modules,
                },
                deployment.projectResources,
                recordingRuntime(std::move(p_calls))
            );
        }
    }

    // The whole path, once: authored bytes in, one compiled program out.
    TEST_CASE("an authored project loads as a Project Tool program")
    {
        auto const authored = AuthoredProject{
            toolsJson(),
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto const project = authored.load();
        INFO(why(project));
        REQUIRE(project.has_value());
        auto const* const p_deployment = project->findDeployment("main");
        REQUIRE(p_deployment != nullptr);

        // The registration the loader derived carries the binding table the
        // deployment block declared, sorted by tool name the way it sorts
        // every other derived member. Nothing in this test wrote it.
        auto const& bindings = p_deployment->generation.projectToolBindings();
        REQUIRE(bindings.size() == 2U);
        CHECK(bindings[0].toolName == k_dismissTool);
        CHECK(bindings[0].entryPoint == k_dismissEntry);
        CHECK(bindings[1].toolName == k_sweepTool);
        CHECK(bindings[1].entryPoint == k_sweepEntry);

        // And the descriptor carries the child effect declaration the `tools`
        // array spelled, which is the half of this cut that had no wire
        // spelling at all before it.
        auto const dismiss =
            p_deployment->toolCatalogSchemaOwner.describe(k_dismissTool);
        REQUIRE(dismiss.has_value());
        CHECK(dismiss->childEffects.childToolNames
              == std::vector<std::string>{std::string{k_sweepTool}});
        CHECK(dismiss->childEffects.maximumChildCalls == 2U);
        CHECK(dismiss->childEffects.maximumChildRisk == Risk::Low);
        CHECK(dismiss->childEffects.maximumChildSurface == ToolSurface::Semantic);
        CHECK(
            dismiss->childEffects.maximumChildMutability == ToolMutability::ReadOnly
        );
        auto const sweep = p_deployment->toolCatalogSchemaOwner.describe(k_sweepTool);
        REQUIRE(sweep.has_value());
        CHECK(sweep->childEffects.childToolNames.empty());
        CHECK(sweep->childEffects.maximumChildCalls == 0U);

        auto       registrar = ProjectGenerationRegistrar{};
        auto       p_calls   = std::make_shared<std::vector<RecordedCall>>();
        auto const loaded    = registerLoaded(registrar, *p_deployment, p_calls);
        INFO(why(loaded));
        REQUIRE(loaded.has_value());

        CHECK(loaded->pluginId() == k_pluginId);
        CHECK(loaded->projectRegistrationHash() == p_deployment->generation.hash());
        CHECK(loaded->catalog().toolCatalogHash()
              == p_deployment->generation.toolCatalogHash());
        CHECK(
            loaded->bindingTable().entryPoints()
            == std::vector<std::string>{
                std::string{k_dismissEntry},
                std::string{k_sweepEntry},
            }
        );

        // The program runs the entry the DOCUMENT bound, in a closure whose
        // exported set the bridge admits exactly. A statement of
        // k_exportedEntryPoints the source did not honour cannot survive this.
        auto const request = script::ScopedRunRequest{
            .parentPosition = runPosition(),
            .budgetOwner    = "fixture.project-tool",
            .maximumElapsedMillis   = 5'000U,
            .cancellation           = {},
        };
        auto const swept = loaded->invokeBoundTool(
            k_sweepTool,
            json::Value::ofObject({{"value", json::Value::ofNumber(1)}}),
            request
        );
        INFO(why(swept));
        REQUIRE(swept.has_value());
        CHECK(json::canonicalBytes(*swept) == R"({"outcome":"swept"})");
        CHECK(p_calls->empty());

        auto const dismissed = loaded->invokeBoundTool(
            k_dismissTool,
            json::Value::ofObject({{"value", json::Value::ofNumber(1)}}),
            request
        );
        INFO(why(dismissed));
        REQUIRE(dismissed.has_value());
        CHECK(json::canonicalBytes(*dismissed) == R"({"outcome":"confirmed"})");
        REQUIRE(p_calls->size() == 1U);
        CHECK(p_calls->front().toolName == k_sweepTool);
        CHECK(p_calls->front().parentPosition == runPosition().hex());
        CHECK(p_calls->front().childIndex == 1U);
    }

    // The binding is inside the registration root, which is what stops a
    // re-registration rebinding a Tool to different code with no detectable
    // identity change. Two directories differing in one entry_point and in
    // nothing else must derive two registration hashes.
    TEST_CASE("rebinding a Tool moves the registration root and not the catalog")
    {
        auto const tools = toolsJson();
        auto const asDeclared = AuthoredProject{
            tools,
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto swapped = acceptedBindings();
        swapped[0].entryPoint = std::string{k_sweepEntry};
        auto const asRebound =
            AuthoredProject{tools, bindingsJson(swapped), k_pluginSource};

        auto const first  = asDeclared.load();
        auto const second = asRebound.load();
        INFO(why(first));
        REQUIRE(first.has_value());
        INFO(why(second));
        REQUIRE(second.has_value());

        auto const& one = first->findDeployment("main")->generation;
        auto const& two = second->findDeployment("main")->generation;
        CHECK(one.hash() != two.hash());

        // And the catalog's own digest does not move, because a binding is not
        // catalog material: a caller can still verify the served bytes against
        // tool_catalog_hash after a handler is rebound.
        CHECK(one.toolCatalogHash() == two.toolCatalogHash());
    }

    // The other direction: child_effects IS catalog material, so editing it
    // moves tool_catalog_hash and therefore the registration root.
    TEST_CASE("a widened child effect declaration moves tool_catalog_hash")
    {
        auto const narrow = AuthoredProject{
            toolsJson(2U),
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto const wide = AuthoredProject{
            toolsJson(64U),
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto const first  = narrow.load();
        auto const second = wide.load();
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());

        auto const& one = first->findDeployment("main")->generation;
        auto const& two = second->findDeployment("main")->generation;
        CHECK(one.toolCatalogHash() != two.toolCatalogHash());
        CHECK(one.hash() != two.hash());
    }

    // A declaration that names a child while admitting no call is two halves of
    // one permission contradicting each other. It is refused when the catalog
    // authority is built -- which is at load, where the declaration is read,
    // before any program exists.
    TEST_CASE("an incoherent child effect declaration is refused where it is written")
    {
        auto edited = toolsJson();
        auto const at = edited.find(R"("maximum_child_calls":2)");
        REQUIRE(at != std::string::npos);
        edited.replace(
            at,
            std::string_view{R"("maximum_child_calls":2)"}.size(),
            R"("maximum_child_calls":0)"
        );
        auto const authored = AuthoredProject{
            edited,
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto const refused = authored.load();
        INFO(why(refused));
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            std::string{refused.error().message()}.contains(
                "names a child tool but admits no child call"
            )
        );
    }

    // There is no absent form of child_effects. A tool that issues no child
    // call writes the empty declaration; a tool that writes nothing is a
    // document this framework does not read, rather than one whose silence
    // means the empty declaration.
    TEST_CASE("a Tool declaration omitting child_effects is refused, not defaulted")
    {
        auto stripped = toolsJson();
        auto const empty = std::string{R"json(,"child_effects":)json"}
            + std::string{k_noChildCalls};
        auto const at = stripped.find(empty);
        REQUIRE(at != std::string::npos);
        stripped.erase(at, empty.size());

        auto const authored = AuthoredProject{
            stripped,
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto const refused = authored.load();
        INFO(why(refused));
        REQUIRE_FALSE(refused.has_value());
        CHECK(std::string{refused.error().message()}.contains("child_effects"));
    }

    TEST_CASE("a Project Tool body declaration is explicit and closed")
    {
        constexpr auto k_body = std::string_view{R"json(,"body":false)json"};

        SUBCASE("omitting body is refused, not defaulted")
        {
            auto stripped = toolsJson();
            auto const at = stripped.find(k_body);
            REQUIRE(at != std::string::npos);
            stripped.erase(at, k_body.size());

            auto const authored = AuthoredProject{
                stripped,
                bindingsJson(acceptedBindings()),
                k_pluginSource,
            };
            auto const refused = authored.load();
            INFO(why(refused));
            REQUIRE_FALSE(refused.has_value());
            CHECK(std::string{refused.error().message()}.contains("body"));
        }

        SUBCASE("true is refused until Project Tool body semantics exist")
        {
            auto enabled = toolsJson();
            auto const at = enabled.find(k_body);
            REQUIRE(at != std::string::npos);
            enabled.replace(at, k_body.size(), R"json(,"body":true)json");

            auto const authored = AuthoredProject{
                enabled,
                bindingsJson(acceptedBindings()),
                k_pluginSource,
            };
            auto const refused = authored.load();
            INFO(why(refused));
            REQUIRE_FALSE(refused.has_value());
            CHECK(std::string{refused.error().message()}.contains("body"));
        }
    }

    // The three bind-time refusals, each reached from an authored document
    // rather than from a binding table built in C++.
    TEST_CASE("the loader refuses an authored contract its code disagrees with")
    {
        auto const tools = toolsJson();

        SUBCASE("a declared Tool with no binding")
        {
            auto only = acceptedBindings();
            only.pop_back();
            auto const authored = AuthoredProject{
                tools,
                bindingsJson(only),
                k_pluginSource,
                {std::string{k_dismissEntry}},
            };
            auto const project = authored.load();
            REQUIRE(project.has_value());

            auto       registrar = ProjectGenerationRegistrar{};
            auto const refused   = registerLoaded(
                registrar,
                *project->findDeployment("main"),
                std::make_shared<std::vector<RecordedCall>>()
            );
            INFO(why(refused));
            REQUIRE_FALSE(refused.has_value());
            CHECK(std::string{refused.error().message()}.contains(
                "is declared with no binding"
            ));
        }

        SUBCASE("a binding no descriptor covers")
        {
            auto extra = acceptedBindings();
            extra.emplace_back(ProjectToolBinding{
                .toolName   = "chaos.project.undeclared",
                .entryPoint = std::string{k_sweepEntry},
            });
            auto const authored =
                AuthoredProject{tools, bindingsJson(extra), k_pluginSource};
            auto const project = authored.load();
            REQUIRE(project.has_value());

            auto       registrar = ProjectGenerationRegistrar{};
            auto const refused   = registerLoaded(
                registrar,
                *project->findDeployment("main"),
                std::make_shared<std::vector<RecordedCall>>()
            );
            INFO(why(refused));
            REQUIRE_FALSE(refused.has_value());
            CHECK(std::string{refused.error().message()}.contains(
                "which this Tool Catalog does not declare"
            ));
        }

        // The document states what its bindings require and the shipped bytes
        // do not carry it. The registrar's own join passes -- declaration and
        // binding union agree -- so the refusal can only come from the bridge,
        // which is the leg no document check can stand in for.
        SUBCASE("a binding naming an entry the closure does not export")
        {
            auto absent = acceptedBindings();
            absent[0].entryPoint = "vanish";
            auto const authored = AuthoredProject{
                tools,
                bindingsJson(absent),
                k_pluginSource,
                {std::string{k_sweepEntry}, "vanish"},
            };
            auto const project = authored.load();
            REQUIRE(project.has_value());

            auto       registrar = ProjectGenerationRegistrar{};
            auto const refused   = registerLoaded(
                registrar,
                *project->findDeployment("main"),
                std::make_shared<std::vector<RecordedCall>>()
            );
            INFO(why(refused));
            REQUIRE_FALSE(refused.has_value());
            CHECK(std::string{refused.error().message()}.contains(
                "missing an entry point"
            ));
        }
    }

    // The document's own rules, refused where the document is read.
    TEST_CASE("a project directory states its Tool bindings exactly once")
    {
        auto const tools = toolsJson();

        SUBCASE("one Tool bound twice")
        {
            auto repeated = acceptedBindings();
            repeated.emplace_back(ProjectToolBinding{
                .toolName   = std::string{k_dismissTool},
                .entryPoint = std::string{k_sweepEntry},
            });
            auto const authored =
                AuthoredProject{tools, bindingsJson(repeated), k_pluginSource};
            auto const refused = authored.load();
            INFO(why(refused));
            REQUIRE_FALSE(refused.has_value());
            CHECK(std::string{refused.error().message()}.contains(
                "binds the Tool chaos.project.dismiss twice"
            ));
        }

        SUBCASE("the member left out entirely")
        {
            auto const authored = AuthoredProject{
                tools,
                bindingsJson(acceptedBindings()),
                k_pluginSource,
            };
            REQUIRE(authored.load().has_value());

            // The same directory with `tool_bindings` deleted from its root
            // document. There is no absent form of the member: a deployment
            // that binds nothing writes the empty array.
            auto const document = AuthoredProject::manifest(
                tools,
                bindingsJson(acceptedBindings())
            );
            auto const at = document.find(R"("tool_bindings":)");
            REQUIRE(at != std::string::npos);
            auto const closed = document.find(']', at);
            REQUIRE(closed != std::string::npos);
            auto stripped = document;
            stripped.erase(at, closed + 2U - at);
            authored.rewriteManifest(stripped);

            auto const refused = authored.load();
            INFO(why(refused));
            REQUIRE_FALSE(refused.has_value());
            CHECK(std::string{refused.error().message()}.contains("tool_bindings"));
        }
    }

}
