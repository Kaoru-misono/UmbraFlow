#pragma once

#include <operator/journal-entry.hpp>
#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/runtime-installation.hpp>
#include <operator/tool-invocation.hpp>

#include <task/page-model-file.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace uf::operator_runtime::test_support
{
    struct ProjectFixture final
    {
        VerifiedProjectRegistration   registration;
        ProjectSchemaOwner            schemaOwner;
        ProjectJournalSchemaOwner     journalSchemaOwner;
        ProjectToolCatalogSchemaOwner toolCatalogSchemaOwner;

        // The exact bytes the document validator last saw as a Reduce input.
        // The fixture records them because the property under test is that the
        // Operator decides those bytes and no caller can.
        std::shared_ptr<std::string>  lastReduceInput;
    };

    // Stands in for a project's reduce-input JSON Schema. It is structural
    // rather than an exact-bytes allowlist because the Operator now builds this
    // envelope from however many events a commit appends, and a real project's
    // schema would be structural too. The exact bytes are pinned separately, by
    // the test that asserts what the reducer was handed.
    [[nodiscard]]
    inline auto looksLikeReduceEnvelope(std::string_view exactJcs) -> bool
    {
        constexpr auto prefix = std::string_view{"{\"journal_events\":["};
        constexpr auto middle = std::string_view{"],\"prior_project_state\":"};
        if (!exactJcs.starts_with(prefix) || !exactJcs.ends_with('}'))
        {
            return false;
        }
        auto const middleAt = exactJcs.find(middle);
        if (middleAt == std::string_view::npos)
        {
            return false;
        }

        auto const priorState = exactJcs.substr(
            middleAt + middle.size(),
            exactJcs.size() - middleAt - middle.size() - 1U
        );
        if (priorState != "null" && priorState != "{\"revision\":0}")
        {
            return false;
        }

        auto events = exactJcs.substr(prefix.size(), middleAt - prefix.size());
        while (!events.empty())
        {
            constexpr auto eventPrefix =
                std::string_view{"{\"namespaced_event_type\":\""};
            constexpr auto eventSuffix =
                std::string_view{",\"provenance\":{\"kind\":\"fixture\"}}"};
            if (!events.starts_with(eventPrefix))
            {
                return false;
            }
            auto const suffixAt = events.find(eventSuffix);
            if (
                suffixAt == std::string_view::npos
                || events.substr(0U, suffixAt)
                        .find("\",\"opaque_project_payload\":")
                    == std::string_view::npos
            )
            {
                return false;
            }
            events.remove_prefix(suffixAt + eventSuffix.size());
            if (events.starts_with(','))
            {
                events.remove_prefix(1U);
                continue;
            }
            if (!events.empty())
            {
                return false;
            }
        }
        return true;
    }

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
        auto const schemaHash = hashOf("registration-schema");
        auto const pluginHash = hashOf(pluginBytes);
        auto const toolCatalogHash = hashOf("catalogue");
        auto const stateSchemaHash = hashOf("state");
        auto const observationSchemaHash = hashOf("observation");
        auto const preconditionSchemaHash = hashOf("precondition");
        auto const reconcileSchemaHash = hashOf("reconcile");
        auto const journalSchemaHash = hashOf("journal");
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
            [exactJcs, claims](
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

        auto lastReduceInput = std::make_shared<std::string>();
        auto schemaOwner = ProjectSchemaOwner::create(
            *registration,
            [](std::string_view exactJcs) -> Status
            {
                constexpr auto accepted = std::array{
                    std::string_view{"{}"},
                    std::string_view{"{\"disposition\":\"confirmed\"}"},
                    std::string_view{"{\"disposition\":\"continue\"}"},
                    std::string_view{"{\"journal_events\":[],\"prior_project_state\":null}"},
                    std::string_view{"{\"journal_events\":[],\"prior_project_state\":{\"revision\":0}}"},
                    std::string_view{"{\"kind\":\"baseline\"}"},
                    std::string_view{"{\"kind\":\"forged\"}"},
                    std::string_view{"{\"kind\":\"fixture\"}"},
                    std::string_view{"{\"revision\":0}"},
                    std::string_view{"{\"value\":1}"},
                    std::string_view{"{\"value\":2}"},
                    std::string_view{"{\"value\":3}"},
                    std::string_view{"{\"value\":99}"},
                };
                if (
                    std::ranges::find(accepted, exactJcs) == accepted.end()
                    && !looksLikeReduceEnvelope(exactJcs)
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture canonical validator rejected bytes"
                    );
                }
                return ok();
            },
            [lastReduceInput](ProjectPluginFunction function,
               ProjectDocumentDirection direction,
               std::string_view exactJcs) -> Status
            {
                auto valid = false;
                if (direction == ProjectDocumentDirection::Input)
                {
                    switch (function)
                    {
                    case ProjectPluginFunction::Reduce:
                        *lastReduceInput = std::string{exactJcs};
                        valid = looksLikeReduceEnvelope(exactJcs);
                        break;
                    case ProjectPluginFunction::Reconcile:
                        valid = exactJcs == "{\"disposition\":\"continue\"}"
                            || exactJcs == "{\"disposition\":\"confirmed\"}";
                        break;
                    case ProjectPluginFunction::Derive:
                    case ProjectPluginFunction::Plan:
                    case ProjectPluginFunction::NextStep:
                        valid = exactJcs == "{}";
                        break;
                    }
                }
                else
                {
                    switch (function)
                    {
                    case ProjectPluginFunction::Reduce:
                        valid = exactJcs == "{\"revision\":0}";
                        break;
                    case ProjectPluginFunction::Reconcile:
                        valid = exactJcs == "{\"disposition\":\"continue\"}"
                            || exactJcs == "{\"disposition\":\"confirmed\"}";
                        break;
                    case ProjectPluginFunction::Derive:
                    case ProjectPluginFunction::Plan:
                    case ProjectPluginFunction::NextStep:
                        valid = exactJcs == "{}";
                        break;
                    }
                }
                if (!valid)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture function-specific schema rejected document"
                    );
                }
                return ok();
            }
        );
        REQUIRE(schemaOwner.has_value());

        auto journalSchemaOwner = ProjectJournalSchemaOwner::create(
            *registration,
            [](std::string_view eventType,
               std::string_view payload) -> Result<ContentHash>
            {
                struct PayloadCase final
                {
                    std::string_view eventType{};
                    std::string_view payload{};
                    std::string_view schemaIdentity{};
                };
                constexpr auto cases = std::array{
                    PayloadCase{
                        .eventType      = "fixture.baseline",
                        .payload        = "{\"kind\":\"baseline\"}",
                        .schemaIdentity = "baseline-schema",
                    },
                    PayloadCase{
                        .eventType      = "fixture.progress",
                        .payload        = "{\"value\":1}",
                        .schemaIdentity = "progress-schema",
                    },
                    PayloadCase{
                        .eventType      = "fixture.stale",
                        .payload        = "{\"value\":2}",
                        .schemaIdentity = "stale-schema",
                    },
                    PayloadCase{
                        .eventType      = "fixture.confirmed",
                        .payload        = "{\"value\":1}",
                        .schemaIdentity = "confirmed-schema",
                    },
                    PayloadCase{
                        .eventType      = "fixture.confirmed",
                        .payload        = "{\"value\":2}",
                        .schemaIdentity = "confirmed-schema",
                    },
                    PayloadCase{
                        .eventType      = "fixture.duplicate",
                        .payload        = "{\"value\":3}",
                        .schemaIdentity = "duplicate-schema",
                    },
                };
                auto const found = std::ranges::find_if(
                    cases,
                    [eventType, payload](PayloadCase const& candidate)
                    {
                        return candidate.eventType == eventType
                            && candidate.payload == payload;
                    }
                );
                if (found == cases.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture event-specific payload schema rejected document"
                    );
                }
                return hashOf(found->schemaIdentity);
            },
            [](std::string_view provenance) -> Status
            {
                if (provenance != "{\"kind\":\"fixture\"}")
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture JournalProvenance schema rejected document"
                    );
                }
                return ok();
            }
        );
        REQUIRE(journalSchemaOwner.has_value());

        auto toolCatalogSchemaOwner = ProjectToolCatalogSchemaOwner::create(
            *registration,
            [](std::string_view toolName,
               std::string_view exactArgsJcs) -> Result<ToolDescriptor>
            {
                struct ToolCase final
                {
                    std::string_view name{};
                    std::string_view version{};
                    ToolMutability   mutability{ToolMutability::Mutating};
                };
                constexpr auto catalog = std::array{
                    ToolCase{
                        .name       = "command-1",
                        .version    = "1",
                        .mutability = ToolMutability::Mutating,
                    },
                    ToolCase{
                        .name       = "command-2",
                        .version    = "1",
                        .mutability = ToolMutability::Mutating,
                    },
                    ToolCase{
                        .name       = "different-command",
                        .version    = "1",
                        .mutability = ToolMutability::Mutating,
                    },
                    ToolCase{
                        .name       = "observe-1",
                        .version    = "1",
                        .mutability = ToolMutability::ReadOnly,
                    },
                };
                auto const found = std::ranges::find_if(
                    catalog,
                    [toolName](ToolCase const& candidate)
                    {
                        return candidate.name == toolName;
                    }
                );
                if (found == catalog.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture Tool Catalog has no such tool"
                    );
                }
                if (exactArgsJcs != "{\"value\":1}")
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture tool argument schema rejected document"
                    );
                }
                return ToolDescriptor{
                    .toolVersion = std::string{found->version},
                    .mutability  = found->mutability,
                };
            }
        );
        REQUIRE(toolCatalogSchemaOwner.has_value());

        return ProjectFixture{
            .registration           = *registration,
            .schemaOwner            = *schemaOwner,
            .journalSchemaOwner     = *journalSchemaOwner,
            .toolCatalogSchemaOwner = *toolCatalogSchemaOwner,
            .lastReduceInput        = std::move(lastReduceInput),
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
        std::string provenance = "{\"kind\":\"fixture\"}"
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

    [[nodiscard]]
    inline auto sessionManifest(
        VerifiedProjectRegistration const& project,
        ContentHash const& runtimeArtifactRootHash
    ) -> SessionManifest
    {
        auto const result = SessionManifest::create(
            SessionManifestSpec{
                .hostProtocolSchemaHash       = hashOf("host"),
                .runtimeModelSchemaHash       = hashOf("runtime-schema"),
                .runtimeModelArtifactRootHash = runtimeArtifactRootHash,
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = project.hash(),
                .policyArtifactHash           = hashOf("policy"),
                .journalEnvelopeSchemaHash    = hashOf("journal-envelope"),
                .agentProfileHash             = hashOf("agent"),
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

    struct RuntimeRelease final
    {
        std::filesystem::path handoffRoot;
        ContentHash           releaseManifestHash;
        ContentHash           artifactRootHash;
    };

    [[nodiscard]]
    inline auto runtimeRelease(std::filesystem::path const& root) -> RuntimeRelease
    {
        auto const handoff = root / "release";
        auto const artifact = handoff / "runtime-artifact";
        auto const model = std::string_view{"not TOML\r\n"};
        writeFile(artifact / task::k_runtimeModelFileName, model);
        auto const manifest = std::format(
            "{{\"assets\":[],\"manifest_schema_hash\":\"{}\","
            "\"page_model\":{{\"path\":\"page-model.toml\",\"sha256\":\"{}\","
            "\"size\":{}}},\"runtime_model_schema_hash\":\"{}\"}}",
            task::k_runtimeArtifactSchemaHash,
            hashOf(model).hex(),
            model.size(),
            task::k_runtimeModelSchemaHash
        );
        writeFile(artifact / task::k_runtimeArtifactManifestFileName, manifest);
        auto const artifactRootHash = hashOf(manifest);
        auto const releaseManifest = std::format(
            "{{\"annotation_workspace_schema_hash\":\"{}\","
            "\"candidate_id\":\"candidate-1\",\"candidate_revision\":1,"
            "\"generation\":1,\"predecessor_publication_id\":null,"
            "\"replay_gate_hash\":\"{}\",\"runtime_artifact_root_hash\":\"{}\","
            "\"workspace_sqlite_schema_hash\":\"{}\"}}",
            detail::k_annotationWorkspaceSchemaHash,
            hashOf("replay-gate").hex(),
            artifactRootHash.hex(),
            hashOf("workspace-schema").hex()
        );
        writeFile(handoff / "release.manifest.json", releaseManifest);
        return RuntimeRelease{
            .handoffRoot         = handoff,
            .releaseManifestHash = hashOf(releaseManifest),
            .artifactRootHash    = artifactRootHash,
        };
    }
}
