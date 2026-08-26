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
        return tools.call("chaos.project.sweep", input)
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
        [[nodiscard]]
        auto declaredTool(std::string_view name) -> std::string
        {
            auto tool = std::string{R"json({"argument_schema":)json"};
            tool += umbraflow::k_toolArgumentSchema;
            tool += R"json(,"description":"A fixture Project Tool leaf.",)json";
            tool += R"json("effect_bounds":[],"idempotency":"read_safe",)json";
            tool += R"json("mutability":"read_only","name":")json";
            tool += name;
            tool += R"json(","required_capabilities":[],"surface":"semantic",)json";
            tool += R"json("timeout_policy":{"maximum_elapsed_ms":30000,)json"
                R"json("on_timeout":"stop"},"ui_action_bounds":[],)json";
            tool += R"json("version":"1"})json";
            return tool;
        }

        // The Tools this deployment declares, inline, as the array the block's
        // `tools` member carries.
        [[nodiscard]]
        auto toolsJson() -> std::string
        {
            auto rendered = std::string{"["};
            rendered += declaredTool(k_dismissTool);
            rendered += ",";
            rendered += declaredTool(k_sweepTool);
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
            LoadedDeployment const& deployment
        ) -> Result<operator_runtime::ProjectGenerationHandle>
        {
            return registrar.registerGeneration(
                deployment.generation,
                deployment.toolCatalogSchemaOwner,
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = deployment.toolClosure.entryModule,
                    .modules     = deployment.toolClosure.modules,
                },
                deployment.projectResources
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

        auto const dismiss =
            p_deployment->toolCatalogSchemaOwner.describe(k_dismissTool);
        REQUIRE(dismiss.has_value());
        auto const sweep = p_deployment->toolCatalogSchemaOwner.describe(k_sweepTool);
        REQUIRE(sweep.has_value());

        auto       registrar = ProjectGenerationRegistrar{};
        auto const loaded    = registerLoaded(registrar, *p_deployment);
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
            .callIdentity = hashOf("authored-run-position"),
            .budgetOwner  = std::string{k_dismissTool},
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

        auto const dismissed = loaded->invokeBoundTool(
            k_dismissTool,
            json::Value::ofObject({{"value", json::Value::ofNumber(1)}}),
            request
        );
        INFO(why(dismissed));
        REQUIRE_FALSE(dismissed.has_value());
        CHECK_MESSAGE(
            std::string{dismissed.error().message()}.contains(
                "Project Tool handler chaos.project.dismiss may not issue Tool call chaos.project.sweep"
            ),
            "Project Tool handler chaos.project.dismiss must refuse Tool call chaos.project.sweep by name"
        );
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

    TEST_CASE("a Project Tool declaration carrying child_effects is refused by name")
    {
        auto edited = toolsJson();
        auto const at = edited.find(R"("description")");
        REQUIRE(at != std::string::npos);
        edited.insert(
            at,
            R"json("child_effects":{"child_tool_names":[],"maximum_child_calls":0},)json"
        );
        auto const authored = AuthoredProject{
            edited,
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto const refused = authored.load();
        INFO(why(refused));
        auto const named = !refused.has_value()
            && std::string{refused.error().message()}.contains("child_effects");
        CHECK_MESSAGE(
            named,
            "Project Tool declaration must refuse child_effects by name"
        );
    }

    TEST_CASE("a Project Tool declaration has no body member")
    {
        for (auto const value : {std::string_view{"false"}, std::string_view{"true"}})
        {
            auto withBody = toolsJson();
            auto const member = std::string{R"json("body":)json"} + std::string{value}
                + ",";
            auto const at = withBody.find(R"json("description")json");
            REQUIRE(at != std::string::npos);
            withBody.insert(at, member);

            auto const authored = AuthoredProject{
                withBody,
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
            auto const refused =
                registerLoaded(registrar, *project->findDeployment("main"));
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
            auto const refused =
                registerLoaded(registrar, *project->findDeployment("main"));
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
            auto const refused =
                registerLoaded(registrar, *project->findDeployment("main"));
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
