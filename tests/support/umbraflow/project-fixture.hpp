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
#include <operator/project-generation.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/runtime-installation.hpp>
#include <operator/tool-invocation.hpp>

#include <script/scoped-tool-program.hpp>

#include <json/value.hpp>

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
#include <stop_token>
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
// directory contains and that exists to give the ledger two resource identities to
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
            R"({{"assets":[{}],"page_model":{},)"
            R"("runtime_artifact_format":{},"runtime_model_format":{}}})",
            assetJson,
            artifactManifestRow(modelFile),
            task::k_runtimeArtifactFormat,
            task::k_runtimeModelFormat
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
            R"({{"annotation_workspace_format":{},)"
            R"("candidate_id":"candidate-1","candidate_revision":1,)"
            R"("generation":1,"predecessor_publication_id":null,)"
            R"("replay_gate_hash":"{}","runtime_artifact_root_hash":"{}",)"
            R"("workspace_sqlite_revision":{}}})",
            detail::k_annotationWorkspaceFormat,
            observationHash("replay-gate").hex(),
            artifactRootHash.hex(),
            detail::k_workspaceSqliteRevision
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
        // The verified two-closure document, and the projection of it every
        // durable seam takes. Both, because they answer different questions: a
        // loader is handed the document -- it compiles the closures the
        // document pinned -- while the ledger, the schema owners and the
        // session manifest are handed the identity, which is all that outlives
        // the document.
        VerifiedProjectGeneration     generation;
        ProjectIdentity               registration;
        ProjectSchemaOwner            schemaOwner;
        ProjectJournalSchemaOwner     journalSchemaOwner;
        ProjectToolCatalogSchemaOwner toolCatalogSchemaOwner;
        ProjectReconcileSchemaOwner   reconcileSchemaOwner;

        // The observed-instance identity authority every snapshot of this
        // project is composed under. It is built once with the fixture because
        // it is bound to the registration like the schema owners are: a
        // snapshot cannot pick its own authority, and neither may a case.
        ObservedInstanceIdentitySchemas observedInstanceIdentitySchemas;

        // The exact Tool Catalog bytes this registration pinned. A case that
        // builds a second catalog owner over the same registration needs them,
        // because such an owner is bound to their hash.
        std::string toolCatalogBytes;

        // The exact bytes the document validator last saw as a Reduce or Derive
        // input. The synchronized log is shared with the retained validator
        // because the property under test is that the Operator decides those
        // bytes and no caller can.
        std::shared_ptr<deployment::ProjectDocumentInputLog> documentInputLog;

        // One of this project's catalog names, from the local half a case
        // knows it by. A fixture tool's namespace is this registration's
        // plugin_id, so a case that spelled the full name itself would be
        // writing down which registration it prepared.
        [[nodiscard]]
        auto toolName(std::string_view localName) const -> std::string
        {
            return fixtureToolName(registration.pluginId(), localName);
        }
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

    [[nodiscard]]
    inline auto hashOf(std::string_view value) -> ContentHash
    {
        auto const result = sha256(std::as_bytes(std::span{value}));
        REQUIRE(result.has_value());
        return *result;
    }

    // The one identity schema this fixture's deployments pin, by the $id its
    // document declares and every instance proposal this fixture's plugins
    // write names. Spelled once: the C++ validator, the registration closure
    // and the proposal envelope in the plugin source all say the same string.
    inline constexpr auto k_fixtureIdentitySchemaId = std::string_view{
        "https://fixture.example/identity/overlay/v1"
    };

    // The observed-instance identity authority this fixture's registrations
    // pin: one schema, the sha256 of k_observedIdentitySchema's bytes, and the
    // hand-written validator that states that document's constraint in C++.
    // It is shared by every case that composes or mints an observation, so the
    // constraint is spelled once rather than once per test file.
    [[nodiscard]]
    inline auto observedInstanceIdentitySchemas(
        ProjectIdentity const& registration
    ) -> ObservedInstanceIdentitySchemas
    {
        auto schemas = ObservedInstanceIdentitySchemas::create(
            registration,
            {
                ObservedInstanceIdentitySchema{
                    .schemaId   = std::string{k_fixtureIdentitySchemaId},
                    .schemaHash = schemaHash(k_observedIdentitySchema),
                    .validate   = [](json::Value const& basis) -> Status
                    {
                        auto const nativeId = basis.find("native_id");
                        auto const epoch    = basis.find("surface_epoch");
                        if (
                            basis.kind() != json::ValueKind::Object
                            || basis.members().size() != 2U
                            || nativeId == nullptr
                            || nativeId->kind() != json::ValueKind::String
                            || nativeId->string().empty()
                            || epoch == nullptr
                            || epoch->kind() != json::ValueKind::Number
                            || !epoch->isInteger()
                            || epoch->number() < 0.0
                        )
                        {
                            return fail(
                                AutomationErrorKind::InvalidResource,
                                "fixture observed-instance basis violates its schema"
                            );
                        }
                        return ok();
                    },
                },
            }
        );
        REQUIRE(schemas.has_value());
        return *std::move(schemas);
    }

    // The Tool binding table a fixture project declares, rendered as the
    // registration's own member. It renders what it is given and sorts nothing:
    // fixtureToolBindings below puts the rows in the one order the claims check
    // accepts, and the claims makeProject hands out carry that same order, so
    // the sort happens once where both readings are minted.
    [[nodiscard]]
    inline auto toolBindingsJcs(
        std::span<ProjectToolBinding const> bindings
    ) -> std::string
    {
        auto rendered = std::string{"["};
        for (auto index = std::size_t{0}; index < bindings.size(); ++index)
        {
            rendered += index == 0U ? "" : ",";
            rendered += std::format(
                R"({{"entry_point":"{}","tool_name":"{}"}})",
                bindings[index].entryPoint,
                bindings[index].toolName
            );
        }
        rendered += "]";
        return rendered;
    }

    // One closure slot of the two-closure registration document.
    [[nodiscard]]
    inline auto closureJcs(
        ContentHash const& moduleManifestHash,
        std::span<std::string const> exportedEntryPoints
    ) -> std::string
    {
        auto rendered = std::string{R"({"exported_entry_points":[)"};
        for (
            auto index = std::size_t{0};
            index < exportedEntryPoints.size();
            ++index
        )
        {
            rendered += index == 0U ? "" : ",";
            rendered += std::format(R"("{}")", exportedEntryPoints[index]);
        }
        rendered += std::format(
            R"(],"module_manifest_hash":"{}"}})",
            moduleManifestHash.hex()
        );
        return rendered;
    }

    // Every fixture closure is one module named `main`, so the blob list and
    // the digest taken over it are derived from the bytes rather than spelled
    // per closure.
    [[nodiscard]]
    inline auto closureModules(std::string_view source)
        -> std::vector<ProjectModuleBlob>
    {
        return {
            ProjectModuleBlob{
                .name   = "main",
                .source = std::string{source},
            },
        };
    }

    [[nodiscard]]
    inline auto closureManifestHash(std::string_view source) -> ContentHash
    {
        auto const hash = derivePluginModuleManifestHash(
            "main",
            closureModules(source)
        );
        REQUIRE(hash.has_value());
        return *hash;
    }

    // Every entry this fixture's tool closure exports, in the JCS order a
    // closure declaration states them in. The entry a Tool binds to is its
    // local name: a Tool's namespace is its registrant's, so the local half is
    // already unique inside one closure and needs no second spelling.
    [[nodiscard]]
    inline auto fixtureToolEntryPoints() -> std::vector<std::string>
    {
        auto entries = std::vector<std::string>{};
        entries.reserve(k_toolSources.size());
        for (auto const& tool : k_toolSources)
        {
            entries.emplace_back(tool.localName);
        }
        std::ranges::sort(entries);
        return entries;
    }

    // The binding table this fixture's registration states: one row per Tool
    // its pinned catalog declares. It is derived rather than passed in because
    // a generation is admitted only when the table covers exactly the Tools the
    // catalog declares, and this fixture's catalog is k_toolSources.
    [[nodiscard]]
    inline auto fixtureToolBindings(
        std::string_view pluginId
    ) -> std::vector<ProjectToolBinding>
    {
        auto bindings = std::vector<ProjectToolBinding>{};
        bindings.reserve(k_toolSources.size());
        for (auto const& tool : k_toolSources)
        {
            bindings.emplace_back(ProjectToolBinding{
                .toolName   = fixtureToolName(pluginId, tool.localName),
                .entryPoint = std::string{tool.localName},
            });
        }
        std::ranges::sort(bindings, {}, &ProjectToolBinding::toolName);
        return bindings;
    }

    // The tool closure this fixture ships: one entry per bound Tool and
    // nothing else. Nothing in tests/operator dispatches through it -- the Tool
    // Runtime's own suites compile their own closures against a live seam --
    // so each entry answers with its own name, which is all the bridge needs to
    // admit the closure against the entry set the registration declared.
    [[nodiscard]]
    inline auto toolClosureSource(std::string_view pluginId) -> std::string
    {
        auto source = std::string{"return {\n    plugin_id = \""};
        source += pluginId;
        source += "\",\n";
        for (auto const& entry : fixtureToolEntryPoints())
        {
            source += "    [\"";
            source += entry;
            source += "\"] = function(_input) return { entry = \"";
            source += entry;
            source += "\" } end,\n";
        }
        source += "}\n";
        return source;
    }

    // `observationSchema` and `preconditionSchema` default to the exemplar's;
    // a case that pins a laxer one -- e.g. a project whose tool arguments admit
    // the observed_instance_id the submitCommand gate resolves -- states it
    // explicitly. Every hash and every validator must see the same bytes.
    //
    // `reducerBytes` is the caller's, because what the fold answers is what a
    // case varies. The tool closure is this fixture's own and is derived from
    // the catalog above.
    [[nodiscard]]
    inline auto makeProject(
        std::string pluginId,
        std::string_view reducerBytes,
        std::string_view observationSchema = k_projectObservationSchema,
        std::string_view preconditionSchema = k_toolPreconditionSchema,
        std::optional<ContentHash> environmentOverride = std::nullopt
    ) -> ProjectFixture
    {
        auto const bundle = DeploymentBundle{pluginId};
        auto sources               = bundle.sources();
        sources.projectObservation = observationSchema;
        auto const deployed = deployment::ProjectDeployment::create(sources);
        {
            auto const why = deployed.has_value()
                ? std::string{}
                : std::string{deployed.error().message()};
            INFO(why);
            REQUIRE(deployed.has_value());
        }

        auto const reducerManifestHash = closureManifestHash(reducerBytes);
        auto const toolManifestHash    = closureManifestHash(
            toolClosureSource(pluginId)
        );
        auto environmentHash = currentProjectPluginEnvironmentHash();
        REQUIRE(environmentHash.has_value());
        if (environmentOverride)
        {
            environmentHash = *environmentOverride;
        }
        auto const toolCatalogHash        = hashOf(bundle.toolCatalog());
        auto const stateSchemaHash        = hashOf(k_projectStateSchema);
        auto const observationSchemaHash  = hashOf(observationSchema);
        auto const preconditionSchemaHash = hashOf(preconditionSchema);
        auto const reconcileSchemaHash    = hashOf(bundle.reconcileManifest());
        auto const journalSchemaHash      = hashOf(bundle.journalEventManifest());
        auto const bindings               = fixtureToolBindings(pluginId);
        auto const toolEntryPoints        = fixtureToolEntryPoints();
        auto const reducerEntryPoints     = std::vector<std::string>{
            std::string{k_reducerEntryPoint},
        };
        auto const exactJcs = std::format(
            "{{\"baseline_event_type\":\"fixture.baseline\","
            "\"journal_event_schema_manifest_hash\":\"{}\","
            "\"observed_instance_identity_schema_hashes\":[\"{}\"],"
            "\"plugin_environment_hash\":\"{}\","
            "\"plugin_id\":\"{}\","
            "\"project_observation_schema_hash\":\"{}\","
            "\"project_registration_format\":{},"
            "\"project_resources\":[],"
            "\"project_state_schema_hash\":\"{}\","
            "\"project_tool_bindings\":{},"
            "\"project_tool_precondition_schema_hash\":\"{}\","
            "\"reconcile_payload_schema_manifest_hash\":\"{}\","
            "\"reducer_closure\":{},"
            "\"tool_catalog_hash\":\"{}\","
            "\"tool_closure\":{}}}",
            journalSchemaHash.hex(),
            hashOf(k_observedIdentitySchema).hex(),
            environmentHash->hex(),
            pluginId,
            observationSchemaHash.hex(),
            k_projectGenerationFormat,
            stateSchemaHash.hex(),
            toolBindingsJcs(bindings),
            preconditionSchemaHash.hex(),
            reconcileSchemaHash.hex(),
            closureJcs(reducerManifestHash, reducerEntryPoints),
            toolCatalogHash.hex(),
            closureJcs(toolManifestHash, toolEntryPoints)
        );
        auto const claims = ProjectGenerationClaims{
            .projectRegistrationFormat = k_projectGenerationFormat,
            .pluginId                  = pluginId,
            .reducerClosure            = ProjectClosureClaims{
                .moduleManifestHash  = reducerManifestHash,
                .exportedEntryPoints = reducerEntryPoints,
            },
            .toolClosure = ProjectClosureClaims{
                .moduleManifestHash  = toolManifestHash,
                .exportedEntryPoints = toolEntryPoints,
            },
            .pluginEnvironmentHash                = *environmentHash,
            .toolCatalogHash                      = toolCatalogHash,
            .projectStateSchemaHash               = stateSchemaHash,
            .projectObservationSchemaHash         = observationSchemaHash,
            .projectToolPreconditionSchemaHash    = preconditionSchemaHash,
            .reconcilePayloadSchemaManifestHash   = reconcileSchemaHash,
            .journalEventSchemaManifestHash       = journalSchemaHash,
            .baselineEventType                    = "fixture.baseline",
            .projectResources                     = {},
            .observedInstanceIdentitySchemaHashes = {hashOf(k_observedIdentitySchema)},
            .projectToolBindings                  = bindings,
        };
        auto registration = ProjectGeneration::verifyExact(
            exactJcs,
            hashOf(exactJcs),
            // Init-captures rather than [exactJcs, claims]: both locals are
            // const, and capturing a const entity by name gives the closure a
            // const member its move constructor must copy rather than move.
            [exactJcs = exactJcs, claims = claims](
                std::string_view candidate
            ) -> Result<ProjectGenerationClaims>
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
        {
            auto const why = registration.has_value()
                ? std::string{}
                : std::string{registration.error().message()};
            INFO(why);
            REQUIRE(registration.has_value());
        }

        auto documentInputLog =
            std::make_shared<deployment::ProjectDocumentInputLog>();
        auto schemaOwner = ProjectSchemaOwner::create(
            *registration,
            ProjectDocumentSchemaBytes{
                .projectState       = k_projectStateSchema,
                .projectObservation = observationSchema,
                .toolPrecondition   = preconditionSchema,
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
            .generation             = *registration,
            .registration           = *registration,
            .schemaOwner            = *schemaOwner,
            .journalSchemaOwner     = *journalSchemaOwner,
            .toolCatalogSchemaOwner = *toolCatalogSchemaOwner,
            .reconcileSchemaOwner   = *reconcileSchemaOwner,
            .observedInstanceIdentitySchemas = observedInstanceIdentitySchemas(
                *registration
            ),
            .toolCatalogBytes = bundle.toolCatalog(),
            .documentInputLog = std::move(documentInputLog),
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
    inline auto routineToolEffect(
        ProjectFixture const& project,
        std::string toolName
    ) -> ProposedEffect
    {
        auto invocation = toolInvocation(project, std::move(toolName));
        REQUIRE_FALSE(invocation.descriptor().effectBounds.empty());
        auto const& bound = invocation.descriptor().effectBounds.front();
        return ProposedEffect{
            .namespacedType    = bound.namespacedType,
            .risk              = Risk::Low,
            .scopeKind         = bound.scopeKind,
            .scopeKey          = "fixture-instance",
            .payloadSchemaHash = bound.payloadSchemaHash,
            .opaqueProjectPayload = R"({"value":1})",
        };
    }

    // The same effect from the ordinary mutating tool, for the many cases that
    // are about the effect rather than about which tool proposed it. It is an
    // overload rather than a default argument because the name is composed from
    // this very project's namespace, which a default argument cannot see.
    [[nodiscard]]
    inline auto routineToolEffect(ProjectFixture const& project) -> ProposedEffect
    {
        return routineToolEffect(project, project.toolName("command-1"));
    }

    // The Tool Runtime seam this fixture's tool closure is compiled against. It
    // refuses every call: a scoped program is required to hold a seam -- one
    // without it is a pure program wearing the wrong type -- and a registration
    // loaded so that an instance can be provisioned from its fold holds no
    // lease, controller or observation authority for a call to be admitted
    // under. It is one value and never a branch on anything.
    [[nodiscard]]
    inline auto refusingToolRuntime() -> script::ToolRuntimeInvoke
    {
        return [](
                   std::string_view,
                   json::Value const&,
                   script::ToolCallCoordinate const&,
                   std::stop_token
               ) -> Result<json::Value>
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "this fixture registration dispatches no Tool call"
            );
        };
    }

    // The loaded generation both closures of this fixture's registration were
    // compiled into. Provisioning needs it because a ProjectInstance row
    // carries the reduction of its complete Journal prefix, and only a loaded
    // generation can perform that fold.
    [[nodiscard]]
    inline auto loadGeneration(
        ProjectFixture const& project,
        std::string_view reducerBytes
    ) -> ProjectGenerationHandle
    {
        auto registrar = ProjectGenerationRegistrar{};
        auto result = registrar.registerGeneration(
            project.generation,
            project.toolCatalogSchemaOwner,
            project.schemaOwner,
            ProjectGenerationRegistrar::ClosureModules{
                .entryModule = "main",
                .modules     = closureModules(reducerBytes),
            },
            ProjectGenerationRegistrar::ClosureModules{
                .entryModule = "main",
                .modules     = closureModules(
                    toolClosureSource(project.registration.pluginId())
                ),
            },
            {},
            [](std::string_view, std::string_view) -> Status { return ok(); },
            refusingToolRuntime()
        );
        {
            auto const why = result.has_value()
                ? std::string{}
                : std::string{result.error().message()};
            INFO(why);
            REQUIRE(result.has_value());
        }
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
        ProjectIdentity const& project,
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
        return R"toml(schema_version = 3
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

    // The reducer closure a prepared store registers. plugin_id must equal the
    // registration's, so the id is inserted rather than fixed.
    //
    // The fold is the whole of the pure type's contract now: it reads the
    // Journal prefix the Operator assembled and answers a ProjectState the
    // pinned schema accepts. `fixture.confirmed` in that prefix is what moves
    // the revision, so a case makes the fold answer differently by writing an
    // entry rather than by substituting an expression.
    [[nodiscard]]
    inline auto reducerSource(std::string_view pluginId) -> std::string
    {
        auto source = std::string{"return {\n    plugin_id = \""};
        source += pluginId;
        source += R"LUAU(",
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
        OperatorCoordinator     store;
        ProjectGenerationHandle generation;
        ProjectFixture          project;
        SessionManifest         manifest;
        OperatorPlanAuthority   planAuthority;

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
            prepared.project.registration,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            observeAgain(prepared)
        );
        REQUIRE(snapshot.has_value());
        return *std::move(snapshot);
    }

    [[nodiscard]]
    inline auto prepareStore(
        std::filesystem::path const& path,
        std::string const& pluginId = "fixture.control"
    ) -> PreparedStore
    {
        auto const release = runtimeRelease(path / "session-handoff");
        auto storeResult = OperatorCoordinator::open(path / "production");
        auto const storeMessage = storeResult.has_value()
            ? std::string{}
            : storeResult.error().message();
        REQUIRE_MESSAGE(storeResult.has_value(), storeMessage);
        auto store = *std::move(storeResult);
        auto installed = store.installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        auto const installMessage = installed.has_value()
                                      ? std::string{}
                                      : std::string{installed.error().message()};
        REQUIRE_MESSAGE(installed.has_value(), installMessage);
        auto const artifactRootHash    = installed->rootHash();
        auto const installedGeneration = installed->installedGeneration();
        auto const source  = reducerSource(pluginId);
        auto const project = makeProject(pluginId, source);
        auto const manifest = sessionManifest(
            project.registration,
            installed->rootHash(),
            hashOf("agent"),
            policyArtifactBytes()
        );
        auto const generation = loadGeneration(project, source);
        REQUIRE(store.registerProject(project.registration).has_value());
        REQUIRE(store.provisionProjectInstance(
            project.registration,
            generation,
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
        auto const worldScope = ObservedInstanceWorldScope::run(
            "target-1",
            1
        );
        REQUIRE(worldScope.has_value());
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
                .worldScope                = *worldScope,
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
            project.registration,
            project.toolCatalogSchemaOwner,
            project.observedInstanceIdentitySchemas,
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
        auto planAuthority = OperatorPlanAuthority::create(
            project.registration,
            manifest,
            *runtimeModel,
            "operator",
            policyArtifactBytes()
        );
        REQUIRE(planAuthority.has_value());
        return PreparedStore{
            .store                   = std::move(store),
            .generation              = generation,
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
        std::optional<AgentBudget> const& budget = std::nullopt,
        std::string controllerId = "controller-1",
        std::vector<std::string> controllerCapabilities = {
            std::string{conformance::k_operateCapability},
        }
    ) -> ControllerBinding
    {
        REQUIRE(prepared.store.provisionProjectInstance(
            prepared.project.registration,
            prepared.generation,
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
        auto const sessionWorldScope = ObservedInstanceWorldScope::run(
            controlledTargetId,
            1
        );
        REQUIRE(sessionWorldScope.has_value());
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = sessionId,
                .authenticatedControllerId = controllerId,
                .idempotencyNamespace      = controllerId,
                .projectRegistrationHash   = prepared.project.registration.hash(),
                .controllerCapabilities    = std::move(controllerCapabilities),
                .controlledTargetId        = controlledTargetId,
                .projectInstanceKey        = projectInstanceKey,
                .mode                      = mode,
                .kind                      = kind,
                .worldScope                = *sessionWorldScope,
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
}
