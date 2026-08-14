#include <operator/host-controller.hpp>

#include <conformance/host-delivery-fixture.hpp>

#include "../support/umbraflow/project-fixture.hpp"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        using test_support::createReadyOperation;
        using test_support::journalEntry;
        using test_support::prepareStore;
        using test_support::TemporaryDirectory;

        [[nodiscard]]
        auto bindSecondTarget(test_support::PreparedStore& prepared)
            -> ControllerBinding
        {
            REQUIRE(prepared.store.provisionProjectInstance(
                prepared.project.registration,
                prepared.plugin,
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

    TEST_CASE("production host controller advances the Host fence with takeover")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto ready     = createReadyOperation(
            prepared,
            "request-production-owner",
            "command-1"
        );
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
            FrameId{901}
        };
        auto minted = task::TaskHostTestAccess::run(
            owner->host(),
            generation,
            runtime.context(),
            task::authorizeActionSource(test_support::k_fixtureUiAction)
        );
        REQUIRE(minted.has_value());
        auto delivered = owner->dispatch(
            ready.operationId,
            ready.revision,
            takeover->lease,
            generation,
            AuthorityDecisionId{"authority-production-owner"},
            std::nullopt,
            runtime.context()
        );
        REQUIRE(delivered.has_value());
        CHECK(delivered->delivery.outcome() == task::DeliveryOutcome::Delivered);
        CHECK(runtime.actions().clicks() == 1U);
    }

    TEST_CASE(
        "fault matrix lease takeover reports in-flight dispatch and fences "
        "the displaced controller"
    )
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto ready     = createReadyOperation(
            prepared,
            "request-target-1",
            "command-1"
        );
        auto targetTwo = bindSecondTarget(prepared);

        auto owner = OperatorTaskHost::create(
            std::move(prepared.store),
            "target-1"
        );
        REQUIRE(owner.has_value());

        // The target-bound owner cannot move its Host fence for another
        // target, and refuses before the Coordinator transaction begins.
        CHECK_FALSE(owner->takeoverLease(targetTwo, "foreign target").has_value());

        // The second target is real rather than a mismatched string: its own
        // takeover succeeds independently.
        auto targetTwoTakeover = owner->coordinator().takeoverLease(
            targetTwo,
            "target-2 takeover"
        );
        REQUIRE(targetTwoTakeover.has_value());
        CHECK(targetTwoTakeover->resolvedDispatches == 0U);

        auto targetOneBeforeDispatch = owner->takeoverLease(
            prepared.controller,
            "target-1 takeover before dispatch"
        );
        REQUIRE(targetOneBeforeDispatch.has_value());
        CHECK(targetOneBeforeDispatch->resolvedDispatches == 0U);

        // The fence displaced by that takeover cannot begin the dispatch.
        CHECK_FALSE_MESSAGE(
            owner->coordinator().reserveDispatch(
                ready.operationId,
                ready.revision,
                prepared.lease,
                prepared.observation.generation,
                AuthorityDecisionId{"authority-displaced"},
                std::nullopt
            ).has_value(),
            "the displaced fence must not begin another dispatch"
        );
        auto unanswered = owner->coordinator().reserveDispatch(
            ready.operationId,
            ready.revision,
            targetOneBeforeDispatch->lease,
            prepared.observation.generation,
            AuthorityDecisionId{"authority-target-1"},
            std::nullopt
        );
        REQUIRE(unanswered.has_value());

        // Another target-2 takeover does not resolve target-1's unanswered
        // dispatch. With one target this distinction is unobservable.
        auto targetTwoAgain = owner->coordinator().takeoverLease(
            targetTwo,
            "target-2 takeover while target-1 is in flight"
        );
        REQUIRE(targetTwoAgain.has_value());
        CHECK(targetTwoAgain->resolvedDispatches == 0U);

        // Taking over target-1 afterwards still finds its own dispatch.
        auto targetOneTakeover = owner->takeoverLease(
            prepared.controller,
            "target-1 takeover while its dispatch is in flight"
        );
        REQUIRE(targetOneTakeover.has_value());
        CHECK_MESSAGE(
            targetOneTakeover->resolvedDispatches == 1U,
            "takeover must report the dispatch already in flight"
        );
    }
}
