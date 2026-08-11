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
// docs/plans/2026-08-11-project-as-data.md 2.7 R8.

#include "umbraflow/project-schemas.hpp"

#include "json/repository-path.hpp"

#include <deployment/project-directory.hpp>

#include <operator/manifest.hpp>

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

        auto write(std::filesystem::path const& path, std::string_view bytes) -> void
        {
            std::filesystem::create_directories(path.parent_path());
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(stream.good());
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
                write(m_root / "runtime/artifact/page-model.toml", "[[page]]\n");
                write(m_root / "runtime/probe-frame.png", "\x89PNG\r\n\x1a\n");
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

            [[nodiscard]]
            auto load(std::span<ExpectedRegistration const> expected = {}) const
                -> Result<LoadedProject>
            {
                return loadProject(m_root, expected);
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
                write(
                    m_root / "blob" / (std::string{name} + ".blob"),
                    "fixture-" + std::string{name} + "-blob"
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

        [[nodiscard]]
        auto why(Result<LoadedProject> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<the directory was accepted>"}
                : std::string{outcome.error().message()};
        }
    }

    // The published document and the one the loader applies are the same bytes.
    // manifest_schema_hash is the sha256 of them, so a reformatting of the file
    // is a different registration for every project in existence, and this is
    // the case that says so before anyone discovers it as a moved hash.
    TEST_CASE("the registration schema the loader carries is the published file")
    {
        constexpr auto k_published =
            std::string_view{"schema/umbraflow-project-registration-v1.schema.json"};
        auto const root = json::repositoryRoot(k_published);
        REQUIRE_FALSE(root.empty());

        auto       stream = std::ifstream{root / k_published, std::ios::binary};
        auto const onDisk = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{},
        };
        REQUIRE_FALSE(onDisk.empty());
        CHECK(onDisk == projectRegistrationSchemaBytes());
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
        CHECK(loaded->underTest.deployment == "alpha");
        CHECK(loaded->foreign.deployment == "beta");
        CHECK(loaded->runtimeArtifactRoot == fixture.path() / "runtime/artifact");
        CHECK_FALSE(loaded->probeFrame.empty());

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

        // The vocabulary is read as strings and nothing else, and the payload
        // members carry the project's exact bytes rather than a shape this
        // loader chose.
        CHECK(loaded->underTest.vocabulary.mutatingTool == "command-1");
        CHECK(loaded->underTest.vocabulary.absentTool == "command-absent");
        CHECK(loaded->underTest.vocabulary.baselineEntry.payload
              == "{\"marker\":\"baseline\"}");
        CHECK(loaded->underTest.vocabulary.uiAction.uiTarget == "fixture.target");

        // The recorders exist and are empty until a validator writes them.
        REQUIRE(loaded->lastReduceInput != nullptr);
        CHECK(loaded->lastReduceInput->empty());
        REQUIRE(loaded->lastDeriveInput != nullptr);
        CHECK(loaded->lastDeriveInput->empty());
    }

    // R1. Both root documents are required at their fixed names, and the
    // refusal names the directory and both names.
    //
    // The message is asserted rather than only the refusal, and that is the
    // whole of what makes this case a check. Reading an absent document as
    // empty bytes also fails the load -- on "is not JSON", naming the document
    // it was looking for -- so a case that asked only whether the load failed
    // is satisfied by the "absent means empty" reading this rule exists to
    // forbid. Measured: that mutation left an earlier version of this case
    // green across all 39 of its assertions.
    TEST_CASE("R1 a project directory without both root documents is refused")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());
        auto const names = [&fixture](std::string const& refusal)
        {
            CHECK(refusal.contains(fixture.path().string()));
            CHECK(refusal.contains(k_projectManifestFileName));
            CHECK(refusal.contains(k_conformanceManifestFileName));
        };

        fixture.remove("umbraflow-conformance.json");
        auto const missingConformance = fixture.load();
        REQUIRE_FALSE(missingConformance.has_value());
        names(why(missingConformance));

        fixture.rewrite("umbraflow-conformance.json", Fixture::conformanceManifest());
        REQUIRE(fixture.load().has_value());
        fixture.remove("umbraflow-project.json");
        auto const missingProject = fixture.load();
        REQUIRE_FALSE(missingProject.has_value());
        names(why(missingProject));
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

    // R5. Every stated sha256 equals the digest of the bytes it names. After
    // the Q3 ruling this is two documents rather than three, and it is the only
    // rule of the eight a mistyped digest can make go red.
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
        CHECK(why(reconcile).contains("reconcile"));
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
        REQUIRE(fixture.load().has_value());

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
        auto const disagreeing = fixture.load();
        REQUIRE_FALSE(disagreeing.has_value());
        CHECK(why(disagreeing).contains("baseline_event_type"));
        CHECK(why(disagreeing).contains("fixture.progress"));
    }

    // Q7. A directory whose two roles are played by one deployment is refused
    // rather than run as a reduced suite: a skipped case is a green result
    // promising more than it verified.
    TEST_CASE("both conformance roles must be played by different deployments")
    {
        auto const fixture = Fixture{};
        REQUIRE(fixture.load().has_value());

        fixture.rewrite(
            "umbraflow-conformance.json",
            substituted(
                Fixture::conformanceManifest(),
                R"json("foreign":{"deployment":"beta")json",
                R"json("foreign":{"deployment":"alpha")json"
            )
        );
        auto const oneRegistration = fixture.load();
        REQUIRE_FALSE(oneRegistration.has_value());
        CHECK(why(oneRegistration).contains("authority is per"));

        fixture.rewrite(
            "umbraflow-conformance.json",
            substituted(
                Fixture::conformanceManifest(),
                R"json("foreign":{"deployment":"beta")json",
                R"json("foreign":{"deployment":"gamma")json"
            )
        );
        auto const undeclared = fixture.load();
        REQUIRE_FALSE(undeclared.has_value());
        CHECK(why(undeclared).contains("gamma"));
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
                .hostProtocolSchemaHash        = p_alpha->registration.hash(),
                .runtimeModelSchemaHash        = p_alpha->registration.hash(),
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
