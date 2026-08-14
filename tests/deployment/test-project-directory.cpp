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

#include "json/repository-path.hpp"

#include <deployment/project-directory.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <image/png.hpp>

#include <operator/manifest.hpp>

#include <task/runtime-model-file.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
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
  "schema": "umbraflow-project/v1",
  "runtime_artifact": "runtime/artifact",
  "primary_deployment": "alpha",
  "deployments": [)json"}
                    + deploymentBlock("alpha") + "," + deploymentBlock("beta")
                    + "]}";
            }

            [[nodiscard]]
            static auto deploymentBlock(std::string_view name) -> std::string
            {
                auto block = std::string{R"json({"name":")json"};
                block += name;
                block += R"json(","plugin_id":"fixture.)json";
                block += name;
                block += R"json(","baseline_event_type":"fixture.baseline",)json";
                block += R"json("plugin":"plugin/)json";
                block += name;
                block += R"json(.luau",)json";
                block += R"json("plugin_justification":"A fixture plugin that )json"
                    R"json(answers from constants: umbraflow-declarative-)json"
                    R"json(workflow-tool/v1 has no member that decides what a )json"
                    R"json(Reduce returns.",)json";
                auto const document = [name](std::string_view leaf)
                {
                    return "\"schema/" + std::string{name} + "/" + std::string{leaf}
                        + "\"";
                };
                block += R"json("project_state_schema":)json" + document("state.json");
                block += R"json(,"project_observation_schema":)json"
                    + document("observation.json");
                block += R"json(,"tool_precondition_schema":)json"
                    + document("precondition.json");
                block +=
                    R"json(,"reconcile_schema":)json" + document("reconcile.json");
                block += R"json(,"tool_catalog":)json" + document("catalog.json");
                block += R"json(,"journal_event_schema_manifest":)json"
                    + document("journal-manifest.json");
                block += R"json(,"reconcile_manifest":)json"
                    + document("reconcile-manifest.json");
                block += R"json(,"journal_payload_schemas":[)json";
                for (auto index = std::size_t{0};
                     index < umbraflow::k_journalPayloadSchemas.size();
                     ++index)
                {
                    block += index == 0U ? "" : ",";
                    block += document("journal-" + std::to_string(index) + ".json");
                }
                block += R"json(],"effect_payload_schemas":[)json"
                    + document("effect-0.json") + R"json(],)json";
                block += R"json("artifact_blobs":[{"name":"page-model","path":"blob/)json";
                block += name;
                block += R"json(.blob"}]})json";
                return block;
            }

            [[nodiscard]] static auto conformanceManifest() -> std::string
            {
                return R"json({"schema":"umbraflow-conformance/v1",)json"
                    R"json("probe_frame":"runtime/probe-frame.png",)json"
                    R"json("under_test":{"deployment":"alpha","vocabulary":)json"
                    + vocabulary()
                    + R"json(},"foreign":{"deployment":"beta","vocabulary":)json"
                    + vocabulary() + "}}";
            }

            [[nodiscard]] static auto vocabulary() -> std::string
            {
                return R"json({"mutating_tool":"command-1",)json"
                       R"json("other_mutating_tool":"command-2",)json"
                       R"json("read_only_tool":"observe-1",)json"
                       R"json("tool_arguments":"{\"value\":1}",)json"
                       R"json("refused_tool_arguments":"{\"value\":0}",)json"
                       R"json("absent_tool":"command-absent",)json"
                       R"json("baseline_entry":{"event_type":"fixture.baseline",)json"
                       R"json("payload":"{\"marker\":\"baseline\"}"},)json"
                       R"json("progress_entry":{"event_type":"fixture.progress",)json"
                       R"json("payload":"{\"value\":1}"},)json"
                       R"json("confirmed_entry":{"event_type":"fixture.confirmed",)json"
                       R"json("payload":"{\"marker\":\"confirmed\"}"},)json"
                       R"json("superseded_entry":{"event_type":"fixture.duplicate",)json"
                       R"json("payload":"{\"marker\":\"duplicate\"}"},)json"
                       R"json("provenance":"{\"kind\":\"observation\"}",)json"
                       R"json("continue_input":"{\"disposition\":\"continue\"}",)json"
                       R"json("confirmed_input":"{\"disposition\":\"confirmed\"}",)json"
                       R"json("rejected_input":"{\"disposition\":\"rejected\"}",)json"
                       R"json("ambiguous_input":"{\"disposition\":\"ambiguous\"}",)json"
                       R"json("approval_required_plan_tool":"approval-plan",)json"
                       R"json("ui_action":{"surface":"fixture.surface",)json"
                       R"json("ui_target":"fixture.target","action":"fixture.press"}})json";
            }

        private:
            auto writeDeployment(std::string_view name) const -> void
            {
                auto const bundle = umbraflow::DeploymentBundle{
                    "fixture." + std::string{name},
                };
                auto const at = [this, name](std::string_view leaf)
                {
                    return m_root / "schema" / std::string{name} / std::string{leaf};
                };
                write(at("state.json"), umbraflow::k_projectStateSchema);
                write(at("observation.json"), umbraflow::k_projectObservationSchema);
                write(at("precondition.json"), umbraflow::k_toolPreconditionSchema);
                write(at("reconcile.json"), umbraflow::k_reconcileSchema);
                write(at("catalog.json"), bundle.toolCatalog());
                write(at("journal-manifest.json"), bundle.journalEventManifest());
                write(at("reconcile-manifest.json"), bundle.reconcileManifest());
                for (auto index = std::size_t{0};
                     index < umbraflow::k_journalPayloadSchemas.size();
                     ++index)
                {
                    write(
                        at("journal-" + std::to_string(index) + ".json"),
                        umbraflow::k_journalPayloadSchemas.at(index)
                    );
                }
                write(at("effect-0.json"), umbraflow::k_effectPayloadSchema);
                write(
                    m_root / "plugin" / (std::string{name} + ".luau"),
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

    // The specification states three documents by worked example, and this is
    // what keeps an example a document the framework accepts. Without it the
    // examples are prose beside a C++ string constant, which is the arrangement
    // that had one consumer writing CamelCase for `mutating` and `semantic`.
    TEST_CASE("the specification's worked documents are documents this accepts")
    {
        constexpr auto k_specification =
            std::string_view{"docs/archive/plans/2026-08-11-project-as-data.md"};
        auto const root = json::repositoryRoot(k_specification);
        REQUIRE_FALSE(root.empty());

        auto       stream = std::ifstream{root / k_specification, std::ios::binary};
        auto const text   = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{},
        };
        REQUIRE_FALSE(text.empty());

        for (auto const format : std::array{
                 std::string_view{"umbraflow-tool-catalog/v1"},
                 std::string_view{"umbraflow-journal-event-schema-manifest/v1"},
                 std::string_view{"umbraflow-reconcile-manifest/v1"},
             })
        {
            INFO(format);
            auto const anchored = text.find(
                std::string{"<!-- example: "} + std::string{format} + " -->"
            );
            REQUIRE(anchored != std::string::npos);
            auto const opened = text.find("```json\n", anchored);
            REQUIRE(opened != std::string::npos);
            auto const begin  = opened + std::string_view{"```json\n"}.size();
            auto const closed = text.find("\n```", begin);
            REQUIRE(closed != std::string::npos);

            auto const example = text.substr(begin, closed - begin);
            auto const judged  = validateFrameworkFormat(example);
            // Both arms are std::string: message() answers with a view into the
            // Error, and a conditional mixing it with a std::string temporary
            // takes the view as its type and outlives what backs it.
            auto const complaint = judged.has_value()
                ? std::string{}
                : std::string{judged.error().message()};
            INFO(complaint);
            CHECK(judged.has_value());
        }

        // Which schema judges a document is the document's own `schema` member,
        // so one naming a format this module does not own is refused rather
        // than judged by whichever schema came first. Without this the loop
        // above would equally describe a function that accepted anything.
        CHECK_FALSE(
            validateFrameworkFormat(R"json({"schema":"umbraflow-project/v1"})json")
                .has_value()
        );
        CHECK_FALSE(validateFrameworkFormat(R"json({"tools":[]})json").has_value());
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

    // Everything below breaks one thing in this directory, so this case is what
    // says the directory is otherwise whole. It also states what a load
    // produces, because no other case reads the result.
    TEST_CASE("a project directory becomes five authorities per deployment")
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
        CHECK(p_alpha->registration.pluginId() == "fixture.alpha");
        CHECK(p_alpha->registration.baselineEventType() == "fixture.baseline");
        CHECK(p_alpha->pluginBytes.starts_with("return {plugin_id ="));
        REQUIRE(p_alpha->artifactBlobs.size() == 1U);
        CHECK(p_alpha->artifactBlobs.front().name == "page-model");

        // The two deployments are two registrations. Without this the suite's
        // whole reason for a foreign role would be satisfied by one.
        auto const* const p_beta = loaded->findDeployment("beta");
        REQUIRE(p_beta != nullptr);
        CHECK(p_alpha->registration.hash() != p_beta->registration.hash());

        // The five authorities are bound to that registration and can be asked
        // to judge, which is the whole of what constructing them was for.
        CHECK(p_alpha->schemaOwner.projectRegistrationHash()
              == p_alpha->registration.hash());
        CHECK(p_alpha->schemaOwner.canonicalize("{\"revision\":0}").has_value());
        CHECK_FALSE(p_alpha->schemaOwner.canonicalize("{\"revision\": 0}").has_value());
        CHECK(p_alpha->toolCatalogSchemaOwner
                  .validate(
                      "command-1",
                      *p_alpha->schemaOwner.canonicalize("{\"value\":1}")
                  )
                  .has_value());
        CHECK_FALSE(p_alpha->toolCatalogSchemaOwner
                        .validate(
                            "command-absent",
                            *p_alpha->schemaOwner.canonicalize("{\"value\":1}")
                        )
                        .has_value());

        // Production loads retain only the authorities and pinned project data;
        // conformance-only input evidence has no member here to accumulate in.
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
        REQUIRE(loaded->documentInputLog != nullptr);
        CHECK(loaded->documentInputLog->lastReduceInput().empty());
        CHECK(loaded->documentInputLog->lastDeriveInput().empty());

        CHECK(loaded->underTest.deployment == "alpha");
        CHECK(loaded->foreign.deployment == "beta");
        CHECK_FALSE(loaded->probeFrame.empty());

        // The vocabulary is read as strings and nothing else, and the payload
        // members carry the project's exact bytes rather than a shape this
        // loader chose.
        CHECK(loaded->underTest.vocabulary.mutatingTool == "command-1");
        CHECK(loaded->underTest.vocabulary.absentTool == "command-absent");
        CHECK(loaded->underTest.vocabulary.baselineEntry.payload
              == "{\"marker\":\"baseline\"}");
        CHECK(loaded->underTest.vocabulary.uiAction.uiTarget == "fixture.target");
    }

    // The defect the split repairs, stated as the directory that could not be
    // expressed before it. A project at a read-only phase declares one
    // deployment, carries no conformance document at all, and every tool its
    // catalog holds is read_only -- so it has no mutating_tool to name, no
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

        // Every mutating row becomes read_only. The catalog stays a catalog
        // this deployment's registration pins, so nothing but the mutability
        // words differ from the accepted directory above.
        auto const bundle    = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto       readOnly  = bundle.toolCatalog();
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
        fixture.rewrite("schema/alpha/catalog.json", readOnly);

        // One deployment, and no second one to play any other role.
        fixture.rewrite(
            "umbraflow-project.json",
            std::string{R"json({"schema":"umbraflow-project/v1",)json"}
                + R"json("runtime_artifact":"runtime/artifact",)json"
                + R"json("primary_deployment":"alpha","deployments":[)json"
                + Fixture::deploymentBlock("alpha") + "]}"
        );
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
                R"json("plugin":"plugin/alpha.luau")json",
                R"json("plugin":"schema/../plugin/alpha.luau")json"
            )
        );
        auto const traversal = fixture.load();
        REQUIRE_FALSE(traversal.has_value());
        CHECK(why(traversal).contains("'..'"));

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("plugin":"plugin/alpha.luau")json",
                R"json("plugin":"plugin\\alpha.luau")json"
            )
        );
        auto const backslash = fixture.load();
        REQUIRE_FALSE(backslash.has_value());
        CHECK(why(backslash).contains("'/'"));
    }

    // The direct-plugin tier is the exception, so a deployment block that names
    // a hand-written plugin must also say which member or semantic of
    // umbraflow-declarative-workflow-tool/v1 cannot express it. Both halves are
    // asserted because they are enforced by two different clauses: `required`
    // catches the absent member and `"pattern": "\\S"` catches a member present
    // and blank, and neither stands in for the other.
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

    // R4. A named file must exist. This is the directory form of the exemplar
    // provider's REQUIRE(found != k_schemaFiles.end()), and it is only
    // equivalent if a missing file is an error rather than a skip.
    TEST_CASE("R4 a named file that is absent is refused rather than skipped")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        fixture.remove("schema/alpha/journal-2.json");
        auto const missing = fixture.load();
        REQUIRE_FALSE(missing.has_value());
        CHECK(why(missing).contains("journal_payload_schemas"));
        CHECK(why(missing).contains("schema/alpha/journal-2.json"));

        // A skip would have left the journal manifest naming a payload schema
        // nobody supplied, which the deployment refuses for another reason.
        // Naming the manifest member and the path is what tells the two apart.
        CHECK_FALSE(why(missing).contains("does not hold together"));
    }

    // R5. Every stated sha256 equals the digest of the bytes it names, and the
    // refusal prints the stated digest and what the deployment carries -- which
    // is the whole of R5's promise that fixing one is a copy. Each half is
    // asserted: a case asserting only the stated digest was satisfied by two of
    // the three sites while neither printed anything to copy.
    TEST_CASE("R5 a stated digest that is not the file's is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        auto const bundle   = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto const progress = umbraflow::schemaHashHex(
            umbraflow::k_progressPayloadSchema
        );
        fixture.rewrite(
            "schema/alpha/journal-manifest.json",
            substituted(bundle.journalEventManifest(), progress, std::string(64U, 'a'))
        );
        auto const journal = fixture.load();
        REQUIRE_FALSE(journal.has_value());
        CHECK(why(journal).contains(std::string(64U, 'a')));
        CHECK(why(journal).contains(progress));

        fixture.rewrite(
            "schema/alpha/journal-manifest.json",
            bundle.journalEventManifest()
        );
        REQUIRE(fixture.load().has_value());

        fixture.rewrite(
            "schema/alpha/reconcile-manifest.json",
            substituted(
                bundle.reconcileManifest(),
                umbraflow::schemaHashHex(umbraflow::k_reconcileSchema),
                std::string(64U, 'b')
            )
        );
        auto const reconcile = fixture.load();
        REQUIRE_FALSE(reconcile.has_value());
        CHECK(why(reconcile).contains(std::string(64U, 'b')));
        CHECK(why(reconcile).contains(
            umbraflow::schemaHashHex(umbraflow::k_reconcileSchema)
        ));
    }

    // R5, third site. The Tool Catalog's tool_precondition_sha256 is the only
    // route by which the precondition schema's bytes reach tool_catalog_hash,
    // and until this case existed the rule had two of its three sites covered.
    TEST_CASE("R5 a catalog naming another tool precondition schema is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};
        fixture.rewrite(
            "schema/alpha/catalog.json",
            substituted(
                bundle.toolCatalog(),
                umbraflow::schemaHashHex(umbraflow::k_toolPreconditionSchema),
                std::string(64U, 'c')
            )
        );
        auto const refused = fixture.load();
        REQUIRE_FALSE(refused.has_value());
        CHECK(why(refused).contains(std::string(64U, 'c')));
        CHECK(why(refused).contains(
            umbraflow::schemaHashHex(umbraflow::k_toolPreconditionSchema)
        ));
    }

    // R6. Every schema must compile under the evaluator's closed keyword set. A
    // keyword the evaluator does not implement is a refusal, not a skip.
    TEST_CASE("R6 a schema the evaluator cannot apply is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        // unevaluatedProperties is Draft 2020-12 and is deliberately outside
        // the implemented set, so a schema carrying it would be silently
        // under-enforced by anything that skipped it.
        fixture.rewrite(
            "schema/alpha/state.json",
            substituted(
                umbraflow::k_projectStateSchema,
                R"json("additionalProperties": false,)json",
                R"json("unevaluatedProperties": false,)json"
            )
        );
        auto const refused = fixture.load();
        REQUIRE_FALSE(refused.has_value());
        CHECK(why(refused).contains("unevaluatedProperties"));
    }

    // R7. Both mutability and surface are required on every catalog row. A row
    // omitting one is refused rather than read as the restricted default that
    // ToolDescriptor carries in C++.
    TEST_CASE("R7 a catalog row without mutability or surface is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};
        fixture.rewrite(
            "schema/alpha/catalog.json",
            substituted(bundle.toolCatalog(), R"json("mutability":"mutating",)json", "")
        );
        auto const noMutability = fixture.load();
        REQUIRE_FALSE(noMutability.has_value());
        CHECK(why(noMutability).contains("mutability"));

        fixture.rewrite(
            "schema/alpha/catalog.json",
            substituted(bundle.toolCatalog(), R"json("surface":"semantic",)json", "")
        );
        auto const noSurface = fixture.load();
        REQUIRE_FALSE(noSurface.has_value());
        CHECK(why(noSurface).contains("surface"));
    }

    // R8, the half a loader can answer. Both sides of the agreement are
    // authored -- the block's baseline_event_type and the vocabulary's
    // baseline_entry -- so it is refused where it was written.
    TEST_CASE("R8 a vocabulary provisioning another baseline event type is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.loadForConformance().has_value());

        // fixture.progress is a real event type of this project, with a payload
        // schema of its own, so nothing but the agreement itself can refuse it.
        fixture.rewrite(
            "umbraflow-conformance.json",
            substituted(
                Fixture::conformanceManifest(),
                R"json("baseline_entry":{"event_type":"fixture.baseline")json",
                R"json("baseline_entry":{"event_type":"fixture.progress")json"
            )
        );
        auto const disagreeing = fixture.loadForConformance();
        REQUIRE_FALSE(disagreeing.has_value());
        CHECK(why(disagreeing).contains("baseline_event_type"));
        CHECK(why(disagreeing).contains("fixture.progress"));

        // The rule belongs to the conformance layer and to nothing below it:
        // the same directory still starts. Without this the case would equally
        // describe a production loader that had read the vocabulary too.
        auto const production = fixture.load();
        INFO(why(production));
        CHECK(production.has_value());
    }

    // R8, applied to the five tool names a vocabulary provisions. Each is a
    // claim about the deployment's Tool Catalog, and each names a case that
    // runs green while proving nothing when the claim is false -- absent_tool
    // above all, because a catalog that carries it turns the refusal the field
    // exists for into a pass with nothing red anywhere.
    //
    // Every substitution below names a tool of this project's own catalog, or
    // no tool at all, so nothing but the agreement itself can refuse it.
    TEST_CASE("R8 a vocabulary the deployment's Tool Catalog contradicts is refused")
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

        // observe-1 is the catalog's ReadOnly row, so this directory would
        // otherwise submit a read-only tool wherever the suite needs an
        // Operation that changes something.
        auto const readOnly = refusing(
            R"json("mutating_tool":"command-1")json",
            R"json("mutating_tool":"observe-1")json"
        );
        CHECK(readOnly.contains("mutating_tool"));
        CHECK(readOnly.contains("read_only"));

        auto const uncarried = refusing(
            R"json("mutating_tool":"command-1")json",
            R"json("mutating_tool":"command-absent")json"
        );
        CHECK(uncarried.contains("does not carry"));
        CHECK(uncarried.contains("command-absent"));

        auto const carriedAsMutating = refusing(
            R"json("read_only_tool":"observe-1")json",
            R"json("read_only_tool":"command-2")json"
        );
        CHECK(carriedAsMutating.contains("read_only_tool"));
        CHECK(carriedAsMutating.contains("mutating"));

        auto const oneTool = refusing(
            R"json("other_mutating_tool":"command-2")json",
            R"json("other_mutating_tool":"command-1")json"
        );
        CHECK(oneTool.contains("other_mutating_tool"));
        CHECK(oneTool.contains("command-1"));

        // command-1 is Mutating and carried, so nothing but absent_tool's own
        // rule -- that the catalog must NOT carry it -- can refuse this one.
        auto const carriedAbsent = refusing(
            R"json("absent_tool":"command-absent")json",
            R"json("absent_tool":"command-1")json"
        );
        CHECK(carriedAbsent.contains("absent_tool names command-1"));
        CHECK(carriedAbsent.contains("falsifiable"));

        // The foreign role carries a vocabulary of its own, and three suite
        // cases reach it. Without this the cases above would equally describe a
        // loader that checked one role and skipped the other.
        auto const foreign = refusing(
            R"json("foreign":{"deployment":"beta","vocabulary":{"mutating_tool":"command-1")json",
            R"json("foreign":{"deployment":"beta","vocabulary":{"mutating_tool":"observe-1")json"
        );
        CHECK(foreign.contains("foreign's mutating_tool"));
    }

    // R8, applied to the four journal payloads. The reducer-input case proves
    // that an entry a commit did not name never reaches the reducer; two
    // entries carrying one payload cannot show which of them arrived, so the
    // agreement is refused where the vocabulary was written.
    TEST_CASE("R8 two journal entries provisioning one payload are refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.loadForConformance().has_value());

        // The event types stay distinct and each keeps a payload schema of its
        // own, so only the equality of the two payloads is left to refuse this.
        fixture.rewrite(
            "umbraflow-conformance.json",
            substituted(
                Fixture::conformanceManifest(),
                R"json("progress_entry":{"event_type":"fixture.progress","payload":"{\"value\":1}"})json",
                R"json("progress_entry":{"event_type":"fixture.progress","payload":"{\"marker\":\"confirmed\"}"})json"
            )
        );
        auto const equal = fixture.loadForConformance();
        REQUIRE_FALSE(equal.has_value());
        CHECK(why(equal).contains("progress_entry"));
        CHECK(why(equal).contains("confirmed_entry"));
    }

    // R8, applied to the two halves of the journal event schema manifest. The
    // block supplies the files and the manifest names their digests, so a
    // supplied schema no entry names is refused rather than compiled and never
    // consulted -- its bytes reach no digest in the design at all.
    TEST_CASE("R8 a journal payload schema no manifest entry names is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        // The surplus file is beta's own effect payload schema: a complete
        // schema this evaluator compiles, carried by the same project, so
        // nothing but the manifest's silence about it can refuse this.
        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("journal_payload_schemas":["schema/alpha/journal-0.json")json",
                R"json("journal_payload_schemas":["schema/beta/effect-0.json","schema/alpha/journal-0.json")json"
            )
        );
        auto const surplus = fixture.load();
        REQUIRE_FALSE(surplus.has_value());
        CHECK(why(surplus).contains(
            umbraflow::schemaHashHex(umbraflow::k_effectPayloadSchema)
        ));
        CHECK(why(surplus).contains("names under no event type"));
    }

    // R5. The effect payload schemas reach a digest through exactly one member,
    // the catalog's effect_payload_sha256s, and both halves are authored in one
    // directory -- so both directions are refused where they were written.
    // Without this member their bytes are inside no hash at all and editing one
    // is answered by a Plan refusal much later
    // (docs/archive/plans/2026-08-11-project-as-data.md 2.2).
    TEST_CASE("R5 the catalog and the effect payload schemas must name each other")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        // A journal payload schema stands in for the effect one: a complete
        // schema this evaluator compiles, already carried by this deployment,
        // so nothing but the catalog's digest can tell the two apart.
        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("effect_payload_schemas":["schema/alpha/effect-0.json"])json",
                R"json("effect_payload_schemas":["schema/alpha/journal-0.json"])json"
            )
        );
        auto const unnamedDigest = fixture.load();
        REQUIRE_FALSE(unnamedDigest.has_value());
        CHECK(why(unnamedDigest).contains(
            umbraflow::schemaHashHex(umbraflow::k_effectPayloadSchema)
        ));
        CHECK(why(unnamedDigest).contains("its effect_payload_schemas hash to"));

        fixture.rewrite(
            "umbraflow-project.json",
            substituted(
                Fixture::projectManifest(),
                R"json("effect_payload_schemas":["schema/alpha/effect-0.json"])json",
                R"json("effect_payload_schemas":["schema/alpha/effect-0.json","schema/alpha/journal-0.json"])json"
            )
        );
        auto const surplusSchema = fixture.load();
        REQUIRE_FALSE(surplusSchema.has_value());
        CHECK(why(surplusSchema).contains(
            umbraflow::schemaHashHex(umbraflow::k_journalPayloadSchemas.front())
        ));
        CHECK(why(surplusSchema).contains("effect_payload_sha256s does not name"));

        // The member is required rather than optional, which is what stops a
        // catalog from omitting it and putting its effect payload schemas back
        // outside every hash while both directions above stay satisfied.
        auto const bundle   = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto const declared = std::string{R"json("effect_payload_sha256s":[")json"}
            + umbraflow::schemaHashHex(umbraflow::k_effectPayloadSchema)
            + R"json("],)json";
        fixture.rewrite("umbraflow-project.json", Fixture::projectManifest());
        fixture.rewrite(
            "schema/alpha/catalog.json",
            substituted(bundle.toolCatalog(), declared, "")
        );
        auto const omitted = fixture.load();
        REQUIRE_FALSE(omitted.has_value());
        CHECK(why(omitted).contains("required has no member"));
        CHECK(why(omitted).contains("effect_payload_sha256s"));
    }

    // R9. probe_frame names a capture, and a file that does not decode is
    // refused where it was written rather than minutes into a suite. Only the
    // extent is out of reach, and R8 says why.
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
                R"json("probe_frame":"schema/alpha/state.json")json"
            )
        );
        auto const notAnImage = fixture.loadForConformance();
        REQUIRE_FALSE(notAnImage.has_value());
        CHECK(why(notAnImage).contains("probe_frame"));
        CHECK(why(notAnImage).contains("schema/alpha/state.json"));

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

    // The one check on this chain that compares two values produced at two
    // different times, and the whole of what the Q3 ruling rests on.
    //
    // The byte flipped below is in the plugin, and that is deliberate: the
    // plugin's bytes reach project_registration_hash through plugin_hash and
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
                .runtimeModelArtifactRootHash  = p_alpha->registration.hash(),
                .operatorProtocolSchemaHash    = p_alpha->registration.hash(),
                .projectRegistrationHash       = p_alpha->registration.hash(),
                .policyArtifactHash            = p_alpha->registration.hash(),
                .journalEnvelopeSchemaHash     = p_alpha->registration.hash(),
                .agentProfileHash              = p_alpha->registration.hash(),
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
                .hash       = p_beta->registration.hash(),
            },
        };
        auto const conflicted = fixture.load(conflicting);
        REQUIRE_FALSE(conflicted.has_value());
        CHECK(why(conflicted).contains("more than once"));

        fixture.rewrite(
            "plugin/alpha.luau",
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
        CHECK(p_moved->registration.hash() != recorded);
        CHECK(refusal.contains(p_moved->registration.hash().hex()));

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
