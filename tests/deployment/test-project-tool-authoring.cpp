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
// So these cases start from bytes a project author writes: umbraflow-project.json
// naming a deployment and its tool_bindings, a Tool Catalog rendered by the
// offline kit's own generator, and a Luau closure on disk. loadProductionProject
// derives the registration from those bytes, and the derived registration is
// what the registrar is handed. Nothing between the author and the compiled
// program is written by a test.
//
// All of it stays production-unreachable: ProductLifecycle registers no Project
// Tool program, and these cases are the only callers.

#include "umbraflow/project-schemas.hpp"

#include <deployment/project-directory.hpp>

#include <project/tool-catalog.hpp>

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

        using operator_runtime::ChildEffectDeclaration;
                using operator_runtime::ProjectToolBinding;
        using operator_runtime::ProjectGenerationRegistrar;
        using operator_runtime::Risk;
        using operator_runtime::TimeoutAction;
        using operator_runtime::TimeoutPolicy;
        using operator_runtime::ToolDescriptor;
        using operator_runtime::ToolIdempotency;
        using operator_runtime::ToolMutability;
        using operator_runtime::ToolSurface;
        using operator_runtime::WorkflowLimits;

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

        // The reducer closure this project ships: `plugin_id` and one entry,
        // and no reach for a scoped module. A generation's reducer is compiled
        // on the pure type, whose resolver would refuse @umbraflow/tools by
        // name, so the fold cannot live beside the entries below.
        constexpr auto k_reducerSource = std::string_view{R"LUAU(
return {
    plugin_id = "chaos.project",
    reduce = function(input)
        return input
    end,
}
)LUAU"};

        // Two entries, one closure. `dismiss` reaches the Tool Runtime through
        // the scoped facade -- which is loadable only because the registrar
        // baked the pinned catalog resource -- and `sweep` answers from the
        // frozen discovery table without spending a call. Both answer the
        // shape their catalog entry's result_schema declares.
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

        // The ordinary limits every tool below declares. Nothing in these cases
        // turns on them; they are here because a catalog entry that omitted one
        // is a catalog the generator refuses.
        [[nodiscard]] auto ordinaryLimits() -> WorkflowLimits
        {
            return WorkflowLimits{
                .maximumSteps        = 4,
                .maximumDispatches   = 4,
                .maximumObservations = 16,
                .maximumWaits        = 4,
                .maximumElapsedMillis = 60'000,
            };
        }

        [[nodiscard]] auto ordinaryTimeout() -> TimeoutPolicy
        {
            return TimeoutPolicy{
                .maximumElapsedMillis = 30'000,
                .onTimeout            = TimeoutAction::Stop,
            };
        }

        // The empty declaration: no name, no call, and the most restricted
        // ceiling of each kind. It is what a tool that issues no child call
        // states, and it is written out rather than left out.
        [[nodiscard]] auto noChildCalls() -> ChildEffectDeclaration
        {
            return ChildEffectDeclaration{};
        }

        [[nodiscard]]
        auto declaredTool(
            std::string_view name,
            ChildEffectDeclaration childEffects
        ) -> project::DeclaredTool
        {
            return project::DeclaredTool{
                .name           = std::string{name},
                .argumentSchema = "FixtureArguments",
                .resultSchema   = "FixtureResult",
                .descriptor     = ToolDescriptor{
                    .toolVersion  = "1",
                    .childEffects = std::move(childEffects),
                    .limits       = ordinaryLimits(),
                    .timeout      = ordinaryTimeout(),
                    .mutability   = ToolMutability::ReadOnly,
                    .surface      = ToolSurface::Semantic,
                    .idempotency  = ToolIdempotency::ReadSafe,
                },
            };
        }

        // The catalog this project authors. `chaos.project.dismiss` declares a real
        // child effect set, because the whole point of giving child_effects a
        // wire spelling is that a project can state one; `chaos.project.sweep` declares
        // the empty one.
        [[nodiscard]]
        auto catalogDeclaration(uint32 maximumChildCalls = 2U)
            -> project::ToolCatalogDeclaration
        {
            auto delegating = ChildEffectDeclaration{
                .childToolNames         = {std::string{k_sweepTool}},
                .maximumChildSurface    = ToolSurface::Semantic,
                .maximumChildMutability = ToolMutability::ReadOnly,
                .maximumChildRisk       = Risk::Low,
                .maximumChildCalls      = maximumChildCalls,
            };
            return project::ToolCatalogDeclaration{
                .comment  = "The catalog this deployment authors, child effects "
                            "and all.",
                .pluginId                   = std::string{k_pluginId},
                .toolPreconditionSchemaHash = hashOf(umbraflow::k_toolPreconditionSchema),
                .effectPayloadSchemaHashes  = {},
                .tools = {
                    declaredTool(k_dismissTool, std::move(delegating)),
                    declaredTool(k_sweepTool, noChildCalls()),
                },
            };
        }

        [[nodiscard]]
        auto generatedCatalog(project::ToolCatalogDeclaration const& declaration)
            -> std::string
        {
            auto rendered = project::generateToolCatalog(declaration);
            auto const why = rendered.has_value()
                ? std::string{}
                : std::string{rendered.error().message()};
            INFO(why);
            REQUIRE(rendered.has_value());
            return *std::move(rendered);
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
                std::string_view toolCatalog,
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

                auto const bundle = umbraflow::DeploymentBundle{k_pluginId};
                write(m_root / "schema/state.json", umbraflow::k_projectStateSchema);
                write(
                    m_root / "schema/precondition.json",
                    umbraflow::k_toolPreconditionSchema
                );
                write(m_root / "schema/catalog.json", toolCatalog);
                write(
                    m_root / "schema/journal-manifest.json",
                    bundle.journalEventManifest()
                );
                for (auto index = std::size_t{0};
                     index < umbraflow::k_journalPayloadSchemas.size();
                     ++index)
                {
                    write(
                        m_root
                            / ("schema/journal-" + std::to_string(index) + ".json"),
                        umbraflow::k_journalPayloadSchemas.at(index)
                    );
                }
                write(m_root / "plugin/main.luau", pluginSource);
                write(m_root / "plugin/reducer.luau", k_reducerSource);
                write(m_root / "runtime/artifact/runtime-model.toml", "[[page]]\n");
                write(
                    m_root / "umbraflow-project.json",
                    manifest(toolBindings, declaredEntryPoints)
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
                std::string_view toolBindings,
                std::vector<std::string> const& declaredEntryPoints
                    = exportedEntryPoints()
            ) -> std::string
            {
                auto document =
                    std::string{R"json({"schema":"umbraflow-project/v2",)json"};
                document += R"json("runtime_artifact":"runtime/artifact",)json";
                document += R"json("primary_deployment":"main",)json";
                document += R"json("template_cuts":[],"deployments":[{)json";
                document += R"json("name":"main","plugin_id":")json";
                document += k_pluginId;
                document += R"json(","baseline_event_type":"fixture.baseline",)json";
                document += R"json("reducer_closure":{"entry":"main",)json"
                    R"json("exported_entry_points":["reduce"],"modules":)json"
                    R"json([{"name":"main","path":"plugin/reducer.luau"}]},)json";
                document += R"json("tool_closure":{"entry":"main",)json"
                    R"json("exported_entry_points":)json";
                document += entryPointsJson(declaredEntryPoints);
                document += R"json(,"modules":)json"
                    R"json([{"name":"main","path":"plugin/main.luau"}]},)json";
                document += R"json("plugin_authoring":"hand-written",)json";
                document += R"json("plugin_justification":"This deployment's )json"
                    R"json(entries are Tool handlers reached through the scoped )json"
                    R"json(Tool Runtime, which umbraflow-declarative-workflow-)json"
                    R"json(tool/v1 has no member for at all.",)json";
                document += R"json("project_state_schema":"schema/state.json",)json";
                document += R"json("tool_precondition_schema":)json"
                    R"json("schema/precondition.json",)json";
                document += R"json("tool_catalog":"schema/catalog.json",)json";
                document += R"json("journal_event_schema_manifest":)json"
                    R"json("schema/journal-manifest.json",)json";
                document += R"json("journal_payload_schemas":[)json";
                for (auto index = std::size_t{0};
                     index < umbraflow::k_journalPayloadSchemas.size();
                     ++index)
                {
                    document += index == 0U ? "" : ",";
                    document += R"json("schema/journal-)json"
                        + std::to_string(index) + R"json(.json")json";
                }
                document += R"json(],"effect_payload_schemas":[],)json";
                document += R"json("observed_instance_identity_schemas":[],)json";
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
            -> script::ToolRuntimeInvoke
        {
            return [p_calls = std::move(p_calls)](
                       std::string_view toolName,
                       json::Value const&,
                       script::ToolCallCoordinate const& coordinate,
                       std::stop_token
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
        // moved when the declaration became an authored member. The result
        // validator is the deployment's own, minted from the pinned tool
        // precondition schema and the catalog that names a definition inside
        // it -- not a lambda that accepts.
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
                deployment.schemaOwner,
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = deployment.reducerClosure.entryModule,
                    .modules     = deployment.reducerClosure.modules,
                },
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = deployment.toolClosure.entryModule,
                    .modules     = deployment.toolClosure.modules,
                },
                deployment.projectResources,
                deployment.catalog.toolResultValidator(),
                recordingRuntime(std::move(p_calls))
            );
        }
    }

    // The whole path, once: authored bytes in, one compiled program out.
    TEST_CASE("an authored project loads as a Project Tool program")
    {
        auto const authored = AuthoredProject{
            generatedCatalog(catalogDeclaration()),
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

        // And the descriptor carries the child effect declaration the catalog
        // document spelled, which is the half of this cut that had no wire
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
            .cancellation   = {},
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

        // The answer is judged against the definition this tool's own
        // result_schema names, by a validator the DEPLOYMENT minted. An
        // accept-all lambda would make both of these pass.
        CHECK(
            loaded->validateToolResult(k_sweepTool, R"({"outcome":"swept"})")
                .has_value()
        );
        CHECK_FALSE(
            loaded->validateToolResult(k_sweepTool, R"({"verdict":"swept"})")
                .has_value()
        );
    }

    // The binding is inside the registration root, which is what stops a
    // re-registration rebinding a Tool to different code with no detectable
    // identity change. Two directories differing in one entry_point and in
    // nothing else must derive two registration hashes.
    TEST_CASE("rebinding a Tool moves the registration root and not the catalog")
    {
        auto const catalog = generatedCatalog(catalogDeclaration());
        auto const asDeclared = AuthoredProject{
            catalog,
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto swapped = acceptedBindings();
        swapped[0].entryPoint = std::string{k_sweepEntry};
        auto const asRebound =
            AuthoredProject{catalog, bindingsJson(swapped), k_pluginSource};

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
            generatedCatalog(catalogDeclaration(2U)),
            bindingsJson(acceptedBindings()),
            k_pluginSource,
        };
        auto const wide = AuthoredProject{
            generatedCatalog(catalogDeclaration(64U)),
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
    // one permission contradicting each other. The kit refuses to render it,
    // and a document that carries it anyway is refused when the catalog
    // authority is built -- which is at load, before any program exists.
    TEST_CASE("an incoherent child effect declaration is refused where it is written")
    {
        auto incoherent = catalogDeclaration();
        incoherent.tools[0].descriptor.childEffects.maximumChildCalls = 0U;
        auto const rendered = project::generateToolCatalog(incoherent);
        REQUIRE_FALSE(rendered.has_value());
        CHECK(
            std::string{rendered.error().message()}.contains(
                "names a child tool but admits no child call"
            )
        );

        // The same contradiction, written straight into the document rather
        // than through the generator, so that the loader's own refusal is
        // exercised and not merely the kit's.
        auto edited = generatedCatalog(catalogDeclaration());
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
            std::string{refused.error().message()}.contains("child_effects")
        );
    }

    // There is no absent form of child_effects. A tool that issues no child
    // call writes the empty declaration; a tool that writes nothing is a
    // document this framework does not read, rather than one whose silence
    // means the empty declaration.
    TEST_CASE("a Tool Catalog omitting child_effects is refused, not defaulted")
    {
        auto stripped = generatedCatalog(catalogDeclaration());
        constexpr auto k_empty = std::string_view{
            R"("child_effects":{"child_tool_names":[],"maximum_child_calls":0,)"
            R"("maximum_child_mutability":"read_only",)"
            R"("maximum_child_risk":"read_only",)"
            R"("maximum_child_surface":"semantic"},)"
        };
        auto const at = stripped.find(k_empty);
        REQUIRE(at != std::string::npos);
        stripped.erase(at, k_empty.size());

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

    // The three bind-time refusals, each reached from an authored document
    // rather than from a binding table built in C++.
    TEST_CASE("the loader refuses an authored contract its code disagrees with")
    {
        auto const catalog = generatedCatalog(catalogDeclaration());

        SUBCASE("a declared Tool with no binding")
        {
            auto only = acceptedBindings();
            only.pop_back();
            auto const authored = AuthoredProject{
                catalog,
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
                AuthoredProject{catalog, bindingsJson(extra), k_pluginSource};
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
                catalog,
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
        auto const catalog = generatedCatalog(catalogDeclaration());

        SUBCASE("one Tool bound twice")
        {
            auto repeated = acceptedBindings();
            repeated.emplace_back(ProjectToolBinding{
                .toolName   = std::string{k_dismissTool},
                .entryPoint = std::string{k_sweepEntry},
            });
            auto const authored =
                AuthoredProject{catalog, bindingsJson(repeated), k_pluginSource};
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
                catalog,
                bindingsJson(acceptedBindings()),
                k_pluginSource,
            };
            REQUIRE(authored.load().has_value());

            // The same directory with `tool_bindings` deleted from its root
            // document. There is no absent form of the member: a deployment
            // that binds nothing writes the empty array.
            auto const document =
                AuthoredProject::manifest(bindingsJson(acceptedBindings()));
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
