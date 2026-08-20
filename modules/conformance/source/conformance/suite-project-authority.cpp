// What a ProjectRegistration's five authorities decide, and what no caller can
// decide for them. Every case here runs against whatever project directory the
// run was pointed at.
//
// The plan authority is here too, for the one thing about it that is the
// suite's rather than the Operator's: whether a plan names the UI action this
// run agreed on.

#include "host-delivery-fixture.hpp"
#include "operator-protocol.hpp"
#include "suite-support.hpp"

#include <operator/journal-entry.hpp>
#include <operator/ledger.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>
#include <type_traits>

namespace uf::operator_runtime::conformance
{
    namespace
    {
        // Re-adding any of these members would reopen the holes the authorities
        // exist to close: a reducer input beside the events lets the Journal
        // say A while the materialized state was reduced from B, and a
        // request-owned tool or mutability makes the mutation chain opt-out.
        // The checks go through concepts because a member lookup on a concrete
        // type is an error rather than a substitution failure.
        template <typename T>
        concept NamesReducerInput = requires(T value) { value.reducerInput; };

        template <typename T>
        concept NamesMutability = requires(T value) { value.mutating; };

        template <typename T>
        concept NamesTool = requires(T value) { value.toolName; };

        static_assert(!NamesReducerInput<ReconciliationCommit>);
        static_assert(!NamesReducerInput<ProjectInstanceBaseline>);
        static_assert(!NamesMutability<CommandRequest>);
        static_assert(!NamesTool<CommandRequest>);

        static_assert(!std::is_aggregate_v<ValidatedJournalEntryData>);
        static_assert(!std::is_aggregate_v<ValidatedToolInvocation>);
        static_assert(!std::is_aggregate_v<ValidatedReconcileOutcome>);
        static_assert(!std::is_aggregate_v<ValidatedDocument>);
        static_assert(!std::is_aggregate_v<CanonicalJson>);
    }

    TEST_CASE("the Tool Catalog owns mutability and tool version")
    {
        auto const project     = loadedProject();
        auto const& underTest  = deploymentFor(project, ProjectRole::UnderTest);
        auto const& words      = vocabularyFor(project, ProjectRole::UnderTest);

        auto const mutating = toolInvocation(
            project,
            ProjectRole::UnderTest,
            words.mutatingTool
        );
        CHECK(mutating.descriptor().mutability == ToolMutability::Mutating);
        CHECK(mutating.projectRegistrationHash() == underTest.registration.hash());
        CHECK_FALSE(mutating.descriptor().toolVersion.empty());

        // The descriptor decides, so the read-only tool reaches the same
        // authority through the same call and comes back restricted.
        CHECK(
            toolInvocation(
                project,
                ProjectRole::UnderTest,
                words.readOnlyTool
            ).descriptor().mutability
            == ToolMutability::ReadOnly
        );

        // No such tool, and arguments the descriptor's own schema refuses.
        CHECK_FALSE(underTest.toolCatalogSchemaOwner.validate(
            words.absentTool,
            canonical(project, ProjectRole::UnderTest, words.toolArguments)
        ).has_value());
        CHECK_FALSE(underTest.toolCatalogSchemaOwner.validate(
            words.mutatingTool,
            canonical(project, ProjectRole::UnderTest, words.refusedToolArguments)
        ).has_value());
    }

    TEST_CASE("a schema owner cannot answer for a schema its registration never named")
    {
        auto const project    = loadedProject();
        auto const& underTest = deploymentFor(project, ProjectRole::UnderTest);

        // Every authority takes the exact bytes it answers for, and the hash in
        // the registration is what decides. A validator for some other catalog,
        // journal or reconcile schema therefore has nowhere to attach, however
        // permissive it is.
        CHECK_FALSE(
            ProjectToolCatalogSchemaOwner::create(
                underTest.registration,
                "not-the-tool-catalog",
                []() -> Result<std::vector<ToolCatalogEntry>>
                {
                    return std::vector<ToolCatalogEntry>{
                        ToolCatalogEntry{
                            .name       = "anything",
                            .descriptor = ToolDescriptor{
                                .toolVersion = "1",
                                .mutability  = ToolMutability::ReadOnly,
                            },
                        },
                    };
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            ).has_value()
        );
        CHECK_FALSE(
            ProjectReconcileSchemaOwner::create(
                underTest.registration,
                "not-the-reconcile-manifest",
                [](std::string_view) -> Result<ReconcileDisposition>
                {
                    return ReconcileDisposition::Confirmed;
                }
            ).has_value()
        );
        CHECK_FALSE(
            ProjectJournalSchemaOwner::create(
                underTest.registration,
                "not-the-journal-manifest",
                [](std::string_view, std::string_view) -> Result<ContentHash>
                {
                    return hashOf("anything");
                }
            ).has_value()
        );
    }

    TEST_CASE("authority does not cross ProjectRegistrations")
    {
        auto const project    = loadedProject();
        auto const& underTest = deploymentFor(project, ProjectRole::UnderTest);
        auto const& foreign   = deploymentFor(project, ProjectRole::Foreign);
        REQUIRE(foreign.registration.hash() != underTest.registration.hash());

        // A second registration mints its own documents perfectly well. What it
        // cannot do is have them accepted anywhere the first one is named.
        auto const foreignInvocation = toolInvocation(
            project,
            ProjectRole::Foreign,
            vocabularyFor(project, ProjectRole::Foreign).mutatingTool
        );
        CHECK(
            foreignInvocation.projectRegistrationHash()
            == foreign.registration.hash()
        );
        CHECK(
            foreignInvocation.projectRegistrationHash()
            != underTest.registration.hash()
        );

        auto const foreignEntry = journalEntry(
            project,
            ProjectRole::Foreign,
            vocabularyFor(project, ProjectRole::Foreign).confirmedEntry
        );
        CHECK(
            foreignEntry.projectRegistrationHash()
            != underTest.registration.hash()
        );

        // The plugin is bound the same way: a handle registered under one
        // registration answers only for that one.
        auto const plugin = loadPlugin(project, ProjectRole::UnderTest);
        CHECK(plugin.projectRegistrationHash() == underTest.registration.hash());
        CHECK(
            plugin.pluginModuleManifestHash()
            == underTest.registration.pluginModuleManifestHash()
        );
    }

    TEST_CASE("a ProjectPlugin cannot be registered against foreign bytes")
    {
        auto const project    = loadedProject();
        auto const& underTest = deploymentFor(project, ProjectRole::UnderTest);
        auto const& foreign   = deploymentFor(project, ProjectRole::Foreign);

        auto registrar = ProjectPluginRegistrar{};

        // The registrar derives the exact module manifest it is handed and
        // compares it with the registration, so a foreign closure cannot load.
        CHECK_FALSE(registrar.registerPlugin(
            underTest.registration,
            foreign.pluginEntryModule,
            foreign.pluginModules,
            underTest.projectResources,
            underTest.schemaOwner
        ).has_value());

        // Nor can a schema owner bound to the other registration stand in for
        // this one.
        CHECK_FALSE(registrar.registerPlugin(
            underTest.registration,
            underTest.pluginEntryModule,
            underTest.pluginModules,
            underTest.projectResources,
            foreign.schemaOwner
        ).has_value());

        CHECK(registrar.registerPlugin(
            underTest.registration,
            underTest.pluginEntryModule,
            underTest.pluginModules,
            underTest.projectResources,
            underTest.schemaOwner
        ).has_value());

        // Startup-only: the same exact registration cannot be replaced.
        CHECK_FALSE(registrar.registerPlugin(
            underTest.registration,
            underTest.pluginEntryModule,
            underTest.pluginModules,
            underTest.projectResources,
            underTest.schemaOwner
        ).has_value());
    }

    // The one refusal conformance/operator-protocol.hpp adds to the
    // deployment's step reader. A run drives exactly one UI action, so a plan
    // naming another is telling the suite two different things about what this
    // Operation does, and nothing else would notice: task::DispatchAuthority
    // carries no UI identifier and the ledger stores the intent bytes without
    // reading their surface_id or ui_target_id.
    //
    // ui_target_id is deliberately not one of the rows: the U2c gates own it,
    // and both of them precede this authority in every production path. An id
    // a command's canonical arguments spell is resolved in submitCommand,
    // before the operation row is created and therefore before this plan
    // authority (or the plugin behind it) is consulted at all; the step's own
    // ui_target_id is resolved again in mintNextStep, before the step row is
    // written. So the agreement here covers surface and action and nothing
    // else, and neither gate has been bypassed by the time it runs.
    //
    // The disagreement is made on the run's side because the plan's side is out
    // of reach. A plugin's bytes are the project's and are pinned by the
    // registration's module-manifest identity, so the suite cannot obtain one that answers
    // with a UIActionIntent of the suite's choosing, and a step minted under the
    // foreign project's plugin is refused for the registration it names long
    // before any step is read. The check compares two pairs of strings and is
    // indifferent to which side of a pair moved.
    TEST_CASE("a plan step must name the UI action the run agreed on")
    {
        auto const root   = TemporaryDirectory{"ui-action-agreement"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.underTest.vocabulary;
        auto const agreed = uiActionOf(words);

        auto const proposed = prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-1"),
            toolInvocation(
                prepared.project,
                ProjectRole::UnderTest,
                words.mutatingTool
            )
        );
        REQUIRE(proposed.has_value());

        // Frozen under the authority that agrees, so the only thing the mints
        // below vary is the UI action their own authority was built for.
        auto const frozen = frozenPlan(prepared, proposed->operation);
        REQUIRE(frozen.has_value());

        auto runtimeModel = prepared.observation.host->runtimeModelBinding(
            prepared.observation.generation
        );
        REQUIRE(runtimeModel.has_value());

        // One row per identifier the agreement covers, because the agreement is
        // three comparisons and a check that lost one of them would stay green
        // on the other two.
        struct DisagreeingRun final
        {
            std::string_view        field{};
            task::UiActionUnderTest action{};
        };
        for (auto const& testCase : std::array{
            DisagreeingRun{
                .field  = "surface_id",
                .action = task::UiActionUnderTest{
                    .surface  = agreed.surface + "-elsewhere",
                    .uiTarget = agreed.uiTarget,
                    .action   = agreed.action,
                },
            },
            DisagreeingRun{
                .field  = "action_id",
                .action = task::UiActionUnderTest{
                    .surface  = agreed.surface,
                    .uiTarget = agreed.uiTarget,
                    .action   = agreed.action + "-elsewhere",
                },
            },
        })
        {
            INFO(testCase.field);
            auto authority = planAuthority(
                deploymentFor(
                    prepared.project,
                    ProjectRole::UnderTest
                ).registration,
                prepared.manifest,
                *runtimeModel,
                "operator",
                policyArtifact(
                    deploymentFor(prepared.project, ProjectRole::UnderTest),
                    prepared.project.underTest.vocabulary
                ),
                testCase.action
            );
            REQUIRE(authority.has_value());

            auto const refused = prepared.store.mintNextStep(
                frozen->operation.operationId,
                frozen->operation.revision,
                prepared.lease,
                prepared.plugin,
                deploymentFor(
                    prepared.project,
                    ProjectRole::UnderTest
                ).toolCatalogSchemaOwner,
                *authority
            );
            REQUIRE_FALSE(refused.has_value());

            // The message rather than the kind. The deployment's own step
            // reader answers InvalidResource for every document it refuses, so
            // a case reading only the kind would pass just as well on a step
            // this suite never judged.
            CHECK(refused.error().message().contains(
                "names a UI action other than the one this run agreed on"
            ));
        }

        // The same Operation, at the same revision, mints under the authority
        // that agrees. Without this, every refusal above would be explained just
        // as well by an Operation that had stopped being mintable at all.
        auto const minted = plannedStep(prepared, frozen->operation);
        REQUIRE(minted.has_value());
    }
}
