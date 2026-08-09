#include <operator/ledger.hpp>
#include <operator/manifest.hpp>

#include "project-fixture.hpp"

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_pluginSource = std::string_view{R"LUAU(
return {
    plugin_id = "fixture.alpha",
    derive = function(_input) return '{}' end,
    plan = function(_input) return '{}' end,
    next_step = function(_input) return '{}' end,
    reconcile = function(input) return input end,
    reduce = function(_input) return '{"revision":0}' end,
}
)LUAU"};

        class TemporaryDirectory final
        {
            std::filesystem::path m_path{};

        public:
            TemporaryDirectory()
            {
                static auto s_sequence = std::atomic<uint64>{1};
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-operator-ledger-{}-{}",
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        s_sequence.fetch_add(1, std::memory_order_relaxed)
                    );
                auto error = std::error_code{};
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

        using test_support::canonical;
        using test_support::hashOf;
        using test_support::journalEntry;
        using test_support::loadPlugin;
        using test_support::makeProject;
        using test_support::sessionManifest;
        using test_support::toolInvocation;

        static_assert(!std::is_aggregate_v<ValidatedJournalEntryData>);
        static_assert(
            !std::is_constructible_v<
                ValidatedJournalEntryData,
                ContentHash,
                std::string,
                ContentHash,
                CanonicalJson,
                CanonicalJson
            >
        );

        struct PreparedStore final
        {
            OperatorCoordinator          store;
            ProjectPluginHandle          plugin;
            test_support::ProjectFixture project;
            ControlLease                 lease;
            SnapshotRecord               snapshot;
        };

        [[nodiscard]]
        auto prepareStore(std::filesystem::path const& path) -> PreparedStore
        {
            auto const release = test_support::runtimeRelease(path / "session-handoff");
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
            auto const project = makeProject("fixture.alpha", k_pluginSource);
            auto const manifest = sessionManifest(
                project.registration,
                installed->rootHash()
            );
            auto const projectPlugin = loadPlugin(project, k_pluginSource);
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
            auto snapshot = store.createSnapshot(*lease, hashOf("snapshot-1"));
            REQUIRE(snapshot.has_value());
            return PreparedStore{
                .store    = std::move(store),
                .plugin   = projectPlugin,
                .project  = project,
                .lease    = *lease,
                .snapshot = *snapshot,
            };
        }

        [[nodiscard]]
        auto reconciliationProposal(
            PreparedStore const& prepared,
            std::string value
        ) -> ValidatedDocument
        {
            auto result = prepared.plugin.reconcile(
                canonical(prepared.project.schemaOwner, std::move(value))
            );
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]]
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
    }

    TEST_CASE("OperatorCoordinator creates only the production database name")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        CHECK(prepared.store.databasePath().filename() == "operator-runtime.sqlite");
        CHECK(std::filesystem::is_regular_file(prepared.store.databasePath()));
    }

    TEST_CASE("Journal schema owner prevents caller-attached payload and provenance labels")
    {
        auto const project = makeProject("fixture.alpha", k_pluginSource);
        auto const accepted = project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":1}"),
            canonical(project.schemaOwner, "{\"kind\":\"fixture\"}")
        );
        REQUIRE(accepted.has_value());
        CHECK(accepted->projectRegistrationHash() == project.registration.hash());
        CHECK(accepted->payloadSchemaHash() == hashOf("progress-schema"));

        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":2}"),
            canonical(project.schemaOwner, "{\"kind\":\"fixture\"}")
        ).has_value());
        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.unknown",
            canonical(project.schemaOwner, "{\"value\":99}"),
            canonical(project.schemaOwner, "{\"kind\":\"fixture\"}")
        ).has_value());
        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":1}"),
            canonical(project.schemaOwner, "{\"kind\":\"forged\"}")
        ).has_value());
    }

    TEST_CASE("ProjectInstance provisioning rejects Journal data from another registration")
    {
        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(temporary.path());
        auto coordinator = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(coordinator.has_value());
        auto const installed = coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());

        auto const project = makeProject("fixture.alpha", k_pluginSource);
        auto const foreign = makeProject("fixture.foreign", "foreign-plugin-bytes");
        auto const plugin = loadPlugin(project, k_pluginSource);
        auto const manifest = sessionManifest(
            project.registration,
            installed->rootHash()
        );
        REQUIRE(coordinator->registerProject(project.registration).has_value());
        CHECK_FALSE(coordinator->provisionProjectInstance(
            project.registration,
            plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-1",
                .sessionManifestHash = manifest.hash(),
                .entry = journalEntry(
                    foreign,
                    foreign.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
    }

    TEST_CASE("production RuntimeArtifact installation owns activation CAS")
    {
        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(temporary.path());
        auto coordinator = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(coordinator.has_value());

        auto installed = coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());
        CHECK(installed->installedGeneration() == 1U);
        CHECK(installed->rootHash() == release.artifactRootHash);

        CHECK_FALSE(coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        ).has_value());

        test_support::writeFile(
            release.handoffRoot / "runtime-artifact" / task::k_runtimeModelFileName,
            "authoring handoff changed"
        );
        auto reopened = coordinator->openInstalledRuntimeArtifact(
            1U,
            release.artifactRootHash
        );
        REQUIRE(reopened.has_value());
        CHECK(reopened->rootHash() == release.artifactRootHash);
    }

    TEST_CASE("lease takeover advances fencing and invalidates stale snapshot creation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const takeover = prepared.store.takeoverLease("session-1", "human takeover");
        REQUIRE(takeover.has_value());
        CHECK(takeover->fencingToken > prepared.lease.fencingToken);
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            hashOf("stale")
        ).has_value());
    }

    TEST_CASE("commands are durable-idempotent and mutation chains are exclusive")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const request = command(prepared.snapshot, "request-1");
        auto first = prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(first.has_value());
        CHECK(first->lookup == CommandLookup::Created);

        auto const repeated = prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(repeated.has_value());
        CHECK(repeated->lookup == CommandLookup::Existing);
        CHECK(repeated->operationId == first->operationId);

        // Same client_request_id, different tool: the stored fingerprint is
        // what decides, and it covers the tool the catalog named.
        CHECK_FALSE(prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, "different-command")
        ).has_value());
        CHECK_FALSE(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, "command-2")
        ).has_value());

        // A read-only tool takes no mutation chain, so it is admitted while the
        // mutating Operation above is still live.
        CHECK(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-3"),
            toolInvocation(prepared.project, "observe-1")
        ).has_value());

        auto const cancelled = prepared.store.transitionOperation(
            first->operationId,
            first->revision,
            OperationEvent::Cancelled
        );
        REQUIRE(cancelled.has_value());
        CHECK(cancelled->state == OperationState::Cancelled);
        CHECK(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, "command-2")
        ).has_value());
    }

    TEST_CASE("a tool invocation cannot cross ProjectRegistrations")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const foreign = makeProject("fixture.foreign", "foreign-plugin-bytes");

        // The catalog owner is bound to its registration root, and that root
        // hashes the registration JCS the tool_catalog_hash sits in.
        CHECK_FALSE(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-1"),
            toolInvocation(foreign, "command-1")
        ).has_value());
    }

    TEST_CASE("the Tool Catalog owns mutability and tool version")
    {
        auto const project = makeProject("fixture.alpha", k_pluginSource);
        auto const mutating = toolInvocation(project, "command-1");
        CHECK(mutating.mutability() == ToolMutability::Mutating);
        CHECK(mutating.toolVersion() == "1");
        CHECK(mutating.projectRegistrationHash() == project.registration.hash());
        CHECK(mutating.toolCatalogHash() == project.registration.toolCatalogHash());

        CHECK(
            toolInvocation(project, "observe-1").mutability()
            == ToolMutability::ReadOnly
        );

        // No tool, and arguments the descriptor's schema refuses.
        CHECK_FALSE(project.toolCatalogSchemaOwner.validate(
            "not-in-the-catalog",
            canonical(project.schemaOwner, "{\"value\":1}")
        ).has_value());
        CHECK_FALSE(project.toolCatalogSchemaOwner.validate(
            "command-1",
            canonical(project.schemaOwner, "{\"value\":2}")
        ).has_value());
    }

    TEST_CASE("dispatch freezes once and every Host outcome enters reconciliation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto operation = prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-1"),
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(operation.has_value());
        operation = prepared.store.transitionOperation(
            operation->operationId,
            operation->revision,
            OperationEvent::ReadyWithoutApproval
        );
        REQUIRE(operation.has_value());

        auto const planHash = hashOf("frozen-plan");
        auto const dispatch = prepared.store.reserveDispatch(
            operation->operationId,
            operation->revision,
            prepared.lease,
            hashOf("decision-basis"),
            planHash,
            hashOf("step-1"),
            "authority-1",
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation->operationId,
            dispatch->operationRevision,
            prepared.lease,
            hashOf("decision-basis"),
            planHash,
            hashOf("step-1"),
            "authority-2",
            std::nullopt
        ).has_value());

        auto const reconciles = prepared.store.recordDeliveryOutcome(
            operation->operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            DeliveryOutcome::TransportUnknown
        );
        REQUIRE(reconciles.has_value());
        CHECK(reconciles->state == OperationState::Reconciling);
        CHECK(reconciles->planFrozen);
        CHECK_FALSE(prepared.store.recordDeliveryOutcome(
            operation->operationId,
            dispatch->dispatchSequence,
            reconciles->revision,
            DeliveryOutcome::Delivered
        ).has_value());

        auto const foreign = makeProject(
            "fixture.foreign",
            "foreign-plugin-bytes"
        );
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId = operation->operationId,
                .expectedOperationRevision = reconciles->revision,
                .expectedProjectStateRevision = 0U,
                .disposition                  = ReconcileDisposition::Confirmed,
                .proposal = reconciliationProposal(
                    prepared,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-foreign",
                        .entry = journalEntry(
                            foreign,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());

        auto const committed = prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId = operation->operationId,
                .expectedOperationRevision = reconciles->revision,
                .expectedProjectStateRevision = 0U,
                .disposition                  = ReconcileDisposition::Confirmed,
                .proposal = reconciliationProposal(
                    prepared,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        );
        REQUIRE(committed.has_value());
        CHECK(committed->state == OperationState::Confirmed);
    }

    TEST_CASE("ApprovalToken is operation-bound and single-use")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto operation = prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-1"),
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(operation.has_value());
        operation = prepared.store.transitionOperation(
            operation->operationId,
            operation->revision,
            OperationEvent::ApprovalRequired
        );
        REQUIRE(operation.has_value());

        auto const planHash = hashOf("plan");
        auto const stepHash = hashOf("step");
        auto const approval = prepared.store.issueApproval(
            ApprovalRequest{
                .operationId = operation->operationId,
                .lease                  = prepared.lease,
                .frozenPlanHash         = planHash,
                .stepIntentHash         = stepHash,
                .decisionBasisHash      = hashOf("decision-basis"),
                .effectEnvelopeHash     = hashOf("effects"),
                .policyHash             = hashOf("policy"),
                .approverPrincipal      = "human-1",
                .approverCapabilityHash = hashOf("approval-capability"),
                .expiresAtUnixMillis    = 4'000'000'000'000U,
            },
            "human-decision-1"
        );
        REQUIRE(approval.has_value());
        auto const dispatch = prepared.store.reserveDispatch(
            operation->operationId,
            operation->revision,
            prepared.lease,
            hashOf("decision-basis"),
            planHash,
            stepHash,
            "dispatch-authority-1",
            *approval
        );
        REQUIRE(dispatch.has_value());
        auto reconciles = prepared.store.recordDeliveryOutcome(
            operation->operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            DeliveryOutcome::NotDelivered
        );
        REQUIRE(reconciles.has_value());
        auto waiting = prepared.store.transitionOperation(
            operation->operationId,
            reconciles->revision,
            OperationEvent::NextStepApprovalRequired
        );
        REQUIRE(waiting.has_value());
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation->operationId,
            waiting->revision,
            prepared.lease,
            hashOf("decision-basis"),
            planHash,
            stepHash,
            "dispatch-authority-2",
            *approval
        ).has_value());
    }
}
