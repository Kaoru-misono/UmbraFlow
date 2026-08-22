// What the Operator's ledger enforces around a project: one linearization per
// controlled target, one dispatch per Operation, one reconciliation authority,
// and a reducer input nobody outside the Operator can choose.

#include "suite-support.hpp"


#include <operator/ledger.hpp>
#include <operator/operation.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace uf::operator_runtime::conformance
{
    TEST_CASE("contract-control-c01")
    {
        auto const root = TemporaryDirectory{"c01"};
        auto prepared   = prepareStore(root.path());

        auto const takeover = prepared.store.takeoverLease(
            prepared.controller,
            "human takeover"
        );
        REQUIRE(takeover.has_value());
        CHECK(takeover->lease.fencingToken > prepared.lease.fencingToken);


        // The displaced lease keeps its value and loses its authority, which is
        // the only difference that matters after a takeover.
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            ProjectIdentity{deploymentFor(prepared.project, ProjectRole::UnderTest).generation},
            deploymentFor(
                prepared.project,
                ProjectRole::UnderTest
            ).toolCatalogSchemaOwner,
            deploymentFor(
                prepared.project,
                ProjectRole::UnderTest
            ).observedInstanceIdentitySchemas,
            observeAgain(prepared)
        ).has_value());
    }

    TEST_CASE("contract-control-c06")
    {
        auto const root   = TemporaryDirectory{"c06"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.underTest.vocabulary;

        auto const request  = command(prepared.snapshot, "request-1");
        auto const first    = prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, ProjectRole::UnderTest, words.mutatingTool)
        );
        auto const repeated = prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, ProjectRole::UnderTest, words.mutatingTool)
        );
        REQUIRE(first.has_value());
        REQUIRE(repeated.has_value());
        CHECK(first->operation.lookup == CommandLookup::Created);
        CHECK(repeated->operation.lookup == CommandLookup::Existing);
        CHECK(first->operation.operationId == repeated->operation.operationId);
        CHECK(first->commandFingerprint == repeated->commandFingerprint);

        // Durable idempotency is by request identity, so the same identity
        // carrying a different command is a conflict rather than a second
        // Operation.
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(
                prepared.project,
                ProjectRole::UnderTest,
                words.otherMutatingTool
            )
        ).has_value());
    }

}
