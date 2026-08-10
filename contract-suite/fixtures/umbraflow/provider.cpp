// This repository's own project, supplied to the exported contract suite the
// way a consuming repository supplies its game.
//
// project-fixture.hpp sits beside this file rather than under tests/, so the
// only dependency runs from tests/ into the suite and never back. It is one
// registration serving both: this provider adapts it, and the Operator's own
// tests include it through the directory tests/CMakeLists.txt adds. A second
// spelling of the same registration would be a second thing to keep true.

#include <operator-contract/project-under-test.hpp>

#include "project-fixture.hpp"

#include <doctest/doctest.h>

#include <string>
#include <utility>

namespace uf::operator_runtime::contract
{
    namespace
    {
        // This project's reconcile is the identity, so its disposition is
        // spelled in the request. Nothing in the suite may rely on that; the
        // second fixture deliberately maps the two apart.
        [[nodiscard]]
        auto vocabulary() -> ProjectVocabulary
        {
            return ProjectVocabulary{
                .mutatingTool         = "command-1",
                .otherMutatingTool    = "command-2",
                .readOnlyTool         = "observe-1",
                .toolArguments        = "{\"value\":1}",
                .refusedToolArguments = "{\"value\":2}",
                .absentTool           = "not-in-the-catalogue",
                .baselineEntry        = JournalDocument{
                    .eventType = "fixture.baseline",
                    .payload   = "{\"kind\":\"baseline\"}",
                },
                .progressEntry = JournalDocument{
                    .eventType = "fixture.progress",
                    .payload   = "{\"value\":1}",
                },
                .confirmedEntry = JournalDocument{
                    .eventType = "fixture.confirmed",
                    .payload   = "{\"value\":2}",
                },
                .supersededEntry = JournalDocument{
                    .eventType = "fixture.duplicate",
                    .payload   = "{\"value\":3}",
                },
                .provenance     = "{\"kind\":\"fixture\"}",
                .continueInput  = "{\"disposition\":\"continue\"}",
                .confirmedInput = "{\"disposition\":\"confirmed\"}",
                .rejectedInput  = "{\"disposition\":\"rejected\"}",
                .ambiguousInput = "{\"disposition\":\"ambiguous\"}",

                // One more mutating tool, told apart by the plan this project's
                // plugin answers it with.
                .approvalRequiredPlanTool = "approval-plan",
            };
        }
    }

    auto projectUnderTest(ProjectRole role) -> ProjectUnderTest
    {
        // Two registrations of the same shape under two plugin ids, so the
        // foreign one can mint documents of its own rather than merely failing
        // to load.
        auto const pluginId = role == ProjectRole::UnderTest
            ? std::string{"fixture.alpha"}
            : std::string{"fixture.foreign"};
        auto const pluginBytes = test_support::pluginSource(pluginId);

        auto fixture = test_support::makeProject(pluginId, pluginBytes);
        REQUIRE(fixture.lastReduceInput != nullptr);
        REQUIRE(fixture.lastDeriveInput != nullptr);
        return ProjectUnderTest{
            .registration           = std::move(fixture.registration),
            .schemaOwner            = std::move(fixture.schemaOwner),
            .journalSchemaOwner     = std::move(fixture.journalSchemaOwner),
            .toolCatalogSchemaOwner = std::move(fixture.toolCatalogSchemaOwner),
            .reconcileSchemaOwner   = std::move(fixture.reconcileSchemaOwner),
            .pluginBytes            = pluginBytes,
            .artifactBlobs          = {},
            .observedReduceInput    = std::move(fixture.lastReduceInput),
            .observedDeriveInput    = std::move(fixture.lastDeriveInput),
            .vocabulary             = vocabulary(),
        };
    }
}
