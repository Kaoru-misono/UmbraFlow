#pragma once

#include <operator-contract/observation-fixture.hpp>

#include <operator/journal-entry.hpp>
#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/runtime-installation.hpp>
#include <operator/tool-invocation.hpp>

#include <task/page-model-file.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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

namespace uf::operator_runtime::test_support
{
    struct ProjectFixture final
    {
        VerifiedProjectRegistration   registration;
        ProjectSchemaOwner            schemaOwner;
        ProjectJournalSchemaOwner     journalSchemaOwner;
        ProjectToolCatalogSchemaOwner toolCatalogSchemaOwner;
        ProjectReconcileSchemaOwner   reconcileSchemaOwner;

        // The exact bytes the document validator last saw as a Reduce input,
        // and as a Derive input. The fixture records them because the property
        // under test is that the Operator decides those bytes and no caller
        // can.
        std::shared_ptr<std::string> lastReduceInput;
        std::shared_ptr<std::string> lastDeriveInput;
    };

    // Stands in for a project's derive-input JSON Schema. Structural for
    // looksLikeReduceEnvelope's reason: the Operator builds this envelope from
    // whatever the world currently holds, and a real project's schema would be
    // structural too.
    [[nodiscard]]
    inline auto looksLikeDeriveEnvelope(std::string_view exactJcs) -> bool
    {
        constexpr auto members = std::array{
            std::string_view{"{\"pending_operation_transition\":"},
            std::string_view{",\"pinned_project_artifact_identities\":["},
            std::string_view{",\"prior_project_observation\":"},
            std::string_view{",\"project_state\":"},
            std::string_view{",\"ui_snapshot\":"},
        };
        if (!exactJcs.starts_with(members.front()) || !exactJcs.ends_with('}'))
        {
            return false;
        }
        auto at = std::size_t{0};
        for (auto const member : members)
        {
            auto const found = exactJcs.find(member, at);
            if (found == std::string_view::npos)
            {
                return false;
            }
            at = found + member.size();
        }
        return true;
    }

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
        auto lastDeriveInput = std::make_shared<std::string>();
        auto schemaOwner = ProjectSchemaOwner::create(
            *registration,
            ProjectDocumentSchemaBytes{
                .projectState       = "state",
                .projectObservation = "observation",
                .toolPrecondition   = "precondition",
            },
            [](std::string_view candidateJcs) -> Status
            {
                constexpr auto accepted = std::array{
                    std::string_view{"{}"},
                    std::string_view{"{\"disposition\":\"ambiguous\"}"},
                    std::string_view{"{\"disposition\":\"confirmed\"}"},
                    std::string_view{"{\"disposition\":\"continue\"}"},
                    std::string_view{"{\"disposition\":\"diverged\"}"},
                    std::string_view{"{\"disposition\":\"rejected\"}"},
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
                    std::ranges::find(accepted, candidateJcs) == accepted.end()
                    && !looksLikeReduceEnvelope(candidateJcs)
                    && !looksLikeDeriveEnvelope(candidateJcs)
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture canonical validator rejected bytes"
                    );
                }
                return ok();
            },
            [lastReduceInput, lastDeriveInput](ProjectPluginFunction function,
               ProjectDocumentDirection direction,
               std::string_view candidateJcs) -> Status
            {
                auto valid = false;
                if (direction == ProjectDocumentDirection::Input)
                {
                    switch (function)
                    {
                    case ProjectPluginFunction::Reduce:
                        *lastReduceInput = std::string{candidateJcs};
                        valid = looksLikeReduceEnvelope(candidateJcs);
                        break;
                    case ProjectPluginFunction::Reconcile:
                        valid = candidateJcs.starts_with("{\"disposition\":\"")
                            && candidateJcs.ends_with("\"}");
                        break;
                    case ProjectPluginFunction::Derive:
                        *lastDeriveInput = std::string{candidateJcs};
                        valid = looksLikeDeriveEnvelope(candidateJcs);
                        break;
                    case ProjectPluginFunction::Plan:
                    case ProjectPluginFunction::NextStep:
                        valid = candidateJcs == "{}";
                        break;
                    }
                }
                else
                {
                    switch (function)
                    {
                    case ProjectPluginFunction::Reduce:
                        valid = candidateJcs == "{\"revision\":0}";
                        break;
                    case ProjectPluginFunction::Reconcile:
                        valid = candidateJcs.starts_with("{\"disposition\":\"")
                            && candidateJcs.ends_with("\"}");
                        break;
                    case ProjectPluginFunction::Derive:
                    case ProjectPluginFunction::Plan:
                    case ProjectPluginFunction::NextStep:
                        valid = candidateJcs == "{}";
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
            "journal",
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
            "catalogue",
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

        auto reconcileSchemaOwner = ProjectReconcileSchemaOwner::create(
            *registration,
            "reconcile",
            [](std::string_view candidateJcs) -> Result<ReconcileDisposition>
            {
                struct DispositionCase final
                {
                    std::string_view     document{};
                    ReconcileDisposition disposition{ReconcileDisposition::Ambiguous};
                };
                constexpr auto cases = std::array{
                    DispositionCase{
                        .document    = "{\"disposition\":\"continue\"}",
                        .disposition = ReconcileDisposition::Continue,
                    },
                    DispositionCase{
                        .document    = "{\"disposition\":\"confirmed\"}",
                        .disposition = ReconcileDisposition::Confirmed,
                    },
                    DispositionCase{
                        .document    = "{\"disposition\":\"rejected\"}",
                        .disposition = ReconcileDisposition::Rejected,
                    },
                    DispositionCase{
                        .document    = "{\"disposition\":\"ambiguous\"}",
                        .disposition = ReconcileDisposition::Ambiguous,
                    },
                    DispositionCase{
                        .document    = "{\"disposition\":\"diverged\"}",
                        .disposition = ReconcileDisposition::Diverged,
                    },
                };
                auto const found = std::ranges::find_if(
                    cases,
                    [candidateJcs](DispositionCase const& candidate)
                    {
                        return candidate.document == candidateJcs;
                    }
                );
                if (found == cases.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture reconcile output carries no known disposition"
                    );
                }
                return found->disposition;
            }
        );
        REQUIRE(reconcileSchemaOwner.has_value());

        return ProjectFixture{
            .registration           = *registration,
            .schemaOwner            = *schemaOwner,
            .journalSchemaOwner     = *journalSchemaOwner,
            .toolCatalogSchemaOwner = *toolCatalogSchemaOwner,
            .reconcileSchemaOwner   = *reconcileSchemaOwner,
            .lastReduceInput        = std::move(lastReduceInput),
            .lastDeriveInput        = std::move(lastDeriveInput),
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

    // The RuntimeArtifact every prepared store installs. It carries a real
    // RuntimeModel and real template assets rather than a placeholder, because
    // a snapshot is now composed from an observation the Host resolved through
    // that model: a placeholder artifact installs, and then nothing observes.
    using RuntimeRelease = contract::ObservationRelease;

    [[nodiscard]]
    inline auto runtimeRelease(std::filesystem::path const& root) -> RuntimeRelease
    {
        return contract::observationRelease(root, contract::observationRuntimeModel());
    }

    // The trusted plugin a prepared store registers. plugin_id must equal the
    // registration's, so the id is inserted rather than fixed.
    [[nodiscard]]
    inline auto pluginSource(std::string_view pluginId) -> std::string
    {
        auto source = std::string{"return {\n    plugin_id = \""};
        source += pluginId;
        source += R"LUAU(",
    derive = function(_input) return '{}' end,
    plan = function(_input) return '{}' end,
    next_step = function(_input) return '{}' end,
    reconcile = function(input) return input end,
    reduce = function(_input) return '{"revision":0}' end,
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
        OperatorCoordinator store;
        ProjectPluginHandle plugin;
        ProjectFixture      project;
        SessionManifest     manifest;
        ControlLease        lease;
        SnapshotRecord      snapshot;

        // The Host whose observations this store composes snapshots from. It is
        // part of the prepared state rather than built per case because
        // TaskHost owns an activated generation, and a second Host over the
        // same artifact would be a second observer of one world.
        contract::ObservationHost  observation;
    };

    // Runs one further observation cycle on the prepared Host. Each call is a
    // new capture with a new observation id and, over an unchanged world, the
    // same state resolution.
    [[nodiscard]]
    inline auto observeAgain(PreparedStore& prepared) -> task::UiObservationSnapshot
    {
        return contract::observeOnce(prepared.observation);
    }

    // A second Host over the SAME installed RuntimeArtifact, looking at a
    // different frame. It is how a case reaches a different state resolution
    // without reaching a different artifact: an observation taken through an
    // artifact the session never pinned is refused before it is resolved, so
    // the two refusals cannot be told apart from one fixture.
    [[nodiscard]]
    inline auto secondObservationHost(
        PreparedStore& prepared,
        std::vector<std::byte> framePixels,
        FrameId frameId
    ) -> contract::ObservationHost
    {
        auto const artifactRootHash =
            contract::observeOnce(prepared.observation).artifactRootHash();
        auto installed = prepared.store.openInstalledRuntimeArtifact(
            1U,
            artifactRootHash
        );
        REQUIRE(installed.has_value());
        return contract::activateObservationHost(
            *std::move(installed),
            std::move(framePixels),
            frameId
        );
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
        auto const source = pluginSource(pluginId);
        auto const project = makeProject(pluginId, source);
        auto const manifest = sessionManifest(
            project.registration,
            installed->rootHash()
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
                .capabilityProfileHash     = hashOf("capability"),
                .controlledTargetKey       = "target-1",
                .projectInstanceKey        = "instance-1",
                .mode                      = SessionMode::Write,
            },
            manifest
        ).has_value());
        auto lease = store.acquireLease("session-1");
        REQUIRE(lease.has_value());
        auto observation = contract::activateObservationHost(
            *std::move(installed),
            contract::resolvedFramePixels(),
            FrameId{101}
        );
        auto snapshot = store.createSnapshot(
            *lease,
            projectPlugin,
            contract::observeOnce(observation)
        );
        REQUIRE(snapshot.has_value());
        return PreparedStore{
            .store       = std::move(store),
            .plugin      = projectPlugin,
            .project     = project,
            .manifest    = manifest,
            .lease       = *lease,
            .snapshot    = *std::move(snapshot),
            .observation = std::move(observation),
        };
    }

    [[nodiscard]]
    inline auto command(
        SnapshotRecord const& snapshot,
        std::string clientRequestId
    ) -> CommandRequest
    {
        return CommandRequest{
            .sessionId            = snapshot.sessionId,
            .snapshotToken        = snapshot.token,
            .idempotencyNamespace = "controller-1",
            .clientRequestId      = std::move(clientRequestId),
        };
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
    inline auto createReadyOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string_view toolName
    ) -> StoredOperation
    {
        auto operation = prepared.store.createOrLoadOperation(
            command(prepared.snapshot, std::move(clientRequestId)),
            toolInvocation(prepared.project, std::string{toolName})
        );
        REQUIRE(operation.has_value());
        operation = prepared.store.transitionOperation(
            operation->operationId,
            operation->revision,
            OperationEvent::ReadyWithoutApproval
        );
        REQUIRE(operation.has_value());
        return *operation;
    }

    // Drives one mutating Operation through a real dispatch to the reconciling
    // state, so a reconciliation contract starts where a reconciliation starts.
    [[nodiscard]]
    inline auto reconcilingOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        DeliveryOutcome outcome
    ) -> StoredOperation
    {
        auto const ready = createReadyOperation(
            prepared,
            std::move(clientRequestId),
            "command-1"
        );
        auto const dispatch = prepared.store.reserveDispatch(
            ready.operationId,
            ready.revision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            "authority-1",
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            ready.operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            outcome
        );
        REQUIRE(reconciling.has_value());
        REQUIRE(reconciling->state == OperationState::Reconciling);
        return *reconciling;
    }
}
