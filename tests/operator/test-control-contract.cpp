// The half of the Operator control contracts this repository owns outright: the
// shape its own schema documents must have, and the authority its own entry
// points re-read. The project-parameterised half of C-01 belongs to the exported
// conformance suite, which a consuming repository runs against its own
// registration; see conformance/source/suite-control-ledger.cpp. No property is
// asserted in both places.
//
// A case named schema-* below asserts only that a schema definition exists with
// certain members. It cannot go red when the behaviour it describes is removed,
// so it does not carry the contract- name.
//
// C-05, C-06, C-07, C-09, C-11, C-13 and C-14 are absent because their subject
// is: every one of them was a shape or a transition of the Operation record,
// and the Operation record is deleted. Their gates were not moved anywhere --
// there is nothing left for them to be about.
//
// C-03 and C-10 survive because Host delivery did not go with the Operation.
// OP:`DeliveryAuthority` is what OperatorCoordinator::reserveToolCallDispatch
// mints for every Tool-call input delivery, OP:`ReceiptRef` is the receipt the
// Host answers with, and OP:`DeliveryOutcome` is the three-valued
// classification toolCallCompletionFor reads. All three are live, so their
// shapes are still shapes of something.

#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/tool-admission-request.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <format>
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
        auto heldReading = std::optional<task::UiObservationSnapshot>{};
        auto heldBinding = std::optional<ControllerBinding>{};
        {
            auto prepared = prepareStore(temporary.path());
            // No renewal call exists, and none is needed: the same lease keeps
            // working for as long as the process holds it.
            CHECK(prepared.store.createSnapshot(
                prepared.lease,
                prepared.project.registration,
                prepared.project.toolCatalogSchemaOwner,
                prepared.project.observedInstanceIdentitySchemas,
                test_support::observeAgain(prepared)
            ).has_value());
            heldLease   = prepared.lease;
            heldBinding = prepared.controller;
            manifest    = prepared.manifest;
            project     = prepared.project;
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
            auto const worldScope = operator_runtime::ObservedInstanceWorldScope::run(
                "target-1",
                1
            );
            REQUIRE(worldScope.has_value());
            return SessionPin{
                .sessionId                 = std::move(sessionId),
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = registrationHash,
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = "target-1",
                .projectInstanceKey        = "instance-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
                .worldScope                = *worldScope,
            };
        };
        CHECK_FALSE(
            restarted->pinSession(pin("session-1"), *manifest,
                test_support::unconstrainedAgentProfile(*manifest)
            ).has_value()
        );

        // There is no binding to present either: the epoch check now lives at
        // the one door onto the path, so a controller from the previous epoch
        // cannot be authenticated at all.
        CHECK_FALSE(restarted->bindController("session-1").has_value());
        CHECK_FALSE(restarted->createSnapshot(
            *heldLease,
            project->registration,
            project->toolCatalogSchemaOwner,
            project->observedInstanceIdentitySchemas,
            *heldReading
        ).has_value());

        // A ControllerBinding is a value, so one survives the coordinator that
        // minted it. It is evidence and not a bearer capability, and a takeover
        // is the sharpest way to show it: takeoverLease succeeds against any
        // state of the target, so nothing downstream of the epoch check would
        // refuse this call. It fails only because the entry point re-reads the
        // pinned row and finds an epoch from a process that has ended.
        CHECK_FALSE(
            restarted->takeoverLease(*heldBinding, "a dead epoch's binding")
                .has_value()
        );

        REQUIRE(
            restarted->pinSession(pin("session-2"), *manifest,
                test_support::unconstrainedAgentProfile(*manifest)
            ).has_value()
        );
        auto const rebound = restarted->bindController("session-2");
        REQUIRE(rebound.has_value());
        auto const fresh = restarted->acquireLease(*rebound);
        REQUIRE(fresh.has_value());
        CHECK(fresh->sessionEpoch > heldLease->sessionEpoch);

        // The fencing high-water outlives the epoch it was reached in, so the
        // new epoch's first token cannot collide with a token the old one may
        // still be presenting somewhere.
        CHECK(fresh->fencingToken > heldLease->fencingToken);
    }

    // The authority a Host acts under, minted by the ledger and never by a
    // caller. Every published field of it is validated behaviourally in
    // tests/operator/test-host-controller.cpp under a HOST_VALIDATION_TEST
    // marker, and check-repository-surface fails when a published field has no
    // such marker -- which is what stops this from being a shape check with
    // nothing behind it.
    TEST_CASE("schema-control-c03")
    {
        auto const schema    = readSchema("umbraflow-operator-v1.schema.json");
        auto const authority = definition(schema, "DeliveryAuthority");
        auto const receipt   = definition(schema, "ReceiptRef");
        checkStrictObject(authority);
        checkStrictObject(receipt);
        CHECK(authority.find("\"controlled_target_id\"") != std::string::npos);
        CHECK(authority.find("\"session_epoch\"") != std::string::npos);
        CHECK(authority.find("\"fencing_token\"") != std::string::npos);
        CHECK(authority.find("\"receipt_ref\"") != std::string::npos);
        CHECK(receipt.find("\"receipt_id\"") != std::string::npos);
        CHECK(receipt.find("coordinate") == std::string::npos);
    }

    // The Host's own delivery classification, and the half of C-10 that
    // outlived the Operation: OP:`DispatchRecord` described a dispatch of a
    // frozen plan step and is deleted with it, while task::DeliveryOutcome is
    // still what a Tool-call delivery answers with and still the only value
    // that can prove an external effect absent.
    TEST_CASE("schema-control-c10")
    {
        auto const schema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const outcome = definition(schema, "DeliveryOutcome");
        CHECK(outcome.find("\"not_delivered\"") != std::string::npos);
        CHECK(outcome.find("\"delivered\"") != std::string::npos);
        CHECK(outcome.find("\"transport_unknown\"") != std::string::npos);

        // not_delivered and transport_unknown each carry a reason and delivered
        // does not, which is the whole of why the three are one oneOf rather
        // than an enum: only the two uncertain arms have anything to say.
        CHECK(outcome.find("\"reason\"") != std::string::npos);
    }

    TEST_CASE("contract-control-c04")
    {
        auto const schema     = readSchema("umbraflow-operator-v1.schema.json");
        auto const invocation = definition(schema, "ToolInvocation");
        checkStrictObject(invocation);

        // The published invocation carries no authority of its own. Who is
        // asking arrives as a ControllerBinding the Operator minted, so an
        // invocation cannot name a controller, a session or a fingerprint that
        // would let a caller present itself.
        CHECK(invocation.find("authenticated_controller_id") == std::string::npos);
        CHECK(invocation.find("authenticated_session_id") == std::string::npos);
        CHECK(invocation.find("command_fingerprint") == std::string::npos);

        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The caller names a tool and its arguments, and nothing else. The
        // version and the mutability that decides the mutation chain are read
        // out of the Tool Catalog descriptor: the same argument bytes reach a
        // mutating and a read-only tool, and the caller stated neither.
        auto const mutating = toolInvocation(
            prepared.project,
            prepared.project.toolName("command-1")
        );
        auto const readOnly = toolInvocation(
            prepared.project,
            prepared.project.toolName("observe-1")
        );
        CHECK(mutating.canonicalArgs() == readOnly.canonicalArgs());
        CHECK(mutating.descriptor().mutability == ToolMutability::Mutating);
        CHECK(readOnly.descriptor().mutability == ToolMutability::ReadOnly);
        CHECK(mutating.descriptor().toolVersion == "1");
        CHECK(readOnly.descriptor().toolVersion == "1");

        // A tool the catalog does not describe cannot be minted at all, so an
        // effect has no spelling that skips the descriptor.
        CHECK_FALSE(prepared.project.toolCatalogSchemaOwner.validate(
            prepared.project.toolName("unlisted-command"),
            canonical("{\"value\":1}")
        ).has_value());

        // Binding is derived too: the Operator takes the registration from the
        // authenticated session, so a call another project's catalog owner
        // minted is refused at the one admission door rather than reconciled.
        // The foreign tool is the read-only one, so the refusal is the
        // registration comparison and not an effect the policy would have
        // denied anyway.
        auto const foreignId = std::string{"fixture.foreign"};
        auto const foreign   = makeProject(foreignId);
        CHECK(
            foreign.registration.hash()
            != prepared.project.registration.hash()
        );
        auto preimage = CanonicalJson::parseExact(
            R"({"objective":"foreign-catalog"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "request-foreign",
            *std::move(preimage)
        );
        REQUIRE(root.has_value());
        auto call = test_support::toolCallAt(
            *root,
            nullptr,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = prepared.manifest.hash(),
                .frameworkReleaseIdentity    = prepared.runtimeArtifactRootHash,
                .toolRuntimeProtocolIdentity = hashOf("c04-protocol"),
                .environmentIdentity         = hashOf("c04-environment"),
            },
            toolInvocation(foreign, foreign.toolName("observe-1"))
        );
        REQUIRE(call.has_value());
        auto const refused = prepared.store.admitToolCall(ToolAdmissionRequest{
            .controller      = prepared.controller,
            .lease           = prepared.lease,
            .root            = *root,
            .call            = *call,
            .policyAuthority = prepared.policyAuthority,
        });
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            refused.error().message().contains("different ProjectRegistration")
        );
    }

    // What a Tool descriptor may bound, which is the half of C-08 that outlived
    // the plan document: PlanProposal carried these two definitions and is
    // deleted, but both are still read out of the Tool Catalog by
    // tool-descriptor.hpp, so both still describe something the framework has.
    TEST_CASE("schema-control-c08")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const intent = definition(schema, "UIActionIntent");
        auto const limits = definition(schema, "WorkflowLimits");
        checkStrictObject(intent);
        checkStrictObject(limits);
        CHECK(intent.find("\"binding_variant_constraints\"") != std::string::npos);
        CHECK(intent.find("\"expected_ui_postconditions\"") != std::string::npos);
        CHECK(intent.find("\"delivery_class\"") != std::string::npos);
        CHECK(limits.find("\"maximum_dispatches\"") != std::string::npos);
        CHECK(limits.find("\"maximum_observations\"") != std::string::npos);
    }

    // The policy artifact is the half of C-12 that outlived the Operation:
    // OP:`ApprovalToken` and OP:`AuthorityDecision` described an approval bound
    // to a plan step and are deleted with it, while the Operator still owns the
    // policy document and still denies an effect it does not recognise.
    TEST_CASE("schema-control-c12")
    {
        auto const policySchema = readSchema("umbraflow-policy-v1.schema.json");
        auto const policy       = definition(policySchema, "PolicyArtifact");
        checkStrictObject(policy);
        CHECK(policy.find("\"owned_by\"") != std::string::npos);
        CHECK(policy.find("\"const\": \"operator\"") != std::string::npos);
        CHECK(policy.find("\"unknown_effect_decision\"") != std::string::npos);
        CHECK(policy.find("\"const\": \"deny\"") != std::string::npos);
    }
}
