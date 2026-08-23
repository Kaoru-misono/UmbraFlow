// What the Operator's ledger enforces around a project: one control authority
// per controlled target, and a lease that loses it the moment another
// controller takes over.
//
// Durable idempotency used to be proved here, against the Operation ledger.
// That ledger is gone; the property now belongs to the Tool root request and is
// proved in suite-tool-runtime.cpp against ToolRootRequestIdentity::relationTo,
// which is where the caller-facing key actually lives.

#include "suite-support.hpp"


#include <operator/ledger.hpp>

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
}
