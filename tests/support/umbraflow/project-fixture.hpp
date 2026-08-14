#pragma once

#include "project-schemas.hpp"

#include <conformance/observation-fixture.hpp>
#include <conformance/operator-protocol.hpp>

#include <deployment/project-directory.hpp>

#include <operator/agent-profile.hpp>
#include <operator/effective-plan.hpp>
#include <operator/journal-entry.hpp>
#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/runtime-installation.hpp>
#include <operator/tool-invocation.hpp>

#include <task/host-delivery.hpp>
#include <task/runtime-model-file.hpp>

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Publishing a RuntimeArtifact from bytes held in C++.
//
// This left the suite's own headers with step 6, because a project directory
// publishes its own artifact and the suite installs those bytes rather than
// re-serializing a manifest. What still needs it is tests/operator, which
// builds a SECOND artifact -- ambiguousRuntimeModel below -- that no project
// directory contains and that exists to give the ledger two artifact roots to
// tell apart. It stays in the conformance namespace because that is the name
// its callers spell, and it dies with the rest of this header when Q5 of
// docs/archive/plans/2026-08-11-project-as-data.md moves those tests onto the loader.
namespace uf::operator_runtime::conformance
{
    // One file inside a RuntimeArtifact: the artifact-relative path a locator
    // names, and the exact bytes stored there.
    struct ArtifactFile final
    {
        std::string            path{};
        std::vector<std::byte> bytes{};
    };

    // One published RuntimeArtifact: a RuntimeModel and the complete asset
    // closure that model's locators name.
    struct ProjectRuntimeArtifact final
    {
        std::string               model{};
        std::vector<ArtifactFile> assets{};
    };

    [[nodiscard]]
    inline auto artifactManifestRow(ArtifactFile const& file) -> std::string
    {
        return std::format(
            R"({{"path":"{}","sha256":"{}","size":{}}})",
            file.path,
            observationHash(file.bytes).hex(),
            file.bytes.size()
        );
    }

    // Writes one RuntimeArtifact directory and returns its root hash, which is
    // the hash of the manifest naming every file in it.
    [[nodiscard]]
    inline auto publishRuntimeArtifact(
        std::filesystem::path const& root,
        std::string_view model,
        std::vector<ArtifactFile> assets
    ) -> ContentHash
    {
        std::ranges::sort(assets, {}, &ArtifactFile::path);
        writeArtifactFile(root / task::k_runtimeModelFileName, model);
        auto rows = std::vector<std::string>{};
        rows.reserve(assets.size());
        for (auto const& asset : assets)
        {
            writeArtifactFile(root / std::filesystem::path{asset.path}, asset.bytes);
            rows.emplace_back(artifactManifestRow(asset));
        }

        auto assetJson = std::string{};
        for (auto index = std::size_t{0}; index < rows.size(); ++index)
        {
            if (index != 0U)
            {
                assetJson.push_back(',');
            }
            assetJson += rows[index];
        }
        auto const modelBytes = std::as_bytes(
            std::span{model.data(), model.size()}
        );
        auto const modelFile = ArtifactFile{
            .path  = std::string{task::k_runtimeModelFileName},
            .bytes = {modelBytes.begin(), modelBytes.end()},
        };
        auto const manifest = std::format(
            R"({{"assets":[{}],"manifest_schema_hash":"{}",)"
            R"("page_model":{},"runtime_model_schema_hash":"{}"}})",
            assetJson,
            task::k_runtimeArtifactSchemaHash,
            artifactManifestRow(modelFile),
            task::k_runtimeModelSchemaHash
        );
        writeArtifactFile(root / task::k_runtimeArtifactManifestFileName, manifest);
        return observationHash(manifest);
    }

    // The same handoff shape observationRelease builds from a published
    // directory, for an artifact that has no directory to be published from.
    [[nodiscard]]
    inline auto observationRelease(
        std::filesystem::path const& root,
        ProjectRuntimeArtifact const& artifact
    ) -> ObservationRelease
    {
        auto const handoff          = root / "release";
        auto const artifactRootHash = publishRuntimeArtifact(
            handoff / "runtime-artifact",
            artifact.model,
            artifact.assets
        );
        auto const releaseManifest = std::format(
            R"({{"annotation_workspace_schema_hash":"{}",)"
            R"("candidate_id":"candidate-1","candidate_revision":1,)"
            R"("generation":1,"predecessor_publication_id":null,)"
            R"("replay_gate_hash":"{}","runtime_artifact_root_hash":"{}",)"
            R"("workspace_sqlite_schema_hash":"{}"}})",
            detail::k_annotationWorkspaceSchemaHash,
            observationHash("replay-gate").hex(),
            artifactRootHash.hex(),
            detail::k_workspaceSqliteSchemaHash
        );
        writeArtifactFile(handoff / "release.manifest.json", releaseManifest);
        return ObservationRelease{
            .handoffRoot         = handoff,
            .releaseManifestHash = observationHash(releaseManifest),
            .artifactRootHash    = artifactRootHash,
        };
    }
}

namespace uf::operator_runtime::test_support
{
    struct ProjectFixture final
    {
        VerifiedProjectRegistration   registration;
        ProjectSchemaOwner            schemaOwner;
        ProjectJournalSchemaOwner     journalSchemaOwner;
        ProjectToolCatalogSchemaOwner toolCatalogSchemaOwner;
        ProjectReconcileSchemaOwner   reconcileSchemaOwner;

        // The exact Tool Catalog bytes this registration pinned. A case that
        // builds a second catalog owner over the same registration needs them,
        // because such an owner is bound to their hash.
        std::string toolCatalogBytes;

        // The exact bytes the document validator last saw as a Reduce or Derive
        // input. The synchronized log is shared with the retained validator
        // because the property under test is that the Operator decides those
        // bytes and no caller can.
        std::shared_ptr<deployment::ProjectDocumentInputLog> documentInputLog;
    };

    // The one conforming JR:`JournalProvenance` this fixture project mints, and
    // six documents that each violate exactly one of that schema's rules. All
    // seven are exact JCS, so the canonical validator admits every one of them
    // and the framework's fixed-schema check is the only thing that can tell
    // them apart. A project supplies these VALUES; the schema that judges them
    // is the framework's and is not delegated.
    inline constexpr auto k_fixtureProvenance = std::string_view{
        "{\"kind\":\"observation\","
        "\"observation_ids\":[\"fixture-observation-1\"],"
        "\"principal_id\":null,\"source_hashes\":[]}"
    };
    inline constexpr auto k_fixtureProvenanceViolations = std::array{
        // kind outside the five-value enum.
        std::string_view{
            "{\"kind\":\"forged\",\"observation_ids\":[],"
            "\"principal_id\":null,\"source_hashes\":[]}"
        },
        // source_hashes missing, so three of four required members are present.
        std::string_view{
            "{\"kind\":\"observation\",\"observation_ids\":[],"
            "\"principal_id\":null}"
        },
        // A fifth member, against additionalProperties: false.
        std::string_view{
            "{\"kind\":\"observation\",\"observation_ids\":[],"
            "\"principal_id\":null,\"source_hashes\":[],\"witness\":\"suite\"}"
        },
        // An element that is not a 64-character lowercase hex Hash.
        std::string_view{
            "{\"kind\":\"observation\",\"observation_ids\":[],"
            "\"principal_id\":null,\"source_hashes\":[\"not-a-hash\"]}"
        },
        // A repeated element, against uniqueItems.
        std::string_view{
            "{\"kind\":\"observation\",\"observation_ids\":[\"a\",\"a\"],"
            "\"principal_id\":null,\"source_hashes\":[]}"
        },
        // An empty principal_id, which the Identifier pattern refuses.
        std::string_view{
            "{\"kind\":\"observation\",\"observation_ids\":[],"
            "\"principal_id\":\"\",\"source_hashes\":[]}"
        },
    };

    // The payload schema identity every fixture effect names: the sha256 of
    // k_effectPayloadSchema's own bytes, so the deployment can find the schema
    // the effect claims and judge the payload against it.
    [[nodiscard]]
    inline auto effectPayloadSchemaHex() -> std::string
    {
        return schemaHashHex(k_effectPayloadSchema);
    }

    // The OP:`PlanProposal` the fixture plugin returns for each mutating tool,
    // and the OP:`UIActionIntent` it returns for every next_step. They are
    // plugin source -- the plugin is what produces them, and prepareStore
    // installs the plugin -- so they are spelled once here and read nowhere
    // else in C++.
    //
    // Luau table literals rather than JSON text: a plugin exchanges decoded
    // values now, so what it returns is a value it constructs rather than bytes
    // it spells. Nothing here has to agree with a serializer any more, which is
    // the point -- the fixture cannot emit a non-canonical proposal because it
    // no longer emits a proposal's bytes at all.
    [[nodiscard]]
    inline auto fixturePlanProposal(
        std::string_view toolName,
        std::string_view effects,
        std::string_view limits
    ) -> std::string
    {
        auto proposal = std::string{"{\n            allowed_ui_actions = { \"fixture.step\" },"};
        proposal += "\n            canonical_args = { value = 1 },";
        proposal += "\n            effects = ";
        proposal += effects;
        proposal += ",\n            tool_name = \"";
        proposal += toolName;
        proposal += "\",\n            tool_version = \"1\",";
        proposal += "\n            workflow_limits = ";
        proposal += limits;
        proposal += ",\n        }";
        return proposal;
    }

    [[nodiscard]]
    inline auto fixtureEffect(
        std::string_view scopeKey,
        std::string_view risk
    ) -> std::string
    {
        auto effect = std::string{"{ namespaced_type = \"fixture.write\", "};
        effect += "opaque_project_payload = { value = 1 }, ";
        effect += "payload_schema_hash = \"";
        effect += effectPayloadSchemaHex();
        effect += "\", risk = \"";
        effect += risk;
        effect += "\", scope_key = \"";
        effect += scopeKey;
        effect += "\", scope_kind = \"instance\" }";
        return effect;
    }

    [[nodiscard]]
    inline auto fixtureWorkflowLimits(
        std::string_view maximumSteps,
        std::string_view maximumDispatches
    ) -> std::string
    {
        auto limits = std::string{"{ maximum_dispatches = "};
        limits += maximumDispatches;
        limits += ", maximum_elapsed_ms = 60000, maximum_observations = 16, ";
        limits += "maximum_steps = ";
        limits += maximumSteps;
        limits += ", maximum_waits = 4 }";
        return limits;
    }

    inline auto const k_fixtureUiActionIntent = std::string{
        "{\n        action = { action_id = \"fixture.press\","
        " canonical_parameters = { value = 1 },"
        " surface_id = \"fixture.surface\", ui_target_id = \"fixture.target\" },"
        "\n        binding_variant_constraints = {}, delivery_class = \"delivery_safe\","
        "\n        expected_ui_postconditions = {}, required_ui_preconditions = {},"
        "\n        step_key = \"fixture.step\","
        "\n        timeout_policy = { maximum_elapsed_ms = 5000, on_timeout = \"reobserve\" },"
        "\n    }"
    };

    inline auto const k_fixtureWaitIntent = std::string{
        "{\n        condition = { settled = true }, observation_budget = 4,"
        "\n        step_key = \"fixture.step\","
        "\n        timeout_policy = { maximum_elapsed_ms = 5000, on_timeout = \"reobserve\" },"
        "\n    }"
    };

    inline auto const k_fixtureUiThenWaitIntent = std::string{
        "input.step_index == 1 and " + k_fixtureUiActionIntent + " or " + k_fixtureWaitIntent
    };

    [[nodiscard]]
    inline auto hashOf(std::string_view value) -> ContentHash
    {
        auto const result = sha256(std::as_bytes(std::span{value}));
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]]
    inline auto makeProject(
        std::string pluginId,
        std::string_view pluginBytes
    ) -> ProjectFixture
    {
        auto const bundle = DeploymentBundle{pluginId};
        auto const deployed = deployment::ProjectDeployment::create(bundle.sources());
        {
            auto const why = deployed.has_value()
                ? std::string{}
                : std::string{deployed.error().message()};
            INFO(why);
            REQUIRE(deployed.has_value());
        }

        auto const schemaHash             = hashOf("registration-schema");
        auto const pluginHash             = hashOf(pluginBytes);
        auto const toolCatalogHash        = hashOf(bundle.toolCatalog());
        auto const stateSchemaHash        = hashOf(k_projectStateSchema);
        auto const observationSchemaHash  = hashOf(k_projectObservationSchema);
        auto const preconditionSchemaHash = hashOf(k_toolPreconditionSchema);
        auto const reconcileSchemaHash    = hashOf(bundle.reconcileManifest());
        auto const journalSchemaHash      = hashOf(bundle.journalEventManifest());
        auto const exactJcs = std::format(
            "{{\"baseline_event_type\":\"fixture.baseline\","
            "\"journal_event_schema_manifest_hash\":\"{}\","
            "\"manifest_schema_hash\":\"{}\",\"plugin_hash\":\"{}\","
            "\"plugin_id\":\"{}\",\"project_artifact_roots\":[],"
            "\"project_observation_schema_hash\":\"{}\","
            "\"project_state_schema_hash\":\"{}\","
            "\"project_tool_precondition_schema_hash\":\"{}\","
            "\"reconcile_payload_schema_manifest_hash\":\"{}\","
            "\"tool_catalog_hash\":\"{}\"}}",
            journalSchemaHash.hex(),
            schemaHash.hex(),
            pluginHash.hex(),
            pluginId,
            observationSchemaHash.hex(),
            stateSchemaHash.hex(),
            preconditionSchemaHash.hex(),
            reconcileSchemaHash.hex(),
            toolCatalogHash.hex()
        );
        auto const claims = ProjectRegistrationClaims{
            .manifestSchemaHash                 = schemaHash,
            .pluginId                           = pluginId,
            .pluginHash                         = pluginHash,
            .toolCatalogHash                    = toolCatalogHash,
            .projectStateSchemaHash             = stateSchemaHash,
            .projectObservationSchemaHash       = observationSchemaHash,
            .projectToolPreconditionSchemaHash  = preconditionSchemaHash,
            .reconcilePayloadSchemaManifestHash = reconcileSchemaHash,
            .journalEventSchemaManifestHash     = journalSchemaHash,
            .baselineEventType                  = "fixture.baseline",
        };
        auto owner = ProjectRegistrationSchemaOwner::create(
            schemaHash,
            // Init-captures rather than [exactJcs, claims]: both locals are
            // const, and capturing a const entity by name gives the closure a
            // const member its move constructor must copy rather than move.
            [exactJcs = exactJcs, claims = claims](
                std::string_view candidate
            ) -> Result<ProjectRegistrationClaims>
            {
                if (candidate != exactJcs)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture registration is not exact JCS"
                    );
                }
                return claims;
            }
        );
        REQUIRE(owner.has_value());
        auto registration = ProjectRegistration::verifyExact(
            exactJcs,
            hashOf(exactJcs),
            *owner
        );
        REQUIRE(registration.has_value());

        auto documentInputLog =
            std::make_shared<deployment::ProjectDocumentInputLog>();
        auto schemaOwner = ProjectSchemaOwner::create(
            *registration,
            ProjectDocumentSchemaBytes{
                .projectState       = k_projectStateSchema,
                .projectObservation = k_projectObservationSchema,
                .toolPrecondition   = k_toolPreconditionSchema,
            },
            deployment::canonicalJsonValidator(),
            // The deployment's own document validator, with the two envelopes
            // the suite asserts against recorded on the way in. Recording is the
            // fixture's; deciding is the deployment's.
            [
                validate = deployed->documentValidator(),
                documentInputLog
            ](
                ProjectPluginFunction function,
                ProjectDocumentDirection direction,
                std::string_view candidateJcs
            ) -> Status
            {
                if (direction == ProjectDocumentDirection::Input)
                {
                    documentInputLog->record(function, candidateJcs);
                }
                return validate(function, direction, candidateJcs);
            }
        );
        REQUIRE(schemaOwner.has_value());

        auto journalSchemaOwner = ProjectJournalSchemaOwner::create(
            *registration,
            bundle.journalEventManifest(),
            deployed->journalPayloadValidator()
        );
        REQUIRE(journalSchemaOwner.has_value());

        auto toolCatalogSchemaOwner = ProjectToolCatalogSchemaOwner::create(
            *registration,
            bundle.toolCatalog(),
            deployed->toolCatalogReader(),
            deployed->toolArgumentValidator()
        );
        REQUIRE(toolCatalogSchemaOwner.has_value());

        auto reconcileSchemaOwner = ProjectReconcileSchemaOwner::create(
            *registration,
            bundle.reconcileManifest(),
            deployed->reconcileDispositionReader()
        );
        REQUIRE(reconcileSchemaOwner.has_value());

        return ProjectFixture{
            .registration           = *registration,
            .schemaOwner            = *schemaOwner,
            .journalSchemaOwner     = *journalSchemaOwner,
            .toolCatalogSchemaOwner = *toolCatalogSchemaOwner,
            .reconcileSchemaOwner   = *reconcileSchemaOwner,
            .toolCatalogBytes       = bundle.toolCatalog(),
            .documentInputLog       = std::move(documentInputLog),
        };
    }

    [[nodiscard]]
    inline auto canonical(
        ProjectSchemaOwner const& owner,
        std::string value
    ) -> CanonicalJson
    {
        auto result = owner.canonicalize(std::move(value));
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]]
    inline auto journalEntry(
        ProjectFixture const& project,
        std::string eventType,
        std::string payload,
        std::string provenance = std::string{k_fixtureProvenance}
    ) -> ValidatedJournalEntryData
    {
        auto result = project.journalSchemaOwner.validate(
            std::move(eventType),
            canonical(project.schemaOwner, std::move(payload)),
            canonical(project.schemaOwner, std::move(provenance))
        );
        REQUIRE(result.has_value());
        return *result;
    }

    // Runs the plugin's reconcile and reads its conclusion through the
    // authority, which is the only way a ReconciliationCommit can name one.
    [[nodiscard]]
    inline auto reconcileOutcome(
        ProjectFixture const& project,
        ProjectPluginHandle const& plugin,
        std::string operationId,
        std::string document
    ) -> ValidatedReconcileOutcome
    {
        auto proposal = plugin.reconcile(
            canonical(project.schemaOwner, std::move(document))
        );
        REQUIRE(proposal.has_value());
        auto outcome = project.reconcileSchemaOwner.validate(
            std::move(operationId),
            *std::move(proposal)
        );
        REQUIRE(outcome.has_value());
        return *outcome;
    }

    [[nodiscard]]
    inline auto toolInvocation(
        ProjectFixture const& project,
        std::string toolName,
        std::string args = "{\"value\":1}"
    ) -> ValidatedToolInvocation
    {
        auto result = project.toolCatalogSchemaOwner.validate(
            std::move(toolName),
            canonical(project.schemaOwner, std::move(args))
        );
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]]
    inline auto loadPlugin(
        ProjectFixture const& project,
        std::string_view pluginBytes
    ) -> ProjectPluginHandle
    {
        auto registrar = ProjectPluginRegistrar{};
        auto result = registrar.registerPlugin(
            project.registration,
            std::string{pluginBytes},
            {},
            project.schemaOwner
        );
        REQUIRE(result.has_value());
        return *result;
    }

    // The exact bytes an AgentProfile is, in the frozen AgentBudget's member
    // order. A budget is a document the deployment writes and the manifest
    // attests to, so the fixture produces bytes and derives the hash from them
    // rather than the other way round.
    [[nodiscard]]
    inline auto agentProfileBytes(AgentBudget const& budget) -> std::string
    {
        return std::format(
            "{{\"maximum_elapsed_ms\":{},\"maximum_mutations\":{},"
            "\"maximum_observations\":{},\"maximum_risk_units\":{},"
            "\"maximum_tool_calls\":{}}}",
            budget.maximumElapsedMillis,
            budget.maximumMutations,
            budget.maximumObservations,
            budget.maximumRiskUnits,
            budget.maximumToolCalls
        );
    }

    // Wide enough that a case which is not about budgets never reaches one. A
    // case that IS about a budget states its own numbers, so no case is ever
    // testing a ceiling it did not choose.
    inline constexpr auto k_unconstrainedAgentBudget = AgentBudget{
        .maximumToolCalls     = 1'000U,
        .maximumMutations     = 1'000U,
        .maximumObservations  = 1'000U,
        .maximumElapsedMillis = 3'600'000U,
        .maximumRiskUnits     = 1'000'000U,
    };

    [[nodiscard]]
    inline auto readProfileMember(
        std::string_view exactJcs,
        std::string_view member
    ) -> std::optional<uint64>
    {
        auto const key = std::format("\"{}\":", member);
        auto const at  = exactJcs.find(key);
        if (at == std::string_view::npos)
        {
            return std::nullopt;
        }
        auto const rest  = exactJcs.substr(at + key.size());
        auto       value = uint64{};
        // SAFETY: std::from_chars names its range as a pointer pair, which is
        // the one shape a bounded view cannot express. Both ends come from
        // rest's own extent, so no caller states a bound and the computed
        // address is rest's one-past-the-end.
        UF_UNSAFE_BUFFER_BEGIN
        auto const read = std::from_chars(
            rest.data(),
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            rest.data() + rest.size(),
            value
        );
        UF_UNSAFE_BUFFER_END
        if (read.ec != std::errc{})
        {
            return std::nullopt;
        }
        return value;
    }

    // Stands in for a deployment's AgentProfile schema owner. It reads the five
    // ceilings out of the exact bytes rather than being handed a struct,
    // because the bytes are what agent_profile_hash attests to and a validator
    // that ignored them would let any budget answer for any manifest.
    [[nodiscard]]
    inline auto agentProfileValidator() -> AgentProfileValidator
    {
        return [](std::string_view exactJcs) -> Result<AgentBudget>
        {
            auto const toolCalls    = readProfileMember(exactJcs, "maximum_tool_calls");
            auto const mutations    = readProfileMember(exactJcs, "maximum_mutations");
            auto const observations = readProfileMember(exactJcs, "maximum_observations");
            auto const elapsed      = readProfileMember(exactJcs, "maximum_elapsed_ms");
            auto const riskUnits    = readProfileMember(exactJcs, "maximum_risk_units");
            if (
                !toolCalls || !mutations || !observations || !elapsed || !riskUnits
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "AgentProfile bytes are not a complete budget document"
                );
            }
            return AgentBudget{
                .maximumToolCalls     = *toolCalls,
                .maximumMutations     = *mutations,
                .maximumObservations  = *observations,
                .maximumElapsedMillis = *elapsed,
                .maximumRiskUnits     = *riskUnits,
            };
        };
    }

    // The exact PolicyArtifact bytes this project's sessions are pinned to. It
    // is derived from the operator protocol schema hash the manifest below
    // names, because the artifact declares which protocol it answers for and
    // verifyExact refuses one that answers for another. The one effect type it
    // speaks about is the one this project's plugin proposes.
    [[nodiscard]]
    inline auto policyArtifactBytes() -> std::string
    {
        auto const types = std::vector<std::string>{std::string{k_effectType}};
        return conformance::policyArtifactBytes(hashOf("operator"), types);
    }

    [[nodiscard]]
    inline auto sessionManifest(
        VerifiedProjectRegistration const& project,
        ContentHash const& runtimeArtifactRootHash,
        ContentHash const& agentProfileHash,
        std::string_view exactPolicyArtifactBytes
    ) -> SessionManifest
    {
        auto const result = SessionManifest::create(
            SessionManifestSpec{
                .runtimeModelArtifactRootHash = runtimeArtifactRootHash,
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = project.hash(),
                .policyArtifactHash           = hashOf(exactPolicyArtifactBytes),
                .agentProfileHash             = agentProfileHash,
            }
        );
        REQUIRE(result.has_value());
        return *result;
    }

    inline auto writeFile(
        std::filesystem::path const& path,
        std::string_view bytes
    ) -> void
    {
        std::filesystem::create_directories(path.parent_path());
        auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(stream.good());
    }

    // This project's own RuntimeModel: one scene with one activatable binding,
    // the smallest model whose resolver output is a resolved state. Every id in
    // it is this project's, and the OP:`UIActionIntent` below names the same
    // surface, target and action, so a step this project plans and a click the
    // Host authorizes describe one thing rather than two that never met.
    [[nodiscard]]
    inline auto umbraflowRuntimeModel() -> std::string
    {
        return R"toml(schema_version = 2
base_resolution = [3, 1]
base_dpi = [96, 96]

[[ui_target]]
id = "fixture.marker"
kind = "region"

[[ui_target]]
id = "fixture.target"
kind = "control"

[[locator]]
id = "fixture.anchor"
kind = "template"
asset_path = "assets/fixture-anchor.png"
threshold = 1

[[locator]]
id = "fixture.mark"
kind = "template"
asset_path = "assets/fixture-mark.png"
threshold = 1

[[binding]]
id = "fixture.surface.anchor"
surface = "fixture.surface"
ui_target = "fixture.marker"
placement = { kind = "fixed", rect = [0, 0, 1, 1] }
variants = [{ name = "primary", detector = { all = [{ kind = "locator_present", locator = "fixture.anchor" }], any = [], none = [] } }]
actions = []

[[binding]]
id = "fixture.target.primary"
surface = "fixture.surface"
ui_target = "fixture.target"
placement = { kind = "fixed", rect = [1, 0, 1, 1], action_point = [1, 0] }
variants = [{ name = "primary", detector = { all = [{ kind = "locator_present", locator = "fixture.mark" }], any = [], none = [] } }]
actions = [{ id = "fixture.press", kind = "click", proof_locator = "fixture.mark" }]

[[surface]]
id = "fixture.surface"
kind = "scene"
covers = []
identity = ["fixture.surface.anchor"]
)toml";
    }

    // The same model with a second scene the probe frame also satisfies, so the
    // resolver reports an ambiguous state and the state_resolution_hash moves
    // without any Operator-held column moving with it. It reuses the mark
    // locator rather than a third asset because the asset closure is verified
    // against the manifest.
    [[nodiscard]]
    inline auto ambiguousRuntimeModel() -> std::string
    {
        return umbraflowRuntimeModel() + R"toml(
[[binding]]
id = "fixture.panel.anchor"
surface = "fixture.panel"
ui_target = "fixture.marker"
placement = { kind = "fixed", rect = [1, 0, 1, 1] }
variants = [{ name = "primary", detector = { all = [{ kind = "locator_present", locator = "fixture.mark" }], any = [], none = [] } }]
actions = []

[[surface]]
id = "fixture.panel"
kind = "scene"
covers = []
identity = ["fixture.panel.anchor"]
)toml";
    }

    // The two grays this project's probe frame carries. Its template assets are
    // authored from them, and they are distinct so that a frame carrying one and
    // not the other resolves to a different state.
    inline constexpr auto k_anchorGray = uint8{2};
    inline constexpr auto k_actionGray = uint8{5};

    // One pixel of one gray. The three-pixel frame below leaves no room for a
    // larger crop, which is the whole point of this world: it is the smallest
    // one a resolver can reach a resolved state in, so nothing a case observes
    // is incidental to the picture.
    [[nodiscard]]
    inline auto templatePng(uint8 gray) -> std::vector<std::byte>
    {
        auto encoded = image::encodeRgbaPng(
            "umbraflow-fixture-template.png",
            1,
            1,
            std::vector<std::byte>{
                static_cast<std::byte>(gray),
                static_cast<std::byte>(gray),
                static_cast<std::byte>(gray),
                std::byte{255},
            }
        );
        REQUIRE(encoded.has_value());
        return *std::move(encoded);
    }

    // The asset closure the model's two template locators name, authored
    // against the grays this project's probe frame carries.
    [[nodiscard]]
    inline auto umbraflowRuntimeAssets() -> std::vector<conformance::ArtifactFile>
    {
        return {
            conformance::ArtifactFile{
                .path  = "assets/fixture-anchor.png",
                .bytes = templatePng(k_anchorGray),
            },
            conformance::ArtifactFile{
                .path  = "assets/fixture-mark.png",
                .bytes = templatePng(k_actionGray),
            },
        };
    }

    [[nodiscard]]
    inline auto umbraflowRuntimeArtifact() -> conformance::ProjectRuntimeArtifact
    {
        return conformance::ProjectRuntimeArtifact{
            .model  = umbraflowRuntimeModel(),
            .assets = umbraflowRuntimeAssets(),
        };
    }

    // One row of grays, encoded the way a real capture arrives.
    [[nodiscard]]
    inline auto umbraflowProbeRow(std::span<uint8 const> grays)
        -> std::vector<std::byte>
    {
        auto pixels = std::vector<std::byte>{};
        pixels.reserve(grays.size() * 4U);
        for (auto const gray : grays)
        {
            pixels.emplace_back(static_cast<std::byte>(gray));
            pixels.emplace_back(static_cast<std::byte>(gray));
            pixels.emplace_back(static_cast<std::byte>(gray));
            pixels.emplace_back(std::byte{255});
        }
        auto encoded = image::encodeRgbaPng(
            "umbraflow-fixture-probe.png",
            static_cast<uint32>(grays.size()),
            1,
            pixels
        );
        REQUIRE(encoded.has_value());
        return *std::move(encoded);
    }

    // A three-pixel capture of this project's world. `left` is the pixel the
    // scene anchor matches and `middle` the one the action's proof locator
    // matches.
    [[nodiscard]]
    inline auto umbraflowProbePng(uint8 left, uint8 middle) -> std::vector<std::byte>
    {
        auto const grays = std::array{left, middle, uint8{0}};
        return umbraflowProbeRow(grays);
    }

    // The same world captured one pixel wider than the model declares, which is
    // the only difference from umbraflowProbeFrame(): both marks are still at
    // the coordinates the model searches.
    [[nodiscard]]
    inline auto umbraflowWiderProbePng() -> std::vector<std::byte>
    {
        auto const grays = std::array{k_anchorGray, k_actionGray, uint8{0}, uint8{0}};
        return umbraflowProbeRow(grays);
    }

    // The frame this project's model resolves its one scene on. Bytes and
    // nothing else: the geometry it must match is the model's, republished by
    // the RuntimeModelBinding the Host parses that model into, so a fixture that
    // carried a fingerprint of its own would be restating a number the model
    // already states.
    [[nodiscard]]
    inline auto umbraflowProbeFrame() -> std::vector<std::byte>
    {
        return umbraflowProbePng(k_anchorGray, k_actionGray);
    }

    // The same world with the scene anchor absent, so the resolver reports an
    // unknown state and the observation's state_resolution_hash differs.
    [[nodiscard]]
    inline auto umbraflowUnresolvedProbeFrame() -> std::vector<std::byte>
    {
        return umbraflowProbePng(0, k_actionGray);
    }

    // The one UI action this project offers a contract run, spelled exactly as
    // k_fixtureUiActionIntent names it.
    inline auto const k_fixtureUiAction = task::UiActionUnderTest{
        .surface  = "fixture.surface",
        .uiTarget = "fixture.target",
        .action   = "fixture.press",
    };

    // The RuntimeArtifact every prepared store installs. It carries this
    // project's RuntimeModel and its template assets rather than a placeholder,
    // because a snapshot is now composed from an observation the Host resolved
    // through that model: a placeholder artifact installs, and then nothing
    // observes.
    [[nodiscard]]
    inline auto runtimeRelease(std::filesystem::path const& root)
        -> conformance::ObservationRelease
    {
        return conformance::observationRelease(root, umbraflowRuntimeArtifact());
    }

    // The trusted plugin a prepared store registers. plugin_id must equal the
    // registration's, so the id is inserted rather than fixed.
    //
    // `plan` reads the tool name out of the envelope the Operator assembled and
    // answers with a different proposal for each one. That is what lets one
    // registration -- one plugin_hash, one session -- reach the clamp, the
    // bound, the approval edge and the tool mismatch: a proposal chosen by the
    // suite instead would be a proposal no plugin produced.
    [[nodiscard]]
    inline auto pluginSource(
        std::string_view pluginId,
        std::string_view nextStepExpression = k_fixtureUiActionIntent
    ) -> std::string
    {
        auto const ordinaryEffects = "{ " + fixtureEffect("alpha", "low") + ", "
            + fixtureEffect("beta", "medium") + " }";
        auto const reorderedEffects = "{ " + fixtureEffect("beta", "medium") + ", "
            + fixtureEffect("alpha", "low") + " }";
        auto const highRiskEffects = "{ " + fixtureEffect("alpha", "high") + " }";
        auto const ordinaryLimits  = fixtureWorkflowLimits("8", "8");

        // The proposals are a module-local table rather than a field of the
        // returned module: a pure data module may export plugin_id and its
        // declared entry points and nothing else.
        auto source = std::string{"local proposals = {\n"};
        struct ProposalCase final
        {
            std::string_view invokedTool{};
            std::string_view proposedTool{};
            std::string_view effects{};
            std::string_view limits{};
        };
        auto const oversizedLimits = fixtureWorkflowLimits("4096", "4096");
        auto const twoStepLimits   = fixtureWorkflowLimits("2", "2");
        auto const cases           = std::array{
            ProposalCase{"command-1", "command-1", ordinaryEffects, ordinaryLimits},
            ProposalCase{"command-2", "command-2", ordinaryEffects, ordinaryLimits},
            ProposalCase{
                "different-command",
                "different-command",
                ordinaryEffects,
                ordinaryLimits,
            },
            // The proposal names the tool the Operation was NOT created for.
            ProposalCase{"mismatched-plan", "command-1", ordinaryEffects, ordinaryLimits},
            ProposalCase{
                "oversized-plan",
                "oversized-plan",
                ordinaryEffects,
                oversizedLimits,
            },
            ProposalCase{"two-step-plan", "two-step-plan", ordinaryEffects, twoStepLimits},
            ProposalCase{
                "approval-plan",
                "approval-plan",
                highRiskEffects,
                ordinaryLimits,
            },
            ProposalCase{
                "reordered-effects",
                "reordered-effects",
                reorderedEffects,
                ordinaryLimits,
            },
            // The read-only tool has a plan too. Without it the plugin refuses
            // for want of an entry, and "read-only Operations carry no plan"
            // would be proved by the fixture rather than by the Operator.
            ProposalCase{"observe-1", "observe-1", ordinaryEffects, ordinaryLimits},
            ProposalCase{
                "raw-coordinate-click",
                "raw-coordinate-click",
                ordinaryEffects,
                ordinaryLimits,
            },
            // Two tools whose plans are ordinary and whose descriptors are not:
            // one declares it cannot be redelivered safely, the other allows
            // less elapsed time than the step this plugin proposes. Each
            // reaches mintStep and is refused by one clause of the descriptor.
            ProposalCase{
                "strict-delivery",
                "strict-delivery",
                ordinaryEffects,
                ordinaryLimits,
            },
            ProposalCase{"brief-timeout", "brief-timeout", ordinaryEffects, ordinaryLimits},
            // Its plan allows the step key every other plan allows, which is
            // the one its own descriptor's ui_action_bounds does not name.
            ProposalCase{"stray-action", "stray-action", ordinaryEffects, ordinaryLimits},
        };
        for (auto const& proposal : cases)
        {
            source += "        [\"";
            source += proposal.invokedTool;
            source += "\"] = ";
            source += fixturePlanProposal(
                proposal.proposedTool,
                proposal.effects,
                proposal.limits
            );
            source += ",\n";
        }
        source += "}\n\nreturn {\n    plugin_id = \"";
        source += pluginId;
        source += R"LUAU(",
    derive = function(_input) return canon.emptyObject end,
    plan = function(input)
        local proposal = proposals[input.tool_name]
        if proposal == nil then
            error("fixture plugin has no plan for " .. tostring(input.tool_name))
        end
        return proposal
    end,
    next_step = function(input) return )LUAU";
        source += nextStepExpression;
        source += R"LUAU( end,
    reconcile = function(input) return input end,
    reduce = function(input)
        for _, event in ipairs(input.journal_events) do
            if event.namespaced_event_type == "fixture.confirmed" then
                return { revision = 1 }
            end
        end
        return { revision = 0 }
    end,
}
)LUAU";
        return source;
    }

    class TemporaryDirectory final
    {
        std::filesystem::path m_path{};

    public:
        TemporaryDirectory()
        {
            static auto s_sequence = std::atomic<uint64>{1};
            m_path = std::filesystem::temp_directory_path()
                / std::format(
                    "umbraflow-contract-{}-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count(),
                    s_sequence.fetch_add(1, std::memory_order_relaxed)
                );
            auto error         = std::error_code{};
            auto const created = std::filesystem::create_directory(m_path, error);
            REQUIRE(created);
            REQUIRE_FALSE(error);
        }

        TemporaryDirectory(TemporaryDirectory const&) = delete;
        TemporaryDirectory(TemporaryDirectory&&) = delete;
        auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
        auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;

        ~TemporaryDirectory() noexcept
        {
            auto error = std::error_code{};
            static_cast<void>(std::filesystem::remove_all(m_path, error));
        }

        [[nodiscard]] auto path() const -> std::filesystem::path const&
        {
            return m_path;
        }
    };

    // An opened Operator carrying an installed RuntimeArtifact, a registered
    // project, one provisioned ProjectInstance, a pinned write session, the
    // lease that session holds and one snapshot taken under it. The manifest
    // travels with it because a restart has to re-pin a session against the
    // same one.
    struct PreparedStore final
    {
        OperatorCoordinator   store;
        ProjectPluginHandle   plugin;
        ProjectFixture        project;
        SessionManifest       manifest;
        OperatorPlanAuthority planAuthority;

        // The authenticated controller every entry point is reached through.
        // bindController is its only mint, so a case cannot assert its own
        // identity, and the kind it carries is the one pinSession pinned.
        ControllerBinding controller;
        ControlLease      lease;
        SnapshotRecord    snapshot;

        // The Host whose observations this store composes snapshots from. It is
        // part of the prepared state rather than built per case because
        // TaskHost owns an activated generation, and a second Host over the
        // same artifact would be a second observer of one world.
        conformance::ObservationHost  observation;

        // What a delivering Host is activated from. A dispatch needs a Host that
        // can act, and the observing one above cannot serve a second
        // TaskContext, so every delivery opens the same installed artifact
        // again rather than sharing that Host.
        ContentHash runtimeArtifactRootHash;
        uint64      installedGeneration{};
    };

    struct PinnedAgentProfile final
    {
        SessionManifest manifest;
        AgentProfile    profile;
    };

    // An Agent session pins a manifest of its own, because the ceilings it runs
    // under are exactly the bytes that manifest attests to: another budget is
    // another agent_profile_hash and therefore another session identity, and
    // that is what makes a permissive budget attributable rather than deniable.
    //
    // The pair is derived from the budget alone, so asking twice for the same
    // budget yields the same manifest and the same profile -- which is what
    // lets a case re-pin an existing Agent session and see whether pinning
    // again refreshes what it already spent.
    [[nodiscard]]
    inline auto agentProfileFor(
        PreparedStore const& prepared,
        AgentBudget const& budget
    ) -> PinnedAgentProfile
    {
        auto const bytes    = agentProfileBytes(budget);
        auto const manifest = sessionManifest(
            prepared.project.registration,
            prepared.runtimeArtifactRootHash,
            hashOf(bytes),
            policyArtifactBytes()
        );
        auto profile = AgentProfile::verifyExact(
            manifest,
            "agent-profile.json",
            bytes,
            agentProfileValidator()
        );
        REQUIRE(profile.has_value());
        return PinnedAgentProfile{
            .manifest = manifest,
            .profile  = *std::move(profile),
        };
    }

    // A Host that can act under the store's current lease. It is a separate
    // Host per call on purpose; see conformance::DeliveringHost.
    [[nodiscard]]
    inline auto deliveringHost(PreparedStore& prepared)
        -> std::unique_ptr<conformance::DeliveringHost>
    {
        return conformance::deliveringHostFor(
            prepared.store,
            prepared.lease,
            prepared.installedGeneration,
            prepared.runtimeArtifactRootHash,
            k_fixtureUiAction,
            umbraflowProbeFrame()
        );
    }

    // Runs one further observation cycle on the prepared Host. Each call is a
    // new capture with a new observation id and, over an unchanged world, the
    // same state resolution.
    [[nodiscard]]
    inline auto observeAgain(PreparedStore& prepared) -> task::UiObservationSnapshot
    {
        return conformance::observeOnce(prepared.observation);
    }

    // A second Host over the SAME installed RuntimeArtifact, looking at a
    // different frame. It is how a case reaches a different state resolution
    // without reaching a different artifact: an observation taken through an
    // artifact the session never pinned is refused before it is resolved, so
    // the two refusals cannot be told apart from one fixture.
    //
    // It assembles the Host itself rather than calling activateObservationHost,
    // because that function now refuses a capture whose extent is not the
    // model's before any observation happens. A case that wants the RESOLVER's
    // answer to such a capture has to reach past that refusal, and only a case
    // building its own world can: a project directory meets the refusal, which
    // is the point of it.
    [[nodiscard]]
    inline auto secondObservationHost(
        PreparedStore& prepared,
        std::span<std::byte const> probeFrame,
        FrameId frameId
    ) -> conformance::ObservationHost
    {
        auto const artifactRootHash =
            conformance::observeOnce(prepared.observation).artifactRootHash();
        auto installed = prepared.store.openInstalledRuntimeArtifact(
            1U,
            artifactRootHash
        );
        REQUIRE(installed.has_value());

        auto host       = std::make_unique<task::TaskHost>();
        auto generation = host->activateRuntimeArtifact(*std::move(installed));
        REQUIRE(generation.has_value());
        auto const fingerprint = conformance::declaredFingerprint(
            *host,
            *generation
        );
        return conformance::ObservationHost{
            .host    = std::move(host),
            .runtime = std::make_unique<conformance::ObservationRuntime>(
                probeFrame,
                fingerprint,
                frameId
            ),
            .generation = *generation,
        };
    }

    // A snapshot over the world as it now stands. A token references a
    // composition rather than a lease, so a reconciliation that advanced
    // ProjectState makes every earlier token stale, and a case that opens a
    // second Operation after a commit has to re-observe first.
    [[nodiscard]]
    inline auto freshSnapshot(PreparedStore& prepared) -> SnapshotRecord
    {
        auto snapshot = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            observeAgain(prepared)
        );
        REQUIRE(snapshot.has_value());
        return *std::move(snapshot);
    }

    [[nodiscard]]
    inline auto prepareStore(
        std::filesystem::path const& path,
        std::string const& pluginId = "fixture.control",
        std::string_view nextStepExpression = k_fixtureUiActionIntent
    ) -> PreparedStore
    {
        auto const release = runtimeRelease(path / "session-handoff");
        auto storeResult = OperatorCoordinator::open(path / "production");
        REQUIRE(storeResult.has_value());
        auto store = *std::move(storeResult);
        auto installed = store.installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());
        auto const artifactRootHash    = installed->rootHash();
        auto const installedGeneration = installed->installedGeneration();
        auto const source = pluginSource(pluginId, nextStepExpression);
        auto const project = makeProject(pluginId, source);
        auto const manifest = sessionManifest(
            project.registration,
            installed->rootHash(),
            hashOf("agent"),
            policyArtifactBytes()
        );
        auto const projectPlugin = loadPlugin(project, source);
        REQUIRE(store.registerProject(project.registration).has_value());
        REQUIRE(store.provisionProjectInstance(
            project.registration,
            projectPlugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-1",
                .sessionManifestHash = manifest.hash(),
                .entry = journalEntry(
                    project,
                    project.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
        REQUIRE(store.pinSession(
            SessionPin{
                .sessionId                 = "session-1",
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = project.registration.hash(),
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = "target-1",
                .projectInstanceKey        = "instance-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest,
            std::nullopt
        ).has_value());
        auto controller = store.bindController("session-1");
        REQUIRE(controller.has_value());
        auto lease = store.acquireLease(*controller);
        REQUIRE(lease.has_value());
        auto observation = conformance::activateObservationHost(
            *std::move(installed),
            umbraflowProbeFrame(),
            FrameId{101}
        );
        auto const reading = conformance::observeOnce(observation);
        conformance::requireResolvedSurface(reading, k_fixtureUiAction.surface);
        auto snapshot = store.createSnapshot(
            *lease,
            projectPlugin,
            project.toolCatalogSchemaOwner,
            reading
        );
        REQUIRE(snapshot.has_value());
        // "operator" is the exact operator protocol schema this fixture's
        // session manifest pins; the authority verifies the bytes rather than
        // the name. The RuntimeModel binding is this generation's own parse of
        // the artifact the manifest pins, and the authority verifies that too.
        auto runtimeModel = observation.host->runtimeModelBinding(
            observation.generation
        );
        REQUIRE(runtimeModel.has_value());
        auto planAuthority = conformance::planAuthority(
            project.registration,
            manifest,
            *runtimeModel,
            "operator",
            policyArtifactBytes(),
            k_fixtureUiAction
        );
        REQUIRE(planAuthority.has_value());
        return PreparedStore{
            .store                   = std::move(store),
            .plugin                  = projectPlugin,
            .project                 = project,
            .manifest                = manifest,
            .planAuthority           = *std::move(planAuthority),
            .controller              = *controller,
            .lease                   = *lease,
            .snapshot                = *std::move(snapshot),
            .observation             = std::move(observation),
            .runtimeArtifactRootHash = artifactRootHash,
            .installedGeneration     = installedGeneration,
        };
    }

    [[nodiscard]]
    inline auto command(
        SnapshotRecord const& snapshot,
        std::string clientRequestId
    ) -> CommandRequest
    {
        return CommandRequest{
            .snapshotToken        = snapshot.token,
            .idempotencyNamespace = "controller-1",
            .clientRequestId      = std::move(clientRequestId),
        };
    }

    // A second authenticated controller of another kind over the same
    // registration. It needs a ProjectInstance of its own because only one
    // write session per instance may be active; the controlled target is the
    // caller's choice, so a case can put two kinds on one target to watch them
    // contend, or on two targets to keep them independent.
    //
    // It returns a binding and nothing else: taking the lease and composing a
    // snapshot are the case's own steps, because whether the second controller
    // acquires a free target or seizes a held one is the property under test.
    //
    // budget is stated for exactly the kinds whose ControllerProfile requires
    // one, which is the same rule pinSession enforces: the fixture cannot mint
    // an Agent without ceilings or a Script with them.
    [[nodiscard]]
    inline auto addController(
        PreparedStore& prepared,
        ControllerKind kind,
        SessionMode mode,
        std::string const& sessionId,
        std::string const& projectInstanceKey,
        std::string const& controlledTargetId,
        std::optional<AgentBudget> const& budget = std::nullopt
    ) -> ControllerBinding
    {
        REQUIRE(prepared.store.provisionProjectInstance(
            prepared.project.registration,
            prepared.plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = projectInstanceKey,
                .eventId             = "baseline-" + projectInstanceKey,
                .sessionManifestHash = prepared.manifest.hash(),
                .entry               = journalEntry(
                    prepared.project,
                    prepared.project.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());

        auto profile  = std::optional<AgentProfile>{};
        auto manifest = prepared.manifest;
        if (budget)
        {
            auto const pinned = agentProfileFor(prepared, *budget);
            manifest = pinned.manifest;
            profile  = pinned.profile;
        }
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = sessionId,
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = prepared.project.registration.hash(),
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = controlledTargetId,
                .projectInstanceKey        = projectInstanceKey,
                .mode                      = mode,
                .kind                      = kind,
            },
            manifest,
            profile
        ).has_value());
        auto binding = prepared.store.bindController(sessionId);
        REQUIRE(binding.has_value());
        REQUIRE(binding->kind() == kind);
        return *binding;
    }

    [[nodiscard]]
    inline auto reconciliationOutcome(
        PreparedStore const& prepared,
        std::string operationId,
        std::string document
    ) -> ValidatedReconcileOutcome
    {
        return reconcileOutcome(
            prepared.project,
            prepared.plugin,
            std::move(operationId),
            std::move(document)
        );
    }

    [[nodiscard]]
    inline auto proposedOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string_view toolName
    ) -> StoredOperation
    {
        auto operation = prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, std::move(clientRequestId)),
            toolInvocation(prepared.project, std::string{toolName})
        );
        REQUIRE(operation.has_value());
        return operation->operation;
    }

    [[nodiscard]]
    inline auto freezePlanFor(
        PreparedStore& prepared,
        StoredOperation const& operation
    ) -> Result<FrozenPlan>
    {
        return prepared.store.freezePlan(
            operation.operationId,
            operation.revision,
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            prepared.planAuthority
        );
    }

    [[nodiscard]]
    inline auto mintStepFor(
        PreparedStore& prepared,
        StoredOperation const& operation
    ) -> Result<PlannedStep>
    {
        return prepared.store.mintNextStep(
            operation.operationId,
            operation.revision,
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            prepared.planAuthority
        );
    }

    // One Operation carried to the point a dispatch may be reserved: proposed,
    // plan frozen by the Operator, first step minted from the plugin's own
    // next_step. No caller states a hash anywhere along it.
    [[nodiscard]]
    inline auto createReadyOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string_view toolName
    ) -> StoredOperation
    {
        auto const proposed = proposedOperation(
            prepared,
            std::move(clientRequestId),
            toolName
        );
        auto const frozen = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());
        return step->operation;
    }

    // Drives one mutating Operation through a real dispatch to the reconciling
    // state, so a reconciliation contract starts where a reconciliation starts.
    //
    // The outcome is not a parameter any more and cannot be: only a TaskHost
    // mints a HostDeliveryReport, so the fixture asks for the delivery it wants
    // and reads back what the Host concluded. NotDelivered is reached by
    // presenting the Receipt to a context that does not hold its cycle, which
    // consumes it and posts nothing.
    [[nodiscard]]
    inline auto reconcilingOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        task::DeliveryOutcome expected
    ) -> StoredOperation
    {
        auto const authority = AuthorityDecisionId{"authority-" + clientRequestId};
        auto const ready     = createReadyOperation(
            prepared,
            std::move(clientRequestId),
            "command-1"
        );
        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            ready.operationId,
            ready.revision,
            prepared.lease,
            host->generation(),
            authority,
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        if (expected == task::DeliveryOutcome::TransportUnknown)
        {
            host->refuseClicks();
        }
        auto const report = expected == task::DeliveryOutcome::NotDelivered
            ? host->deliverIntoAnotherCycle(dispatch->authority)
            : host->deliverReport(dispatch->authority);
        REQUIRE(report.outcome() == expected);
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            report
        );
        REQUIRE(reconciling.has_value());
        REQUIRE(reconciling->state == OperationState::Reconciling);
        return *reconciling;
    }
}
