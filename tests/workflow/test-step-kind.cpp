#include "project-fixture.hpp"

#include <doctest/doctest.h>

#include <string>

namespace uf::operator_runtime
{
    namespace test_support
    {
        TEST_CASE("a wait step stays in reconciliation and needs no dispatch")
        {
            auto temporary = TemporaryDirectory{};
            auto prepared  = prepareStore(
                temporary.path(),
                "fixture.control",
                k_fixtureUiThenWaitIntent
            );
            auto const operation = reconcilingOperation(
                prepared,
                "wait-sequence",
                task::DeliveryOutcome::Delivered
            );
            REQUIRE(operation.state == OperationState::Reconciling);

            auto const step = mintStepFor(prepared, operation);
            auto diagnostic = std::string{};
            if (!step.has_value())
            {
                diagnostic = step.error().message();
            }
            INFO(diagnostic);
            CHECK_MESSAGE(
                step.has_value(),
                "a schema-valid WaitIntent must reach the ledger as a wait step"
            );
            REQUIRE(step.has_value());
            CHECK(step->kind == StepKind::Wait);
            CHECK_MESSAGE(
                step->operation.state == OperationState::Reconciling,
                "only a UI-action step may advance an Operation out of reconciliation"
            );
        }
    }
}
