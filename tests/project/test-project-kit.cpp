#include <project/project-kit.hpp>
#include <project/declarative-workflow-tool.hpp>

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/error/error.hpp>

#include <domain/content-hash.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        class TemporaryWorkspace final
        {
            std::filesystem::path m_path;

        public:
            explicit TemporaryWorkspace(std::string_view label)
                : m_path{std::filesystem::temp_directory_path() / label}
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
                REQUIRE(std::filesystem::create_directories(source(), error));
            }

            TemporaryWorkspace(TemporaryWorkspace const&)                    = delete;
            TemporaryWorkspace(TemporaryWorkspace&&)                         = delete;
            auto operator=(TemporaryWorkspace const&) -> TemporaryWorkspace& = delete;
            auto operator=(TemporaryWorkspace&&) -> TemporaryWorkspace&      = delete;

            ~TemporaryWorkspace()
            {
                auto error = std::error_code{};
                auto iterator = std::filesystem::recursive_directory_iterator{
                    m_path,
                    std::filesystem::directory_options::skip_permission_denied,
                    error,
                };
                auto const end = std::filesystem::recursive_directory_iterator{};
                for (; !error && iterator != end; iterator.increment(error))
                {
                    std::filesystem::permissions(
                        iterator->path(),
                        std::filesystem::perms::owner_all,
                        std::filesystem::perm_options::add,
                        error
                    );
                }
                error = std::error_code{};
                std::filesystem::permissions(
                    m_path,
                    std::filesystem::perms::owner_all,
                    std::filesystem::perm_options::add,
                    error
                );
                error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
            }

            [[nodiscard]] auto source() const -> std::filesystem::path
            {
                return m_path / "source";
            }

            [[nodiscard]] auto build() const -> std::filesystem::path
            {
                return m_path / "build";
            }

            [[nodiscard]] auto releases() const -> std::filesystem::path
            {
                return m_path / "releases";
            }
        };

        template <typename Value>
        [[nodiscard]]
        auto messageOf(Result<Value> const& result) -> std::string
        {
            if (result.has_value())
            {
                return std::string{};
            }
            return std::string{result.error().message()};
        }

        auto writeFile(
            std::filesystem::path const& path,
            std::string_view text
        ) -> void
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(
                path.parent_path(),
                error
            );
            REQUIRE_FALSE(error);

            auto stream = std::ofstream{
                path,
                std::ios::binary | std::ios::trunc
            };
            REQUIRE(stream.is_open());
            stream << text;
            REQUIRE(stream.good());
        }

        [[nodiscard]]
        auto snapshotTree(
            std::filesystem::path const& root
        ) -> std::map<std::string, std::string>
        {
            auto snapshot = std::map<std::string, std::string>{};
            for (auto const& entry : std::filesystem::recursive_directory_iterator{root})
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }

                auto stream = std::ifstream{entry.path(), std::ios::binary};
                REQUIRE(stream.is_open());
                auto const bytes = std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{}
                };
                snapshot.emplace(
                    entry.path().lexically_relative(root).generic_string(),
                    bytes
                );
            }
            return snapshot;
        }

        [[nodiscard]]
        auto initializedWorkspace(
            TemporaryWorkspace const& workspace
        ) -> Status
        {
            writeFile(workspace.source() / "content" / "facts.txt", "facts\n");
            writeFile(workspace.source() / "decisions.txt", "decisions\n");
            return initProject(
                ProjectInitSpec{
                    .sourceDirectory = workspace.source(),
                    .buildDirectory  = workspace.build(),
                    .inputs          = {
                        "decisions.txt",
                        "content/facts.txt",
                    },
                }
            );
        }

        inline constexpr auto k_workflowEntryPoints = std::array{
            std::string_view{"derive"},
            std::string_view{"plan"},
            std::string_view{"next_step"},
            std::string_view{"reconcile"},
            std::string_view{"reduce"},
        };

        inline constexpr auto k_observedInstanceId = std::string_view{
            "oi1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        };
        inline constexpr auto k_workflowDeclarationInput = std::string_view{
            "declarative-tools/chaos.project/dismiss-known-overlay.json"
        };
        inline constexpr auto k_generatedWorkflowAdapter = std::string_view{
            "generated/adapters/chaos.project/dismiss-known-overlay.luau"
        };
        inline constexpr auto k_generatedToolCatalog = std::string_view{
            "generated/tool-catalogs/chaos.project/tool-catalog-v1.json"
        };
        inline constexpr auto k_generatedFrameworkSchemaCatalog = std::string_view{
            "generated/framework-schemas/framework-schema-catalog-v1.json"
        };

        [[nodiscard]]
        auto validWorkflowDeclaration() -> std::string
        {
            return R"json({
  "schema": "umbraflow-declarative-workflow-tool/v1",
  "tool_name": "chaos.dismiss_known_overlay",
  "target_argument": "observed_instance_id",
  "allowed_instance_kinds": ["chaos.overlay"],
  "fresh_observation": {
    "required_surface": "chaos.overlay_layer",
    "require_unambiguous": true
  },
  "ui_finding": {"kind": "observed_instance_absent"},
  "states": [
    {
      "state_key": "await-overlay",
      "kind": "wait",
      "observation_budget": 1,
      "timeout_ms": 1000
    },
    {
      "state_key": "dismiss-overlay",
      "kind": "ui_action",
      "ui_action": "chaos.ui.dismiss_overlay",
      "timeout_ms": 2000
    }
  ],
  "steps": ["await-overlay", "dismiss-overlay"],
  "bounds": {
    "maximum_states": 2,
    "maximum_steps": 2,
    "maximum_dispatches": 1,
    "maximum_observations": 2,
    "maximum_waits": 1,
    "maximum_elapsed_ms": 3000
  }
})json";
        }

        [[nodiscard]]
        auto initializedWorkflowWorkspace(
            TemporaryWorkspace const& workspace
        ) -> Status
        {
            writeFile(
                workspace.source() / k_workflowDeclarationInput,
                validWorkflowDeclaration()
            );
            return initProject(
                ProjectInitSpec{
                    .sourceDirectory = workspace.source(),
                    .buildDirectory  = workspace.build(),
                    .inputs          = {
                        std::filesystem::path{k_workflowDeclarationInput},
                    },
                }
            );
        }

        inline constexpr auto k_deploymentManifestInput = std::string_view{
            "umbraflow-project.json"
        };
        inline constexpr auto k_handWrittenPlugin = std::string_view{
            "plugin/dream.luau"
        };

        // The deployment manifest as a project author writes it, with the one
        // member under test spliced in exactly as given. The whole document is
        // written rather than the three members the gate reads, so a fixture
        // that stopped being a manifest could not go on satisfying the gate.
        [[nodiscard]]
        auto deploymentManifest(std::string_view justificationMember) -> std::string
        {
            return std::string{R"json({
  "schema": "umbraflow-project/v1",
  "runtime_artifact": "runtime/artifact",
  "primary_deployment": "dream",
  "deployments": [
    {
      "name": "dream",
      "plugin_id": "chaos.dream",
      "baseline_event_type": "project.baseline_created",
      "plugin": "plugin/dream.luau",
)json"}
                + std::string{justificationMember}
                + R"json(      "project_state_schema": "schema/state.json",
      "project_observation_schema": "schema/observation.json",
      "tool_precondition_schema": "schema/precondition.json",
      "reconcile_schema": "schema/reconcile.json",
      "tool_catalog": "schema/catalog.json",
      "journal_event_schema_manifest": "schema/journal-manifest.json",
      "reconcile_manifest": "schema/reconcile-manifest.json",
      "journal_payload_schemas": ["schema/journal-0.json"],
      "effect_payload_schemas": [],
      "artifact_blobs": []
    }
  ]
})json";
        }

        [[nodiscard]]
        auto initializedDeploymentWorkspace(
            TemporaryWorkspace const& workspace,
            std::string_view justificationMember
        ) -> Status
        {
            writeFile(
                workspace.source() / k_deploymentManifestInput,
                deploymentManifest(justificationMember)
            );
            return initProject(
                ProjectInitSpec{
                    .sourceDirectory = workspace.source(),
                    .buildDirectory  = workspace.build(),
                    .inputs          = {
                        std::filesystem::path{k_deploymentManifestInput},
                    },
                }
            );
        }

        [[nodiscard]]
        auto requiredHash(std::string_view text) -> ContentHash
        {
            auto const digest = sha256(std::as_bytes(std::span{text}));
            REQUIRE_MESSAGE(digest.has_value(), messageOf(digest));
            return *digest;
        }

        [[nodiscard]]
        auto toolCatalogDeclaration() -> ToolCatalogDeclaration
        {
            auto const payloadHash = requiredHash("effect payload schema");
            return ToolCatalogDeclaration{
                .comment  = "A generated catalog fixture.",
                .pluginId = "chaos.project",
                .toolPreconditionSchemaHash = requiredHash(
                    "tool precondition schema"
                ),
                .effectPayloadSchemaHashes = {payloadHash},
                .tools = {
                    DeclaredTool{
                        .name           = "chaos.dismiss_known_overlay",
                        .argumentSchema = "DismissArguments",
                        .descriptor     = operator_runtime::ToolDescriptor{
                            .toolVersion          = "7",
                            .requiredCapabilities = {"overlay"},
                            .effectBounds = {
                                operator_runtime::EffectBound{
                                    .namespacedType    = "chaos.dismissed",
                                    .scopeKind         = "overlay",
                                    .payloadSchemaHash = payloadHash,
                                    .maximumRisk       = operator_runtime::Risk::Medium,
                                },
                            },
                            .uiActionBounds = {"chaos.ui.dismiss_overlay"},
                            .limits = operator_runtime::WorkflowLimits{
                                .maximumSteps        = 1,
                                .maximumDispatches   = 1,
                                .maximumObservations = 3,
                                .maximumWaits        = 0,
                                .maximumElapsedMillis = 3'000,
                            },
                            .timeout = operator_runtime::TimeoutPolicy{
                                .maximumElapsedMillis = 3'000,
                                .onTimeout = operator_runtime::TimeoutAction::Reconcile,
                            },
                            .mutability = operator_runtime::ToolMutability::Mutating,
                            .surface    = operator_runtime::ToolSurface::Semantic,
                            .idempotency =
                                operator_runtime::ToolIdempotency::DeliverySafe,
                        },
                    },
                },
            };
        }

        [[nodiscard]]
        auto replacedOnce(
            std::string input,
            std::string_view before,
            std::string_view after
        ) -> std::string
        {
            auto const position = input.find(before);
            REQUIRE_MESSAGE(
                position != std::string::npos,
                "workflow vector mutation must name existing source bytes"
            );
            input.replace(position, before.size(), after);
            return input;
        }

        [[nodiscard]]
        auto generatedWorkflowProgram() -> script::PureDataProgram
        {
            auto const generated = generateDeclarativeWorkflowAdapter(
                "chaos.project",
                validWorkflowDeclaration()
            );
            auto const generatedMessage = (
                generated.has_value()
                    ? std::string{}
                    : std::string{generated.error().message()}
            );
            REQUIRE_MESSAGE(
                generated.has_value(),
                generatedMessage
            );
            auto compiled = script::PureDataProgram::compile(
                "chaos.project",
                *generated,
                k_workflowEntryPoints,
                {}
            );
            auto const compiledMessage = (
                compiled.has_value()
                    ? std::string{}
                    : std::string{
                          "generated adapter must expose only the derive, plan, "
                          "next_step, reconcile and reduce SPI: "
                      }
                        + std::string{compiled.error().message()}
            );
            REQUIRE_MESSAGE(
                compiled.has_value(),
                compiledMessage
            );
            return *std::move(compiled);
        }

        [[nodiscard]]
        auto parsedJson(std::string_view text) -> json::Value
        {
            auto parsed = json::parse(text);
            auto const parsedMessage = (
                parsed.has_value()
                    ? std::string{}
                    : std::string{parsed.error().message()}
            );
            REQUIRE_MESSAGE(
                parsed.has_value(),
                parsedMessage
            );
            return *std::move(parsed);
        }

        [[nodiscard]]
        auto observation(
            bool includesTarget,
            std::string_view targetKind = "chaos.overlay",
            bool surfaceFresh = true,
            std::string_view surfaceResolution = "resolved",
            bool surfaceUnambiguous = true
        ) -> std::string
        {
            auto text = std::string{
                R"json({"canonical_opaque_payload":{"surface_observations":[{"fresh":)json"
            };
            text += surfaceFresh ? "true" : "false";
            text += R"json(,"resolution":")json";
            text += surfaceResolution;
            text += R"json(","surface_id":"chaos.overlay_layer","unambiguous":)json";
            text += surfaceUnambiguous ? "true" : "false";
            text += R"json(}]},"observed_instances":)json";
            if (includesTarget)
            {
                text += R"json([{"kind":")json";
                text += targetKind;
                text += R"json(","observed_instance_id":")json";
                text += k_observedInstanceId;
                text += R"json("}])json";
            }
            else
            {
                text += "[]";
            }
            text += '}';
            return text;
        }

        [[nodiscard]]
        auto adapterInput(
            std::string_view observationBytes,
            bool includeTool
        ) -> json::Value
        {
            auto text = std::string{R"json({"canonical_args":{"observed_instance_id":")json"};
            text += k_observedInstanceId;
            text += R"json("},"project_observation":)json";
            text += observationBytes;
            if (includeTool)
            {
                text += R"json(,"tool_name":"chaos.dismiss_known_overlay","tool_version":"1")json";
            }
            text += '}';
            return parsedJson(text);
        }

        [[nodiscard]]
        auto invoked(
            script::PureDataProgram const& program,
            std::string_view entryPoint,
            json::Value const& input
        ) -> json::Value
        {
            auto result = program.invoke(entryPoint, input);
            auto const resultMessage = (
                result.has_value()
                    ? std::string{}
                    : std::string{result.error().message()}
            );
            REQUIRE_MESSAGE(
                result.has_value(),
                resultMessage
            );
            return *std::move(result);
        }
    }

    TEST_CASE("project init records canonical declared inputs outside the source tree")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-init"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

        auto const sourceSnapshot = snapshotTree(workspace.source());
        REQUIRE_FALSE_MESSAGE(
            sourceSnapshot.contains(std::string{k_inputManifestName}),
            "project init must not write its input manifest into the source tree"
        );

        auto const snapshot = snapshotTree(workspace.build());
        REQUIRE_MESSAGE(
            snapshot.contains(std::string{k_inputManifestName}),
            "project init must write its input manifest into the build directory"
        );
        CHECK_MESSAGE(
            snapshot.at(std::string{k_inputManifestName})
            == "umbraflow-project-kit-inputs-v1\n"
               "content/facts.txt\n"
               "decisions.txt\n",
            "project init must record declared inputs in canonical sorted order"
        );
    }

    TEST_CASE("project build changes no source bytes and writes its receipt under build")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-build-boundary"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const sourceBefore = snapshotTree(workspace.source());

        auto const built = buildProject(
            ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
                .toolCatalogs    = {},
            },
            {}
        );
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        REQUIRE_MESSAGE(
            snapshotTree(workspace.source()) == sourceBefore,
            "project build must not change any source file"
        );
        CHECK_MESSAGE(
            std::filesystem::is_regular_file(
                workspace.build() / k_buildReceiptName
            ),
            "project build must write its receipt under the build directory"
        );
    }

    TEST_CASE("project build cuts a declared template from content hashes")
    {
        auto const workspace = TemporaryWorkspace{
            "uf-project-template-cut"
        };
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const firstPixels = std::vector<std::byte>{
            std::byte{180}, std::byte{180}, std::byte{180}, std::byte{1},
            std::byte{20}, std::byte{20}, std::byte{20}, std::byte{2},
        };
        auto const secondPixels = std::vector<std::byte>{
            std::byte{180}, std::byte{180}, std::byte{180}, std::byte{3},
            std::byte{220}, std::byte{220}, std::byte{220}, std::byte{4},
        };
        auto const firstEncoded = image::encodeRgbaPng(
            "first-source",
            2,
            1,
            firstPixels
        );
        auto const secondEncoded = image::encodeRgbaPng(
            "second-source",
            2,
            1,
            secondPixels
        );
        REQUIRE(firstEncoded.has_value());
        REQUIRE(secondEncoded.has_value());
        auto const firstHash  = sha256(*firstEncoded);
        auto const secondHash = sha256(*secondEncoded);
        REQUIRE(firstHash.has_value());
        REQUIRE(secondHash.has_value());
        auto const rect = PixelRect::create(0, 0, 2, 1);
        REQUIRE(rect.has_value());

        auto const resolver = TemplateSourceResolver{
            [
                first = *firstHash,
                second = *secondHash,
                firstBytes = *firstEncoded,
                secondBytes = *secondEncoded
            ](ContentHash const& requested) -> Result<std::vector<std::byte>>
            {
                if (requested == first)
                {
                    return firstBytes;
                }
                if (requested == second)
                {
                    return secondBytes;
                }
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "unexpected template source hash"
                );
            }
        };
        auto const spec = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
            .toolCatalogs    = {},
            .templateCuts = {
                ProjectTemplateCutSpec{
                    .templatePath = "locator/mark.png",
                    .sourceHashes = {*firstHash, *secondHash},
                    .rect         = *rect,
                },
            },
        };

        auto const built = buildProject(spec, resolver);
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));
        auto const snapshot = snapshotTree(workspace.build());
        auto const artifact = std::string{
            "generated/templates/locator/mark.png"
        };
        REQUIRE(snapshot.contains(artifact));
        auto const& encodedTemplate = snapshot.at(artifact);
        auto const decoded = image::decodePng(
            std::as_bytes(std::span{encodedTemplate}),
            artifact
        );
        REQUIRE(decoded.has_value());
        CHECK(decoded->width == 2U);
        CHECK(decoded->height == 1U);
        CHECK(decoded->pixels.at(3) == std::byte{255});
        CHECK(decoded->pixels.at(7) == std::byte{0});
        CHECK(checkProject(spec, resolver).has_value());
    }

    TEST_CASE("project build regenerates five-function adapters solely from declared source")
    {
        auto const workspace = TemporaryWorkspace{
            "uf-project-workflow-generation"
        };
        auto const initialized = initializedWorkflowWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const sourceBefore = snapshotTree(workspace.source());
        auto const directories  = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
            .toolCatalogs    = {},
        };

        auto const built = buildProject(directories, {});
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));
        REQUIRE_MESSAGE(
            snapshotTree(workspace.source()) == sourceBefore,
            "adapter generation must not change its declared source"
        );

        auto const expected = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            validWorkflowDeclaration()
        );
        auto const expectedMessage = (
            expected.has_value()
                ? std::string{}
                : std::string{expected.error().message()}
        );
        REQUIRE_MESSAGE(
            expected.has_value(),
            expectedMessage
        );
        auto snapshot = snapshotTree(workspace.build());
        REQUIRE_MESSAGE(
            snapshot.contains(std::string{k_generatedWorkflowAdapter}),
            "project build must generate the named workflow adapter"
        );
        CHECK_MESSAGE(
            snapshot.at(std::string{k_generatedWorkflowAdapter}) == *expected,
            "generated adapter bytes must come from the declared source"
        );

        writeFile(
            workspace.build() / k_generatedWorkflowAdapter,
            "hand edited\n"
        );
        auto const rebuilt = buildProject(directories, {});
        REQUIRE_MESSAGE(rebuilt.has_value(), messageOf(rebuilt));
        snapshot = snapshotTree(workspace.build());
        CHECK_MESSAGE(
            snapshot.at(std::string{k_generatedWorkflowAdapter}) == *expected,
            "a generated adapter must never become the next build's input"
        );
    }

    TEST_CASE("project build regenerates Tool Catalogs solely from declared tools")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-tool-catalog"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const declaration = toolCatalogDeclaration();
        auto const sourceBefore = snapshotTree(workspace.source());
        auto const spec = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
            .toolCatalogs    = {declaration},
        };

        auto const built = buildProject(spec, {});
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));
        REQUIRE_MESSAGE(
            snapshotTree(workspace.source()) == sourceBefore,
            "Tool Catalog generation must not change declared source"
        );
        auto snapshot = snapshotTree(workspace.build());
        REQUIRE_MESSAGE(
            snapshot.contains(std::string{k_generatedToolCatalog}),
            "project build must generate the named Tool Catalog"
        );
        auto const& catalog = snapshot.at(std::string{k_generatedToolCatalog});
        auto const document = parsedJson(catalog);
        auto const* schema  = document.find("schema");
        REQUIRE_MESSAGE(
            schema != nullptr,
            "generated Tool Catalog must carry its wire schema"
        );
        CHECK_MESSAGE(
            schema->string() == "umbraflow-tool-catalog/v1",
            "generated Tool Catalog must identify the v1 wire schema"
        );

        auto const* pluginId = document.find("plugin_id");
        REQUIRE_MESSAGE(
            pluginId != nullptr,
            "generated Tool Catalog must carry its declared plugin id"
        );
        CHECK_MESSAGE(
            pluginId->string() == "chaos.project",
            "generated Tool Catalog must render its declared plugin id"
        );

        auto const* tools = document.find("tools");
        REQUIRE_MESSAGE(
            tools != nullptr,
            "generated Tool Catalog must carry its declared tools"
        );
        REQUIRE_MESSAGE(
            tools->items().size() == 1U,
            "generated Tool Catalog must carry exactly its declared tools"
        );
        auto const& tool = tools->items().front();
        auto const* name = tool.find("name");
        REQUIRE_MESSAGE(
            name != nullptr,
            "generated Tool Catalog tool must carry its declared name"
        );
        CHECK_MESSAGE(
            name->string() == "chaos.dismiss_known_overlay",
            "generated Tool Catalog must render its declared tool name"
        );

        auto const* workflowLimits = tool.find("workflow_limits");
        REQUIRE_MESSAGE(
            workflowLimits != nullptr,
            "generated Tool Catalog tool must carry its workflow limits"
        );
        auto const* maximumElapsed = workflowLimits->find("maximum_elapsed_ms");
        REQUIRE_MESSAGE(
            maximumElapsed != nullptr,
            "generated Tool Catalog workflow limits must carry maximum elapsed"
        );
        CHECK_MESSAGE(
            maximumElapsed->number() == 3000.0,
            "generated Tool Catalog must render its declared workflow bound"
        );

        auto const* effectBounds = tool.find("effect_bounds");
        REQUIRE_MESSAGE(
            effectBounds != nullptr,
            "generated Tool Catalog tool must carry its declared effects"
        );
        REQUIRE_MESSAGE(
            effectBounds->items().size() == 1U,
            "generated Tool Catalog tool must carry exactly its declared effects"
        );
        auto const* maximumRisk = effectBounds->items().front().find("maximum_risk");
        REQUIRE_MESSAGE(
            maximumRisk != nullptr,
            "generated Tool Catalog effect must carry its declared risk"
        );
        CHECK_MESSAGE(
            maximumRisk->string() == "medium",
            "generated Tool Catalog must render its declared effect risk"
        );

        auto const* idempotency = tool.find("idempotency");
        REQUIRE_MESSAGE(
            idempotency != nullptr,
            "generated Tool Catalog tool must carry its declared idempotency"
        );
        CHECK_MESSAGE(
            idempotency->string() == "delivery_safe",
            "generated Tool Catalog must render its declared idempotency"
        );
        auto const catalogBefore = catalog;

        writeFile(workspace.build() / k_generatedToolCatalog, "hand edited\n");
        auto const rebuilt = buildProject(spec, {});
        REQUIRE_MESSAGE(rebuilt.has_value(), messageOf(rebuilt));
        snapshot = snapshotTree(workspace.build());
        CHECK_MESSAGE(
            snapshot.at(std::string{k_generatedToolCatalog}) == catalogBefore,
            "a generated Tool Catalog must never become the next build's input"
        );
    }

    TEST_CASE("project check verifies the generated Tool Catalog closure by name")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-catalog-closure"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const spec = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
            .toolCatalogs    = {toolCatalogDeclaration()},
        };
        auto const built = buildProject(spec, {});
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));
        auto const catalogPath = workspace.build() / k_generatedToolCatalog;

        SUBCASE("altered")
        {
            writeFile(catalogPath, "hand edited\n");
            auto const checked = checkProject(spec, {});
            REQUIRE_FALSE_MESSAGE(
                checked.has_value(),
                "project check must reject an altered generated Tool Catalog"
            );
            CHECK_MESSAGE(
                messageOf(checked).find(k_generatedToolCatalog) != std::string::npos,
                "altered-catalog refusal must name its generated artifact"
            );
            CHECK_MESSAGE(
                messageOf(checked).find("does not match its declared source")
                    != std::string::npos,
                "altered-catalog refusal must name the byte mismatch"
            );
        }

        SUBCASE("missing")
        {
            REQUIRE(std::filesystem::remove(catalogPath));
            auto const checked = checkProject(spec, {});
            REQUIRE_FALSE_MESSAGE(
                checked.has_value(),
                "project check must reject a missing generated Tool Catalog"
            );
            CHECK_MESSAGE(
                messageOf(checked).find(k_generatedToolCatalog) != std::string::npos,
                "missing-catalog refusal must name its generated artifact"
            );
            CHECK_MESSAGE(
                messageOf(checked).find("is missing") != std::string::npos,
                "missing-catalog refusal must name the missing property"
            );
        }

        SUBCASE("extra")
        {
            constexpr auto k_extra = std::string_view{
                "generated/tool-catalogs/chaos.project/extra.json"
            };
            writeFile(workspace.build() / k_extra, "{}\n");
            auto const checked = checkProject(spec, {});
            REQUIRE_FALSE_MESSAGE(
                checked.has_value(),
                "project check must reject an extra generated Tool Catalog artifact"
            );
            CHECK_MESSAGE(
                messageOf(checked).find(k_extra) != std::string::npos,
                "extra-catalog refusal must name its generated artifact"
            );
            CHECK_MESSAGE(
                messageOf(checked).find("has no declared source") != std::string::npos,
                "extra-catalog refusal must name the absent declaration"
            );
        }

        SUBCASE("linked")
        {
            auto const secondName = workspace.build() / "catalog-hard-link.json";
            auto error            = std::error_code{};
            std::filesystem::create_hard_link(catalogPath, secondName, error);
            REQUIRE_FALSE(error);
            auto const checked = checkProject(spec, {});
            REQUIRE_FALSE_MESSAGE(
                checked.has_value(),
                "project check must reject a linked generated Tool Catalog"
            );
            CHECK_MESSAGE(
                messageOf(checked).find(k_generatedToolCatalog) != std::string::npos,
                "linked-catalog refusal must name its generated artifact"
            );
            CHECK_MESSAGE(
                messageOf(checked).find("must not be a link") != std::string::npos,
                "linked-catalog refusal must name the link property"
            );
        }
    }

    TEST_CASE("project rebuilds a complete byte-identical artifact set at two paths")
    {
        auto const first  = TemporaryWorkspace{"uf-project-determinism-first"};
        auto const second = TemporaryWorkspace{"uf-project-determinism-second"};
        auto const firstInitialized  = initializedWorkflowWorkspace(first);
        auto const secondInitialized = initializedWorkflowWorkspace(second);
        REQUIRE_MESSAGE(
            firstInitialized.has_value(),
            messageOf(firstInitialized)
        );
        REQUIRE_MESSAGE(
            secondInitialized.has_value(),
            messageOf(secondInitialized)
        );
        auto const firstSpec = ProjectBuildSpec{
            .sourceDirectory = first.source(),
            .buildDirectory  = first.build(),
            .toolCatalogs    = {},
        };
        auto const secondSpec = ProjectBuildSpec{
            .sourceDirectory = second.source(),
            .buildDirectory  = second.build(),
            .toolCatalogs    = {},
        };

        auto const firstBuilt  = buildProject(firstSpec, {});
        auto const secondBuilt = buildProject(secondSpec, {});
        REQUIRE_MESSAGE(firstBuilt.has_value(), messageOf(firstBuilt));
        REQUIRE_MESSAGE(secondBuilt.has_value(), messageOf(secondBuilt));
        auto const firstSnapshot  = snapshotTree(first.build());
        auto const secondSnapshot = snapshotTree(second.build());
        CHECK_MESSAGE(
            firstSnapshot == secondSnapshot,
            "the complete artifact set must be byte-identical at two build paths"
        );

        auto const manifest = parsedJson(firstSnapshot.at(
            std::string{k_artifactManifestName}
        ));
        auto const* inputs    = manifest.find("inputs");
        auto const* artifacts = manifest.find("artifacts");
        REQUIRE(inputs != nullptr);
        REQUIRE(artifacts != nullptr);
        REQUIRE(inputs->items().size() == 1U);
        CHECK_MESSAGE(
            inputs->items().front().find("path")->string()
                == k_workflowDeclarationInput,
            "the hand-written plugin must be pinned as an input"
        );
        for (auto const& artifact : artifacts->items())
        {
            CHECK_MESSAGE(
                artifact.find("path")->string()
                    != k_workflowDeclarationInput,
                "the hand-written plugin must not enter the RuntimeArtifact closure"
            );
        }

        writeFile(
            second.source() / k_workflowDeclarationInput,
            validWorkflowDeclaration() + "\n"
        );
        auto const changedBuilt = buildProject(secondSpec, {});
        REQUIRE_MESSAGE(changedBuilt.has_value(), messageOf(changedBuilt));
        auto const changedSnapshot = snapshotTree(second.build());
        CHECK_MESSAGE(
            changedSnapshot.at(std::string{k_artifactManifestName})
                != firstSnapshot.at(std::string{k_artifactManifestName}),
            "one changed source byte must change the full digest manifest"
        );
    }

    TEST_CASE("project build rejects a registration outside the RuntimeArtifact closure")
    {
        auto const workspace = TemporaryWorkspace{
            "uf-project-artifact-closure-negative"
        };
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const built = buildProject(
            ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
                .toolCatalogs    = {},
                .artifactBlobs = {
                    ProjectArtifactBlobSpec{
                        .name        = "facts",
                        .sourceInput = "content/facts.txt",
                    },
                },
                .registration = ProjectRegistrationBuildSpec{
                    .artifactBlobNames = {"facts", "outside"},
                },
            },
            {}
        );

        REQUIRE_FALSE_MESSAGE(
            built.has_value(),
            "project build must reject a registration outside the closure"
        );
        CHECK_MESSAGE(
            messageOf(built).find("outside the RuntimeArtifact closure")
                != std::string::npos,
            "closure refusal must land on the exact RuntimeArtifact property"
        );
        CHECK_FALSE_MESSAGE(
            std::filesystem::exists(
                workspace.build() / k_artifactManifestName
            ),
            "closure refusal must happen before build artifacts are written"
        );
    }

    // The direct-plugin tier's admission gate, in all three directions. The
    // accepted subcase is not decoration: without it a gate that refused every
    // deployment manifest would satisfy the two refusals and prove nothing.
    //
    // Presence only. No case here states that the text is a true reason, and
    // none can; whether a plugin could have been a declaration instead is
    // program equivalence and stays a review obligation at plugin acceptance
    // (docs/pitfalls/checks-that-cannot-fail.md).
    TEST_CASE("project refuses a hand-written plugin with no stated justification")
    {
        auto const justification = std::string_view{
            R"json(      "plugin_justification": "umbraflow-declarative-workflow-tool/v1 has no member that decides what a Reduce returns.",
)json"
        };

        SUBCASE("absent")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-justification-absent"
            };
            auto const initialized = initializedDeploymentWorkspace(
                workspace,
                ""
            );
            REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
            auto const spec = ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
                .toolCatalogs    = {},
            };

            // CHECK rather than REQUIRE so that neutralizing the gate reports
            // both commands in one run instead of stopping at the first.
            auto const built = buildProject(spec, {});
            CHECK_FALSE_MESSAGE(
                built.has_value(),
                "project build must refuse a hand-written plugin that states "
                "no justification"
            );
            auto const checked = checkProject(spec, {});
            REQUIRE_FALSE_MESSAGE(
                checked.has_value(),
                "project check must refuse a hand-written plugin that states "
                "no justification"
            );
            CHECK_MESSAGE(
                messageOf(checked).find(k_handWrittenPlugin) != std::string::npos,
                "the refusal must name the hand-written plugin"
            );
            CHECK_MESSAGE(
                messageOf(checked).find("plugin_justification") != std::string::npos,
                "the refusal must name the absent member"
            );
        }

        SUBCASE("blank")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-justification-blank"
            };
            auto const initialized = initializedDeploymentWorkspace(
                workspace,
                R"json(      "plugin_justification": " \t\n ",
)json"
            );
            REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
            auto const spec = ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
                .toolCatalogs    = {},
            };

            auto const built = buildProject(spec, {});
            CHECK_FALSE_MESSAGE(
                built.has_value(),
                "project build must refuse a whitespace-only justification"
            );
            auto const checked = checkProject(spec, {});
            REQUIRE_FALSE_MESSAGE(
                checked.has_value(),
                "project check must refuse a whitespace-only justification"
            );
            CHECK_MESSAGE(
                messageOf(checked).find(k_handWrittenPlugin) != std::string::npos,
                "the refusal must name the hand-written plugin"
            );
            CHECK_MESSAGE(
                messageOf(checked).find("plugin_justification") != std::string::npos,
                "the refusal must name the blank member"
            );
        }

        SUBCASE("stated")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-justification-stated"
            };
            auto const initialized = initializedDeploymentWorkspace(
                workspace,
                justification
            );
            REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
            auto const spec = ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
                .toolCatalogs    = {},
            };

            auto const built = buildProject(spec, {});
            REQUIRE_MESSAGE(built.has_value(), messageOf(built));
            auto const checked = checkProject(spec, {});
            REQUIRE_MESSAGE(checked.has_value(), messageOf(checked));
        }
    }

    TEST_CASE("fault matrix tamper names the altered frozen release file")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-release"};
        auto const initialized = initializedWorkflowWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const candidate = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
            .toolCatalogs    = {},
        };
        auto const built = buildProject(candidate, {});
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));
        auto const spec = ProjectFreezeSpec{
            .candidate   = candidate,
            .releaseRoot = workspace.releases(),
        };

        auto const first = freezeProject(spec, {});
        REQUIRE_MESSAGE(first.has_value(), messageOf(first));
        auto const second = freezeProject(spec, {});
        REQUIRE_MESSAGE(second.has_value(), messageOf(second));
        CHECK_MESSAGE(
            *first == *second,
            "freezing the same candidate twice must produce one release id"
        );
        CHECK(first->filename().string().size() == 64U);
        auto const releaseSnapshot = snapshotTree(*first);
        CHECK_FALSE_MESSAGE(
            releaseSnapshot.contains(std::string{k_workflowDeclarationInput}),
            "a hand-written plugin input must not be copied into the release"
        );

        auto error = std::error_code{};
        auto const releaseStatus = std::filesystem::status(*first, error);
        REQUIRE_FALSE(error);
        CHECK_MESSAGE(
            (
                releaseStatus.permissions()
                & std::filesystem::perms::owner_write
            ) == std::filesystem::perms::none,
            "the frozen release directory must be read-only"
        );
        auto pythonFiles = std::size_t{0};
        for (auto const& entry : std::filesystem::recursive_directory_iterator{
                 *first
             })
        {
            error             = std::error_code{};
            auto const status = entry.status(error);
            REQUIRE_FALSE(error);
            CHECK_MESSAGE(
                (
                    status.permissions()
                    & std::filesystem::perms::owner_write
                ) == std::filesystem::perms::none,
                "every frozen release entry must be read-only"
            );
            if (entry.is_regular_file() && entry.path().extension() == ".py")
            {
                ++pythonFiles;
            }
        }
        CHECK_MESSAGE(
            pythonFiles == 0U,
            "the complete frozen release must contain zero Python files"
        );
        auto const loaded = loadProjectRelease(*first);
        REQUIRE_MESSAGE(loaded.has_value(), messageOf(loaded));

        auto const payload = *first / k_generatedFrameworkSchemaCatalog;
        auto stream = std::ofstream{
            payload,
            std::ios::binary | std::ios::app
        };
        CHECK_FALSE_MESSAGE(
            stream.is_open(),
            "a write attempt against a frozen artifact must be refused"
        );
        error = std::error_code{};
        std::filesystem::permissions(
            payload,
            std::filesystem::perms::owner_write,
            std::filesystem::perm_options::add,
            error
        );
        REQUIRE_FALSE(error);
        auto changedBytes = releaseSnapshot.at(
            std::string{k_generatedFrameworkSchemaCatalog}
        );
        REQUIRE_FALSE(changedBytes.empty());
        changedBytes.front() = changedBytes.front() == '{' ? '[' : '{';
        writeFile(payload, changedBytes);
        std::filesystem::permissions(
            payload,
            std::filesystem::perms::owner_read,
            std::filesystem::perm_options::replace,
            error
        );
        REQUIRE_FALSE(error);

        auto const changed = loadProjectRelease(*first);
        REQUIRE_FALSE_MESSAGE(
            changed.has_value(),
            "loading must refuse after one frozen byte changes"
        );
        CHECK_MESSAGE(
            messageOf(changed).find("digest does not match")
                != std::string::npos,
            "modified-release refusal must land on the artifact digest guard"
        );
        CHECK_MESSAGE(
            messageOf(changed).find(
                payload.lexically_relative(*first).generic_string()
            ) != std::string::npos,
            "modified-release refusal must name the altered file"
        );
    }

    TEST_CASE("project check names a declared input removed after build")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-missing-input"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const directories = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
            .toolCatalogs    = {},
        };
        auto const built = buildProject(directories, {});
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        REQUIRE(std::filesystem::remove(
            workspace.source() / "content" / "facts.txt"
        ));
        auto const checked = checkProject(directories, {});

        REQUIRE_FALSE_MESSAGE(
            checked.has_value(),
            "project check must reject a removed declared input"
        );
        CHECK_MESSAGE(
            messageOf(checked).find("content/facts.txt") != std::string::npos,
            "missing-input diagnostic must name content/facts.txt"
        );
        CHECK_MESSAGE(
            messageOf(checked).find("is missing") != std::string::npos,
            "missing-input diagnostic must state that the input is missing"
        );
    }

    TEST_CASE("project check rejects a build receipt for different declared inputs")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-stale-receipt"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const directories = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
            .toolCatalogs    = {},
        };
        auto const built = buildProject(directories, {});
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        writeFile(
            workspace.build() / k_buildReceiptName,
            "umbraflow-project-kit-build-v1\ndecisions.txt\n"
        );
        auto const checked = checkProject(directories, {});

        REQUIRE_FALSE_MESSAGE(
            checked.has_value(),
            "project check must reject a receipt for different declared inputs"
        );
        CHECK_MESSAGE(
            messageOf(checked).find("does not match declared inputs")
                != std::string::npos,
            "receipt mismatch diagnostic must name the declared-input mismatch"
        );
    }

    TEST_CASE("project build refuses a build directory inside the source tree")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-overlap"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

        auto const nestedBuild = workspace.source() / "generated";
        auto error             = std::error_code{};
        REQUIRE(std::filesystem::create_directories(nestedBuild, error));
        REQUIRE_FALSE(error);
        REQUIRE(std::filesystem::copy_file(
            workspace.build() / k_inputManifestName,
            nestedBuild / k_inputManifestName,
            error
        ));
        REQUIRE_FALSE(error);

        auto const built = buildProject(
            ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = nestedBuild,
                .toolCatalogs    = {},
            },
            {}
        );

        REQUIRE_FALSE_MESSAGE(
            built.has_value(),
            "project build must reject a build directory inside the source tree"
        );
        CHECK_MESSAGE(
            messageOf(built).find("must be separate") != std::string::npos,
            "overlap diagnostic must name the source/build separation rule"
        );
        CHECK_FALSE_MESSAGE(
            std::filesystem::exists(nestedBuild / k_buildReceiptName),
            "overlap refusal must happen before a source-side receipt is written"
        );
    }

    TEST_CASE("workflow declaration refuses every exceeded bound")
    {
        struct BoundCase final
        {
            std::string name{};
            std::string before{};
            std::string after{};
            std::string expectedError{};
            std::string assertion{};
        };

        auto const cases = std::array{
            BoundCase{
                .name          = "state",
                .before        = "\"maximum_states\": 2",
                .after         = "\"maximum_states\": 1",
                .expectedError = "WorkflowStateBound",
                .assertion     = "exceeding the state bound must be refused",
            },
            BoundCase{
                .name          = "step",
                .before        = "\"maximum_steps\": 2",
                .after         = "\"maximum_steps\": 1",
                .expectedError = "WorkflowStepBound",
                .assertion     = "exceeding the step bound must be refused",
            },
            BoundCase{
                .name          = "dispatch",
                .before        = "\"maximum_dispatches\": 1",
                .after         = "\"maximum_dispatches\": 0",
                .expectedError = "WorkflowDispatchBound",
                .assertion     = "exceeding the dispatch bound must be refused",
            },
            BoundCase{
                .name          = "observation",
                .before        = "\"maximum_observations\": 2",
                .after         = "\"maximum_observations\": 1",
                .expectedError = "WorkflowObservationBound",
                .assertion     = "exceeding the observation bound must be refused",
            },
            BoundCase{
                .name          = "wait",
                .before        = "\"maximum_waits\": 1",
                .after         = "\"maximum_waits\": 0",
                .expectedError = "WorkflowWaitBound",
                .assertion     = "exceeding the wait bound must be refused",
            },
            BoundCase{
                .name          = "elapsed",
                .before        = "\"maximum_elapsed_ms\": 3000",
                .after         = "\"maximum_elapsed_ms\": 2999",
                .expectedError = "WorkflowElapsedBound",
                .assertion     = "exceeding the elapsed bound must be refused",
            },
        };

        for (auto const& testCase : cases)
        {
            auto const generated = generateDeclarativeWorkflowAdapter(
                "chaos.project",
                replacedOnce(
                    validWorkflowDeclaration(),
                    testCase.before,
                    testCase.after
                )
            );
            CAPTURE(testCase.name);
            REQUIRE_FALSE_MESSAGE(generated.has_value(), testCase.assertion);
            CHECK_MESSAGE(
                generated.error().message().starts_with(testCase.expectedError),
                testCase.assertion
            );
        }
    }

    // The bounded workflow ABSORBED the single-step tool rather than replacing
    // it: a schedule of one state and one step is the single-step case, and the
    // single-step generator was deleted on that basis. Nothing else proves the
    // degenerate schedule still works, and a shape that supports a case no test
    // exercises is a claim rather than a capability.
    TEST_CASE("a one-state, one-step schedule is still a whole tool")
    {
        auto const declaration = R"json({
  "schema": "umbraflow-declarative-workflow-tool/v1",
  "tool_name": "chaos.dismiss_known_overlay",
  "target_argument": "observed_instance_id",
  "allowed_instance_kinds": ["chaos.overlay"],
  "fresh_observation": {
    "required_surface": "chaos.overlay_layer",
    "require_unambiguous": true
  },
  "ui_finding": {"kind": "observed_instance_absent"},
  "states": [
    {
      "state_key": "dismiss-overlay",
      "kind": "ui_action",
      "ui_action": "chaos.ui.dismiss_overlay",
      "timeout_ms": 2000
    }
  ],
  "steps": ["dismiss-overlay"],
  "bounds": {
    "maximum_states": 1,
    "maximum_steps": 1,
    "maximum_dispatches": 1,
    "maximum_observations": 1,
    "maximum_waits": 0,
    "maximum_elapsed_ms": 2000
  }
})json";

        auto const generated = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            declaration
        );
        REQUIRE_MESSAGE(
            generated.has_value(),
            "a one-step schedule must generate an adapter"
        );

        auto compiled = script::PureDataProgram::compile(
            "chaos.project",
            *generated,
            k_workflowEntryPoints,
            {}
        );
        REQUIRE_MESSAGE(
            compiled.has_value(),
            "a one-step adapter must compile against the five entry points"
        );

        auto const present = observation(true);
        auto const planned = invoked(
            *compiled,
            "plan",
            adapterInput(present, true)
        );
        auto const* limits = planned.find("workflow_limits");
        REQUIRE(limits != nullptr);
        CHECK_MESSAGE(
            limits->find("maximum_steps")->number() == 1.0,
            "a one-step schedule must carry a step bound of one"
        );
        CHECK_MESSAGE(
            limits->find("maximum_waits")->number() == 0.0,
            "a schedule with no wait state must carry a wait bound of zero"
        );
    }

    TEST_CASE("generated bounded workflow runs through the five-function SPI")
    {
        auto const program = generatedWorkflowProgram();
        auto const present = observation(true);

        auto const derived = invoked(program, "derive", parsedJson("{}"));
        auto const* deriveSchema = derived.find("schema");
        REQUIRE(deriveSchema != nullptr);
        CHECK(
            deriveSchema->string()
            == "umbraflow-project-observation-proposal/v1"
        );

        auto const planned = invoked(
            program,
            "plan",
            adapterInput(present, true)
        );
        auto const* limits = planned.find("workflow_limits");
        REQUIRE(limits != nullptr);
        CHECK_MESSAGE(
            limits->find("maximum_steps")->number() == 2.0,
            "bounded workflow plan must carry its finite step bound"
        );
        CHECK_MESSAGE(
            limits->find("maximum_dispatches")->number() == 1.0,
            "bounded workflow plan must carry its finite dispatch bound"
        );
        CHECK_MESSAGE(
            limits->find("maximum_observations")->number() == 2.0,
            "bounded workflow plan must carry its finite observation bound"
        );
        CHECK_MESSAGE(
            limits->find("maximum_waits")->number() == 1.0,
            "bounded workflow plan must carry its finite wait bound"
        );
        CHECK_MESSAGE(
            limits->find("maximum_elapsed_ms")->number() == 3000.0,
            "bounded workflow plan must carry its finite elapsed bound"
        );

        auto stepInput = [&present](uint32 stepIndex) -> json::Value
        {
            auto text = std::string{
                R"json({"canonical_args":{"observed_instance_id":")json"
            };
            text += k_observedInstanceId;
            text += R"json("},"project_observation":)json";
            text += present;
            text += R"json(,"project_state":{},"step_index":)json";
            text += std::to_string(stepIndex);
            text += '}';
            return parsedJson(text);
        };

        auto const wait = invoked(program, "next_step", stepInput(1U));
        CHECK_MESSAGE(
            wait.find("action") == nullptr,
            "the first bounded state must produce a real WaitIntent"
        );
        REQUIRE(wait.find("condition") != nullptr);
        REQUIRE(wait.find("observation_budget") != nullptr);
        CHECK_MESSAGE(
            wait.find("observation_budget")->number() == 1.0,
            "the wait state must carry its declared observation budget"
        );

        auto const action = invoked(program, "next_step", stepInput(2U));
        REQUIRE(action.find("action") != nullptr);
        CHECK_MESSAGE(
            action.find("action")->find("action_id")->string()
                == "chaos.ui.dismiss_overlay",
            "the second bounded state must dispatch its declared UI action"
        );
        CHECK_MESSAGE(
            action.find("step_key")->string() == "dismiss-overlay",
            "the workflow must advance to its second named state"
        );

        auto const reconciled = invoked(
            program,
            "reconcile",
            adapterInput(observation(false), false)
        );
        auto const* findings = reconciled.find("findings");
        REQUIRE(findings != nullptr);
        REQUIRE(findings->items().size() == 1U);
        CHECK(
            findings->items().front().find("kind")->string()
            == "observed_instance_absent"
        );

        auto const reduced = invoked(program, "reduce", parsedJson("{}"));
        CHECK(reduced.kind() == json::ValueKind::Object);
        CHECK(reduced.members().empty());
    }

    TEST_CASE("missing step observation stops the bounded workflow")
    {
        auto const program = generatedWorkflowProgram();
        auto const missing = parsedJson(
            R"json({
                "canonical_args": {
                    "observed_instance_id":
                        "oi1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                },
                "project_observation": null,
                "project_state": {},
                "step_index": 2
            })json"
        );
        auto const result = program.invoke("next_step", missing);

        REQUIRE_FALSE_MESSAGE(
            result.has_value(),
            "a step whose observation is missing must not advance the workflow"
        );
        CHECK_MESSAGE(
            result.error().message().find("MissingStepObservation")
                != std::string_view::npos,
            "missing observation refusal must name the fresh-evidence property"
        );
    }

    TEST_CASE("workflow declaration is closed and uniquely names every state")
    {
        auto const undeclared = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            replacedOnce(
                validWorkflowDeclaration(),
                "\"bounds\": {",
                "\"script\": \"while true do end\", \"bounds\": {"
            )
        );
        REQUIRE_FALSE(undeclared.has_value());
        CHECK(undeclared.error().message().starts_with("ClosedSchema"));

        auto const duplicate = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            replacedOnce(
                validWorkflowDeclaration(),
                "\"state_key\": \"dismiss-overlay\"",
                "\"state_key\": \"await-overlay\""
            )
        );
        REQUIRE_FALSE(duplicate.has_value());
        CHECK(
            duplicate.error().message().find("state_key values must be unique")
            != std::string_view::npos
        );
    }
}
