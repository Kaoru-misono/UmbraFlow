#include <project/project-kit.hpp>
#include <project/declarative-workflow-tool.hpp>

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <operator/project-plugin.hpp>

#include <core/error/error.hpp>

#include <domain/content-hash.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
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

        inline constexpr auto k_deploymentManifestInput = std::string_view{
            "umbraflow-project.json"
        };
        inline constexpr auto k_handWrittenPlugin = std::string_view{
            "plugin/dream.luau"
        };
        inline constexpr auto k_generatedPlugin = std::string_view{
            "generated/adapters/acme.tool/do-work/tool.luau"
        };
        inline constexpr auto k_handWrittenAuthoring = std::string_view{
            R"json(      "plugin_authoring": "hand-written",
)json"
        };
        inline constexpr auto k_generatedAuthoring = std::string_view{
            R"json(      "plugin_authoring": "generated",
)json"
        };
        inline constexpr auto k_statedJustification = std::string_view{
            R"json(      "plugin_justification": "umbraflow-declarative-workflow-tool/v1 has no member that decides what a handler returns.",
)json"
        };

        // The deployment manifest as a project author writes it, with the
        // members under test spliced in exactly as given. The whole document is
        // written rather than the few members the gate reads, so a fixture that
        // stopped being a manifest could not go on satisfying the gate.
        [[nodiscard]]
        auto deploymentManifest(
            std::string_view plugin,
            std::string_view authoringMember,
            std::string_view justificationMember,
            std::string_view templateCuts
        ) -> std::string
        {
            // A deployment states one closure, and the module a case names is
            // that closure's. It declares no Tool and binds none, which is
            // what a project shipping no handler states.
            return std::string{R"json({
  "schema": "umbraflow-project/v3",
  "runtime_artifact": "runtime/artifact",
  "primary_deployment": "dream",
  "template_cuts": )json"}
                + std::string{templateCuts}
                + R"json(,
  "deployments": [
    {
      "name": "dream",
      "plugin_id": "chaos.dream",
      "tool_closure": {"entry":"main","exported_entry_points":[],"modules":[{"name":"main","path":")json"
                + std::string{plugin}
                + R"json("}]},
)json"
                + std::string{authoringMember}
                + std::string{justificationMember}
                + R"json(      "tools": [],
      "observed_instance_identity_schemas": [],
      "tool_bindings": [],
      "resources": []
    }
  ]
})json";
        }

        // The manifest every workspace below holds at its source root, because
        // a source tree with no umbraflow-project.json is not a project and
        // `project build` refuses it. It is written but not declared, which is
        // the point: the gate must not be reachable only through the declared
        // input list.
        [[nodiscard]]
        auto acceptedDeploymentManifest() -> std::string
        {
            return deploymentManifest(
                k_handWrittenPlugin,
                k_handWrittenAuthoring,
                k_statedJustification,
                "[]"
            );
        }

        // The same accepted manifest, declaring the given template_cuts array.
        // A cut is declared here and nowhere else: there is no spec member a
        // caller can fill in, so a test that wants one written it the way a
        // project author writes it.
        //
        // It also declares one resource, because the input set a build acts on
        // is derived from this document: a file no member names is a file no
        // build has any reason to read.
        [[nodiscard]]
        auto manifestDeclaringCuts(std::string_view cuts) -> std::string
        {
            constexpr auto empty = std::string_view{R"json("resources": [])json"};
            auto manifest        = deploymentManifest(
                k_handWrittenPlugin,
                k_handWrittenAuthoring,
                k_statedJustification,
                cuts
            );
            auto const at = manifest.find(empty);
            REQUIRE(at != std::string::npos);
            manifest.replace(
                at,
                empty.size(),
                R"json("resources": [{"kind":"utf8","name":"facts","path":"content/facts.txt"}])json"
            );
            return manifest;
        }

        auto writeRootManifest(
            TemporaryWorkspace const& workspace,
            std::string_view manifest
        ) -> void
        {
            writeFile(workspace.source() / k_deploymentManifestInput, manifest);
        }

        [[nodiscard]]
        auto validWorkflowDeclaration() -> std::string;

        [[nodiscard]]
        auto initializedDeploymentWorkspace(
            TemporaryWorkspace const& workspace,
            std::string_view manifest
        ) -> Status
        {
            writeFile(workspace.source() / k_handWrittenPlugin, "return {}\n");
            writeRootManifest(workspace, manifest);
            if (manifest.contains(k_generatedPlugin))
            {
                writeFile(
                    workspace.source()
                        / std::filesystem::path{
                            "declarative-tools/acme.tool/do-work.json"
                        },
                    validWorkflowDeclaration()
                );
            }
            return initProject(ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            });
        }

        [[nodiscard]]
        auto initializedWorkspaceDeclaring(
            TemporaryWorkspace const& workspace,
            std::string_view templateCuts
        ) -> Status
        {
            writeFile(workspace.source() / "content" / "facts.txt", "facts\n");
            writeFile(workspace.source() / k_handWrittenPlugin, "return {}\n");
            writeRootManifest(workspace, manifestDeclaringCuts(templateCuts));
            return initProject(
                ProjectBuildSpec{
                    .sourceDirectory = workspace.source(),
                    .buildDirectory  = workspace.build(),
                }
            );
        }

        [[nodiscard]]
        auto initializedWorkspace(
            TemporaryWorkspace const& workspace
        ) -> Status
        {
            return initializedWorkspaceDeclaring(workspace, "[]");
        }

        inline constexpr auto k_workflowDeclarationInput = std::string_view{
            "declarative-tools/chaos.project/dismiss-known-overlay.json"
        };
        inline constexpr auto k_generatedWorkflowAdapter = std::string_view{
            "generated/adapters/chaos.project/dismiss-known-overlay/tool.luau"
        };
        inline constexpr auto k_generatedFrameworkSchemaCatalog = std::string_view{
            "generated/framework-schemas/framework-schema-catalog-v1.json"
        };

        [[nodiscard]]
        auto validWorkflowDeclaration() -> std::string
        {
            return R"json({
  "schema": "umbraflow-declarative-workflow-tool/v1",
  "tool_name": "chaos.project.dismiss_known_overlay",
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
            // A generated deployment, because that is the only way a workflow
            // declaration enters a build: the deployment names the module the
            // generator writes, and the declaration is derived back from that
            // module's path. There is no flag that adds a source the
            // declaration does not carry.
            writeRootManifest(
                workspace,
                deploymentManifest(
                    k_generatedWorkflowAdapter,
                    k_generatedAuthoring,
                    "",
                    "[]"
                )
            );
            return initProject(
                ProjectBuildSpec{
                    .sourceDirectory = workspace.source(),
                    .buildDirectory  = workspace.build(),
                }
            );
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
    }

    // The whole of the iterate loop's first requirement: edit the declaration,
    // re-run one command, and be told what is left -- with no earlier command
    // repeated first.
    //
    // Every assertion here was a failure in the field. A build derived its file
    // set from a ledger only `project init` wrote, so naming a new module in
    // the declaration made `project build` refuse the module it had just been
    // told about, and a source tree with no build directory could not be built
    // at all. Both are one bug: a cached derivation.
    TEST_CASE("project build derives its input set from the declaration alone")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-derived-inputs"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

        auto const sourceSnapshot = snapshotTree(workspace.source());
        CHECK_FALSE_MESSAGE(
            sourceSnapshot.contains(std::string{k_buildReceiptName}),
            "project init must write nothing into the source tree"
        );

        auto const directories = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        };
        auto const built = buildProject(directories, {});
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        // A module the declaration did not name a moment ago, with no init
        // between the edit and the build.
        writeFile(workspace.source() / "plugin/added.luau", "return {}\n");
        writeRootManifest(
            workspace,
            replacedOnce(
                manifestDeclaringCuts("[]"),
                R"json({"name":"main","path":"plugin/dream.luau"})json",
                R"json({"name":"added","path":"plugin/added.luau"},)json"
                R"json({"name":"main","path":"plugin/dream.luau"})json"
            )
        );
        auto const rebuilt = buildProject(directories, {});
        REQUIRE_MESSAGE(rebuilt.has_value(), messageOf(rebuilt));
        CHECK_MESSAGE(
            snapshotTree(workspace.build())
                .contains("generated/modules/dream/tool/added.luau"),
            "a module named by the declaration must reach the build that "
            "follows the edit"
        );

        // And with the whole build tree gone, which is what a fresh clone is.
        auto error = std::error_code{};
        std::filesystem::remove_all(workspace.build(), error);
        REQUIRE_FALSE(error);
        auto const fromNothing = buildProject(directories, {});
        CHECK_MESSAGE(
            fromNothing.has_value(),
            messageOf(fromNothing)
        );
    }

    TEST_CASE("project init never fills a missing authored module closure")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-init-modules"};
        auto manifest = replacedOnce(
            acceptedDeploymentManifest(),
            R"json("tool_closure": {"entry":"main","exported_entry_points":[],"modules":[{"name":"main","path":"plugin/dream.luau"}]})json",
            R"json("tool_closure": {"entry":"main","exported_entry_points":[],"modules":[{"name":"support","path":"plugin/support.luau"},{"name":"main","path":"plugin/main.luau"}]})json"
        );
        writeRootManifest(workspace, manifest);

        auto const initialized = initProject(ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        });
        REQUIRE_FALSE(initialized.has_value());
        CHECK(messageOf(initialized).contains("plugin/main.luau"));

        auto const source = snapshotTree(workspace.source());
        CHECK_FALSE(source.contains("plugin/main.luau"));
        CHECK_FALSE(source.contains("plugin/support.luau"));
    }

    TEST_CASE("project scaffold creates buildable generated and hand-written projects")
    {
        constexpr auto forms = std::array{
            ProjectPluginForm::Generated,
            ProjectPluginForm::HandWritten,
        };
        for (auto const form : forms)
        {
            auto const label = (
                form == ProjectPluginForm::Generated
                    ? "uf-project-scaffold-generated"
                    : "uf-project-scaffold-hand-written"
            );
            auto const workspace = TemporaryWorkspace{label};
            auto const scaffolded = scaffoldProject(ProjectScaffoldSpec{
                .sourceDirectory = workspace.source(),
                .pluginId        = "chaos.project",
                .pluginForm      = form,
            });
            REQUIRE_MESSAGE(scaffolded.has_value(), messageOf(scaffolded));

            auto const initialized = initProject(ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            });
            REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

            auto const spec = ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            };
            auto const built = buildProject(spec, {});
            REQUIRE_MESSAGE(built.has_value(), messageOf(built));
            auto const checked = checkProject(spec, {});
            REQUIRE_MESSAGE(checked.has_value(), messageOf(checked));

            auto const source = snapshotTree(workspace.source());
            CHECK(source.contains("umbraflow-project.json"));
            CHECK(source.contains("content/placeholder.txt"));
            // A starter authors no JSON Schema at all: its one Tool carries
            // its argument shape inline, so the only files beside the root
            // document are the placeholder resource and the plugin sources.
            CHECK_FALSE(std::ranges::any_of(
                source,
                [](auto const& entry)
                {
                    return entry.first.ends_with(".schema.json");
                }
            ));
            if (form == ProjectPluginForm::Generated)
            {
                CHECK(source.contains(
                    "declarative-tools/chaos.project/scaffold.json"
                ));
            }
            else
            {
                CHECK(source.contains("plugin/tool.luau"));
                CHECK(source.contains("plugin/support.luau"));
            }
        }
    }

    TEST_CASE("project scaffold refuses conflicts without changing the source tree")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-scaffold-conflict"};
        writeFile(workspace.source() / "content/placeholder.txt", "owned\n");
        auto const before = snapshotTree(workspace.source());

        auto const scaffolded = scaffoldProject(ProjectScaffoldSpec{
            .sourceDirectory = workspace.source(),
            .pluginId        = "chaos.project",
            .pluginForm      = ProjectPluginForm::Generated,
        });
        REQUIRE_FALSE(scaffolded.has_value());
        CHECK(messageOf(scaffolded).contains("already exists"));
        CHECK(snapshotTree(workspace.source()) == before);
    }

    TEST_CASE("project build records exact module resource and environment identities")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-execution-closure"};
        auto manifest = replacedOnce(
            acceptedDeploymentManifest(),
            R"json("tool_closure": {"entry":"main","exported_entry_points":[],"modules":[{"name":"main","path":"plugin/dream.luau"}]})json",
            R"json("tool_closure": {"entry":"main","exported_entry_points":[],"modules":[{"name":"support","path":"plugin/support.luau"},{"name":"main","path":"plugin/main.luau"}]})json"
        );
        manifest = replacedOnce(
            manifest,
            R"json("resources": [])json",
            R"json("resources": [{"kind":"json","name":"runtime.corpus","path":"runtime/corpus.json"}])json"
        );
        writeRootManifest(workspace, manifest);
        writeFile(workspace.source() / "plugin/main.luau", "return require(\"./support\")\n");
        writeFile(workspace.source() / "plugin/support.luau", "return {}\n");
        writeFile(workspace.source() / "runtime/corpus.json", "{\"answer\":42}\n");
        auto const initialized = initProject(ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        });
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

        auto const spec = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        };
        auto const built = buildProject(spec, {});
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        auto const snapshot = snapshotTree(workspace.build());
        CHECK(snapshot.at("generated/modules/dream/tool/main.luau")
              == "return require(\"./support\")\n");
        CHECK(snapshot.at("generated/modules/dream/tool/support.luau")
              == "return {}\n");
        CHECK(snapshot.at("generated/resources/dream/runtime.corpus.blob")
              == "{\"answer\":42}\n");

        auto const record = parsedJson(snapshot.at("generated/registrations/dream.json"));
        auto const environmentHash = operator_runtime::currentProjectPluginEnvironmentHash();
        REQUIRE(environmentHash.has_value());
        CHECK(record.find("plugin_environment_hash")->string() == environmentHash->hex());
        auto const modules = std::array{
            operator_runtime::ProjectModuleBlob{
                .name = "main", .source = "return require(\"./support\")\n"
            },
            operator_runtime::ProjectModuleBlob{
                .name = "support", .source = "return {}\n"
            },
        };
        auto const moduleHash = operator_runtime::derivePluginModuleManifestHash(
            "main",
            modules
        );
        REQUIRE(moduleHash.has_value());
        auto const* toolClosure = record.find("tool_closure");
        REQUIRE(toolClosure != nullptr);
        CHECK(
            toolClosure->find("module_manifest_hash")->string()
            == moduleHash->hex()
        );
        auto const* resourceRows = record.find("project_resources");
        REQUIRE(resourceRows != nullptr);
        REQUIRE(resourceRows->items().size() == 1U);
        CHECK(resourceRows->items()[0].find("kind")->string() == "json");
        CHECK(resourceRows->items()[0].find("size")->number() == 14.0);
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

    // A template cut is declared in umbraflow-project.json and nowhere else.
    // There is no spec member a caller can fill in, so every case below writes
    // the declaration the way a project author writes it, and the only thing
    // the caller still supplies is the resolver -- which is the whole point:
    // the kit is told what to cut by the project, and how to reach a source by
    // the caller, and it never learns what a directory is from either.
    TEST_CASE("a declared template cut")
    {
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

        auto const declaration =
            R"json([{"template": "locator/mark.png", "source_sha256s": [")json"
            + firstHash->hex() + R"json(", ")json" + secondHash->hex()
            + R"json("], "rect": {"x": 0, "y": 0, "width": 2, "height": 1}}])json";

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

        // The positive control. Without it every case below is satisfied by a
        // kit that refuses each declaration for its own reasons.
        SUBCASE("is cut from the sources the document names")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-template-cut"
            };
            auto const initialized = initializedWorkspaceDeclaring(
                workspace,
                declaration
            );
            REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
            auto const spec = ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
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

        // Not silently skipped: the build fails, the refusal names the hash
        // nobody could answer for, and the artifact the declaration asked for
        // is not in the build directory at all.
        SUBCASE("whose source cannot be resolved is refused by name")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-template-unresolvable"
            };
            auto const initialized = initializedWorkspaceDeclaring(
                workspace,
                declaration
            );
            REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
            auto const spec = ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            };
            auto const empty = TemplateSourceResolver{
                [](ContentHash const&) -> Result<std::vector<std::byte>>
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "this machine holds no corpus"
                    );
                }
            };

            auto const built = buildProject(spec, empty);
            REQUIRE_FALSE_MESSAGE(
                built.has_value(),
                "an unresolvable template source must fail the build"
            );
            CHECK_MESSAGE(
                messageOf(built).find(firstHash->hex()) != std::string::npos,
                "the refusal must name the source hash it could not resolve"
            );
            CHECK_MESSAGE(
                !snapshotTree(workspace.build()).contains(
                    "generated/templates/locator/mark.png"
                ),
                "a cut that could not be made must not leave an artifact"
            );
            CHECK_FALSE_MESSAGE(
                checkProject(spec, empty).has_value(),
                "project check must refuse exactly what project build refused"
            );
        }

        // The resolver is not trusted to answer honestly. It is handed a hash
        // and its answer is re-hashed, so a store whose file names lie is
        // caught here rather than in whichever caller happened to look.
        SUBCASE("whose resolved bytes hash to something else is refused")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-template-mismatch"
            };
            auto const initialized = initializedWorkspaceDeclaring(
                workspace,
                declaration
            );
            REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
            auto const spec = ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            };
            // Answers every hash with the second source's bytes, so the first
            // request gets bytes that are a valid PNG and the wrong one.
            auto const liar = TemplateSourceResolver{
                [bytes = *secondEncoded](
                    ContentHash const&
                ) -> Result<std::vector<std::byte>>
                {
                    return bytes;
                }
            };

            auto const built = buildProject(spec, liar);
            REQUIRE_FALSE_MESSAGE(
                built.has_value(),
                "bytes that do not hash to the declared source must be refused"
            );
            CHECK_MESSAGE(
                messageOf(built).find(firstHash->hex()) != std::string::npos,
                "the refusal must name the source that was asked for"
            );
            CHECK_MESSAGE(
                messageOf(built).find(secondHash->hex()) != std::string::npos,
                "the refusal must name the hash the bytes actually have"
            );
        }
    }

    TEST_CASE("project build regenerates its adapter closure solely from declared source")
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
            "project build must generate the named tool closure"
        );
        CHECK_MESSAGE(
            snapshot.at(std::string{k_generatedWorkflowAdapter})
                == expected->toolModule,
            "generated tool bytes must come from the declared source"
        );

        writeFile(
            workspace.build() / k_generatedWorkflowAdapter,
            "hand edited\n"
        );
        auto const rebuilt = buildProject(directories, {});
        REQUIRE_MESSAGE(rebuilt.has_value(), messageOf(rebuilt));
        snapshot = snapshotTree(workspace.build());
        CHECK_MESSAGE(
            snapshot.at(std::string{k_generatedWorkflowAdapter})
                == expected->toolModule,
            "a generated tool closure must never become the next build's input"
        );
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
        };
        auto const secondSpec = ProjectBuildSpec{
            .sourceDirectory = second.source(),
            .buildDirectory  = second.build(),
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
        REQUIRE(inputs->items().size() == 2U);
        CHECK_MESSAGE(
            std::ranges::any_of(
                inputs->items(),
                [](json::Value const& row)
                {
                    return row.find("path")->string() == k_workflowDeclarationInput;
                }
            ),
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

    // A module the declaration names and the tree does not hold. It replaces a
    // case that used to name a module outside a separately written input
    // ledger: with the input set derived from this document, a module the
    // document names is in it by construction and that refusal could no longer
    // fire. What remains reachable -- and is what a migration actually hits --
    // is the file simply not being there.
    TEST_CASE("project build names a declared module the source tree lacks")
    {
        auto const workspace = TemporaryWorkspace{
            "uf-project-artifact-closure-negative"
        };
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        writeRootManifest(
            workspace,
            replacedOnce(
                manifestDeclaringCuts("[]"),
                std::string{k_handWrittenPlugin},
                "plugin/absent.luau"
            )
        );
        auto const built = buildProject(
            ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            },
            {}
        );

        REQUIRE_FALSE_MESSAGE(
            built.has_value(),
            "project build must reject a module the source tree does not hold"
        );
        CHECK_MESSAGE(
            messageOf(built).find("plugin/absent.luau") != std::string::npos,
            "the refusal must name the module source"
        );
        CHECK_FALSE_MESSAGE(
            std::filesystem::exists(
                workspace.build() / k_artifactManifestName
            ),
            "the refusal must happen before build artifacts are written"
        );
    }

    TEST_CASE("project build check and freeze share runtime module admission")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-module-admission"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const candidate = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        };
        auto const baseline = buildProject(candidate, {});
        REQUIRE_MESSAGE(baseline.has_value(), messageOf(baseline));

        auto rejectedSource = std::string{};
        SUBCASE("empty")
        {
            rejectedSource.clear();
        }
        SUBCASE("invalid UTF-8")
        {
            rejectedSource.assign(1U, static_cast<char>(0xff));
        }
        SUBCASE("over the per-module byte ceiling")
        {
            rejectedSource.assign(
                script::PureDataProgram::k_maximumModuleSourceBytes + 1U,
                'x'
            );
        }
        writeFile(workspace.source() / k_handWrittenPlugin, rejectedSource);

        CHECK_FALSE_MESSAGE(
            buildProject(candidate, {}).has_value(),
            "project build must refuse a module the runtime cannot admit"
        );
        CHECK_FALSE_MESSAGE(
            checkProject(candidate, {}).has_value(),
            "project check must refuse a module the runtime cannot admit"
        );
        CHECK_FALSE_MESSAGE(
            freezeProject(
                ProjectFreezeSpec{
                    .candidate   = candidate,
                    .releaseRoot = workspace.releases(),
                },
                {}
            ).has_value(),
            "project freeze must refuse a module the runtime cannot admit"
        );
    }

    TEST_CASE("project build check and freeze share runtime resource admission")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-resource-admission"};
        auto resourceKind  = std::string_view{};
        auto acceptedBytes = std::string{};
        auto rejectedBytes = std::string{};
        SUBCASE("invalid JSON")
        {
            resourceKind  = "json";
            acceptedBytes = "{\"ok\":true}\n";
            rejectedBytes = "not-json\n";
        }
        SUBCASE("over the per-resource byte ceiling")
        {
            resourceKind  = "bytes";
            acceptedBytes = "ok";
            rejectedBytes.assign(
                script::PureDataProgram::k_maximumResourceBytes + 1U,
                'x'
            );
        }
        auto manifest = replacedOnce(
            acceptedDeploymentManifest(),
            R"json("resources": [])json",
            std::string{R"json("resources": [{"kind":")json"}
                + std::string{resourceKind}
                + R"json(","name":"runtime.corpus","path":"runtime/corpus.blob"}])json"
        );
        writeRootManifest(workspace, manifest);
        writeFile(workspace.source() / k_handWrittenPlugin, "return {}\n");
        writeFile(workspace.source() / "runtime/corpus.blob", acceptedBytes);
        auto const initialized = initProject(ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        });
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const candidate = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        };
        auto const baseline = buildProject(candidate, {});
        REQUIRE_MESSAGE(baseline.has_value(), messageOf(baseline));

        writeFile(workspace.source() / "runtime/corpus.blob", rejectedBytes);
        CHECK_FALSE_MESSAGE(
            buildProject(candidate, {}).has_value(),
            "project build must refuse a resource the runtime cannot admit"
        );
        CHECK_FALSE_MESSAGE(
            checkProject(candidate, {}).has_value(),
            "project check must refuse a resource the runtime cannot admit"
        );
        CHECK_FALSE_MESSAGE(
            freezeProject(
                ProjectFreezeSpec{
                    .candidate   = candidate,
                    .releaseRoot = workspace.releases(),
                },
                {}
            ).has_value(),
            "project freeze must refuse a resource the runtime cannot admit"
        );
    }

    // The direct-plugin tier's admission gate, in every direction it has. The
    // accepted subcases are not decoration: without them a gate that refused
    // every deployment manifest would satisfy the refusals and prove nothing.
    //
    // The two generated subcases are the ones the first version of this gate
    // got backwards. It demanded a justification from every deployment with a
    // `plugin` member, including one naming an adapter the kit itself
    // generated -- so an author staying on the declarative tier had to invent a
    // reason, which is the exact failure the member exists to prevent.
    //
    // Presence only. No case here states that the text is a true reason, and
    // none can; whether a plugin could have been a declaration instead is
    // program equivalence and stays a review obligation at plugin acceptance
    // (docs/pitfalls/checks-that-cannot-fail.md).
    TEST_CASE("project refuses a hand-written plugin with no stated justification")
    {
        auto const specFor = [](
            TemporaryWorkspace const& workspace,
            std::string const& manifest
        )
        {
            auto const validManifest = manifest.contains(k_generatedPlugin)
                ? deploymentManifest(
                    k_generatedPlugin,
                    k_generatedAuthoring,
                    "",
                    "[]"
                )
                : acceptedDeploymentManifest();
            auto const initialized = initializedDeploymentWorkspace(
                workspace,
                validManifest
            );
            REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
            writeRootManifest(workspace, manifest);
            return ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            };
        };

        SUBCASE("absent")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-justification-absent"
            };
            auto const spec = specFor(
                workspace,
                deploymentManifest(
                    k_handWrittenPlugin,
                    k_handWrittenAuthoring,
                    "",
                    "[]"
                )
            );

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
                messageOf(checked).find("deployments[0]") != std::string::npos,
                "the refusal must name the deployment block it judged"
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
            auto const spec = specFor(
                workspace,
                deploymentManifest(
                    k_handWrittenPlugin,
                    k_handWrittenAuthoring,
                    R"json(      "plugin_justification": " \t\n ",
)json",
                    "[]"
                )
            );

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
                messageOf(checked).find("plugin_justification") != std::string::npos,
                "the refusal must name the blank member"
            );
            CHECK_MESSAGE(
                messageOf(checked).find("pattern") != std::string::npos,
                "a blank justification must be refused by the pattern, not by "
                "the required clause an absent one trips"
            );
        }

        SUBCASE("stated")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-justification-stated"
            };
            auto const spec = specFor(workspace, acceptedDeploymentManifest());

            auto const built = buildProject(spec, {});
            REQUIRE_MESSAGE(built.has_value(), messageOf(built));
            auto const checked = checkProject(spec, {});
            REQUIRE_MESSAGE(checked.has_value(), messageOf(checked));
        }

        SUBCASE("generated, stating none")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-justification-generated"
            };
            auto const spec = specFor(
                workspace,
                deploymentManifest(
                    k_generatedPlugin,
                    k_generatedAuthoring,
                    "",
                    "[]"
                )
            );

            // A deployment naming a generated adapter owes no justification:
            // the adapter IS the declarative tier.
            auto const built = buildProject(spec, {});
            REQUIRE_MESSAGE(built.has_value(), messageOf(built));
            auto const checked = checkProject(spec, {});
            REQUIRE_MESSAGE(checked.has_value(), messageOf(checked));
        }

        SUBCASE("generated, stating one")
        {
            auto const workspace = TemporaryWorkspace{
                "uf-project-justification-generated-stated"
            };
            auto const spec = specFor(
                workspace,
                deploymentManifest(
                    k_generatedPlugin,
                    k_generatedAuthoring,
                    k_statedJustification,
                    "[]"
                )
            );

            auto const checked = checkProject(spec, {});
            REQUIRE_FALSE_MESSAGE(
                checked.has_value(),
                "a justification beside a generated adapter is the invented "
                "reason this member exists to keep out"
            );
            CHECK_MESSAGE(
                messageOf(checked).find("plugin_justification") != std::string::npos,
                "the refusal must name the member it refuses"
            );
        }
    }

    // The root document is what makes a source tree a project, so every action
    // reads and judges it.
    TEST_CASE("project init judges the root document")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-manifest-undeclared"};
        writeFile(workspace.source() / k_handWrittenPlugin, "return {}\n");
        writeRootManifest(
            workspace,
            deploymentManifest(
                k_handWrittenPlugin,
                k_handWrittenAuthoring,
                "",
                "[]"
            )
        );

        auto const initialized = initProject(
            ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            }
        );
        CHECK_FALSE_MESSAGE(
            initialized.has_value(),
            "project init must judge umbraflow-project.json"
        );
        CHECK_MESSAGE(
            messageOf(initialized).find("plugin_justification") != std::string::npos,
            "the refusal must be the justification gate rather than an "
            "unrelated failure"
        );

        // And a source tree with no root document at all is not a project,
        // rather than a project whose gate is vacuous.
        auto const bare    = TemporaryWorkspace{"uf-project-manifest-absent"};
        auto const bareInit = initProject(
            ProjectBuildSpec{
                .sourceDirectory = bare.source(),
                .buildDirectory  = bare.build(),
            }
        );
        REQUIRE_FALSE_MESSAGE(
            bareInit.has_value(),
            "a source tree with no umbraflow-project.json is not a project"
        );
        CHECK_MESSAGE(
            messageOf(bareInit).find(k_deploymentManifestInput)
                != std::string::npos,
            "the refusal must name the document the tree lacks"
        );
    }

    TEST_CASE("fault matrix tamper names the altered frozen release file")
    {
        auto const workspace = TemporaryWorkspace{"uf-project-release"};
        auto const initialized = initializedWorkflowWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const candidate = ProjectBuildSpec{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
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

    TEST_CASE("project build accepts a build directory inside the source tree")
    {
        // The pin domain is the declared file set read by path, never a
        // directory scan, so a build tree nested under the source cannot leak
        // into the hashed input; the C++-shaped layout is legal.
        auto const workspace   = TemporaryWorkspace{"uf-project-overlap"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

        auto const nestedBuild = workspace.source() / "generated";
        auto error             = std::error_code{};
        REQUIRE(std::filesystem::create_directories(nestedBuild, error));
        REQUIRE_FALSE(error);

        auto const built = buildProject(
            ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = nestedBuild,
            },
            {}
        );
        REQUIRE_MESSAGE(
            built.has_value(),
            "project build must accept a build directory inside the source "
            "tree"
        );
        CHECK_MESSAGE(
            std::filesystem::exists(nestedBuild / k_buildReceiptName),
            "a nested build must receive the build receipt"
        );
    }

    TEST_CASE("project build refuses a source directory inside the build tree")
    {
        // The other direction stays refused: a build directory that contains
        // the source has no use.
        auto const workspace   = TemporaryWorkspace{"uf-project-overlap"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

        auto const built = buildProject(
            ProjectBuildSpec{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.source().parent_path(),
            },
            {}
        );
        REQUIRE_FALSE_MESSAGE(
            built.has_value(),
            "project build must reject a build directory that contains the "
            "source tree"
        );
        CHECK_MESSAGE(
            messageOf(built).find("source directory must not be inside") != std::string::npos,
            "containment diagnostic must name the source/build rule"
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
  "tool_name": "chaos.project.dismiss_known_overlay",
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

        CHECK_MESSAGE(
            !generated->toolModule.empty(),
            "a one-step adapter must carry its tool closure"
        );
    }

    TEST_CASE("workflow declaration is closed and uniquely names every state")
    {
        // The lock's rule, measured: an additionalProperties: false site that
        // catches an undeclared member is ClosedSchema at every depth, a
        // required member missing is a shape violation, and both verdicts come
        // from the published schema, which is the single validation mechanism.
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
        CHECK(
            undeclared.error().message().find("script") != std::string_view::npos
        );

        auto const requiredMissing = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            replacedOnce(
                validWorkflowDeclaration(),
                "  \"ui_finding\": {\"kind\": \"observed_instance_absent\"},\n",
                ""
            )
        );
        REQUIRE_FALSE(requiredMissing.has_value());
        CHECK(
            requiredMissing.error().message().starts_with("MalformedWorkflowTool")
        );

        auto const freshExtra = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            replacedOnce(
                validWorkflowDeclaration(),
                "\"require_unambiguous\": true",
                "\"require_unambiguous\": true,\n    \"fresh_extra\": 1"
            )
        );
        REQUIRE_FALSE(freshExtra.has_value());
        CHECK(freshExtra.error().message().starts_with("ClosedSchema"));

        auto const findingExtra = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            replacedOnce(
                validWorkflowDeclaration(),
                "\"ui_finding\": {\"kind\": \"observed_instance_absent\"}",
                "\"ui_finding\": {\"kind\": \"observed_instance_absent\", \"ui_extra\": 1}"
            )
        );
        REQUIRE_FALSE(findingExtra.has_value());
        CHECK(findingExtra.error().message().starts_with("ClosedSchema"));

        auto const waitExtra = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            replacedOnce(
                validWorkflowDeclaration(),
                "\"timeout_ms\": 1000",
                "\"timeout_ms\": 1000,\n      \"wait_extra\": 1"
            )
        );
        REQUIRE_FALSE(waitExtra.has_value());
        CHECK(waitExtra.error().message().starts_with("ClosedSchema"));

        auto const actionExtra = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            replacedOnce(
                validWorkflowDeclaration(),
                "\"timeout_ms\": 2000",
                "\"timeout_ms\": 2000,\n      \"action_extra\": 1"
            )
        );
        REQUIRE_FALSE(actionExtra.has_value());
        CHECK(actionExtra.error().message().starts_with("ClosedSchema"));

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

    // The schedule's keys leave the generator as next_step step_key values,
    // which operator-v1 constrains as its Identifier: a leading digit, and
    // '.', '_', '-' and ':' inside, all admitted there, and 128 characters
    // at most. The declaration must admit exactly what the emitted adapter
    // may carry, so the published schema speaks the operator's grammar and
    // the generator hands the schema the whole question.
    TEST_CASE("workflow state_key follows the operator Identifier grammar")
    {
        auto const digitLed = replacedOnce(
            replacedOnce(
                validWorkflowDeclaration(),
                "\"state_key\": \"await-overlay\"",
                "\"state_key\": \"9await:overlay\""
            ),
            "\"await-overlay\", \"dismiss-overlay\"",
            "\"9await:overlay\", \"dismiss-overlay\""
        );
        auto const accepted = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            digitLed
        );
        REQUIRE_MESSAGE(
            accepted.has_value(),
            "a state key with a leading digit and a colon must be accepted"
        );

        auto const longestKey = std::string(128, 'a');
        auto const boundary = replacedOnce(
            replacedOnce(
                validWorkflowDeclaration(),
                "\"state_key\": \"await-overlay\"",
                "\"state_key\": \"" + longestKey + "\""
            ),
            "\"await-overlay\", \"dismiss-overlay\"",
            "\"" + longestKey + "\", \"dismiss-overlay\""
        );
        REQUIRE_MESSAGE(
            generateDeclarativeWorkflowAdapter("chaos.project", boundary)
                .has_value(),
            "a 128-character state key, the Identifier cap, must be accepted"
        );

        auto const overlong = generateDeclarativeWorkflowAdapter(
            "chaos.project",
            replacedOnce(
                validWorkflowDeclaration(),
                "\"state_key\": \"await-overlay\"",
                "\"state_key\": \"" + std::string(129, 'a') + "\""
            )
        );
        REQUIRE_FALSE_MESSAGE(
            overlong.has_value(),
            "a 129-character state key must be refused, not generated"
        );
        CHECK_MESSAGE(
            overlong.error().message().starts_with("MalformedWorkflowTool"),
            "an overlong key is a shape violation, not a closed-object one"
        );
        CHECK_MESSAGE(
            overlong.error().message().find("refused") != std::string_view::npos,
            "the refusal must come from the published schema"
        );
    }
}
