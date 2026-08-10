// The half of the Operator control contracts this repository owns outright: the
// shape its own schema documents must have, and the transitions OperationMachine
// decides on its own. The project-parameterised half of C-01, C-06 and C-09
// through C-13 belongs to the exported contract suite, which a consuming
// repository runs against its own registration; see
// contract-suite/source/suite-control-ledger.cpp. No property is asserted in
// both places.
//
// A case named schema-* below asserts only that a schema definition exists with
// certain members. It cannot go red when the behaviour it describes is removed,
// so it does not carry the contract- name; for C-09 through C-13 that name is
// registered against the suite's cases instead.

#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/operation.hpp>

#include "project-fixture.hpp"

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

        using test_support::canonical;
        using test_support::command;
        using test_support::hashOf;
        using test_support::makeProject;
        using test_support::prepareStore;
        using test_support::TemporaryDirectory;
        using test_support::toolInvocation;
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

        auto temporary   = TemporaryDirectory{};
        auto heldLease   = std::optional<ControlLease>{};
        auto manifest    = std::optional<SessionManifest>{};
        auto project     = std::optional<test_support::ProjectFixture>{};
        auto heldPlugin  = std::optional<ProjectPluginHandle>{};
        auto heldReading = std::optional<task::UiObservationSnapshot>{};
        {
            auto prepared = prepareStore(temporary.path());
            // No renewal call exists, and none is needed: the same lease keeps
            // working for as long as the process holds it.
            CHECK(prepared.store.createSnapshot(
                prepared.lease,
                prepared.plugin,
                test_support::observeAgain(prepared)
            ).has_value());
            heldLease   = prepared.lease;
            manifest    = prepared.manifest;
            project     = prepared.project;
            heldPlugin  = prepared.plugin;
            heldReading = test_support::observeAgain(prepared);
        }

        // Dropping the coordinator closes the database; reopening it is the
        // restart. Everything below is what the new session epoch does to what
        // the previous one left behind.
        auto restarted = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(restarted.has_value());
        auto const registrationHash = project->registration.hash();
        auto const pin = [&registrationHash](std::string sessionId)
        {
            return SessionPin{
                .sessionId                 = std::move(sessionId),
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = registrationHash,
                .capabilityProfileHash     = hashOf("capability"),
                .controlledTargetKey       = "target-1",
                .projectInstanceKey        = "instance-1",
                .mode                      = SessionMode::Write,
            };
        };
        CHECK_FALSE(restarted->pinSession(pin("session-1"), *manifest).has_value());
        CHECK_FALSE(restarted->acquireLease("session-1").has_value());
        CHECK_FALSE(restarted->createSnapshot(
            *heldLease,
            *heldPlugin,
            *heldReading
        ).has_value());

        REQUIRE(restarted->pinSession(pin("session-2"), *manifest).has_value());
        auto const fresh = restarted->acquireLease("session-2");
        REQUIRE(fresh.has_value());
        CHECK(fresh->sessionEpoch > heldLease->sessionEpoch);

        // The fencing high-water outlives the epoch it was reached in, so the
        // new epoch's first token cannot collide with a token the old one may
        // still be presenting somewhere.
        CHECK(fresh->fencingToken > heldLease->fencingToken);
    }

    TEST_CASE("schema-control-c03")
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

        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The caller names a tool and its arguments, and nothing else. The
        // version and the mutability that decides the mutation chain are read
        // out of the Tool Catalog descriptor: the same argument bytes reach a
        // mutating and a read-only tool, and the caller stated neither.
        auto const mutating = toolInvocation(prepared.project, "command-1");
        auto const readOnly = toolInvocation(prepared.project, "observe-1");
        CHECK(mutating.canonicalArgs() == readOnly.canonicalArgs());
        CHECK(mutating.mutability() == ToolMutability::Mutating);
        CHECK(readOnly.mutability() == ToolMutability::ReadOnly);
        CHECK(mutating.toolVersion() == "1");
        CHECK(readOnly.toolVersion() == "1");

        // A tool the catalog does not describe cannot be minted at all, so an
        // effect has no spelling that skips the descriptor.
        CHECK_FALSE(prepared.project.toolCatalogSchemaOwner.validate(
            "unlisted-command",
            canonical(prepared.project.schemaOwner, "{\"value\":1}")
        ).has_value());

        // Binding is derived too: the Operator takes the registration from the
        // authenticated session, so an invocation another project's catalog
        // owner minted is refused rather than reconciled.
        auto const foreignId = std::string{"fixture.foreign"};
        auto const foreign   = makeProject(
            foreignId,
            test_support::pluginSource(foreignId)
        );
        CHECK(
            foreign.registration.hash()
            != prepared.project.registration.hash()
        );
        CHECK_FALSE(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-foreign"),
            toolInvocation(foreign, "command-1")
        ).has_value());
    }

    TEST_CASE("schema-control-c05")
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

    TEST_CASE("schema-control-c08")
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

    TEST_CASE("schema-control-c09")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        checkStrictObject(definition(schema, "DispatchOutcome"));
        auto const result = definition(schema, "ToolResult");
        checkStrictObject(result);
        CHECK(result.find("\"dispatch_outcomes\"") != std::string::npos);
        CHECK(result.find("\"reconciliation_progress\"") != std::string::npos);
        CHECK(result.find("\"success\"") == std::string::npos);
    }

    TEST_CASE("schema-control-c10")
    {
        auto const schema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const record  = definition(schema, "DispatchRecord");
        auto const outcome = definition(schema, "DeliveryOutcome");
        checkStrictObject(record);
        CHECK(record.find("\"dispatch_started_at\"") != std::string::npos);
        CHECK(record.find("\"delivery_authority\"") != std::string::npos);
        CHECK(outcome.find("\"not_delivered\"") != std::string::npos);
        CHECK(outcome.find("\"transport_unknown\"") != std::string::npos);
    }

    TEST_CASE("schema-control-c11")
    {
        auto const schema    = readSchema("umbraflow-operator-v1.schema.json");
        auto const reconcile = definition(schema, "ReconcileProposal");
        checkStrictObject(reconcile);
        CHECK(reconcile.find("\"journal_events\"") != std::string::npos);
        CHECK(reconcile.find("\"observed_outcomes\"") != std::string::npos);
        CHECK(reconcile.find("\"continue\"") != std::string::npos);
        CHECK(reconcile.find("\"diverged\"") != std::string::npos);
    }

    TEST_CASE("schema-control-c12")
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
    }

    TEST_CASE("schema-control-c13")
    {
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
