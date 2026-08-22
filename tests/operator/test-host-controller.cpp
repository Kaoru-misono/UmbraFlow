#include <operator/host-controller.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-invocation.hpp>

#include <conformance/host-delivery-fixture.hpp>

#include "../support/umbraflow/project-fixture.hpp"
#include "tool-call-fixture.hpp"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        using test_support::journalEntry;
        using test_support::prepareStore;
        using test_support::TemporaryDirectory;

        [[nodiscard]]
        auto bindSecondTarget(test_support::PreparedStore& prepared)
            -> ControllerBinding
        {
            REQUIRE(prepared.store.provisionProjectInstance(
                prepared.project.registration,
                prepared.generation,
                ProjectInstanceBaseline{
                    .projectInstanceKey  = "instance-2",
                    .eventId             = "baseline-2",
                    .sessionManifestHash = prepared.manifest.hash(),
                    .entry = journalEntry(
                        prepared.project,
                        prepared.project.registration.baselineEventType(),
                        R"({"kind":"baseline"})"
                    ),
                }
            ).has_value());
            auto const worldScope = ObservedInstanceWorldScope::run(
                "target-2",
                1
            );
            REQUIRE(worldScope.has_value());
            auto pinned = prepared.store.pinSession(
                SessionPin{
                    .sessionId                 = "session-2",
                    .authenticatedControllerId = "controller-2",
                    .idempotencyNamespace      = "controller-2",
                    .projectRegistrationHash   = prepared.project.registration.hash(),
                    .controllerCapabilities    = {
                        std::string{conformance::k_operateCapability},
                    },
                    .controlledTargetId = "target-2",
                    .projectInstanceKey = "instance-2",
                    .mode               = SessionMode::Write,
                    .kind               = ControllerKind::Script,
                    .worldScope         = *worldScope,
                },
                prepared.manifest,
                std::nullopt
            );
            CAPTURE(pinned.has_value() ? std::string{} : pinned.error().message());
            REQUIRE(pinned.has_value());
            auto controller = prepared.store.bindController("session-2");
            REQUIRE(controller.has_value());
            return *std::move(controller);
        }
    }

    TEST_CASE("production host controller joins lease acquire and release")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
        auto owner = OperatorTaskHost::create(
            std::move(prepared.store),
            "target-1"
        );
        REQUIRE(owner.has_value());

        auto lease = owner->acquireLease(prepared.controller);
        REQUIRE(lease.has_value());
        CHECK(
            task::TaskHostTestAccess::fence(owner->host()).fencingToken
            == lease->fencingToken
        );
        CHECK(owner->releaseLease(*lease).has_value());

        auto reacquired = owner->acquireLease(prepared.controller);
        REQUIRE(reacquired.has_value());
        CHECK(reacquired->fencingToken > lease->fencingToken);
        CHECK(owner->releaseLease(*reacquired).has_value());
    }

    TEST_CASE("production host controller advances the Host fence with takeover")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto owner     = OperatorTaskHost::create(
            std::move(prepared.store),
            "target-1"
        );
        REQUIRE(owner.has_value());

        auto takeover = owner->takeoverLease(
            prepared.controller,
            "production owner takeover"
        );
        REQUIRE(takeover.has_value());
        CHECK(takeover->lease.fencingToken > prepared.lease.fencingToken);
        CHECK(
            task::TaskHostTestAccess::fence(owner->host()).controlledTargetId
            == "target-1"
        );
        CHECK(
            task::TaskHostTestAccess::fence(owner->host()).fencingToken
            == takeover->lease.fencingToken
        );
    }

    // The Tool-call-native delivery seam, end to end and without an Operation.
    // What it proves is that the two things a Tool call could not reach exist
    // and are joined: the ledger mints Host delivery authority over a
    // dispatching Tool call, and the Host captures its own frame, resolves the
    // named target on it and posts the input the Receipt that mint produced
    // authorizes.
    TEST_CASE(
        "the Tool call input seam delivers over a dispatching call and refuses "
        "what the model does not declare"
    )
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // A second live target, bound before the store moves. It is what makes
        // "the lease must own the target the admission recorded" a refusal a
        // case can reach: a lease that is live for its own target and foreign
        // to this call's.
        auto targetTwo = bindSecondTarget(prepared);
        auto owner     = OperatorTaskHost::create(
            std::move(prepared.store),
            "target-1"
        );
        REQUIRE(owner.has_value());

        auto takeover = owner->takeoverLease(
            prepared.controller,
            "tool call input seam"
        );
        REQUIRE(takeover.has_value());
        auto const& lease = takeover->lease;

        auto installed = owner->coordinator().openInstalledRuntimeArtifact(
            prepared.installedGeneration,
            prepared.runtimeArtifactRootHash
        );
        REQUIRE(installed.has_value());
        auto const generation = conformance::activateDeliveringGeneration(
            owner->host(),
            *std::move(installed)
        );
        auto const fingerprint = conformance::declaredFingerprint(
            owner->host(),
            generation
        );
        auto runtime = conformance::ObservationRuntime{
            test_support::umbraflowProbeFrame(),
            fingerprint,
            FrameId{902}
        };

        auto preimage = CanonicalJson::parseExact(
            R"({"objective":"tool call input"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "tool-call-input",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto catalog   = FrameworkToolCatalogOwner::create();
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(catalog.has_value());
        REQUIRE(arguments.has_value());
        auto invocation = catalog->validate(
            "framework.screen.observe",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = test_support::toolCallAt(
            *root,
            nullptr,
            1U,
            ToolExecutionIdentity{
                .runIdentity              = test_support::hashOf("seam-run"),
                .frameworkReleaseIdentity = test_support::hashOf("seam-release"),
                .toolRuntimeProtocolIdentity =
                    test_support::hashOf("seam-protocol"),
                .environmentIdentity = test_support::hashOf("seam-environment"),
            },
            *invocation
        );
        REQUIRE(call.has_value());
        REQUIRE(owner->coordinator().persistToolRootRequest(*root).has_value());
        REQUIRE(
            owner->coordinator().persistToolCallPosition(*root, *call).has_value()
        );

        auto const intent = OperatorTaskHost::ToolCallInputIntent{
            .uiTarget = test_support::k_fixtureUiAction.uiTarget,
            .uiAction = test_support::k_fixtureUiAction.action,
        };

        auto admitted = owner->coordinator().admitToolCall(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = lease,
                .root       = *root,
                .call       = *call,
            }
        );
        REQUIRE(admitted.has_value());

        // The durable boundary is the whole authority, and admission is not it.
        // The row is admitted and its attempt is live, so the only thing left
        // to refuse on is the state itself -- and no capture is spent finding
        // that out.
        auto const undispatched = owner->deliverToolCallInput(
            *call,
            lease,
            generation,
            intent,
            runtime.context()
        );
        REQUIRE_FALSE(undispatched.has_value());
        CHECK(undispatched.error().message().contains(
            "names no dispatching Tool call"
        ));

        REQUIRE(
            owner->coordinator().beginToolCallDispatch(*admitted).has_value()
        );

        auto const unnamed = owner->deliverToolCallInput(
            *call,
            lease,
            generation,
            OperatorTaskHost::ToolCallInputIntent{.uiAction = intent.uiAction},
            runtime.context()
        );
        REQUIRE_FALSE(unnamed.has_value());
        CHECK(unnamed.error().message().contains("must name the model target"));

        // A lease that is live for another target cannot mint authority over
        // this call. It is asked of the Coordinator directly because the Host
        // above is bound to one target and refuses a foreign lease before the
        // mint is reached at all.
        auto targetTwoLease = owner->coordinator().takeoverLease(
            targetTwo,
            "target-2 lease"
        );
        REQUIRE(targetTwoLease.has_value());
        auto const foreign = owner->coordinator().reserveToolCallDispatch(
            *call,
            targetTwoLease->lease,
            generation,
            intent.uiTarget
        );
        REQUIRE_FALSE(foreign.has_value());
        CHECK(foreign.error().message().contains(
            "does not own the controlled target the admission recorded"
        ));

        // Two names the model does not declare. Both refuse before a frame is
        // captured, which is also what makes interpolating a declared name
        // into the trusted chunk safe.
        auto const foreignTarget = owner->deliverToolCallInput(
            *call,
            lease,
            generation,
            OperatorTaskHost::ToolCallInputIntent{
                .uiTarget = "fixture.absent",
                .uiAction = intent.uiAction,
            },
            runtime.context()
        );
        REQUIRE_FALSE(foreignTarget.has_value());
        CHECK(foreignTarget.error().message().contains("declares no ui target"));

        auto const foreignAction = owner->deliverToolCallInput(
            *call,
            lease,
            generation,
            OperatorTaskHost::ToolCallInputIntent{
                .uiTarget = intent.uiTarget,
                .uiAction = "fixture.absent",
            },
            runtime.context()
        );
        REQUIRE_FALSE(foreignAction.has_value());
        CHECK(foreignAction.error().message().contains("declares no UI action"));
        CHECK(runtime.actions().clicks() == 0U);

        // A declared target whose Binding does not carry this action. The
        // resolver refuses inside the trusted chunk, which raises with the
        // cycle it opened still open, so the delivery below is also the proof
        // that the Host swept that frame rather than leaving its generation
        // holding one no ticket names.
        auto const unauthorized = owner->deliverToolCallInput(
            *call,
            lease,
            generation,
            OperatorTaskHost::ToolCallInputIntent{
                .uiTarget = "fixture.marker",
                .uiAction = intent.uiAction,
            },
            runtime.context()
        );
        REQUIRE_FALSE(unauthorized.has_value());
        CHECK(runtime.actions().clicks() == 0U);

        // The authority this door mints, field by field, out of the ledger's
        // own rows. Every one is read from the pinned lease and the recorded
        // admission; a caller states none of them.
        //
        // Every field of OP:`DeliveryAuthority` is validated here, including
        // the three an Operation dispatch would fill and this door does not.
        // Leaving operation_id, authority_decision_id and target_generation
        // unasserted would have been the weaker choice: emptiness is what makes
        // a Tool-call report unanswerable as an Operation, so it is a value
        // this door states rather than a gap in it, and something has to hold
        // the door to stating it.
        auto const reserved = owner->coordinator().reserveToolCallDispatch(
            *call,
            lease,
            generation,
            intent.uiTarget
        );
        auto const reservedWhy = reserved.has_value()
            ? std::string{}
            : std::string{reserved.error().message()};
        REQUIRE_MESSAGE(reserved.has_value(), reservedWhy);
        // HOST_VALIDATION_TEST(DeliveryAuthority.controlled_target_id)
        CHECK(reserved->authority.controlledTargetId == lease.controlledTargetId);
        // HOST_VALIDATION_TEST(DeliveryAuthority.lease_id)
        CHECK(reserved->authority.leaseId == lease.leaseId);
        // HOST_VALIDATION_TEST(DeliveryAuthority.session_epoch)
        CHECK(reserved->authority.sessionEpoch == lease.sessionEpoch);
        // HOST_VALIDATION_TEST(DeliveryAuthority.fencing_token)
        CHECK(reserved->authority.fencingToken == lease.fencingToken);
        // The attempt number the admission recorded. A Tool call is dispatched
        // once per admitted attempt, so this is the sequence for this door.
        // HOST_VALIDATION_TEST(DeliveryAuthority.dispatch_seq)
        CHECK(reserved->authority.dispatchSequence == 1U);
        // The call identity, which is what this door pins a delivery to: a
        // Tool call has no frozen plan, and the field carries the immutable
        // coordinate the admission was recorded at instead.
        // HOST_VALIDATION_TEST(DeliveryAuthority.frozen_plan_hash)
        CHECK(reserved->authority.frozenPlanHash == call->identity());


        auto const delivered = owner->deliverToolCallInput(
            *call,
            lease,
            generation,
            intent,
            runtime.context()
        );
        auto const deliveredWhy = delivered.has_value()
            ? std::string{}
            : std::string{delivered.error().message()};
        REQUIRE_MESSAGE(delivered.has_value(), deliveredWhy);
        CHECK(delivered->outcome() == task::DeliveryOutcome::Delivered);
        // The engine receipt the Host acted under. It is minted by TaskHost and
        // by nothing else, so a report carrying one is proof a real delivery
        // path ran.
        // HOST_VALIDATION_TEST(DeliveryAuthority.receipt_ref)
        CHECK(delivered->receiptId() != 0U);
        CHECK(runtime.actions().clicks() == 1U);

        // The classification is derived from what the Host reported and from
        // nothing a caller says.
        auto const completion = toolCallCompletionFor(*delivered);
        REQUIRE(completion.has_value());
        CHECK(completion->kind() == ToolCallCompletionKind::Confirmed);
        CHECK(completion->payload().bytes().contains(R"("delivered":true)"));

        // The sink refuses, so the Host reached the delivery path and cannot
        // say whether the input arrived. Uncertainty is what the ledger records
        // for that, and never absence: only NotDelivered proves absence.
        runtime.actions().refuseClicks();
        auto const uncertain = owner->deliverToolCallInput(
            *call,
            lease,
            generation,
            intent,
            runtime.context()
        );
        REQUIRE(uncertain.has_value());
        CHECK(uncertain->outcome() == task::DeliveryOutcome::TransportUnknown);
        auto const uncertainCompletion = toolCallCompletionFor(*uncertain);
        REQUIRE(uncertainCompletion.has_value());
        CHECK(uncertainCompletion->kind() == ToolCallCompletionKind::Possible);

        // A superseded lease mints nothing, whatever its call's row still says.
        auto const displaced = owner->takeoverLease(
            prepared.controller,
            "tool call input seam takeover"
        );
        REQUIRE(displaced.has_value());
        auto const stale = owner->deliverToolCallInput(
            *call,
            lease,
            generation,
            intent,
            runtime.context()
        );
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error().message().contains("lease was superseded"));
    }
}
