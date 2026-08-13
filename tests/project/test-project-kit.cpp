#include <project/project-kit.hpp>
#include <project/declarative-single-step-tool.hpp>

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/error/error.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

        inline constexpr auto k_singleStepEntryPoints = std::array{
            std::string_view{"derive"},
            std::string_view{"plan"},
            std::string_view{"next_step"},
            std::string_view{"reconcile"},
            std::string_view{"reduce"},
        };

        inline constexpr auto k_observedInstanceId = std::string_view{
            "oi1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        };
        inline constexpr auto k_singleStepDeclarationInput = std::string_view{
            "declarative-tools/chaos.project/dismiss-known-overlay.json"
        };
        inline constexpr auto k_generatedSingleStepAdapter = std::string_view{
            "generated/adapters/chaos.project/dismiss-known-overlay.luau"
        };
        inline constexpr auto k_generatedToolCatalog = std::string_view{
            "generated/tool-catalogs/chaos.project/tool-catalog-v1.json"
        };

        [[nodiscard]]
        auto validSingleStepDeclaration() -> std::string
        {
            return R"json({
  "schema": "umbraflow-declarative-single-step-tool/v1",
  "tool_name": "chaos.dismiss_known_overlay",
  "target_argument": "observed_instance_id",
  "allowed_instance_kinds": ["chaos.overlay"],
  "ui_action": "chaos.ui.dismiss_overlay",
  "fresh_observation": {
    "required_surface": "chaos.overlay_layer",
    "require_unambiguous": true
  },
  "ui_finding": {"kind": "observed_instance_absent"},
  "bounds": {
    "max_dispatches": 1,
    "max_observations": 3,
    "timeout_ms": 3000
  }
})json";
        }

        [[nodiscard]]
        auto initializedSingleStepWorkspace(
            TemporaryWorkspace const& workspace
        ) -> Status
        {
            writeFile(
                workspace.source() / k_singleStepDeclarationInput,
                validSingleStepDeclaration()
            );
            return initProject(
                ProjectInitSpec{
                    .sourceDirectory = workspace.source(),
                    .buildDirectory  = workspace.build(),
                    .inputs          = {
                        std::filesystem::path{k_singleStepDeclarationInput},
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
                "single-step vector mutation must name existing source bytes"
            );
            input.replace(position, before.size(), after);
            return input;
        }

        [[nodiscard]]
        auto generatedSingleStepProgram() -> script::PureDataProgram
        {
            auto const generated = generateDeclarativeSingleStepAdapter(
                "chaos.project",
                validSingleStepDeclaration()
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
                k_singleStepEntryPoints,
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
            }
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

    TEST_CASE("project build regenerates five-function adapters solely from declared source")
    {
        auto const workspace = TemporaryWorkspace{
            "uf-project-single-step-generation"
        };
        auto const initialized = initializedSingleStepWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const sourceBefore = snapshotTree(workspace.source());
        auto const directories  = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
            .toolCatalogs    = {},
        };

        auto const built = buildProject(directories);
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));
        REQUIRE_MESSAGE(
            snapshotTree(workspace.source()) == sourceBefore,
            "adapter generation must not change its declared source"
        );

        auto const expected = generateDeclarativeSingleStepAdapter(
            "chaos.project",
            validSingleStepDeclaration()
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
            snapshot.contains(std::string{k_generatedSingleStepAdapter}),
            "project build must generate the named single-step adapter"
        );
        CHECK_MESSAGE(
            snapshot.at(std::string{k_generatedSingleStepAdapter}) == *expected,
            "generated adapter bytes must come from the declared source"
        );

        writeFile(
            workspace.build() / k_generatedSingleStepAdapter,
            "hand edited\n"
        );
        auto const rebuilt = buildProject(directories);
        REQUIRE_MESSAGE(rebuilt.has_value(), messageOf(rebuilt));
        snapshot = snapshotTree(workspace.build());
        CHECK_MESSAGE(
            snapshot.at(std::string{k_generatedSingleStepAdapter}) == *expected,
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

        auto const built = buildProject(spec);
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
        auto const rebuilt = buildProject(spec);
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
        auto const built = buildProject(spec);
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));
        auto const catalogPath = workspace.build() / k_generatedToolCatalog;

        SUBCASE("altered")
        {
            writeFile(catalogPath, "hand edited\n");
            auto const checked = checkProject(spec);
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
            auto const checked = checkProject(spec);
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
            auto const checked = checkProject(spec);
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
            auto const checked = checkProject(spec);
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
        auto const built = buildProject(directories);
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        REQUIRE(std::filesystem::remove(
            workspace.source() / "content" / "facts.txt"
        ));
        auto const checked = checkProject(directories);

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
        auto const built = buildProject(directories);
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        writeFile(
            workspace.build() / k_buildReceiptName,
            "umbraflow-project-kit-build-v1\ndecisions.txt\n"
        );
        auto const checked = checkProject(directories);

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
            }
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

    TEST_CASE("single-step golden refusals emit their normative error codes")
    {
        struct InvalidCase final
        {
            std::string name{};
            std::string expectedError{};
            std::string document{};
        };

        auto const valid = validSingleStepDeclaration();
        auto const cases = std::array{
            InvalidCase{
                .name          = "coordinate_injection",
                .expectedError = "ClosedSchema",
                .document      = replacedOnce(
                    valid,
                    R"json("bounds": {)json",
                    R"json("coordinates": [100, 200], "bounds": {)json"
                ),
            },
            InvalidCase{
                .name          = "more_than_one_dispatch",
                .expectedError = "SingleStepDispatchBound",
                .document      = replacedOnce(
                    valid,
                    R"json("max_dispatches": 1)json",
                    R"json("max_dispatches": 2)json"
                ),
            },
            InvalidCase{
                .name          = "hidden_script",
                .expectedError = "ClosedSchema",
                .document      = replacedOnce(
                    valid,
                    R"json("bounds": {)json",
                    R"json("script": "while true do end", "bounds": {)json"
                ),
            },
            InvalidCase{
                .name          = "ambiguous_observation_allowed",
                .expectedError = "AmbiguousObservationAllowed",
                .document      = replacedOnce(
                    valid,
                    R"json("require_unambiguous": true)json",
                    R"json("require_unambiguous": false)json"
                ),
            },
            InvalidCase{
                .name          = "missing_fresh_observation_guard",
                .expectedError = "IncompleteSingleStep",
                .document      = replacedOnce(
                    valid,
                    R"json(  "fresh_observation": {
    "required_surface": "chaos.overlay_layer",
    "require_unambiguous": true
  },
)json",
                    ""
                ),
            },
            InvalidCase{
                .name          = "ui_finding_without_kind",
                .expectedError = "IncompleteSingleStep",
                .document      = replacedOnce(
                    valid,
                    R"json({"kind": "observed_instance_absent"})json",
                    "{}"
                ),
            },
            InvalidCase{
                .name          = "second_target_binding_in_ui_finding",
                .expectedError = "ClosedSchema",
                .document      = replacedOnce(
                    valid,
                    R"json({"kind": "observed_instance_absent"})json",
                    R"json({"kind": "observed_instance_absent", )json"
                    R"json("target_argument": "some_other_argument"})json"
                ),
            },
            InvalidCase{
                .name          = "fresh_observation_with_undeclared_member",
                .expectedError = "ClosedSchema",
                .document      = replacedOnce(
                    valid,
                    R"json("require_unambiguous": true)json",
                    R"json("require_unambiguous": true, "settle_delay_ms": 250)json"
                ),
            },
            InvalidCase{
                .name          = "bounds_with_undeclared_member",
                .expectedError = "ClosedSchema",
                .document      = replacedOnce(
                    valid,
                    R"json("timeout_ms": 3000)json",
                    R"json("timeout_ms": 3000, "max_retries": 5)json"
                ),
            },
            InvalidCase{
                .name          = "empty_allowed_instance_kinds",
                .expectedError = "SingleStepInstanceKindsEmpty",
                .document      = replacedOnce(
                    valid,
                    R"json(["chaos.overlay"])json",
                    "[]"
                ),
            },
            InvalidCase{
                .name          = "observation_bound_below_one",
                .expectedError = "SingleStepObservationBound",
                .document      = replacedOnce(
                    valid,
                    R"json("max_observations": 3)json",
                    R"json("max_observations": 0)json"
                ),
            },
            InvalidCase{
                .name          = "timeout_bound_below_one",
                .expectedError = "SingleStepTimeoutBound",
                .document      = replacedOnce(
                    valid,
                    R"json("timeout_ms": 3000)json",
                    R"json("timeout_ms": 0)json"
                ),
            },
            InvalidCase{
                .name          = "tool_name_not_namespaced",
                .expectedError = "MalformedSingleStepTool",
                .document      = replacedOnce(
                    valid,
                    "chaos.dismiss_known_overlay",
                    "dismiss_known_overlay"
                ),
            },
        };

        for (auto const& testCase : cases)
        {
            auto const generated = generateDeclarativeSingleStepAdapter(
                "chaos.project",
                testCase.document
            );
            CAPTURE(testCase.name);
            REQUIRE_FALSE_MESSAGE(
                generated.has_value(),
                "locked invalid single-step vector must be refused"
            );
            CHECK_MESSAGE(
                generated.error().message().starts_with(testCase.expectedError),
                "single-step refusal must emit the vector's normative error code"
            );
        }
    }

    TEST_CASE("generated dismiss-known-overlay tool runs through the five-function SPI")
    {
        auto const program = generatedSingleStepProgram();
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
        auto const* effects = planned.find("effects");
        REQUIRE(effects != nullptr);
        CHECK_MESSAGE(
            effects->items().empty(),
            "single-step declaration must not submit an expected domain effect"
        );
        auto const* limits = planned.find("workflow_limits");
        REQUIRE(limits != nullptr);
        REQUIRE(limits->find("maximum_dispatches") != nullptr);
        CHECK_MESSAGE(
            limits->find("maximum_dispatches")->number() == 1.0,
            "generated single-step plan must allow exactly one dispatch"
        );
        REQUIRE(limits->find("maximum_observations") != nullptr);
        CHECK_MESSAGE(
            limits->find("maximum_observations")->number() == 3.0,
            "generated single-step plan must carry its finite observation bound"
        );
        REQUIRE(limits->find("maximum_elapsed_ms") != nullptr);
        CHECK_MESSAGE(
            limits->find("maximum_elapsed_ms")->number() == 3000.0,
            "generated single-step plan must carry its finite timeout"
        );

        auto const step = invoked(
            program,
            "next_step",
            adapterInput(present, false)
        );
        auto const* action = step.find("action");
        REQUIRE(action != nullptr);
        REQUIRE(action->find("action_id") != nullptr);
        CHECK_MESSAGE(
            action->find("action_id")->string() == "chaos.ui.dismiss_overlay",
            "generated action must use the declared namespaced UI action"
        );
        REQUIRE(action->find("canonical_parameters") != nullptr);
        auto const* target = action->find("canonical_parameters")
            ->find("observed_instance_id");
        REQUIRE(target != nullptr);
        CHECK_MESSAGE(
            action->find("canonical_parameters")->members().size() == 1U,
            "generated action must carry exactly one target binding"
        );
        CHECK_MESSAGE(
            target->string() == k_observedInstanceId,
            "generated action must bind its only target from canonical args"
        );
        auto const* expectedUiPostconditions = step.find(
            "expected_ui_postconditions"
        );
        REQUIRE(expectedUiPostconditions != nullptr);
        CHECK_MESSAGE(
            expectedUiPostconditions->items().empty(),
            "single-step declaration must not smuggle in a UI result assertion"
        );

        auto const reconciled = invoked(
            program,
            "reconcile",
            adapterInput(observation(false), false)
        );
        auto const* disposition = reconciled.find("disposition");
        REQUIRE(disposition != nullptr);
        CHECK_MESSAGE(
            disposition->string() == "continue",
            "UI finding must not mark the Operation Confirmed"
        );
        CHECK_MESSAGE(
            reconciled.find("effects") == nullptr,
            "UI finding must not submit an expected domain effect"
        );
        auto const* journalEvents = reconciled.find("journal_events");
        REQUIRE(journalEvents != nullptr);
        CHECK_MESSAGE(
            journalEvents->items().empty(),
            "UI finding must not submit a domain event"
        );
        auto const* observedOutcomes = reconciled.find("observed_outcomes");
        REQUIRE(observedOutcomes != nullptr);
        CHECK_MESSAGE(
            observedOutcomes->items().empty(),
            "UI finding must remain evidence rather than become an observed outcome"
        );
        auto const* findings = reconciled.find("findings");
        REQUIRE(findings != nullptr);
        REQUIRE_MESSAGE(
            findings->items().size() == 1U,
            "a genuinely absent target must produce one UI finding"
        );
        auto const& finding = findings->items().front();
        REQUIRE(finding.find("kind") != nullptr);
        CHECK_MESSAGE(
            finding.find("kind")->string() == "observed_instance_absent",
            "reconcile must return the declaration's UI finding as evidence"
        );
        REQUIRE(finding.find("observed_instance_id") != nullptr);
        CHECK_MESSAGE(
            finding.find("observed_instance_id")->string()
                == k_observedInstanceId,
            "UI finding must refer to the declaration's sole canonical target"
        );

        auto const reduced = invoked(program, "reduce", parsedJson("{}"));
        CHECK(reduced.kind() == json::ValueKind::Object);
        CHECK(reduced.members().empty());
    }

    TEST_CASE("generated single-step adapter names stale observed-instance refusal")
    {
        auto const program = generatedSingleStepProgram();
        auto const stale = program.invoke(
            "next_step",
            adapterInput(observation(false), false)
        );

        REQUIRE_FALSE_MESSAGE(
            stale.has_value(),
            "next_step must refuse a target absent from the fresh observation"
        );
        CHECK_MESSAGE(
            stale.error().message().find("ObservedInstanceStale")
                != std::string_view::npos,
            "stale target refusal must name ObservedInstanceStale"
        );
    }

    TEST_CASE("generated single-step adapter rejects an undeclared target kind")
    {
        auto const program = generatedSingleStepProgram();
        auto const wrongKind = program.invoke(
            "next_step",
            adapterInput(observation(true, "chaos.dialog"), false)
        );

        REQUIRE_FALSE_MESSAGE(
            wrongKind.has_value(),
            "next_step must reject a target kind outside allowed_instance_kinds"
        );
        CHECK_MESSAGE(
            wrongKind.error().message().find("SingleStepTargetKindRejected")
                != std::string_view::npos,
            "target-kind refusal must name SingleStepTargetKindRejected"
        );
    }

    TEST_CASE("generated finding distinguishes lost Surface evidence from target absence")
    {
        struct SurfaceCase final
        {
            std::string name{};
            bool        fresh{};
            std::string resolution{};
            bool        unambiguous{};
            std::string expectedError{};
        };
        auto const cases = std::array{
            SurfaceCase{
                .name          = "page vanished",
                .fresh         = false,
                .resolution    = "resolved",
                .unambiguous   = true,
                .expectedError = "StaleObservation",
            },
            SurfaceCase{
                .name          = "recognition lost",
                .fresh         = true,
                .resolution    = "unresolved",
                .unambiguous   = true,
                .expectedError = "FreshSurfaceUnresolved",
            },
            SurfaceCase{
                .name          = "recognition ambiguous",
                .fresh         = true,
                .resolution    = "resolved",
                .unambiguous   = false,
                .expectedError = "FreshSurfaceAmbiguous",
            },
        };
        auto const program = generatedSingleStepProgram();

        for (auto const& testCase : cases)
        {
            auto const result = program.invoke(
                "reconcile",
                adapterInput(
                    observation(
                        false,
                        "chaos.overlay",
                        testCase.fresh,
                        testCase.resolution,
                        testCase.unambiguous
                    ),
                    false
                )
            );
            CAPTURE(testCase.name);
            REQUIRE_FALSE_MESSAGE(
                result.has_value(),
                "reconcile must not collapse invalid Surface evidence into target absence"
            );
            CHECK_MESSAGE(
                result.error().message().find(testCase.expectedError)
                    != std::string_view::npos,
                "Surface guard refusal must name the failed evidence property"
            );
        }
    }
}
