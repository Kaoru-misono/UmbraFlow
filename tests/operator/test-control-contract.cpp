#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/operation.hpp>

#include "project-fixture.hpp"

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_pluginSource = std::string_view{R"LUAU(
return {
    plugin_id = "fixture.control",
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
                        "umbraflow-control-contract-{}-{}",
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

        using test_support::canonical;
        using test_support::hashOf;
        using test_support::journalEntry;
        using test_support::loadPlugin;
        using test_support::makeProject;
        using test_support::sessionManifest;
        using test_support::toolInvocation;

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
            auto const project = makeProject("fixture.control", k_pluginSource);
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
        auto reconciliationOutcome(
            PreparedStore const& prepared,
            std::string document
        ) -> ValidatedReconcileOutcome
        {
            return test_support::reconcileOutcome(
                prepared.project,
                prepared.plugin,
                std::move(document)
            );
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

        [[nodiscard]]
        auto createReadyOperation(
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
    }

    TEST_CASE("contract-control-c01")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const takeover = prepared.store.takeoverLease("session-1", "human takeover");
        REQUIRE(takeover.has_value());
        CHECK(takeover->fencingToken > prepared.lease.fencingToken);
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            hashOf("stale-snapshot")
        ).has_value());
    }

    TEST_CASE("contract-control-c02")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const epoch  = definition(schema, "SessionEpoch");
        auto const lease  = definition(schema, "ControlLease");
        checkStrictObject(lease);
        CHECK(epoch.find("\"minimum\": 1") != std::string::npos);
        CHECK(lease.find("\"session_epoch\"") != std::string::npos);
        CHECK(lease.find("\"fencing_token\"") != std::string::npos);
        CHECK(lease.find("expiry") == std::string::npos);
        CHECK(lease.find("renew") == std::string::npos);
    }

    TEST_CASE("contract-control-c03")
    {
        // HOST_VALIDATION_TEST(DeliveryAuthority.controlled_target_id)
        // HOST_VALIDATION_TEST(DeliveryAuthority.target_generation)
        // HOST_VALIDATION_TEST(DeliveryAuthority.session_epoch)
        // HOST_VALIDATION_TEST(DeliveryAuthority.lease_id)
        // HOST_VALIDATION_TEST(DeliveryAuthority.fencing_token)
        // HOST_VALIDATION_TEST(DeliveryAuthority.operation_id)
        // HOST_VALIDATION_TEST(DeliveryAuthority.dispatch_seq)
        // HOST_VALIDATION_TEST(DeliveryAuthority.frozen_plan_hash)
        // HOST_VALIDATION_TEST(DeliveryAuthority.authority_decision_id)
        // HOST_VALIDATION_TEST(DeliveryAuthority.receipt_ref)
        auto const schema    = readSchema("umbraflow-operator-v1.schema.json");
        auto const authority = definition(schema, "DeliveryAuthority");
        auto const receipt   = definition(schema, "ReceiptRef");
        checkStrictObject(authority);
        checkStrictObject(receipt);
        CHECK(authority.find("\"target_generation\"") != std::string::npos);
        CHECK(authority.find("\"session_epoch\"") != std::string::npos);
        CHECK(authority.find("\"fencing_token\"") != std::string::npos);
        CHECK(authority.find("\"receipt_ref\"") != std::string::npos);
        CHECK(receipt.find("\"receipt_id\"") != std::string::npos);
        CHECK(receipt.find("coordinate") == std::string::npos);
    }

    TEST_CASE("contract-control-c04")
    {
        auto const schema        = readSchema("umbraflow-operator-v1.schema.json");
        auto const invocation    = definition(schema, "ToolInvocation");
        auto const commandRecord = definition(schema, "CommandRecord");
        checkStrictObject(invocation);
        checkStrictObject(commandRecord);
        CHECK(invocation.find("authenticated_controller_id") == std::string::npos);
        CHECK(invocation.find("command_fingerprint") == std::string::npos);
        CHECK(commandRecord.find("\"authenticated_session_id\"") != std::string::npos);
        CHECK(commandRecord.find("\"authenticated_controller_id\"") != std::string::npos);
        CHECK(commandRecord.find("\"command_fingerprint\"") != std::string::npos);
    }

    TEST_CASE("contract-control-c05")
    {
        auto const schema    = readSchema("umbraflow-operator-v1.schema.json");
        auto const proposal  = definition(schema, "PlanProposal");
        auto const effective = definition(schema, "EffectivePlan");
        checkStrictObject(proposal);
        checkStrictObject(effective);
        CHECK(proposal.find("\"effects\"") != std::string::npos);
        CHECK(proposal.find("\"allowed_ui_actions\"") != std::string::npos);
        CHECK(effective.find("\"command_fingerprint\"") != std::string::npos);
        CHECK(effective.find("\"project_registration_hash\"") != std::string::npos);
        CHECK(effective.find("\"decision_basis_hash\"") != std::string::npos);
        CHECK(effective.find("\"required_approvals\"") != std::string::npos);
    }

    TEST_CASE("contract-control-c06")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const request  = command(prepared.snapshot, "request-1");
        auto const first    = prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, "command-1")
        );
        auto const repeated = prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(first.has_value());
        REQUIRE(repeated.has_value());
        CHECK(first->lookup == CommandLookup::Created);
        CHECK(repeated->lookup == CommandLookup::Existing);
        CHECK(first->operationId == repeated->operationId);

        CHECK_FALSE(prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, "different-command")
        ).has_value());
    }

    TEST_CASE("contract-control-c07")
    {
        auto machine    = OperationMachine{};
        auto transition = machine.transition(OperationEvent::DecisionInputsChanged);
        REQUIRE(transition.has_value());
        CHECK(*transition == OperationState::NeedsRevalidation);
        transition = machine.transition(OperationEvent::Revalidated);
        REQUIRE(transition.has_value());
        CHECK(*transition == OperationState::Proposed);
        transition = machine.transition(OperationEvent::ReadyWithoutApproval);
        REQUIRE(transition.has_value());
        transition = machine.transition(OperationEvent::DispatchStarted);
        REQUIRE(transition.has_value());
        CHECK(machine.planFrozen());
        CHECK_FALSE(machine.transition(OperationEvent::DecisionInputsChanged).has_value());

        auto const schema      = readSchema("umbraflow-operator-v1.schema.json");
        auto const planVersion = definition(schema, "PlanVersion");
        checkStrictObject(planVersion);
        CHECK(planVersion.find("\"provisional\"") != std::string::npos);
        CHECK(planVersion.find("\"frozen\"") != std::string::npos);
        CHECK(planVersion.find("\"superseded\"") != std::string::npos);
    }

    TEST_CASE("contract-control-c08")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const plan   = definition(schema, "EffectivePlan");
        auto const intent = definition(schema, "UIActionIntent");
        auto const limits = definition(schema, "WorkflowLimits");
        checkStrictObject(plan);
        checkStrictObject(intent);
        checkStrictObject(limits);
        CHECK(plan.find("\"allowed_ui_actions\"") != std::string::npos);
        CHECK(intent.find("\"binding_variant_constraints\"") != std::string::npos);
        CHECK(intent.find("\"expected_ui_postconditions\"") != std::string::npos);
        CHECK(intent.find("\"delivery_class\"") != std::string::npos);
        CHECK(limits.find("\"maximum_dispatches\"") != std::string::npos);
        CHECK(limits.find("\"maximum_observations\"") != std::string::npos);
    }

    TEST_CASE("contract-control-c09")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = createReadyOperation(prepared, "request-1", "command-1");
        auto const dispatch  = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            "authority-1",
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        auto const reconciles = prepared.store.recordDeliveryOutcome(
            operation.operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            DeliveryOutcome::TransportUnknown
        );
        REQUIRE(reconciles.has_value());
        CHECK(reconciles->state == OperationState::Reconciling);

        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        checkStrictObject(definition(schema, "DispatchOutcome"));
        auto const result = definition(schema, "ToolResult");
        checkStrictObject(result);
        CHECK(result.find("\"dispatch_outcomes\"") != std::string::npos);
        CHECK(result.find("\"reconciliation_progress\"") != std::string::npos);
        CHECK(result.find("\"success\"") == std::string::npos);
    }

    TEST_CASE("contract-control-c10")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = createReadyOperation(prepared, "request-1", "command-1");
        auto const dispatch  = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            "authority-1",
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation.operationId,
            dispatch->operationRevision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            "authority-2",
            std::nullopt
        ).has_value());

        auto const schema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const record  = definition(schema, "DispatchRecord");
        auto const outcome = definition(schema, "DeliveryOutcome");
        checkStrictObject(record);
        CHECK(record.find("\"dispatch_started_at\"") != std::string::npos);
        CHECK(record.find("\"delivery_authority\"") != std::string::npos);
        CHECK(outcome.find("\"not_delivered\"") != std::string::npos);
        CHECK(outcome.find("\"transport_unknown\"") != std::string::npos);
    }

    TEST_CASE("contract-control-c11")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = createReadyOperation(prepared, "request-1", "command-1");
        auto const dispatch  = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            "authority-1",
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            operation.operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            DeliveryOutcome::Delivered
        );
        REQUIRE(reconciling.has_value());

        auto const progress = prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = reconciling->revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(prepared, "{\"disposition\":\"continue\"}"),
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
        REQUIRE(progress.has_value());
        CHECK(progress->state == OperationState::Reconciling);

        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = progress->revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(prepared, "{\"disposition\":\"confirmed\"}"),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-stale",
                        .entry = journalEntry(
                            prepared.project,
                            "fixture.stale",
                            "{\"value\":2}"
                        ),
                    },
                },
            }
        ).has_value());

        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = progress->revision,
                .expectedProjectStateRevision = 1U,
                .outcome                      = reconciliationOutcome(prepared, "{\"disposition\":\"confirmed\"}"),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-2",
                        .entry = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":2}"
                        ),
                    },
                    JournalAppend{
                        .eventId = "event-1",
                        .entry = journalEntry(
                            prepared.project,
                            "fixture.duplicate",
                            "{\"value\":3}"
                        ),
                    },
                },
            }
        ).has_value());

        auto const confirmed = prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = progress->revision,
                .expectedProjectStateRevision = 1U,
                .outcome                      = reconciliationOutcome(prepared, "{\"disposition\":\"confirmed\"}"),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-2",
                        .entry = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":2}"
                        ),
                    },
                },
            }
        );
        REQUIRE(confirmed.has_value());
        CHECK(confirmed->state == OperationState::Confirmed);
        CHECK(confirmed->revision > progress->revision);

        auto const schema    = readSchema("umbraflow-operator-v1.schema.json");
        auto const reconcile = definition(schema, "ReconcileProposal");
        checkStrictObject(reconcile);
        CHECK(reconcile.find("\"journal_events\"") != std::string::npos);
        CHECK(reconcile.find("\"observed_outcomes\"") != std::string::npos);
        CHECK(reconcile.find("\"continue\"") != std::string::npos);
        CHECK(reconcile.find("\"diverged\"") != std::string::npos);
    }

    TEST_CASE("contract-control-c12")
    {
        auto const policySchema   = readSchema("umbraflow-policy-v1.schema.json");
        auto const operatorSchema = readSchema("umbraflow-operator-v1.schema.json");
        auto const policy         = definition(policySchema, "PolicyArtifact");
        auto const approvalToken  = definition(operatorSchema, "ApprovalToken");
        auto const authority      = definition(operatorSchema, "AuthorityDecision");
        checkStrictObject(policy);
        checkStrictObject(approvalToken);
        checkStrictObject(authority);
        CHECK(policy.find("\"owned_by\"") != std::string::npos);
        CHECK(policy.find("\"const\": \"operator\"") != std::string::npos);
        CHECK(policy.find("\"unknown_effect_decision\"") != std::string::npos);
        CHECK(policy.find("\"const\": \"deny\"") != std::string::npos);
        CHECK(approvalToken.find("\"approval_authority_decision_id\"") != std::string::npos);
        CHECK(approvalToken.find("\"step_intent_hash\"") != std::string::npos);
        CHECK(approvalToken.find("\"effect_envelope_hash\"") != std::string::npos);
        CHECK(approvalToken.find("\"lease_id\"") != std::string::npos);
        CHECK(authority.find("\"approval_token_ids\"") != std::string::npos);

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
                .operationId            = operation->operationId,
                .lease                  = prepared.lease,
                .frozenPlanHash         = planHash,
                .stepIntentHash         = stepHash,
                .decisionBasisHash      = hashOf("decision"),
                .effectEnvelopeHash     = hashOf("effects"),
                .policyHash             = hashOf("policy"),
                .approverPrincipal      = "human-1",
                .approverCapabilityHash = hashOf("approval-capability"),
                .expiresAtUnixMillis    = 4'000'000'000'000U,
            },
            "approval-authority-1"
        );
        REQUIRE(approval.has_value());
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation->operationId,
            operation->revision,
            prepared.lease,
            hashOf("decision"),
            planHash,
            hashOf("wrong-step"),
            "dispatch-authority-invalid",
            *approval
        ).has_value());
        auto const dispatch = prepared.store.reserveDispatch(
            operation->operationId,
            operation->revision,
            prepared.lease,
            hashOf("decision"),
            planHash,
            stepHash,
            "dispatch-authority-1",
            *approval
        );
        REQUIRE(dispatch.has_value());
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            operation->operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            DeliveryOutcome::NotDelivered
        );
        REQUIRE(reconciling.has_value());
        auto const awaitingApproval = prepared.store.transitionOperation(
            operation->operationId,
            reconciling->revision,
            OperationEvent::NextStepApprovalRequired
        );
        REQUIRE(awaitingApproval.has_value());
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation->operationId,
            awaitingApproval->revision,
            prepared.lease,
            hashOf("decision"),
            planHash,
            stepHash,
            "dispatch-authority-2",
            *approval
        ).has_value());
    }

    TEST_CASE("contract-control-c13")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto first     = prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-1"),
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(first.has_value());
        CHECK_FALSE(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, "command-2")
        ).has_value());
        first = prepared.store.transitionOperation(
            first->operationId,
            first->revision,
            OperationEvent::Cancelled
        );
        REQUIRE(first.has_value());
        CHECK(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, "command-2")
        ).has_value());

        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const chain  = definition(schema, "MutationChain");
        checkStrictObject(chain);
        CHECK(chain.find("\"controlled_target_id\"") != std::string::npos);
        CHECK(chain.find("\"project_instance_key\"") != std::string::npos);
        CHECK(chain.find("\"operation_id\"") != std::string::npos);
    }

    TEST_CASE("contract-control-c14")
    {
        auto machine = OperationMachine{};
        REQUIRE(machine.transition(OperationEvent::ApprovalRequired).has_value());
        REQUIRE(machine.transition(OperationEvent::ApprovalObtained).has_value());
        REQUIRE(machine.transition(OperationEvent::DispatchStarted).has_value());
        auto const postDispatchAbort = machine.transition(OperationEvent::PostDispatchAbort);
        REQUIRE(postDispatchAbort.has_value());
        CHECK(*postDispatchAbort == OperationState::Reconciling);
        CHECK(machine.planFrozen());
        CHECK(machine.hasDispatched());
        CHECK(machine.mutationLocked());
        CHECK_FALSE(machine.transition(OperationEvent::Cancelled).has_value());
        CHECK_FALSE(machine.transition(OperationEvent::DeadlineExpired).has_value());
    }
}
