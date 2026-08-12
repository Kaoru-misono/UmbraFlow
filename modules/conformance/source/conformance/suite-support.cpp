#include "suite-support.hpp"

#include <deployment/project-directory.hpp>

#include <operator/runtime-installation.hpp>

#include <task/runtime-model-file.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::operator_runtime::conformance
{
    namespace
    {
        // Written once by main before any case runs, and read from then on. It
        // is a function-local static rather than a namespace-scope variable so
        // that nothing outside the two accessors below can name it.
        [[nodiscard]] auto projectDirectorySlot() -> std::filesystem::path&
        {
            static auto s_directory = std::filesystem::path{};
            return s_directory;
        }

        [[nodiscard]]
        auto roleOf(
            deployment::ConformanceProject const& project UF_LIFETIME_BOUND,
            ProjectRole role
        ) -> deployment::ProjectConformanceRole const&
        {
            return role == ProjectRole::UnderTest
                ? project.underTest
                : project.foreign;
        }
    }

    auto setProjectDirectory(std::filesystem::path directory) -> void
    {
        projectDirectorySlot() = std::move(directory);
    }

    auto projectDirectory() -> std::filesystem::path
    {
        return projectDirectorySlot();
    }

    auto loadedProject() -> deployment::ConformanceProject
    {
        auto const directory = projectDirectory();
        REQUIRE_MESSAGE(
            !directory.empty(),
            "no project directory was set; run umbra-flow-conformance --project"
        );
        auto loaded = deployment::loadConformanceProject(directory, {});
        REQUIRE_MESSAGE(
            loaded.has_value(),
            "the project directory could not be loaded: ",
            loaded.error().message()
        );
        return *std::move(loaded);
    }

    auto deploymentFor(
        deployment::ConformanceProject const& project,
        ProjectRole role
    ) -> deployment::LoadedDeployment const&
    {
        auto const* p_deployment = project.loaded.findDeployment(
            roleOf(project, role).deployment
        );
        REQUIRE(p_deployment != nullptr);
        return *p_deployment;
    }

    auto vocabularyFor(
        deployment::ConformanceProject const& project,
        ProjectRole role
    ) -> deployment::ProjectVocabulary const&
    {
        return roleOf(project, role).vocabulary;
    }

    auto uiActionOf(deployment::ProjectVocabulary const& vocabulary)
        -> task::UiActionUnderTest
    {
        return task::UiActionUnderTest{
            .surface  = vocabulary.uiAction.surface,
            .uiTarget = vocabulary.uiAction.uiTarget,
            .action   = vocabulary.uiAction.action,
        };
    }

    TemporaryDirectory::TemporaryDirectory(std::string_view label)
    {
        static auto s_sequence = std::atomic<uint64>{1};
        m_path = std::filesystem::temp_directory_path()
            / std::format(
                "umbraflow-conformance-{}-{}-{}",
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
        deployment::ConformanceProject const& project,
        ProjectRole role,
        std::string value
    ) -> CanonicalJson
    {
        auto result = deploymentFor(project, role).schemaOwner.canonicalize(
            std::move(value)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto journalEntry(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        deployment::ProjectJournalDocument const& document
    ) -> ValidatedJournalEntryData
    {
        auto result = deploymentFor(project, role).journalSchemaOwner.validate(
            document.eventType,
            canonical(project, role, document.payload),
            canonical(project, role, vocabularyFor(project, role).provenance)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto toolInvocation(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        std::string toolName
    ) -> ValidatedToolInvocation
    {
        auto result = deploymentFor(project, role).toolCatalogSchemaOwner.validate(
            std::move(toolName),
            canonical(project, role, vocabularyFor(project, role).toolArguments)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto loadPlugin(
        deployment::ConformanceProject const& project,
        ProjectRole role
    ) -> ProjectPluginHandle
    {
        auto const& one = deploymentFor(project, role);
        auto registrar  = ProjectPluginRegistrar{};
        auto result     = registrar.registerPlugin(
            one.registration,
            one.pluginBytes,
            one.artifactBlobs,
            one.schemaOwner
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto reconcileOutcome(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        ProjectPluginHandle const& plugin,
        std::string operationId,
        std::string input
    ) -> ValidatedReconcileOutcome
    {
        auto proposal = plugin.reconcile(
            canonical(project, role, std::move(input))
        );
        REQUIRE(proposal.has_value());
        auto outcome = deploymentFor(project, role).reconcileSchemaOwner.validate(
            std::move(operationId),
            *std::move(proposal)
        );
        REQUIRE(outcome.has_value());
        return *outcome;
    }

    auto policyArtifact(
        deployment::LoadedDeployment const& deployed,
        deployment::ProjectVocabulary const& vocabulary
    ) -> std::string
    {
        // Read out of this project's own descriptors rather than named here: a
        // policy speaking about an effect type this project never proposes
        // would allow nothing, and every plan would meet the artifact's
        // default deny instead of the rule a case is about.
        auto types = std::vector<std::string>{};
        for (auto const& tool : std::array{
                 vocabulary.mutatingTool,
                 vocabulary.approvalRequiredPlanTool,
             })
        {
            auto const descriptor = deployed.catalog.carriedTool(tool);
            REQUIRE(descriptor.has_value());
            for (auto const& bound : descriptor->effectBounds)
            {
                types.emplace_back(bound.namespacedType);
            }
        }
        std::ranges::sort(types);
        types.erase(std::ranges::unique(types).begin(), types.end());
        REQUIRE_FALSE(types.empty());
        return policyArtifactBytes(hashOf("operator"), types);
    }

    auto sessionManifest(
        VerifiedProjectRegistration const& registration,
        ContentHash const& runtimeArtifactRootHash,
        std::string_view exactPolicyArtifactBytes
    ) -> SessionManifest
    {
        auto const result = SessionManifest::create(
            SessionManifestSpec{
                .hostProtocolSchemaHash       = hashOf("host"),
                .runtimeModelSchemaHash       = hashOf("runtime-schema"),
                .runtimeModelArtifactRootHash = runtimeArtifactRootHash,
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = registration.hash(),
                .policyArtifactHash           = hashOf(exactPolicyArtifactBytes),
                .journalEnvelopeSchemaHash    = hashOf("journal-envelope"),
                .agentProfileHash             = hashOf("agent"),
            }
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto prepareStore(std::filesystem::path const& root) -> PreparedStore
    {
        auto project             = loadedProject();
        auto const& underTest    = deploymentFor(project, ProjectRole::UnderTest);
        auto const& vocabulary   = vocabularyFor(project, ProjectRole::UnderTest);

        // umbraflow-conformance.json decides the baseline entry and
        // umbraflow-project.json decides the baseline event type. When they
        // disagree nothing below can run, and saying so here names the project
        // directory rather than the Operator.
        REQUIRE(
            vocabulary.baselineEntry.eventType
            == underTest.registration.baselineEventType()
        );

        auto const release = observationRelease(
            root / "session-handoff",
            project.loaded.runtimeArtifactRoot
        );
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
        auto const artifactRootHash    = installed->rootHash();
        auto const installedGeneration = installed->installedGeneration();

        auto const policy   = policyArtifact(underTest, vocabulary);
        auto const manifest = sessionManifest(
            underTest.registration,
            installed->rootHash(),
            policy
        );
        auto const plugin = loadPlugin(project, ProjectRole::UnderTest);
        REQUIRE(store.registerProject(underTest.registration).has_value());
        REQUIRE(store.provisionProjectInstance(
            underTest.registration,
            plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-1",
                .sessionManifestHash = manifest.hash(),
                .entry               = journalEntry(
                    project,
                    ProjectRole::UnderTest,
                    vocabulary.baselineEntry
                ),
            }
        ).has_value());
        REQUIRE(store.pinSession(
            SessionPin{
                .sessionId                 = "session-1",
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = underTest.registration.hash(),
                .controllerCapabilities    = {std::string{k_operateCapability}},
                .controlledTargetId        = "target-1",
                .projectInstanceKey        = "instance-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest,
            std::nullopt
        ).has_value());

        auto const controller = store.bindController("session-1");
        REQUIRE(controller.has_value());
        auto const lease = store.acquireLease(*controller);
        REQUIRE(lease.has_value());

        // requireProbeGeometry runs inside this call rather than at the top of
        // this function: after the Q2 ruling the extent the capture must match
        // is the model's, and the model is not parsed until the Host activates
        // the artifact. It is still ahead of the observation below, so
        // requireResolvedSurface's account of what can reach it is unchanged.
        auto observation = activateObservationHost(
            *std::move(installed),
            project.probeFrame,
            FrameId{301}
        );
        auto const reading = observeOnce(observation);

        // Checked once, here, rather than per case: every case below plans on a
        // resolved state, so a probe frame this project's model does not satisfy
        // must be named where it was supplied.
        requireResolvedSurface(reading, vocabulary.uiAction.surface);

        auto snapshot = store.createSnapshot(*lease, plugin, reading);
        REQUIRE(snapshot.has_value());

        // "operator" is the exact operator protocol schema sessionManifest
        // above pins. The authority hashes the bytes and compares, so a suite
        // that named the wrong ones could not build one at all.
        //
        // The RuntimeModel binding comes from the Host that just activated this
        // project's artifact. It is what lets the Operator refuse a step naming
        // UI the model does not define, and the suite cannot substitute one: the
        // binding is minted only by TaskHost and only for the artifact the
        // session manifest pins.
        auto runtimeModel = observation.host->runtimeModelBinding(
            observation.generation
        );
        REQUIRE(runtimeModel.has_value());
        auto authority = planAuthority(
            underTest.registration,
            manifest,
            *runtimeModel,
            "operator",
            policy,
            uiActionOf(vocabulary)
        );
        REQUIRE(authority.has_value());
        return PreparedStore{
            .store                   = std::move(store),
            .plugin                  = plugin,
            .project                 = std::move(project),
            .manifest                = manifest,
            .planAuthority           = *std::move(authority),
            .controller              = *controller,
            .lease                   = *lease,
            .snapshot                = *std::move(snapshot),
            .observation             = std::move(observation),
            .runtimeArtifactRootHash = artifactRootHash,
            .installedGeneration     = installedGeneration,
        };
    }

    auto deliveringHost(PreparedStore& prepared)
        -> std::unique_ptr<DeliveringHost>
    {
        return deliveringHostFor(
            prepared.store,
            prepared.lease,
            prepared.installedGeneration,
            prepared.runtimeArtifactRootHash,
            uiActionOf(prepared.project.underTest.vocabulary),
            prepared.project.probeFrame
        );
    }

    auto deliverAndRecord(
        PreparedStore& prepared,
        DeliveringHost& host,
        DispatchReservation const& reservation
    ) -> Result<StoredOperation>
    {
        return prepared.store.recordDeliveryOutcome(
            prepared.lease,
            reservation.operationRevision,
            host.deliverReport(reservation.authority)
        );
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
            .snapshotToken        = snapshot.token,
            .idempotencyNamespace = "controller-1",
            .clientRequestId      = std::move(clientRequestId),
        };
    }

    auto frozenPlan(
        PreparedStore& prepared,
        StoredOperation const& operation
    ) -> Result<FrozenPlan>
    {
        return prepared.store.freezePlan(
            operation.operationId,
            operation.revision,
            prepared.lease,
            prepared.plugin,
            deploymentFor(prepared.project, ProjectRole::UnderTest).toolCatalogSchemaOwner,
            prepared.planAuthority
        );
    }

    auto plannedStep(
        PreparedStore& prepared,
        StoredOperation const& operation
    ) -> Result<PlannedStep>
    {
        return prepared.store.mintNextStep(
            operation.operationId,
            operation.revision,
            prepared.lease,
            prepared.plugin,
            deploymentFor(prepared.project, ProjectRole::UnderTest).toolCatalogSchemaOwner,
            prepared.planAuthority
        );
    }

    auto readyOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string toolName
    ) -> StoredOperation
    {
        auto operation = prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, std::move(clientRequestId)),
            toolInvocation(
                prepared.project,
                ProjectRole::UnderTest,
                std::move(toolName)
            )
        );
        REQUIRE(operation.has_value());
        auto const frozen = frozenPlan(prepared, operation->operation);
        REQUIRE(frozen.has_value());
        auto const step = plannedStep(prepared, frozen->operation);
        REQUIRE(step.has_value());
        return step->operation;
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
        auto const authority = AuthorityDecisionId{"authority-" + clientRequestId};
        auto const ready     = readyOperation(
            prepared,
            std::move(clientRequestId),
            std::move(toolName)
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
        auto const reconciling = deliverAndRecord(prepared, *host, *dispatch);
        REQUIRE(reconciling.has_value());
        REQUIRE(host->clicks() == 1U);
        return *reconciling;
    }

    auto confirmedCommit(
        PreparedStore const& prepared,
        StoredOperation const& operation,
        uint64 expectedProjectStateRevision,
        std::string eventId,
        deployment::ProjectJournalDocument const& entry
    ) -> ReconciliationCommit
    {
        return ReconciliationCommit{
            .operationId                  = operation.operationId,
            .expectedOperationRevision    = operation.revision,
            .expectedProjectStateRevision = expectedProjectStateRevision,
            .outcome                      = reconcileOutcome(
                prepared.project,
                ProjectRole::UnderTest,
                prepared.plugin,
                operation.operationId,
                prepared.project.underTest.vocabulary.confirmedInput
            ),
            .journalEvents = {
                JournalAppend{
                    .eventId = std::move(eventId),
                    .entry   = journalEntry(
                        prepared.project,
                        ProjectRole::UnderTest,
                        entry
                    ),
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
