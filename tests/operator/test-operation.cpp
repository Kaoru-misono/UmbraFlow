#include <operator/operation.hpp>

#include <doctest/doctest.h>

TEST_CASE("first dispatch freezes the plan and every Host outcome reconciles")
{
    using namespace uf::operator_runtime;

    auto operation = OperationMachine{};
    REQUIRE(operation.transition(OperationEvent::ReadyWithoutApproval).has_value());
    REQUIRE(operation.transition(OperationEvent::DispatchStarted).has_value());
    CHECK(operation.planFrozen());
    CHECK(operation.hasDispatched());
    CHECK(operation.mutationLocked());
    REQUIRE(operation.transition(OperationEvent::HostOutcomeObserved).has_value());
    CHECK(operation.state() == OperationState::Reconciling);
}

TEST_CASE("post-dispatch approval wait aborts only through reconciliation")
{
    using namespace uf::operator_runtime;

    auto operation = OperationMachine{};
    REQUIRE(operation.transition(OperationEvent::ReadyWithoutApproval).has_value());
    REQUIRE(operation.transition(OperationEvent::DispatchStarted).has_value());
    REQUIRE(operation.transition(OperationEvent::HostOutcomeObserved).has_value());
    REQUIRE(operation.transition(OperationEvent::NextStepApprovalRequired).has_value());
    REQUIRE(operation.transition(OperationEvent::PostDispatchAbort).has_value());
    CHECK(operation.state() == OperationState::Reconciling);
    CHECK(operation.mutationLocked());
}

TEST_CASE("terminal operations cannot be revived")
{
    using namespace uf::operator_runtime;

    auto operation = OperationMachine{};
    REQUIRE(operation.transition(OperationEvent::Cancelled).has_value());
    CHECK(operation.terminal());
    CHECK_FALSE(operation.transition(OperationEvent::Revalidated).has_value());
}

TEST_CASE("ambiguous reconciliation keeps the mutation chain locked")
{
    using namespace uf::operator_runtime;

    auto operation = OperationMachine{};
    REQUIRE(operation.transition(OperationEvent::ReadyWithoutApproval).has_value());
    REQUIRE(operation.transition(OperationEvent::DispatchStarted).has_value());
    REQUIRE(operation.transition(OperationEvent::HostOutcomeObserved).has_value());
    REQUIRE(operation.transition(OperationEvent::ReconciliationAmbiguous).has_value());
    CHECK(operation.state() == OperationState::Ambiguous);
    CHECK_FALSE(operation.terminal());
    CHECK(operation.mutationLocked());
    REQUIRE(operation.transition(OperationEvent::NewEvidence).has_value());
    CHECK(operation.state() == OperationState::Reconciling);
}
