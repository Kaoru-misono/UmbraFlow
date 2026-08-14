#include <service/product-lifecycle.hpp>

#include <operator/ledger.hpp>

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include "project-fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace uf::service
{
    namespace
    {
        using operator_runtime::AuthorityDecisionId;
        using operator_runtime::ProjectInstanceBaseline;
        using operator_runtime::SessionMode;
        using operator_runtime::SessionPin;
        using operator_runtime::ControllerKind;
        using operator_runtime::test_support::createReadyOperation;
        using operator_runtime::test_support::addController;
        using operator_runtime::test_support::k_unconstrainedAgentBudget;
        using operator_runtime::test_support::observeAgain;
        using operator_runtime::test_support::prepareStore;
        using operator_runtime::test_support::TemporaryDirectory;

        [[nodiscard]]
        auto recoverUnfinishedDispatch(std::filesystem::path const& root)
            -> std::vector<operator_runtime::RecoveredUncertainDispatch>
        {
            {
                auto prepared = prepareStore(root);
                auto const ready = createReadyOperation(
                    prepared,
                    "lifecycle-restart",
                    "command-1"
                );
                auto const reserved = prepared.store.reserveDispatch(
                    ready.operationId,
                    ready.revision,
                    prepared.lease,
                    prepared.observation.generation,
                    AuthorityDecisionId{"lifecycle-restart-authority"},
                    std::nullopt
                );
                REQUIRE(reserved.has_value());
            }

            auto restarted = operator_runtime::OperatorCoordinator::open(
                root / "production"
            );
            REQUIRE(restarted.has_value());
            auto recoveries = restarted->recoveredUncertainDispatches();
            REQUIRE(recoveries.has_value());
            return *std::move(recoveries);
        }
    }

    TEST_CASE("a failed close does not displace the failure a caller must act on")
    {
        auto work = Result<std::string>{
            fail(AutomationErrorKind::InvalidResource, "the observation refused"),
        };
        auto closed = Status{
            fail(AutomationErrorKind::IoFailure, "the lease could not be released"),
        };

        auto const reported = reportAfterClose(std::move(work), std::move(closed));
        REQUIRE_FALSE(reported.has_value());
        CHECK(
            automationErrorKind(reported.error())
            == AutomationErrorKind::InvalidResource
        );
        CHECK(reported.error().message() == "the observation refused");

        // Kept, not merged away: a cleanup failure that left no trace would be
        // a lease nobody can find out was not released.
        auto const notes = reported.error().context();
        CHECK_MESSAGE(
            std::ranges::any_of(
                notes,
                [](std::string const& note)
                {
                    return note.contains("the lease could not be released");
                }
            ),
            "the close failure must survive on the reported error"
        );
    }

    TEST_CASE("a failed close that stands alone is the failure that is reported")
    {
        auto closed = Status{
            fail(AutomationErrorKind::IoFailure, "the lease could not be released"),
        };
        auto const reported = reportAfterClose(
            Result<std::string>{"the observation succeeded"},
            std::move(closed)
        );
        REQUIRE_FALSE(reported.has_value());
        CHECK(
            automationErrorKind(reported.error())
            == AutomationErrorKind::IoFailure
        );
        CHECK(reported.error().message() == "the lease could not be released");

        auto const clean = reportAfterClose(
            Result<std::string>{"the observation succeeded"},
            ok()
        );
        REQUIRE(clean.has_value());
        CHECK(*clean == "the observation succeeded");
    }

    TEST_CASE("lifecycle restart with unfinished state is read-only")
    {
        auto temporary = TemporaryDirectory{};
        auto const recoveries = recoverUnfinishedDispatch(temporary.path());
        REQUIRE_FALSE(recoveries.empty());
        CHECK_MESSAGE(
            lifecycleAccessAfterRestart(recoveries)
                == LifecycleAccess::ReadOnly,
            "unfinished restart recovery must expose read-only access"
        );
    }

    TEST_CASE("lifecycle no-baseline ruling reduces an empty Journal to first state")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
        auto const provisioned = prepared.store.provisionProjectInstance(
            prepared.project.registration,
            prepared.plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-without-baseline",
                .eventId             = {},
                .sessionManifestHash = prepared.manifest.hash(),
                .entry               = std::nullopt,
            }
        );
        auto const provisionWhy = provisioned.has_value()
            ? std::string{}
            : provisioned.error().message();
        REQUIRE_MESSAGE(
            provisioned.has_value(),
            "no-baseline provisioning must reduce an empty Journal: ",
            provisionWhy
        );
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = "session-without-baseline",
                .authenticatedControllerId = "controller-without-baseline",
                .idempotencyNamespace      = "controller-without-baseline",
                .projectRegistrationHash   = prepared.project.registration.hash(),
                .controllerCapabilities    = {},
                .controlledTargetId        = "target-without-baseline",
                .projectInstanceKey        = "instance-without-baseline",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Human,
            },
            prepared.manifest,
            std::nullopt
        ).has_value());
        auto controller = prepared.store.bindController("session-without-baseline");
        REQUIRE(controller.has_value());
        auto lease = prepared.store.acquireLease(*controller);
        REQUIRE(lease.has_value());
        auto snapshot = prepared.store.createSnapshot(
            *lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            observeAgain(prepared)
        );
        CHECK(snapshot.has_value());
    }

    TEST_CASE("lifecycle tool list comes from U8 offer side")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const agent = addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "agent-offer-session",
            "agent-offer-instance",
            "agent-offer-target",
            k_unconstrainedAgentBudget
        );

        auto const offered = offeredProductTools(
            prepared.store,
            agent,
            prepared.project.toolCatalogSchemaOwner
        );
        REQUIRE(offered.has_value());
        REQUIRE_FALSE(offered->empty());

        auto const privileged = prepared.project.toolCatalogSchemaOwner.describe(
            "raw-coordinate-click"
        );
        REQUIRE(privileged.has_value());
        CHECK_MESSAGE(
            std::ranges::none_of(
                *offered,
                [](operator_runtime::OfferedTool const& tool)
                {
                    return tool.name == "raw-coordinate-click";
                }
            ),
            "the facade tool list must come from U8's session-aware offer side"
        );
    }
}
