#include <service/product-lifecycle.hpp>

#include <operator/ledger.hpp>

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include "project-fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

namespace uf::service
{
    namespace
    {
        using operator_runtime::SessionMode;
        using operator_runtime::ControllerKind;
        using operator_runtime::test_support::addController;
        using operator_runtime::test_support::k_unconstrainedAgentBudget;
        using operator_runtime::test_support::observeAgain;
        using operator_runtime::test_support::prepareStore;
        using operator_runtime::test_support::TemporaryDirectory;
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

        // The composition of the snapshot is the only mint of the offered
        // set: the U8 offer side is read off the observed world, not off a
        // second list kept beside it.
        auto const lease = prepared.store.acquireLease(agent);
        REQUIRE(lease.has_value());
        auto const snapshot = prepared.store.createSnapshot(
            *lease,
            prepared.project.registration,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            observeAgain(prepared)
        );
        REQUIRE(snapshot.has_value());
        REQUIRE_FALSE(snapshot->availableTools.empty());

        auto const privilegedName = prepared.project.toolName("raw-coordinate-click");
        auto const privileged = prepared.project.toolCatalogSchemaOwner.describe(
            privilegedName
        );
        REQUIRE(privileged.has_value());
        CHECK_MESSAGE(
            std::ranges::none_of(
                snapshot->availableTools,
                [&privilegedName](operator_runtime::OfferedTool const& tool)
                {
                    return tool.name == privilegedName;
                }
            ),
            "the facade tool list must come from U8's session-aware offer side"
        );
    }
}
