// One case per rule the project directory format states, each built by taking
// a directory the loader accepts and breaking exactly one thing in it.
//
// Every case opens with the acceptance it is the negation of. That is not
// politeness: a loader that refused everything would satisfy every refusal
// below, and the fixture directory these cases start from is the only evidence
// that it does not.
//
// R8's second half has no case here and cannot have one. The probe frame's
// decoded extent must be the model's, and after the Q2 ruling the model's
// extent does not exist until the Host has activated the artifact -- which a
// loader does not do. It moved into the conformance run instead; see
// docs/archive/plans/2026-08-11-project-as-data.md 2.7 R8. What R9's case covers is
// the other claim about the same file, that it decodes at all, which is
// answerable here and was unchecked until 2026-08-11.

#include "umbraflow/project-schemas.hpp"

#include <deployment/project-directory.hpp>

#include <project/project-kit.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <image/png.hpp>

#include <json/value.hpp>

#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>

#include <script/pure-data-program.hpp>

#include <task/runtime-model-file.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace uf::deployment
{
    namespace
    {
        namespace umbraflow = operator_runtime::test_support;

        [[nodiscard]]
        auto readAll(std::filesystem::path const& path) -> std::vector<std::byte>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            auto const text = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
            auto bytes = std::vector<std::byte>{};
            bytes.reserve(text.size());
            for (auto const character : text)
            {
                bytes.emplace_back(static_cast<std::byte>(character));
            }
            return bytes;
        }

        auto write(std::filesystem::path const& path, std::string_view bytes) -> void
        {
            std::filesystem::create_directories(path.parent_path());
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(stream.good());
        }

        // An inline identity schema's digest, as the registration pins it.
        // Everything declarative is inline now, so the loader renders the
        // schema canonically itself and hashes what it rendered -- a project
        // author never writes that digest, and the source spelling above is not
        // what it is taken over.
        [[nodiscard]]
        auto identitySchemaHash(std::string_view schema) -> ContentHash
        {
            auto const parsed = json::parse(schema);
            REQUIRE(parsed.has_value());
            return umbraflow::schemaHash(json::canonicalBytes(*parsed));
        }

        [[nodiscard]]
        auto readText(std::filesystem::path const& path) -> std::string
        {
            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            return std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{}
            };
        }

        class TemporaryScaffold final
        {
            std::filesystem::path m_root;

        public:
            TemporaryScaffold()
                : m_root{
                    std::filesystem::temp_directory_path()
                    / ("uf-project-scaffold-"
                       + std::to_string(std::random_device{}()))
                }
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_root, error);
                REQUIRE(std::filesystem::create_directories(m_root, error));
            }

            TemporaryScaffold(TemporaryScaffold const&)                    = delete;
            TemporaryScaffold(TemporaryScaffold&&)                         = delete;
            auto operator=(TemporaryScaffold const&) -> TemporaryScaffold& = delete;
            auto operator=(TemporaryScaffold&&) -> TemporaryScaffold&      = delete;

            ~TemporaryScaffold()
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_root, error);
            }

            [[nodiscard]] auto path() const -> std::filesystem::path const&
            {
                return m_root;
            }
        };

        // The fixture's probe frame, encoded by the framework's own PNG writer.
        // R9 decodes what probe_frame names, so the eight signature bytes this
        // fixture used to write are no longer a capture.
        [[nodiscard]] auto probeFramePng() -> std::string
        {
            auto const pixel   = std::array<std::byte, 4>{};
            auto const encoded = image::encodeRgbaPng("probe frame", 1U, 1U, pixel);
            REQUIRE(encoded.has_value());

            auto bytes = std::string{};
            for (auto const value : *encoded)
            {
                bytes.push_back(static_cast<char>(std::to_integer<uint8>(value)));
            }
            return bytes;
        }

        // One project directory the loader accepts, on disk, with every file
        // written from the exemplar's real schema bytes. Cases mutate a copy of
        // the manifest text and rewrite one file; nothing here is shared
        // between cases.
        class Fixture final
        {
            std::filesystem::path m_root{};

        public:
            Fixture()
            {
                auto const unique = std::filesystem::path{
                    "uf-project-" + std::to_string(std::random_device{}()),
                };
                m_root = std::filesystem::temp_directory_path() / unique;
                std::filesystem::remove_all(m_root);
                std::filesystem::create_directories(m_root);

                for (auto const& deployment : std::array{
                         std::string_view{"alpha"},
                         std::string_view{"beta"},
                     })
                {
                    writeDeployment(deployment);
                }
                write(m_root / "runtime/artifact/runtime-model.toml", "[[page]]\n");
                write(m_root / "runtime/probe-frame.png", probeFramePng());
                write(m_root / "umbraflow-project.json", projectManifest());
                write(m_root / "umbraflow-conformance.json", conformanceManifest());
            }

            Fixture(Fixture const&)                        = delete;
            Fixture(Fixture&&)                             = delete;
            auto operator=(Fixture const&) -> Fixture&     = delete;
            auto operator=(Fixture&&) -> Fixture&          = delete;

            ~Fixture()
            {
                auto discarded = std::error_code{};
                std::filesystem::remove_all(m_root, discarded);
            }

            [[nodiscard]] auto path() const -> std::filesystem::path const&
            {
                return m_root;
            }

            auto rewrite(std::string_view relative, std::string_view bytes) const
                -> void
            {
                write(m_root / relative, bytes);
            }

            auto remove(std::string_view relative) const -> void
            {
                std::filesystem::remove(m_root / relative);
            }

            // The two entry points, against one directory. Which one a case
            // calls is the case's claim about which reader owns the rule it
            // breaks: a rule the product enforces must refuse the production
            // load, and a rule only a suite needs must leave it accepting.
            [[nodiscard]]
            auto load(std::span<ExpectedRegistration const> expected = {}) const
                -> Result<LoadedProject>
            {
                return loadProductionProject(m_root, expected);
            }

            [[nodiscard]]
            auto loadForConformance(
                std::span<ExpectedRegistration const> expected = {}
            ) const -> Result<ConformanceProject>
            {
                return loadConformanceProject(m_root, expected);
            }

            // The accepted manifest text, so a case can substitute one span of
            // it and write the result back. Restating it is the point: a case
            // that built its own manifest would prove nothing about the one the
            // loader accepts.
            [[nodiscard]] static auto projectManifest() -> std::string
            {
                return std::string{R"json({
  "$comment": "Why this fixture has two deployments: the conformance document needs a second registration that can mint documents of its own.",
  "schema": "umbraflow-project/v3",
  "runtime_artifact": "runtime/artifact",
  "primary_deployment": "alpha",
  "template_cuts": [],
  "deployments": [)json"}
                    + deploymentBlock("alpha") + "," + deploymentBlock("beta")
                    + "]}";
            }

            // The whole declaration is this one block: there is no schema file
            // beside it and no Tool Catalog document. `tools` and the identity
            // schema are inline, and the only paths left name assets -- the
            // Luau closure and the opaque resource.
            [[nodiscard]]
            static auto deploymentBlock(std::string_view name) -> std::string
            {
                auto const pluginId = "fixture." + std::string{name};
                auto block = std::string{R"json({"name":")json"};
                block += name;
                block += R"json(","plugin_id":")json";
                block += pluginId;
                // This deployment binds no Tool, so its closure states an empty
                // export set. The member is written out rather than omitted: an
                // absent member would be an absence carrying a meaning.
                block += R"json(","tool_closure":{"entry":"main",)json"
                    R"json("exported_entry_points":[],)json"
                    R"json("modules":[{"name":"main","path":"plugin/)json";
                block += name;
                block += R"json(-tool.luau"}]},)json";
                block += R"json("plugin_authoring":"hand-written",)json";
                block += R"json("plugin_justification":"A fixture plugin that )json"
                    R"json(answers from constants: umbraflow-declarative-)json"
                    R"json(workflow-tool/v1 has no member that decides what a )json"
                    R"json(handler returns.",)json";
                block += R"json("tools":)json"
                    + umbraflow::toolDeclarations(pluginId) + ",";
                block += R"json("observed_instance_identity_schemas":[)json"
                    R"json({"name":")json"
                    + std::string{umbraflow::k_observedIdentitySchemaName}
                    + R"json(","schema":)json"
                    + std::string{umbraflow::k_observedIdentitySchema}
                    + R"json(}],)json";
                block += R"json("tool_bindings":[],)json";
                block += R"json("resources":[{"kind":"bytes","name":"page-model","path":"blob/)json";
                block += name;
                block += R"json(.blob"}]})json";
                return block;
            }

            [[nodiscard]] static auto conformanceManifest() -> std::string
            {
                return R"json({"schema":"umbraflow-conformance/v3",)json"
                    R"json("probe_frame":"runtime/probe-frame.png",)json"
                    R"json("under_test":{"deployment":"alpha","vocabulary":)json"
                    + vocabulary("fixture.alpha")
                    + R"json(},"foreign":{"deployment":"beta","vocabulary":)json"
                    + vocabulary("fixture.beta") + "}}";
            }

            // The two roles are two registrations, and a Tool is named inside
            // the namespace its own registration owns, so the two vocabularies
            // share no tool name. The names are composed from the plugin id
            // rather than written out twice, which is also why the role that
            // owns a name is visible at every call site below.
            [[nodiscard]]
            static auto vocabulary(std::string_view pluginId) -> std::string
            {
                auto const tool = [pluginId](std::string_view local)
                {
                    return std::string{pluginId} + "." + std::string{local};
                };
                return R"json({"mutating_tool":")json" + tool("command-1")
                       + R"json(",)json"
                       + R"json("other_mutating_tool":")json" + tool("command-2")
                       + R"json(",)json"
                       + R"json("read_only_tool":")json" + tool("observe-1")
                       + R"json(",)json"
                       + R"json("tool_arguments":"{\"value\":1}",)json"
                       R"json("refused_tool_arguments":"{\"value\":0}",)json"
                       + R"json("absent_tool":")json" + tool("command-absent")
                       + R"json(",)json"
                       + R"json("approval_required_plan_tool":")json"
                       + tool("approval-plan")
                       + R"json(",)json"
                       + R"json("ui_action":{"surface":"fixture.surface",)json"
                       R"json("ui_target":"fixture.target","action":"fixture.press"}})json";
            }

        private:
            // The only files a deployment names: its one Luau closure and its
            // one opaque resource. Everything declarative is in the block.
            auto writeDeployment(std::string_view name) const -> void
            {
                write(
                    m_root / "plugin" / (std::string{name} + "-tool.luau"),
                    "return {plugin_id = \"fixture." + std::string{name} + "\"}\n"
                );
                // An artifact root is a JSON document: what a plugin reaches is
                // the value it denotes, and bytes that are not one are refused
                // when the plugin is registered.
                write(
                    m_root / "blob" / (std::string{name} + ".blob"),
                    R"({"blob":"fixture-)" + std::string{name} + R"("})"
                );
            }
        };

        // One substring of an accepted document, restated. Every refusal below
        // is about the single substitution it makes, so the document it starts
        // from has to be the accepted one and the substitution has to be found.
        [[nodiscard]]
        auto substituted(
            std::string_view exact,
            std::string_view from,
            std::string_view to
        ) -> std::string
        {
            auto const at = exact.find(from);
            REQUIRE(at != std::string_view::npos);
            auto restated = std::string{exact};
            restated.replace(at, from.size(), to);
            return restated;
        }

        template <typename Loaded>
        [[nodiscard]]
        auto why(Result<Loaded> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<the directory was accepted>"}
                : std::string{outcome.error().message()};
        }
    }

    // The two project directories this repository ships as data, read from the
    // generated build stage rather than built by the fixture above.
    //
    // The RuntimeArtifact half is here for a second reason. Its manifest is the
    // one file in a project directory that must be byte-exact, and the single
    // trailing newline scripts/fix_format.py adds to every .json is exactly
    // what parseManifest refuses -- which is why that script no longer owns a
    // directory holding umbraflow-project.json
    // (docs/archive/plans/2026-08-11-project-as-data.md 2.5). This is what says the
    // exclusion held.
    TEST_CASE("this repository's own example project directories load")
    {
        for (auto const& example : std::array{
                 std::filesystem::path{UF_STAGED_UMBRAFLOW_PROJECT},
                 std::filesystem::path{UF_STAGED_ARCANA_PROJECT},
             })
        {
            INFO(example.string());
            auto const loaded = loadProductionProject(example, {});
            INFO(why(loaded));
            REQUIRE(loaded.has_value());
            CHECK(loaded->deployments.size() == 2U);
            CHECK(loaded->findDeployment(loaded->primaryDeployment) != nullptr);

            // Each also ships the conformance fixture the suite runs against,
            // which is a second document and not a second reading of the one
            // above.
            auto const suite = loadConformanceProject(example, {});
            INFO(why(suite));
            REQUIRE(suite.has_value());
            CHECK(suite->underTest.deployment != suite->foreign.deployment);

            // The artifact root the project names, opened the way the installer
            // opens it. The root hash handed in is this case's own arithmetic
            // over the bytes it just read, so that one comparison proves
            // nothing; what is measured is that the manifest is exact canonical
            // bytes with nothing trailing and that every file it declares is
            // present at the size and digest it states.
            auto const manifestBytes = readAll(
                loaded->runtimeArtifactRoot
                / std::filesystem::path{task::k_runtimeArtifactManifestFileName}
            );
            REQUIRE_FALSE(manifestBytes.empty());
            auto const rootHash = sha256(std::span{manifestBytes});
            REQUIRE(rootHash.has_value());

            auto const installed =
                task::loadRuntimeArtifact(loaded->runtimeArtifactRoot, *rootHash);
            auto const refused = installed.has_value()
                ? std::string{}
                : std::string{installed.error().message()};
            INFO(refused);
            REQUIRE(installed.has_value());
            CHECK_FALSE(installed->assetPaths().empty());
        }
    }

    // A starter Project writes no JSON Schema at all: the one Tool it declares
    // carries its argument shape inline, and the whole declaration is the root
    // document. What this says is that the document the kit writes is one the
    // runtime reader compiles -- the two readers cannot link each other, so
    // nothing else states it.
    TEST_CASE("a scaffolded declaration forms one runtime deployment authority")
    {
        auto const workspace = TemporaryScaffold{};
        auto const scaffolded = project::scaffoldProject(
            project::ProjectScaffoldSpec{
                .sourceDirectory = workspace.path(),
                .pluginId        = "scaffold.project",
                .pluginForm      = project::ProjectPluginForm::Generated,
            }
        );
        auto const scaffoldDiagnostic = scaffolded.has_value()
            ? std::string{}
            : std::string{scaffolded.error().message()};
        INFO(scaffoldDiagnostic);
        REQUIRE(scaffolded.has_value());

        // Not one schema file, anywhere in the starter tree.
        for (auto const& entry : std::filesystem::recursive_directory_iterator{
                 workspace.path()
             })
        {
            CAPTURE(entry.path().string());
            CHECK_FALSE(entry.path().filename().string().ends_with(
                ".schema.json"
            ));
        }

        auto document = json::parse(
            readText(workspace.path() / k_projectManifestFileName)
        );
        REQUIRE(document.has_value());
        auto const* const p_deployments = document->find("deployments");
        REQUIRE(p_deployments != nullptr);
        REQUIRE(p_deployments->items().size() == 1U);
        auto const& block = p_deployments->items().front();
        auto const* const p_tools = block.find("tools");
        REQUIRE(p_tools != nullptr);
        auto const tools = json::canonicalBytes(*p_tools);

        auto const deployed = ProjectDeployment::create(
            ProjectDeploymentSources{
                .pluginId                        = "scaffold.project",
                .tools                           = tools,
                .observedInstanceIdentitySchemas = {},
            }
        );
        auto const deploymentDiagnostic = deployed.has_value()
            ? std::string{}
            : std::string{deployed.error().message()};
        INFO(deploymentDiagnostic);
        REQUIRE(deployed.has_value());
    }

    // Everything below breaks one thing in this directory, so this case is what
    // says the directory is otherwise whole. It also states what a load
    // produces, because no other case reads the result.
    TEST_CASE("a project directory becomes three authorities per deployment")
    {
        auto const  fixture = Fixture{};
        auto const  loaded  = fixture.load();
        INFO(why(loaded));
        REQUIRE(loaded.has_value());

        CHECK(loaded->deployments.size() == 2U);
        CHECK(loaded->primaryDeployment == "alpha");
        CHECK(loaded->runtimeArtifactRoot == fixture.path() / "runtime/artifact");

        auto const* const p_alpha = loaded->findDeployment("alpha");
        REQUIRE(p_alpha != nullptr);
        CHECK(p_alpha->generation.pluginId() == "fixture.alpha");
        // The deployment ships one closure, present and explicitly empty, which
        // is the whole statement that it binds no Tool.
        CHECK(p_alpha->toolClosure.entryModule == "main");
        REQUIRE(p_alpha->toolClosure.modules.size() == 1U);
        CHECK(p_alpha->toolClosure.modules.front().source.starts_with(
            "return {plugin_id ="
        ));
        CHECK(p_alpha->toolClosure.declaredEntryPoints.empty());
        REQUIRE(p_alpha->projectResources.size() == 1U);
        CHECK(p_alpha->projectResources.front().name == "page-model");

        // The two deployments are two registrations. Without this the suite's
        // whole reason for a foreign role would be satisfied by one.
        auto const* const p_beta = loaded->findDeployment("beta");
        REQUIRE(p_beta != nullptr);
        CHECK(p_alpha->generation.hash() != p_beta->generation.hash());

        // The authorities are bound to that registration and can be asked to
        // judge, which is the whole of what constructing them was for.
        CHECK(p_alpha->toolCatalogSchemaOwner.projectRegistrationHash()
              == p_alpha->generation.hash());
        auto const arguments = operator_runtime::CanonicalJson::parseExact(
            R"({"value":1})"
        );
        REQUIRE(arguments.has_value());
        CHECK(p_alpha->toolCatalogSchemaOwner
                  .validate("fixture.alpha.command-1", *arguments)
                  .has_value());
        CHECK_FALSE(p_alpha->toolCatalogSchemaOwner
                        .validate("fixture.alpha.command-absent", *arguments)
                        .has_value());

        // The last authority: the loader derived the identity hashes from the
        // bytes it read, pinned them in the registration, and built the
        // authority from the very bindings the deployment compiled -- so the
        // authority answers for exactly the registration it was bound to.
        REQUIRE(
            operator_runtime::ProjectIdentity{p_alpha->generation}
                .observedInstanceIdentitySchemaHashes().size()
            == 1U
        );
        CHECK(
            operator_runtime::ProjectIdentity{p_alpha->generation}
                .observedInstanceIdentitySchemaHashes()[0]
            == identitySchemaHash(umbraflow::k_observedIdentitySchema)
        );
        CHECK(
            p_alpha->observedInstanceIdentitySchemas.projectRegistrationHash()
            == p_alpha->generation.hash()
        );

        // Production loads retain only the authorities and pinned project data;
        // conformance-only input evidence has no member here to accumulate in.
    }

    // The member is required even when the set is empty, so a deployment
    // that omits it is refused rather than read as a silent default of zero
    // identity schemas -- the loader must have been told the shape of the
    // identity set by a document it read.
    TEST_CASE("a deployment is refused when the identity schema member is absent")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        auto const declared = std::string{
            R"json("observed_instance_identity_schemas":[{"name":")json"
        }
            + std::string{umbraflow::k_observedIdentitySchemaName}
            + R"json(","schema":)json"
            + std::string{umbraflow::k_observedIdentitySchema}
            + R"json(}],)json";
        fixture.rewrite(
            "umbraflow-project.json",
            substituted(Fixture::projectManifest(), declared, "")
        );
        auto const refused = fixture.load();
        REQUIRE_FALSE(refused.has_value());
        CHECK(why(refused).contains("observed_instance_identity_schemas"));
    }

    // The loader derives the identity hashes from the bytes it read and sorts
    // the derivation, so the declaration order in the block is never consulted
    // and the derived registration is sorted whatever the author wrote. This
    // declares the two documents in hash-descending order and requires the
    // load to succeed, which it can only do if the loader sorted before it
    // wrote the registration -- validateClaims refuses any other order.
    TEST_CASE("the loader sorts identity schema hashes before deriving the registration")
    {
        auto const secondSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://fixture.example/identity/overlay/v2",
    "type": "object",
    "additionalProperties": false,
    "required": ["native_id"],
    "properties": {
        "native_id": {"type": "string", "minLength": 1}
    }
})json"};

        auto const firstHash  = identitySchemaHash(umbraflow::k_observedIdentitySchema);
        auto const secondHash = identitySchemaHash(secondSchema);
        REQUIRE(firstHash != secondHash);

        auto const entry =
            [](std::string_view name, std::string_view schema) -> std::string
        {
            return std::string{R"json({"name":")json"} + std::string{name}
                + R"json(","schema":)json" + std::string{schema} + "}";
        };
        auto const first = entry(
            umbraflow::k_observedIdentitySchemaName,
            umbraflow::k_observedIdentitySchema
        );
        auto const second =
            entry("https://fixture.example/identity/overlay/v2", secondSchema);

        auto const fixture = Fixture{};
        auto block = Fixture::deploymentBlock("alpha");
        // Hash-descending declaration: whichever document hashes lower is
        // declared second, so a loader that kept the declaration order would
        // derive an unsorted registration and the load would refuse below.
        block = substituted(
            block,
            R"json("observed_instance_identity_schemas":[)json" + first + "],",
            R"json("observed_instance_identity_schemas":[)json"
                + (secondHash < firstHash
                       ? second + "," + first
                       : first + "," + second)
                + "],"
        );
        fixture.rewrite(
            "umbraflow-project.json",
            std::string{R"json({"schema":"umbraflow-project/v3",)json"}
                + R"json("runtime_artifact":"runtime/artifact",)json"
                + R"json("primary_deployment":"alpha","template_cuts":[],)json"
                + R"json("deployments":[)json" + block + "]}"
        );

        auto const loaded = fixture.load();
        INFO(why(loaded));
        REQUIRE(loaded.has_value());
        auto const* const p_alpha = loaded->findDeployment("alpha");
        REQUIRE(p_alpha != nullptr);
        // The identity is named rather than left a temporary: the accessor
        // returns a reference into it, and binding that to a name would leave
        // the vector destroyed before the first assertion reads it.
        auto const identity = operator_runtime::ProjectIdentity{p_alpha->generation};
        auto const& hashes  = identity.observedInstanceIdentitySchemaHashes();
        REQUIRE(hashes.size() == 2U);
        CHECK(hashes[0] < hashes[1]);
        CHECK(hashes[0] == std::min(firstHash, secondHash));
        CHECK(hashes[1] == std::max(firstHash, secondHash));
        CHECK(
            p_alpha->observedInstanceIdentitySchemas.projectRegistrationHash()
            == p_alpha->generation.hash()
        );
    }

    // The conformance load is the production load plus a layer, so this states
    // the layer: the same directory, the same deployments, and the three things
    // only umbraflow-conformance.json supplies.
    TEST_CASE("a conformance directory becomes that load plus two roles")
    {
        auto const fixture = Fixture{};
        auto const loaded  = fixture.loadForConformance();
        INFO(why(loaded));
        REQUIRE(loaded.has_value());

        // The production half is carried whole rather than restated.
        CHECK(loaded->loaded.deployments.size() == 2U);
        CHECK(loaded->loaded.primaryDeployment == "alpha");
        CHECK(loaded->loaded.findDeployment("alpha") != nullptr);

        CHECK(loaded->underTest.deployment == "alpha");
        CHECK(loaded->foreign.deployment == "beta");
        CHECK_FALSE(loaded->probeFrame.empty());

        // The vocabulary is read as strings and nothing else, and the argument
        // members carry the project's exact bytes rather than a shape this
        // loader chose.
        CHECK(
            loaded->underTest.vocabulary.mutatingTool
            == "fixture.alpha.command-1"
        );
        CHECK(
            loaded->underTest.vocabulary.absentTool
            == "fixture.alpha.command-absent"
        );
        CHECK(loaded->underTest.vocabulary.toolArguments == "{\"value\":1}");
        CHECK(loaded->underTest.vocabulary.uiAction.uiTarget == "fixture.target");
    }

    // The defect the split repairs, stated as the directory that could not be
    // expressed before it. A project at a read-only phase declares one
    // deployment, carries no conformance document at all, and every Tool it
    // declares is read_only -- so it has no mutating_tool to name, no
    // other_mutating_tool to distinguish from it, and no second deployment to
    // play a foreign role.
    //
    // Measured before this case existed: the loader required both documents,
    // both roles, two deployments and four mutating tools of every directory it
    // opened, and refused this one with "foreign is played by the deployment
    // beta, which umbraflow-project.json does not declare".
    TEST_CASE("a production project needs no conformance document at all")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        // Every mutating Tool becomes read_only. The declaration stays this
        // deployment's own, so nothing but the mutability words differ from the
        // accepted directory above.
        auto readOnly = std::string{R"json({"schema":"umbraflow-project/v3",)json"}
            + R"json("runtime_artifact":"runtime/artifact",)json"
            + R"json("primary_deployment":"alpha","template_cuts":[],)json"
            + R"json("deployments":[)json"
            + Fixture::deploymentBlock("alpha") + "]}";
        constexpr auto k_was = std::string_view{R"json("mutability":"mutating")json"};
        constexpr auto k_now = std::string_view{R"json("mutability":"read_only")json"};
        auto       replaced  = std::size_t{0};
        for (auto at = readOnly.find(k_was);
             at != std::string::npos;
             at = readOnly.find(k_was, at + k_now.size()))
        {
            readOnly.replace(at, k_was.size(), k_now);
            ++replaced;
        }
        REQUIRE(replaced > std::size_t{0});
        REQUIRE_FALSE(readOnly.contains(k_was));

        // One deployment, and no second one to play any other role.
        fixture.rewrite("umbraflow-project.json", readOnly);
        fixture.remove("umbraflow-conformance.json");

        auto const loaded = fixture.load();
        INFO(why(loaded));
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->deployments.size() == 1U);
        CHECK(loaded->primaryDeployment == "alpha");
        CHECK(loaded->findDeployment("alpha") != nullptr);
        CHECK(loaded->runtimeArtifactRoot == fixture.path() / "runtime/artifact");

        // The other half of the same fact: this directory is a project and is
        // not a conformance fixture, and the refusal names the document it
        // lacks rather than a role or a tool.
        auto const suite = fixture.loadForConformance();
        REQUIRE_FALSE(suite.has_value());
        CHECK(why(suite).contains(k_conformanceManifestFileName));
        CHECK(why(suite).contains(fixture.path().string()));
    }

    // R1. Each load requires the root documents it reads, at their fixed names,
    // and neither requires the other's.
    //
    // The message is asserted rather than only the refusal, and that is the
    // whole of what makes this case a check. Reading an absent document as
    // empty bytes also fails the load -- on "is not JSON", naming the document
    // it was looking for -- so a case that asked only whether the load failed
    // is satisfied by the "absent means empty" reading this rule exists to
    // forbid. Measured: that mutation left an earlier version of this case
    // green across all 39 of its assertions.
    TEST_CASE("R1 a load is refused without the root document it reads")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());
        REQUIRE(fixture.loadForConformance().has_value());

        // The conformance document is the one the product does not read, so
        // removing it refuses the conformance load and leaves the production
        // one accepting. A production loader that opened it would be red on the
        // second assertion of this block rather than on a message.
        fixture.remove("umbraflow-conformance.json");
        auto const missingConformance = fixture.loadForConformance();
        REQUIRE_FALSE(missingConformance.has_value());
        CHECK(why(missingConformance).contains(fixture.path().string()));
        CHECK(why(missingConformance).contains(k_conformanceManifestFileName));
        CHECK_FALSE(why(missingConformance).contains(k_projectManifestFileName));

        auto const withoutFixture = fixture.load();
        INFO(why(withoutFixture));
        CHECK(withoutFixture.has_value());

        // The project document is read by both, so its absence refuses both.
        fixture.rewrite("umbraflow-conformance.json", Fixture::conformanceManifest());
        REQUIRE(fixture.load().has_value());
        fixture.remove("umbraflow-project.json");

        auto const missingProject = fixture.load();
        REQUIRE_FALSE(missingProject.has_value());
        CHECK(why(missingProject).contains(fixture.path().string()));
        CHECK(why(missingProject).contains(k_projectManifestFileName));

        auto const suiteWithoutProject = fixture.loadForConformance();
        REQUIRE_FALSE(suiteWithoutProject.has_value());
        CHECK(why(suiteWithoutProject).contains(k_projectManifestFileName));
    }

    // R2. Every member of every framework-owned document is required and every
    // object is closed. The one member every object also admits is $comment,
    // and the accepted manifest above carries one, so this case proves the
    // exception exists as well as the rule.
    TEST_CASE("R2 a missing member and an unknown member are both refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("primary_deployment": "alpha",)json",
                ""
            )
        );
        auto const missing = fixture.load();
        REQUIRE_FALSE(missing.has_value());
        CHECK(why(missing).contains("primary_deployment"));

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("primary_deployment")json",
                R"json("invented_member": 1, "primary_deployment")json"
            )
        );
        auto const unknown = fixture.load();
        REQUIRE_FALSE(unknown.has_value());
        CHECK(why(unknown).contains("invented_member"));

        // A $comment is the one member that is admitted and read by nobody.
        // Without this the case above would equally describe a format that
        // gives a project no place to say why its document is shaped as it is.
        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json({"name":"alpha")json",
                R"json({"$comment":"why alpha is the primary","name":"alpha")json"
            )
        );
        auto const commented = fixture.load();
        INFO(why(commented));
        CHECK(commented.has_value());
    }

    // R3. Every path member is a manifest spelling in ConfinedRoot's sense,
    // checked before anything is opened.
    //
    // Both refusals below assert the reason and not only the fact, because
    // ConfinedRoot refuses these two paths as well -- measured: removing the
    // spelling check leaves both loads failing and only the messages change.
    // What R3 buys is that the refusal names the manifest member and the
    // spelling rule rather than whatever the operating system said about a
    // path nobody should have offered it.
    TEST_CASE("R3 a path that is not a manifest spelling is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        // The file the traversal names exists, and is the very file the block
        // it replaces already named -- so nothing but the spelling rule can
        // refuse this one.
        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("path":"plugin/alpha-tool.luau")json",
                R"json("path":"schema/../plugin/alpha-tool.luau")json"
            )
        );
        auto const traversal = fixture.load();
        REQUIRE_FALSE(traversal.has_value());
        CHECK(why(traversal).contains("'..'"));

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("path":"plugin/alpha-tool.luau")json",
                R"json("path":"plugin\\alpha-tool.luau")json"
            )
        );
        auto const backslash = fixture.load();
        REQUIRE_FALSE(backslash.has_value());
        CHECK(why(backslash).contains("'/'"));
    }

    // The direct-plugin tier is the exception, so a deployment block whose
    // plugin_authoring is "hand-written" must also say which member or semantic
    // of umbraflow-declarative-workflow-tool/v1 cannot express it. Both halves
    // are asserted because they are enforced by two different clauses:
    // `required` under `then` catches the absent member and the
    // PluginJustification pattern catches a member present and blank, and
    // neither stands in for the other.
    //
    // PRESENCE ONLY. Nothing here judges whether the stated reason is true.
    TEST_CASE("a hand-written plugin without a stated justification is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        auto const stated = Fixture::projectManifest();
        auto const at     = stated.find(R"json("plugin_justification":")json");
        REQUIRE(at != std::string::npos);
        auto const end = stated.find(R"json(",)json", at);
        REQUIRE(end != std::string::npos);
        auto const member = stated.substr(at, end + 2U - at);

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(stated, member, "")
        );
        auto const absent = fixture.load();
        REQUIRE_FALSE(absent.has_value());
        CHECK(why(absent).contains("plugin_justification"));

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                stated,
                member,
                R"json("plugin_justification":" \t ",)json"
            )
        );
        auto const blank = fixture.load();
        REQUIRE_FALSE(blank.has_value());
        CHECK(why(blank).contains("pattern"));
    }

    // The other direction of the same rule, and the one the gate got backwards
    // until 2026-08-14: a deployment whose plugin is an adapter the project kit
    // generated IS the declarative tier, and owes no justification at all. A
    // gate that demanded one from every deployment with a `plugin` member was
    // asking the declarative tier to invent a reason it does not have, which is
    // precisely what the member exists to prevent.
    //
    // The path is a generated one and the tier is stated in the document,
    // because only the document can carry it: the loader has no access to the
    // kit's declared inputs or its output tree, so a rule that read the path
    // would be one the kit could apply and this loader could not.
    TEST_CASE("a generated adapter owes no justification and is accepted")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        constexpr auto k_generated = std::string_view{
            "generated/adapters/fixture.alpha/dismiss-known-overlay.luau"
        };
        fixture.rewrite(k_generated, "return {plugin_id = \"fixture.alpha\"}\n");

        auto const stated = Fixture::projectManifest();
        auto const at     = stated.find(R"json("plugin_justification":")json");
        REQUIRE(at != std::string::npos);
        auto const end = stated.find(R"json(",)json", at);
        REQUIRE(end != std::string::npos);
        auto const member = stated.substr(at, end + 2U - at);

        auto const declarative = substituted(
            substituted(
                substituted(stated, member, ""),
                R"json("path":"plugin/alpha-tool.luau")json",
                std::string{R"json("path":")json"}
                    + std::string{k_generated} + R"json(")json"
            ),
            R"json("plugin_authoring":"hand-written")json",
            R"json("plugin_authoring":"generated")json"
        );
        fixture.rewrite("umbraflow-project.json", declarative);

        auto const accepted = fixture.load();
        INFO(why(accepted));
        CHECK(accepted.has_value());

        // And the inverse is a refusal rather than a member nobody reads. A
        // justification beside a generated adapter is the false reason this
        // rule exists to keep out, so the schema refuses the member instead of
        // ignoring it.
        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                declarative,
                R"json("plugin_authoring":"generated",)json",
                R"json("plugin_authoring":"generated",)json" + member
            )
        );
        auto const invented = fixture.load();
        REQUIRE_FALSE(invented.has_value());
        CHECK(why(invented).contains("plugin_justification"));
    }

    // R4. A named file must exist. Only assets are named by path now -- the
    // closure's modules and the deployment's resources -- so those are what a
    // missing file can be, and each must be an error rather than a skip.
    TEST_CASE("R4 a named file that is absent is refused rather than skipped")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        fixture.remove("blob/alpha.blob");
        auto const noResource = fixture.load();
        REQUIRE_FALSE(noResource.has_value());
        CHECK(why(noResource).contains("blob/alpha.blob"));

        fixture.rewrite("blob/alpha.blob", R"({"blob":"fixture-alpha"})");
        REQUIRE(fixture.load().has_value());

        fixture.remove("plugin/alpha-tool.luau");
        auto const noModule = fixture.load();
        REQUIRE_FALSE(noModule.has_value());
        CHECK(why(noModule).contains("plugin/alpha-tool.luau"));
    }

    // R6. Every schema must compile under the evaluator's closed keyword set. A
    // keyword the evaluator does not implement is a refusal, not a skip.
    TEST_CASE("R6 a schema the evaluator cannot apply is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        // unevaluatedProperties is Draft 2020-12 and is deliberately outside
        // the implemented set, so a schema carrying it would be silently
        // under-enforced by anything that skipped it. It goes into the inline
        // identity schema, which is one of the two documents a project still
        // supplies and this deployment compiles.
        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("additionalProperties": false,)json",
                R"json("unevaluatedProperties": false,)json"
            )
        );
        auto const refused = fixture.load();
        REQUIRE_FALSE(refused.has_value());
        CHECK(why(refused).contains("unevaluatedProperties"));
    }

    // R7. Both mutability and surface are required on every Tool a deployment
    // declares. An entry omitting one is refused rather than read as the
    // restricted default that ToolDescriptor carries in C++.
    TEST_CASE("R7 a Tool declared without mutability or surface is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("mutability":"mutating",)json",
                ""
            )
        );
        auto const noMutability = fixture.load();
        REQUIRE_FALSE(noMutability.has_value());
        CHECK(why(noMutability).contains("mutability"));

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("surface":"semantic",)json",
                ""
            )
        );
        auto const noSurface = fixture.load();
        REQUIRE_FALSE(noSurface.has_value());
        CHECK(why(noSurface).contains("surface"));
    }

    // R8, applied to the five tool names a vocabulary provisions. Each is a
    // claim about the deployment's Tool declarations, and each names a case
    // that runs green while proving nothing when the claim is false --
    // absent_tool above all, because a declaration that carries it turns the
    // refusal the member exists for into a pass with nothing red anywhere.
    //
    // Every substitution below names a Tool of this project's own declaration,
    // or no Tool at all, so nothing but the agreement itself can refuse it.
    TEST_CASE("R8 a vocabulary the deployment's Tool declarations contradict is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.loadForConformance().has_value());

        auto const refusing =
            [&fixture](std::string_view from, std::string_view to)
        {
            INFO("substituted ", from, " with ", to);
            fixture.rewrite(
                "umbraflow-conformance.json",
                substituted(Fixture::conformanceManifest(), from, to)
            );
            auto const refused = fixture.loadForConformance();
            REQUIRE_FALSE(refused.has_value());

            // Every substitution here is a claim about a vocabulary, and a
            // vocabulary is a thing the product does not read. A production
            // loader that started requiring a mutating tool would be red here.
            auto const production = fixture.load();
            INFO(why(production));
            CHECK(production.has_value());
            return why(refused);
        };

        // The alpha declaration's ReadOnly Tool is observe-1, so this directory
        // would otherwise submit a read-only tool wherever the suite needs a
        // call that changes something.
        auto const readOnly = refusing(
            R"json("mutating_tool":"fixture.alpha.command-1")json",
            R"json("mutating_tool":"fixture.alpha.observe-1")json"
        );
        CHECK(readOnly.contains("mutating_tool"));
        CHECK(readOnly.contains("read_only"));

        auto const uncarried = refusing(
            R"json("mutating_tool":"fixture.alpha.command-1")json",
            R"json("mutating_tool":"fixture.alpha.command-absent")json"
        );
        CHECK(uncarried.contains("does not declare"));
        CHECK(uncarried.contains("fixture.alpha.command-absent"));

        auto const carriedAsMutating = refusing(
            R"json("read_only_tool":"fixture.alpha.observe-1")json",
            R"json("read_only_tool":"fixture.alpha.command-2")json"
        );
        CHECK(carriedAsMutating.contains("read_only_tool"));
        CHECK(carriedAsMutating.contains("mutating"));

        auto const oneTool = refusing(
            R"json("other_mutating_tool":"fixture.alpha.command-2")json",
            R"json("other_mutating_tool":"fixture.alpha.command-1")json"
        );
        CHECK(oneTool.contains("other_mutating_tool"));
        CHECK(oneTool.contains("fixture.alpha.command-1"));

        // The alpha command-1 is Mutating and declared, so nothing but
        // absent_tool's own rule -- that the deployment must NOT declare it --
        // can refuse this one.
        auto const carriedAbsent = refusing(
            R"json("absent_tool":"fixture.alpha.command-absent")json",
            R"json("absent_tool":"fixture.alpha.command-1")json"
        );
        CHECK(
            carriedAbsent.contains("absent_tool names fixture.alpha.command-1")
        );
        CHECK(carriedAbsent.contains("falsifiable"));

        // The foreign role carries a vocabulary of its own, and three suite
        // cases reach it. Without this the cases above would equally describe a
        // loader that checked one role and skipped the other.
        auto const foreign = refusing(
            R"json("foreign":{"deployment":"beta","vocabulary":{"mutating_tool":"fixture.beta.command-1")json",
            R"json("foreign":{"deployment":"beta","vocabulary":{"mutating_tool":"fixture.beta.observe-1")json"
        );
        CHECK(foreign.contains("foreign's mutating_tool"));
    }

    // R9. probe_frame names a capture, and a file that does not decode is
    // refused where it was written rather than minutes into a suite. Only the
    // extent is out of reach, and the header says why.
    TEST_CASE("R9 a probe frame that is not a PNG is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.loadForConformance().has_value());

        // A document this directory already holds and already reads, so the
        // path resolves and the file exists: R3 and R4 are both satisfied and
        // only the decode is left.
        fixture.rewrite(
            "umbraflow-conformance.json",
            substituted(
                Fixture::conformanceManifest(),
                R"json("probe_frame":"runtime/probe-frame.png")json",
                R"json("probe_frame":"umbraflow-project.json")json"
            )
        );
        auto const notAnImage = fixture.loadForConformance();
        REQUIRE_FALSE(notAnImage.has_value());
        CHECK(why(notAnImage).contains("probe_frame"));
        CHECK(why(notAnImage).contains("umbraflow-project.json"));

        // A truncated capture is the case a signature check cannot reach: the
        // first eight bytes are a PNG's and the file is not one.
        fixture.rewrite(
            "runtime/probe-frame.png",
            probeFramePng().substr(0U, 16U)
        );
        auto const truncated = fixture.loadForConformance();
        REQUIRE_FALSE(truncated.has_value());
        CHECK(why(truncated).contains("probe_frame"));
    }

    // Q7. A directory whose two roles are played by one deployment is refused
    // rather than run as a reduced suite: a skipped case is a green result
    // promising more than it verified.
    //
    // Both halves are the conformance load's and neither is the production
    // load's, and the third load in each block is what says so: a directory
    // whose roles do not hold together is still a directory the product starts,
    // which is the whole reason the two entry points are separate.
    TEST_CASE("both conformance roles must be played by different deployments")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.loadForConformance().has_value());

        fixture.rewrite(
            "umbraflow-conformance.json",
            substituted(
                Fixture::conformanceManifest(),
                R"json("foreign":{"deployment":"beta")json",
                R"json("foreign":{"deployment":"alpha")json"
            )
        );
        auto const oneRegistration = fixture.loadForConformance();
        REQUIRE_FALSE(oneRegistration.has_value());
        CHECK(why(oneRegistration).contains("authority is per"));

        auto const startsAnyway = fixture.load();
        INFO(why(startsAnyway));
        CHECK(startsAnyway.has_value());

        fixture.rewrite(
            "umbraflow-conformance.json",
            substituted(
                Fixture::conformanceManifest(),
                R"json("foreign":{"deployment":"beta")json",
                R"json("foreign":{"deployment":"gamma")json"
            )
        );
        auto const undeclared = fixture.loadForConformance();
        REQUIRE_FALSE(undeclared.has_value());
        CHECK(why(undeclared).contains("gamma"));

        auto const startsWithoutGamma = fixture.load();
        INFO(why(startsWithoutGamma));
        CHECK(startsWithoutGamma.has_value());
    }

    TEST_CASE("the loader bounds executable closures before runtime admission")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        SUBCASE("one module cannot exceed the VM source ceiling")
        {
            fixture.rewrite(
                "plugin/alpha-tool.luau",
                std::string(
                    script::PureDataProgram::k_maximumModuleSourceBytes + 1U,
                    'x'
                )
            );
            auto const refused = fixture.load();
            REQUIRE_FALSE(refused.has_value());
            CHECK(why(refused).contains("ceiling"));
        }

        SUBCASE("one resource cannot exceed the VM resource ceiling")
        {
            fixture.rewrite(
                "blob/alpha.blob",
                std::string(
                    script::PureDataProgram::k_maximumResourceBytes + 1U,
                    'x'
                )
            );
            auto const refused = fixture.load();
            REQUIRE_FALSE(refused.has_value());
            CHECK(why(refused).contains("ceiling"));
        }

        SUBCASE("the module closure has a cumulative read ceiling")
        {
            auto modules = std::string{"["};
            for (auto index = std::size_t{0U}; index < 17U; ++index)
            {
                auto const suffix = std::to_string(index);
                modules += index == 0U ? "" : ",";
                modules += std::format(
                    R"json({{"name":"m{}","path":"plugin/limit-{}.luau"}})json",
                    suffix,
                    suffix
                );
                fixture.rewrite(
                    "plugin/limit-" + suffix + ".luau",
                    std::string(
                        script::PureDataProgram::k_maximumModuleSourceBytes,
                        'x'
                    )
                );
            }
            modules += "]";
            fixture.rewrite(
                "umbraflow-project.json",
                substituted(
                    Fixture::projectManifest(),
                    R"json("tool_closure":{"entry":"main","exported_entry_points":[],"modules":[{"name":"main","path":"plugin/alpha-tool.luau"}]})json",
                    "\"tool_closure\":{\"entry\":\"m0\","
                        "\"exported_entry_points\":[],"
                        "\"modules\":" + modules + "}"
                )
            );
            auto const refused = fixture.load();
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                why(refused).contains(
                    "module closure exceeds its byte ceiling"
                )
            );
        }

        SUBCASE("the resource closure has a cumulative read ceiling")
        {
            auto resources = std::string{"["};
            for (auto index = std::size_t{0U}; index < 5U; ++index)
            {
                auto const suffix = std::to_string(index);
                resources += index == 0U ? "" : ",";
                resources += std::format(
                    R"json({{"kind":"bytes","name":"r{}","path":"blob/limit-{}.blob"}})json",
                    suffix,
                    suffix
                );
                fixture.rewrite(
                    "blob/limit-" + suffix + ".blob",
                    std::string(
                        script::PureDataProgram::k_maximumResourceBytes,
                        'x'
                    )
                );
            }
            resources += "]";
            fixture.rewrite(
                "umbraflow-project.json",
                substituted(
                    Fixture::projectManifest(),
                    R"json("resources":[{"kind":"bytes","name":"page-model","path":"blob/alpha.blob"}])json",
                    "\"resources\":" + resources
                )
            );
            auto const refused = fixture.load();
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                why(refused).contains(
                    "resource closure exceeds its byte ceiling"
                )
            );
        }

        SUBCASE("the schema caps resource count before any resource path is read")
        {
            auto resources = std::string{"["};
            for (auto index = std::size_t{0U}; index < 65U; ++index)
            {
                auto const suffix = std::to_string(index);
                resources += index == 0U ? "" : ",";
                resources += std::format(
                    R"json({{"kind":"bytes","name":"r{}","path":"missing/{}"}})json",
                    suffix,
                    suffix
                );
            }
            resources += "]";
            fixture.rewrite(
                "umbraflow-project.json",
                substituted(
                    Fixture::projectManifest(),
                    R"json("resources":[{"kind":"bytes","name":"page-model","path":"blob/alpha.blob"}])json",
                    "\"resources\":" + resources
                )
            );
            auto const refused = fixture.load();
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                why(refused).contains(
                    "schema/umbraflow-project-v3.schema.json"
                )
            );
            CHECK_FALSE(why(refused).contains("missing/0"));
        }
    }

    // The one check on this chain that compares two values produced at two
    // different times, and the whole of what the Q3 ruling rests on.
    //
    // The byte flipped below is in the plugin, and that is deliberate: the
    // plugin's bytes reach project_registration_hash through the module manifest and
    // through nothing else, so no other rule in this file can refuse the
    // flipped directory. The third load proves exactly that -- it is the
    // negative control that stops a second mechanism from being mistaken for
    // this one.
    TEST_CASE("a project whose bytes moved under a stored session is refused")
    {
        auto const fixture = Fixture{};
        auto const first   = fixture.load();
        INFO(why(first));
        REQUIRE(first.has_value());

        auto const* const p_alpha = first->findDeployment("alpha");
        REQUIRE(p_alpha != nullptr);

        // What a stored session records. SessionManifest is the document a
        // session is pinned to, and project_registration_hash is the member
        // that names the project it was pinned against.
        auto const manifest = operator_runtime::SessionManifest::create(
            operator_runtime::SessionManifestSpec{
                .runtimeModelArtifactRootHash  = p_alpha->generation.hash(),
                .operatorProtocolSchemaHash    = p_alpha->generation.hash(),
                .projectRegistrationHash       = p_alpha->generation.hash(),
                .policyArtifactHash            = p_alpha->generation.hash(),
                .agentProfileHash              = p_alpha->generation.hash(),
            }
        );
        REQUIRE(manifest.has_value());
        auto const recorded = manifest->projectRegistrationHash();

        auto const stored = std::array{ExpectedRegistration{
            .deployment = "alpha",
            .hash       = recorded,
        }};

        // Resuming against the unchanged directory is the positive control:
        // without it the refusal below would be satisfied by a check that
        // refused every resume.
        auto const unchanged = fixture.load(stored);
        INFO(why(unchanged));
        REQUIRE(unchanged.has_value());

        auto const* const p_beta = first->findDeployment("beta");
        REQUIRE(p_beta != nullptr);
        auto const duplicate = std::array{
            ExpectedRegistration{.deployment = "alpha", .hash = recorded},
            ExpectedRegistration{.deployment = "alpha", .hash = recorded},
        };
        auto const duplicated = fixture.load(duplicate);
        REQUIRE_FALSE(duplicated.has_value());
        CHECK(why(duplicated).contains("more than once"));

        auto const conflicting = std::array{
            ExpectedRegistration{.deployment = "alpha", .hash = recorded},
            ExpectedRegistration{
                .deployment = "alpha",
                .hash       = p_beta->generation.hash(),
            },
        };
        auto const conflicted = fixture.load(conflicting);
        REQUIRE_FALSE(conflicted.has_value());
        CHECK(why(conflicted).contains("more than once"));

        fixture.rewrite(
            "plugin/alpha-tool.luau",
            "return {plugin_id = \"fixture.alpha\"} -- one byte more\n"
        );

        auto const resumed = fixture.load(stored);
        REQUIRE_FALSE(resumed.has_value());
        auto const refusal = why(resumed);
        CHECK(refusal.contains(recorded.hex()));

        auto const reloaded = fixture.load();
        REQUIRE(reloaded.has_value());
        auto const* const p_moved = reloaded->findDeployment("alpha");
        REQUIRE(p_moved != nullptr);
        CHECK(p_moved->generation.hash() != recorded);
        CHECK(refusal.contains(p_moved->generation.hash().hex()));

        // A commitment for a deployment this directory does not declare is a
        // refusal, not a value nobody read. Without it a misspelled name would
        // disarm the check above in silence.
        auto const misnamed = std::array{ExpectedRegistration{
            .deployment = "alpha-2",
            .hash       = recorded,
        }};
        auto const wrongName = fixture.load(misnamed);
        REQUIRE_FALSE(wrongName.has_value());
        CHECK(why(wrongName).contains("alpha-2"));
    }
}
