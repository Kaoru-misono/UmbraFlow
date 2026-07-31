#include "test-helpers.hpp"

#include <target-setup.hpp>

#include <controller/discovery.hpp>
#include <controller/target.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>

namespace uf::input_agent
{
    TEST_CASE("target revalidation requires an unchanged identity")
    {
        CHECK(
            requireUnchangedTarget(
                RevalidateOutcome::Unchanged
            ).has_value()
        );

        for (auto const outcome : std::array{
            RevalidateOutcome::GenerationBumped,
            RevalidateOutcome::InstanceUnconfirmed,
        })
        {
            auto const result = requireUnchangedTarget(outcome);
            REQUIRE_FALSE(result.has_value());
            test_input_agent::requireErrorKind(
                result.error(),
                AutomationErrorKind::StaleObservation
            );
        }

        auto const lost = requireUnchangedTarget(RevalidateOutcome::Lost);
        REQUIRE_FALSE(lost.has_value());
        test_input_agent::requireErrorKind(
            lost.error(),
            AutomationErrorKind::ControllerDisconnected
        );
    }

    TEST_CASE("empty client area is target unavailable")
    {
        auto const emptySizes = std::array{
            ClientSize{0, 480},
            ClientSize{640, 0},
            ClientSize{0, 0},
        };
        for (auto const size : emptySizes)
        {
            auto const result = ensureClientAreaUsable(size);
            REQUIRE_FALSE(result.has_value());
            test_input_agent::requireErrorKind(
                result.error(),
                AutomationErrorKind::TargetUnavailable
            );
        }
        CHECK(ensureClientAreaUsable(ClientSize{640, 480}).has_value());
    }
}
