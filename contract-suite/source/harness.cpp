#include "harness.hpp"

#include <operator/runtime-installation.hpp>

#include <task/page-model-file.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::operator_runtime::contract
{
    namespace
    {
    }

    TemporaryDirectory::TemporaryDirectory(std::string_view label)
    {
        static auto s_sequence = std::atomic<uint64>{1};
        m_path = std::filesystem::temp_directory_path()
            / std::format(
                "umbraflow-contract-suite-{}-{}-{}",
                label,
                std::chrono::steady_clock::now().time_since_epoch().count(),
                s_sequence.fetch_add(1, std::memory_order_relaxed)
            );
        auto error         = std::error_code{};
        auto const created = std::filesystem::create_directory(m_path, error);
        REQUIRE(created);
        REQUIRE_FALSE(error);
    }

    TemporaryDirectory::~TemporaryDirectory() noexcept
    {
        auto error = std::error_code{};
        static_cast<void>(std::filesystem::remove_all(m_path, error));
    }

    auto TemporaryDirectory::path() const -> std::filesystem::path const&
    {
        return m_path;
    }

    auto hashOf(std::string_view value) -> ContentHash
    {
        auto const result = sha256(std::as_bytes(std::span{value}));
        REQUIRE(result.has_value());
        return *result;
    }

    auto canonical(
        ProjectUnderTest const& project,
        std::string value
    ) -> CanonicalJson
    {
        auto result = project.schemaOwner.canonicalize(std::move(value));
        REQUIRE(result.has_value());
        return *result;
    }

    auto journalEntry(
        ProjectUnderTest const& project,
        JournalDocument const& document
    ) -> ValidatedJournalEntryData
    {
        auto result = project.journalSchemaOwner.validate(
            document.eventType,
            canonical(project, document.payload),
            canonical(project, project.vocabulary.provenance)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto toolInvocation(
        ProjectUnderTest const& project,
        std::string toolName
    ) -> ValidatedToolInvocation
    {
        auto result = project.toolCatalogSchemaOwner.validate(
            std::move(toolName),
            canonical(project, project.vocabulary.toolArguments)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto loadPlugin(ProjectUnderTest const& project) -> ProjectPluginHandle
    {
        auto registrar = ProjectPluginRegistrar{};
        auto result    = registrar.registerPlugin(
            project.registration,
            project.pluginBytes,
            project.artifactBlobs,
            project.schemaOwner
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto reconcileOutcome(
        ProjectUnderTest const& project,
        ProjectPluginHandle const& plugin,
        std::string operationId,
        std::string input
    ) -> ValidatedReconcileOutcome
    {
        auto proposal = plugin.reconcile(canonical(project, std::move(input)));
        REQUIRE(proposal.has_value());
        auto outcome = project.reconcileSchemaOwner.validate(
            std::move(operationId),
            *std::move(proposal)
        );
        REQUIRE(outcome.has_value());
        return *outcome;
    }

    auto sessionManifest(
        VerifiedProjectRegistration const& registration,
        ContentHash const& runtimeArtifactRootHash
    ) -> SessionManifest
    {
        auto const result = SessionManifest::create(
            SessionManifestSpec{
                .hostProtocolSchemaHash       = hashOf("host"),
                .runtimeModelSchemaHash       = hashOf("runtime-schema"),
                .runtimeModelArtifactRootHash = runtimeArtifactRootHash,
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = registration.hash(),
                .policyArtifactHash           = hashOf("policy"),
                .journalEnvelopeSchemaHash    = hashOf("journal-envelope"),
                .agentProfileHash             = hashOf("agent"),
            }
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto runtimeRelease(std::filesystem::path const& root) -> RuntimeRelease
    {
        return observationRelease(root, observationRuntimeModel());
    }

    auto prepareStore(std::filesystem::path const& root) -> PreparedStore
    {
        auto const project = projectUnderTest(ProjectRole::UnderTest);

        // The provider decides the baseline entry and the registration decides
        // the baseline event type. When they disagree nothing below can run,
        // and saying so here names the provider rather than the Operator.
        REQUIRE(
            project.vocabulary.baselineEntry.eventType
            == project.registration.baselineEventType()
        );

        auto const release = runtimeRelease(root / "session-handoff");
        auto storeResult   = OperatorCoordinator::open(root / "production");
        REQUIRE(storeResult.has_value());
        auto store     = *std::move(storeResult);
        auto installed = store.installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());

        auto const manifest = sessionManifest(
            project.registration,
            installed->rootHash()
        );
        auto const plugin = loadPlugin(project);
        REQUIRE(store.registerProject(project.registration).has_value());
        REQUIRE(store.provisionProjectInstance(
            project.registration,
            plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-1",
                .sessionManifestHash = manifest.hash(),
                .entry               = journalEntry(project, project.vocabulary.baselineEntry),
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

        auto const lease = store.acquireLease("session-1");
        REQUIRE(lease.has_value());
        auto observation = activateObservationHost(
            *std::move(installed),
            resolvedFramePixels(),
            FrameId{301}
        );
        auto snapshot = store.createSnapshot(*lease, plugin, observeOnce(observation));
        REQUIRE(snapshot.has_value());
        return PreparedStore{
            .store       = std::move(store),
            .plugin      = plugin,
            .project     = project,
            .lease       = *lease,
            .snapshot    = *std::move(snapshot),
            .observation = std::move(observation),
        };
    }

    auto observeAgain(PreparedStore& prepared) -> task::UiObservationSnapshot
    {
        return observeOnce(prepared.observation);
    }

    auto freshSnapshot(PreparedStore& prepared) -> SnapshotRecord
    {
        auto snapshot = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            observeAgain(prepared)
        );
        REQUIRE(snapshot.has_value());
        return *std::move(snapshot);
    }

    auto command(
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

    auto readyOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string toolName
    ) -> StoredOperation
    {
        auto operation = prepared.store.createOrLoadOperation(
            command(prepared.snapshot, std::move(clientRequestId)),
            toolInvocation(prepared.project, std::move(toolName))
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

    auto reconcilingOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string toolName
    ) -> StoredOperation
    {
        // Every dispatch needs its own authority decision id, so it is derived
        // from the request rather than fixed: two Operations in one store
        // otherwise collide on the second one's reservation.
        auto const authority = "authority-" + clientRequestId;
        auto const ready     = readyOperation(
            prepared,
            std::move(clientRequestId),
            std::move(toolName)
        );
        auto const dispatch = prepared.store.reserveDispatch(
            ready.operationId,
            ready.revision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            authority,
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            ready.operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            DeliveryOutcome::Delivered
        );
        REQUIRE(reconciling.has_value());
        return *reconciling;
    }

    auto confirmedCommit(
        PreparedStore const& prepared,
        StoredOperation const& operation,
        uint64 expectedProjectStateRevision,
        std::string eventId,
        JournalDocument const& entry
    ) -> ReconciliationCommit
    {
        return ReconciliationCommit{
            .operationId                  = operation.operationId,
            .expectedOperationRevision    = operation.revision,
            .expectedProjectStateRevision = expectedProjectStateRevision,
            .outcome                      = reconcileOutcome(
                prepared.project,
                prepared.plugin,
                operation.operationId,
                prepared.project.vocabulary.confirmedInput
            ),
            .journalEvents = {
                JournalAppend{
                    .eventId = std::move(eventId),
                    .entry   = journalEntry(prepared.project, entry),
                },
            },
        };
    }

    auto occurrences(
        std::string_view text,
        std::string_view needle
    ) -> std::size_t
    {
        auto count = std::size_t{0};
        for (
            auto at = text.find(needle);
            at != std::string_view::npos;
            at = text.find(needle, at + needle.size())
        )
        {
            ++count;
        }
        return count;
    }
}
