#include <operator/ledger.hpp>
#include <operator/manifest.hpp>

#include "project-fixture.hpp"
#include "schema-binding.hpp"

#include <script/pure-data-program.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto repositoryRoot() -> std::filesystem::path
        {
            auto source = std::filesystem::path{__FILE__};
            if (source.is_relative())
            {
                source = std::filesystem::absolute(source);
            }
            auto candidate = source.parent_path().parent_path().parent_path();
            if (std::filesystem::is_directory(candidate / "schema"))
            {
                return candidate;
            }

            candidate = std::filesystem::current_path();
            while (!candidate.empty())
            {
                if (std::filesystem::is_directory(candidate / "schema"))
                {
                    return candidate;
                }
                auto const parent = candidate.parent_path();
                if (parent == candidate)
                {
                    break;
                }
                candidate = parent;
            }

            FAIL("repository root containing schema/ was not found");
            return {};
        }

        [[nodiscard]]
        auto readSchema(std::string_view filename) -> std::string
        {
            auto stream = std::ifstream{
                repositoryRoot() / "schema" / filename,
                std::ios::binary,
            };
            REQUIRE(stream.good());
            return {
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
        }

        [[nodiscard]]
        auto definition(
            std::string const& schema,
            std::string_view name
        ) -> std::string
        {
            auto const declaration  = std::string{"\""} + std::string{name} + "\"";
            auto const namePosition = schema.find(declaration);
            REQUIRE(namePosition != std::string::npos);
            auto const begin = schema.find('{', namePosition + declaration.size());
            REQUIRE(begin != std::string::npos);

            auto depth    = std::size_t{};
            auto inString = false;
            auto escaped  = false;
            for (auto index = begin; index < schema.size(); ++index)
            {
                auto const character = schema[index];
                if (inString)
                {
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (character == '\\')
                    {
                        escaped = true;
                    }
                    else if (character == '"')
                    {
                        inString = false;
                    }
                    continue;
                }
                if (character == '"')
                {
                    inString = true;
                }
                else if (character == '{')
                {
                    ++depth;
                }
                else if (character == '}')
                {
                    REQUIRE(depth > 0);
                    --depth;
                    if (depth == 0)
                    {
                        return schema.substr(begin, index - begin + 1);
                    }
                }
            }

            FAIL("schema definition has no closing object delimiter");
            return {};
        }

        auto checkStrictObject(std::string const& value) -> void
        {
            CHECK(value.find("\"type\": \"object\"") != std::string::npos);
            CHECK(value.find("\"additionalProperties\": false") != std::string::npos);
            CHECK(value.find("\"required\": [") != std::string::npos);
            CHECK(value.find("\"properties\": {") != std::string::npos);
        }

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const hash = sha256(std::as_bytes(std::span{value}));
            REQUIRE(hash.has_value());
            return *hash;
        }

        using test_support::command;
        using test_support::journalEntry;
        using test_support::makeProject;
        using test_support::prepareStore;
        using test_support::reconciliationOutcome;
        using test_support::reconcilingOperation;
        using test_support::TemporaryDirectory;
        using test_support::toolInvocation;

        [[nodiscard]]
        auto manifestSpec() -> SessionManifestSpec
        {
            return SessionManifestSpec{
                .hostProtocolSchemaHash       = hashOf("host"),
                .runtimeModelSchemaHash       = hashOf("runtime-schema"),
                .runtimeModelArtifactRootHash = hashOf("runtime-root"),
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = hashOf("registration"),
                .policyArtifactHash           = hashOf("policy"),
                .journalEnvelopeSchemaHash    = hashOf("journal"),
                .agentProfileHash             = hashOf("agent"),
            };
        }
    }

    TEST_CASE("schema-state-s01")
    {
        auto const operatorSchema = readSchema("umbraflow-operator-v1.schema.json");
        auto const journalSchema  = readSchema("umbraflow-journal-v1.schema.json");
        auto const parts          = definition(operatorSchema, "SnapshotParts");
        auto const state          = definition(journalSchema, "ProjectState");
        checkStrictObject(parts);
        checkStrictObject(state);
        CHECK(parts.find("\"observation_id\"") != std::string::npos);
        CHECK(parts.find("\"project_state_revision\"") != std::string::npos);
        CHECK(parts.find("\"lease_id\"") != std::string::npos);
        CHECK(state.find("\"last_journal_sequence\"") != std::string::npos);
        CHECK(state.find("\"canonical_opaque_payload\"") != std::string::npos);
    }

    TEST_CASE("schema-state-s02")
    {
        auto const schema   = readSchema("umbraflow-operator-v1.schema.json");
        auto const snapshot = definition(schema, "ProjectSnapshot");
        checkStrictObject(snapshot);
        CHECK(snapshot.find("\"identity\"") != std::string::npos);
        CHECK(snapshot.find("\"token\"") != std::string::npos);
        CHECK(snapshot.find("\"project_observation\"") != std::string::npos);
        CHECK(snapshot.find("\"project_state\"") != std::string::npos);
        CHECK(snapshot.find("\"event_cursor\"") != std::string::npos);
        CHECK(snapshot.find("frame") == std::string::npos);
    }

    // s01: the five state kinds have separate owners. What was absent is
    // ProjectObservation -- a kind whose only writer is ProjectPlugin.derive,
    // invoked only by the Snapshot Coordinator, on an envelope the Operator
    // assembled rather than accepted.
    TEST_CASE("contract-state-s01")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        REQUIRE(prepared.project.documentInputLog != nullptr);

        // The UI observation and the ProjectState reach the plugin as two
        // separate members. Project state is not writable back as UI evidence
        // because neither member is a caller's to supply.
        auto const reading     = test_support::observeAgain(prepared);
        auto const deriveInput = prepared.project.documentInputLog->lastDeriveInput();
        CHECK(deriveInput.find("\"ui_snapshot\":" + reading.canonicalJcs()) != std::string::npos);
        CHECK(deriveInput.find("\"project_state\":{\"revision\":0}") != std::string::npos);
        CHECK(deriveInput.find("\"prior_project_observation\":null") != std::string::npos);
        CHECK(deriveInput.find(reading.observationId()) == std::string::npos);

        // Its own revision line: an identical reading of an identical world
        // stays on one revision, so a recapture does not invent a state change.
        auto const first = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(first.has_value());
        CHECK(first->observation.revision() == prepared.snapshot.observation.revision());
        CHECK(first->observation.hash() == prepared.snapshot.observation.hash());
        CHECK(first->observation.projectStateRevision() == 0U);
        CHECK(first->observation.projectStateHash() == first->projectStateHash);

        // A committed reconciliation moves ProjectState, which is one of the
        // derive fingerprint's inputs, so the next reading is a new revision.
        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            task::DeliveryOutcome::Delivered
        );
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    operation.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.progress",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());
        auto const moved = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(moved.has_value());
        CHECK(moved->observation.revision() == first->observation.revision() + 1U);
        CHECK(moved->observation.projectStateRevision() == 1U);

        // The reading is bound to the registration that produced it. A handle
        // for another registration is valid on its own and still refused here.
        auto const foreignSource  = test_support::pluginSource("fixture.foreign");
        auto const foreignProject = makeProject("fixture.foreign", foreignSource);
        auto const foreignPlugin  = test_support::loadPlugin(foreignProject, foreignSource);
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            foreignPlugin,
            test_support::observeAgain(prepared)
        ).has_value());

        // And the observation cannot come through another RuntimeArtifact: a
        // second store installs a different model, and its Host resolves a
        // world this session's manifest attests nothing about.
        auto foreignTemporary = TemporaryDirectory{};
        auto foreignPrepared  = prepareStore(
            foreignTemporary.path(),
            "fixture.other"
        );
        auto foreignReading = test_support::observeAgain(foreignPrepared);
        CHECK(foreignReading.artifactRootHash() == reading.artifactRootHash());

        auto const otherRelease = conformance::observationRelease(
            foreignTemporary.path() / "other-handoff",
            conformance::ProjectRuntimeArtifact{
                .model  = test_support::ambiguousRuntimeModel(),
                .assets = test_support::umbraflowRuntimeAssets(),
            }
        );
        CHECK(otherRelease.artifactRootHash != reading.artifactRootHash());
        auto otherInstalled = foreignPrepared.store.installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = otherRelease.handoffRoot,
                .expectedReleaseManifestHash = otherRelease.releaseManifestHash,
                .expectedInstalledGeneration = 1U,
            }
        );
        REQUIRE(otherInstalled.has_value());
        auto otherHost = conformance::activateObservationHost(
            *std::move(otherInstalled),
            test_support::umbraflowProbeFrame(),
            FrameId{909}
        );
        auto const otherReading = conformance::observeOnce(otherHost);
        CHECK(otherReading.artifactRootHash() != reading.artifactRootHash());
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            otherReading
        ).has_value());
    }

    // DecisionBasis has four members and exactly one of them stands for the
    // world the plugin was shown. That member is a digest over the WHOLE
    // ui_snapshot document rather than over a list of its parts, which is what
    // lets the resolver put a new member -- an observed reading -- inside that
    // document without a second hash having to be remembered.
    //
    // The property is that the two are the same bytes. Nothing enforces it
    // today except that ledger.cpp reads canonicalJcs() for one and
    // stateResolutionHash() for the other, and a later member added to the
    // envelope beside ui_snapshot, or a ui_snapshot assembled from more than the
    // observation, would be a plugin input that no replay is checked against and
    // would raise no error anywhere.
    TEST_CASE("the derive input's world is the exact document the decision basis digests")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        REQUIRE(prepared.project.documentInputLog != nullptr);

        auto const envelope = prepared.project.documentInputLog->lastDeriveInput();
        auto const at       = envelope.find("\"ui_snapshot\":");
        REQUIRE(at != std::string::npos);

        // ui_snapshot sorts last of the five members, so it runs to the closing
        // brace of the envelope. Taking it by position rather than by parsing is
        // deliberate: a parse would agree with a re-serialization, and what has
        // to hold is that these BYTES are the hashed ones.
        constexpr auto member = std::string_view{"\"ui_snapshot\":"};
        auto const opened     = at + member.size();
        REQUIRE(envelope.size() > opened);
        REQUIRE(envelope.back() == '}');
        auto const uiSnapshot = envelope.substr(opened, envelope.size() - opened - 1U);
        REQUIRE_FALSE(uiSnapshot.empty());
        CHECK(uiSnapshot.front() == '{');

        CHECK(hashOf(uiSnapshot) == prepared.snapshot.stateResolutionHash);
        CHECK(
            prepared.snapshot.canonicalParts.find(
                "\"state_resolution_hash\":\"" + hashOf(uiSnapshot).hex() + "\""
            )
            != std::string::npos
        );

        // And the same document reached the ProjectObservation the basis's other
        // world member covers, so the two agree about one reading rather than
        // about two.
        CHECK(
            prepared.snapshot.observation.stateResolutionHash() == hashOf(uiSnapshot)
        );
    }

    // s02: one complete record, published atomically, whose identity the
    // publishing transaction derived from what it read.
    TEST_CASE("contract-state-s02")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // Recomputable: the identity is sha256 over the exact parts the row
        // stores, so every member is falsifiable rather than merely compared
        // against itself.
        auto const& parts = prepared.snapshot.canonicalParts;
        CHECK(hashOf(parts) == prepared.snapshot.identityHash);
        CHECK(parts.find("\"decision_basis_hash\":\"" + prepared.snapshot.decisionBasisHash.hex() + "\"") != std::string::npos);
        CHECK(parts.find("\"state_resolution_hash\":\"" + prepared.snapshot.stateResolutionHash.hex() + "\"") != std::string::npos);
        CHECK(parts.find("\"project_state_hash\":\"" + prepared.snapshot.projectStateHash.hex() + "\"") != std::string::npos);
        CHECK(parts.find("\"project_observation_hash\":\"" + prepared.snapshot.observation.hash().hex() + "\"") != std::string::npos);
        CHECK(parts.find("\"session_manifest_hash\":\"" + prepared.manifest.hash().hex() + "\"") != std::string::npos);
        CHECK(parts.find("\"lease_id\":\"" + prepared.lease.leaseId + "\"") != std::string::npos);
        CHECK(parts.find("\"fencing_token\":" + std::to_string(prepared.lease.fencingToken)) != std::string::npos);
        CHECK(parts.find("\"session_epoch\":" + std::to_string(prepared.lease.sessionEpoch)) != std::string::npos);
        CHECK(parts.find("\"controlled_target_id\":\"target-1\"") != std::string::npos);
        CHECK(parts.find("\"project_instance_key\":\"instance-1\"") != std::string::npos);
        CHECK(parts.find("\"project_state_revision\":0") != std::string::npos);
        CHECK(parts.find("\"project_observation_revision\":1") != std::string::npos);
        CHECK(parts.find("\"availability_revision\":1") != std::string::npos);
        CHECK(parts.find("\"target_generation\":3") != std::string::npos);

        // Record naming is outside the parts, which is why re-observing an
        // unchanged world does not re-decide it: the token and the revision
        // move, the decision basis does not.
        CHECK(parts.find("\"token\"") == std::string::npos);
        CHECK(parts.find("\"snapshot_revision\"") == std::string::npos);

        auto const again = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(again.has_value());
        CHECK(again->token != prepared.snapshot.token);
        CHECK(again->snapshotRevision == prepared.snapshot.snapshotRevision + 1U);
        CHECK(again->decisionBasisHash == prepared.snapshot.decisionBasisHash);
        CHECK(again->stateResolutionHash == prepared.snapshot.stateResolutionHash);

        // observation_id is a SnapshotParts member and is per capture, so two
        // readings of one world are the same DECISION and not the same
        // SNAPSHOT. Erasing that one member is what makes the rest equal.
        auto const withoutObservationId = [](std::string value)
        {
            auto const at = value.find("\"observation_id\":");
            REQUIRE(at != std::string::npos);
            auto const end = value.find(',', at);
            REQUIRE(end != std::string::npos);
            return value.erase(at, end - at + 1U);
        };
        CHECK(again->identityHash != prepared.snapshot.identityHash);
        CHECK(
            withoutObservationId(again->canonicalParts)
            == withoutObservationId(prepared.snapshot.canonicalParts)
        );

        // A different state resolution is a different world, so both hashes
        // move even though nothing the Operator owns did.
        auto unresolvedHost = test_support::secondObservationHost(
            prepared,
            test_support::umbraflowUnresolvedProbeFrame(),
            FrameId{707}
        );
        auto const unresolved = conformance::observeOnce(unresolvedHost);
        CHECK(unresolved.stateResolutionHash() != prepared.snapshot.stateResolutionHash);
        auto const different = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            unresolved
        );
        REQUIRE(different.has_value());
        CHECK(different->identityHash != prepared.snapshot.identityHash);
        CHECK(different->decisionBasisHash != prepared.snapshot.decisionBasisHash);

        // A token is a reference to a composition. A reconciliation that moved
        // ProjectState therefore ends every token taken before it, and a token
        // taken after it opens an Operation.
        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            task::DeliveryOutcome::Delivered
        );
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    operation.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.progress",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-stale"),
            toolInvocation(prepared.project, "observe-1")
        ).has_value());

        auto const afterCommit = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(afterCommit.has_value());
        CHECK(afterCommit->projectStateRevision == 1U);
        CHECK(afterCommit->identityHash != prepared.snapshot.identityHash);

        // The revision moved and the CONTENT did not -- this project's reducer
        // answers with the same bytes -- so the decision basis is unchanged.
        // That is the requirement rather than a gap: the basis covers the four
        // content hashes and no counter, so a world that re-materialized
        // identically does not force a re-plan or a re-approval. The unresolved
        // reading above is the positive control that it can move at all.
        CHECK(afterCommit->projectStateHash == prepared.snapshot.projectStateHash);
        CHECK(afterCommit->decisionBasisHash == prepared.snapshot.decisionBasisHash);
        CHECK(prepared.store.submitCommand(
            prepared.controller,
            command(*afterCommit, "request-fresh"),
            toolInvocation(prepared.project, "observe-1")
        ).has_value());
    }

    // What a state resolution is resolved FROM. The suite carries no frame and
    // no geometry of its own: a project supplies the capture, the model it
    // published states the geometry, and the two have to agree or the resolver
    // is comparing the model's rectangles against pixels from somewhere else.
    //
    // What the RESOLVER can say about it is deliberately coarse: the engine's
    // refusal reaches observe.luau as unknown evidence, so a mismatched pair
    // resolves nothing and carries no message naming the mismatch. That is why
    // this case asserts the resolution rather than only that an observation
    // happened -- and why a project's own capture meets requireProbeGeometry
    // first, inside activateObservationHost, which is the only place holding
    // both numbers. The world below is built by this case rather than supplied,
    // so it reaches the resolver as a project's never can.
    //
    // The mirror case -- a declared geometry that is not the capture's extent --
    // was deleted here rather than moved. After the Q2 ruling a caller cannot
    // declare a geometry at all: the model states base_resolution and the
    // trusted parser publishes it, so there is no second value left to make
    // wrong. docs/archive/plans/2026-08-11-project-as-data.md 7.0 Q2 records that as
    // what deleting the restated number cost.
    TEST_CASE("an observation is resolved from the frame and the geometry its model declares")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());

        // The positive control. Without it the refusal below would prove only
        // that something in this fixture stopped working.
        conformance::requireResolvedSurface(
            test_support::observeAgain(prepared),
            test_support::k_fixtureUiAction.surface
        );

        // One pixel wider than the model's base_resolution, and otherwise the
        // same world: the anchor and the mark are still where the model looks
        // for them.
        auto host = test_support::secondObservationHost(
            prepared,
            test_support::umbraflowWiderProbePng(),
            FrameId{811}
        );
        CHECK(conformance::observeOnce(host).canonicalJcs().contains(
            R"("kind":"unknown_state")"
        ));
    }

    TEST_CASE("contract-state-s03")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const token  = definition(schema, "SnapshotToken");
        CHECK(token.find("\"type\": \"string\"") != std::string::npos);
        CHECK(token.find("{32,128}") != std::string::npos);
        CHECK(token.find("receipt") == std::string::npos);
        CHECK(token.find("fencing") == std::string::npos);
        CHECK(token.find("project_state") == std::string::npos);

        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // Opaque: the Operator mints the token, so a string shaped like one is
        // worth nothing. Business code has no field to construct.
        auto forged          = command(prepared.snapshot, "request-forged");
        forged.snapshotToken = std::string(prepared.snapshot.token.size(), 'a');
        CHECK(forged.snapshotToken != prepared.snapshot.token);
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            forged,
            toolInvocation(prepared.project, "command-1")
        ).has_value());

        // Not a live Receipt: presenting it does not consume it. One token
        // opens as many Operations as the session asks for.
        REQUIRE(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-1"),
            toolInvocation(prepared.project, "command-1")
        ).has_value());
        REQUIRE(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, "observe-1")
        ).has_value());

        // Not a permission either: it is a compare-and-swap reference into the
        // session that made it, so a human takeover ends it without anything
        // about the token itself changing.
        REQUIRE(
            prepared.store.takeoverLease(prepared.controller, "human takeover").has_value()
        );
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-3"),
            toolInvocation(prepared.project, "observe-1")
        ).has_value());
    }

    TEST_CASE("schema-state-s04")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const basis  = definition(schema, "DecisionBasis");
        checkStrictObject(basis);
        CHECK(basis.find("\"state_resolution_hash\"") != std::string::npos);
        CHECK(basis.find("\"project_observation_hash\"") != std::string::npos);
        CHECK(basis.find("\"project_state_hash\"") != std::string::npos);
        CHECK(basis.find("\"session_manifest_hash\"") != std::string::npos);
        CHECK(basis.find("\"decision_basis_hash\"") != std::string::npos);
    }

    TEST_CASE("contract-state-s05")
    {
        auto const schema             = readSchema("umbraflow-operator-v1.schema.json");
        auto const manifestDefinition = definition(schema, "SessionManifest");
        checkStrictObject(manifestDefinition);
        CHECK(manifestDefinition.find("\"runtime_model_artifact_root_hash\"") != std::string::npos);
        CHECK(manifestDefinition.find("\"project_registration_hash\"") != std::string::npos);
        CHECK(manifestDefinition.find("\"policy_artifact_hash\"") != std::string::npos);
        CHECK(manifestDefinition.find("\"journal_envelope_schema_hash\"") != std::string::npos);
        CHECK(manifestDefinition.find("\"plugin_environment_hash\"") != std::string::npos);

        auto const first  = SessionManifest::create(manifestSpec());
        auto const second = SessionManifest::create(manifestSpec());
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->canonicalBytes() == second->canonicalBytes());
        CHECK(first->hash() == second->hash());

        // Determinism above says the two mints agree; it does not say what they
        // agree about. S-05 requires the manifest to BIND these values, so each
        // one is moved on its own and the identity has to move with it. A field
        // that leaves the canonical form changes both mints identically and
        // would pass every assertion above.
        using SpecField = ContentHash SessionManifestSpec::*;
        constexpr auto fields = std::array<std::pair<std::string_view, SpecField>, 8U>{{
            {"agent_profile_hash", &SessionManifestSpec::agentProfileHash},
            {"host_protocol_schema_hash", &SessionManifestSpec::hostProtocolSchemaHash},
            {"journal_envelope_schema_hash", &SessionManifestSpec::journalEnvelopeSchemaHash},
            {"operator_protocol_schema_hash", &SessionManifestSpec::operatorProtocolSchemaHash},
            {"policy_artifact_hash", &SessionManifestSpec::policyArtifactHash},
            {"project_registration_hash", &SessionManifestSpec::projectRegistrationHash},
            {"runtime_model_artifact_root_hash",
             &SessionManifestSpec::runtimeModelArtifactRootHash},
            {"runtime_model_schema_hash", &SessionManifestSpec::runtimeModelSchemaHash},
        }};

        for (auto const& [name, field] : fields)
        {
            CAPTURE(name);
            auto perturbed = manifestSpec();
            perturbed.*field = hashOf(std::string{"moved-"} + std::string{name});
            auto const moved = SessionManifest::create(perturbed);
            REQUIRE(moved.has_value());
            CHECK(moved->canonicalBytes() != first->canonicalBytes());
            CHECK(moved->hash() != first->hash());
            CHECK(
                moved->canonicalBytes().find(std::string{"\""} + std::string{name} + "\"")
                != std::string::npos
            );
        }

        // The ninth value the manifest binds, which no spec field can move
        // because no caller states it. The bridge source that wraps every
        // plugin call, the global whitelist and the frozen tables published
        // beside it are its whole preimage, and until it was bound here a
        // framework upgrade that changed any of them changed what every plugin
        // did under a session manifest whose hash had not moved.
        CHECK(
            first->canonicalBytes().find(
                "\"plugin_environment_hash\":\"" + first->pluginEnvironmentHash().hex() + "\""
            )
            != std::string::npos
        );

        // And the mint reads it from the framework rather than restating a
        // constant of its own: the same digest the script module publishes is
        // the one this manifest carries, so a moved environment moves the
        // manifest by construction.
        auto const environment = script::pluginEnvironmentHash();
        REQUIRE(environment.has_value());
        CHECK(first->pluginEnvironmentHash() == *environment);
    }

    TEST_CASE("contract-state-s06")
    {
        auto const schema   = readSchema("umbraflow-journal-v1.schema.json");
        auto const instance = definition(schema, "ProjectInstance");
        auto const state    = definition(schema, "ProjectState");
        checkStrictObject(instance);
        checkStrictObject(state);
        CHECK(instance.find("\"project_instance_key\"") != std::string::npos);
        CHECK(instance.find("\"creation_event_id\"") != std::string::npos);
        CHECK(instance.find("\"current_project_state_revision\"") != std::string::npos);
        CHECK(instance.find("\"mutation_chain_operation_id\"") != std::string::npos);
        CHECK(state.find("\"project_registration_hash\"") != std::string::npos);
        CHECK(state.find("\"state_hash\"") != std::string::npos);

        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // Everything above reads schema text and passes whether or not the
        // store agrees. JR:`ProjectState` is not a shape nothing produces: the
        // project_state row IS that record, member for member, so the schema's
        // required list and the columns the Operator's own DDL created are the
        // same set. A column renamed on either side is red here.
        auto schemaProbe = TemporaryDirectory{};
        CHECK(
            test_support::operatorTableColumns(schemaProbe.path(), "project_state")
            == test_support::requiredMembers(state)
        );

        // The same check on a record that is assembled rather than stored is
        // the positive control: JR:`ProjectInstance` names nine members and the
        // project_instances row carries four, the rest coming from
        // project_registrations and project_state. Without it, a comparison that
        // could never fail would read the same as one that pins something.
        CHECK(
            test_support::operatorTableColumns(schemaProbe.path(), "project_instances")
            != test_support::requiredMembers(instance)
        );

        auto const& registration = prepared.project.registration;
        auto const baseline = [&prepared, &registration](
            std::string instanceKey,
            std::string eventId
        )
        {
            return ProjectInstanceBaseline{
                .projectInstanceKey  = std::move(instanceKey),
                .eventId             = std::move(eventId),
                .sessionManifestHash = prepared.manifest.hash(),
                .entry               = journalEntry(
                    prepared.project,
                    registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            };
        };

        // The key is immutable, so no second baseline can restart the revision
        // line of a key that snapshots and Operations already name.
        CHECK_FALSE(prepared.store.provisionProjectInstance(
            registration,
            prepared.plugin,
            baseline("instance-1", "baseline-again")
        ).has_value());

        // A session may only pin a provisioned key, so naming a fresh one is
        // not a way to reach revision zero either.
        CHECK_FALSE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = "session-missing-instance",
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = registration.hash(),
                .capabilityProfileHash     = hashOf("capability"),
                .controlledTargetId        = "target-2",
                .projectInstanceKey        = "instance-missing",
                .mode                      = SessionMode::Write,
            },
            prepared.manifest,
            std::nullopt
        ).has_value());

        // Re-baselining is therefore always a NEW key, and a new key is its own
        // revision line starting at zero, side by side with the old one.
        REQUIRE(prepared.store.provisionProjectInstance(
            registration,
            prepared.plugin,
            baseline("instance-0", "baseline-0")
        ).has_value());

        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            task::DeliveryOutcome::Delivered
        );
        auto const progressed = prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    operation.operationId,
                    "{\"disposition\":\"continue\"}"
                ),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry = journalEntry(
                            prepared.project,
                            "fixture.progress",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        );
        REQUIRE(progressed.has_value());

        // The ABA itself: instance-0 really is at revision 0, and that does not
        // make revision 0 current for the instance this session is pinned to.
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = progressed->revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    operation.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
            }
        ).has_value());

        auto const confirmed = prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = progressed->revision,
                .expectedProjectStateRevision = 1U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    operation.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
            }
        );
        REQUIRE(confirmed.has_value());
        CHECK(confirmed->state == OperationState::Confirmed);
    }
    // The decision basis is a property of the observed world, not of the
    // request and not of the authority holding it. T4, T5 and T6 of the W2
    // specification are one case on purpose: an empty or constant derivation
    // satisfies the first two alone, and the third is what proves the
    // derivation can produce a different value at all.
    TEST_CASE("contract-state-s04")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const before = prepared.snapshot;

        // A takeover replaces the lease id, the fencing token, the lease
        // revision and the control availability revision -- everything the
        // identity carries about authority and nothing about content.
        auto const takeover = prepared.store.takeoverLease(prepared.controller, "human");
        REQUIRE(takeover.has_value());
        CHECK(takeover->lease.fencingToken > prepared.lease.fencingToken);
        CHECK(takeover->resolvedDispatches == 0U);
        prepared.lease = takeover->lease;

        auto const after = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(after.has_value());

        // Same world, different authority: the same decision, a different
        // snapshot.
        CHECK(after->decisionBasisHash == before.decisionBasisHash);
        CHECK(after->identityHash != before.identityHash);
        CHECK(after->availabilityRevision > before.availabilityRevision);

        // The positive control. A committed reconciliation moves ProjectState,
        // which is one of the four inputs, so the basis must move with it --
        // without this a derivation returning a constant passes the two checks
        // above.
        prepared.snapshot    = *after;
        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            task::DeliveryOutcome::Delivered
        );
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    operation.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());

        auto const moved = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(moved.has_value());
        CHECK(moved->projectStateHash != after->projectStateHash);
        CHECK(moved->decisionBasisHash != after->decisionBasisHash);

        // And the basis is recomputable from the row: canonical_parts carries
        // the same value the record does, so a test can falsify the derivation
        // rather than only compare it against itself.
        CHECK(
            moved->canonicalParts.find(
                "\"decision_basis_hash\":\"" + moved->decisionBasisHash.hex() + "\""
            ) != std::string::npos
        );
    }
}
