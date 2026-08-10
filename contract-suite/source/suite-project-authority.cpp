// What a ProjectRegistration's five authorities decide, and what no caller can
// decide for them. Every case here runs against whatever project the consuming
// repository supplied through projectUnderTest.

#include "harness.hpp"

#include <operator-contract/project-under-test.hpp>

#include <operator/journal-entry.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <string_view>
#include <type_traits>

namespace uf::operator_runtime::contract
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
        auto const project = projectUnderTest(ProjectRole::UnderTest);
        auto const& words  = project.vocabulary;

        auto const mutating = toolInvocation(project, words.mutatingTool);
        CHECK(mutating.mutability() == ToolMutability::Mutating);
        CHECK(mutating.projectRegistrationHash() == project.registration.hash());
        CHECK(mutating.toolCatalogHash() == project.registration.toolCatalogHash());
        CHECK_FALSE(mutating.toolVersion().empty());

        // The descriptor decides, so the read-only tool reaches the same
        // authority through the same call and comes back restricted.
        CHECK(
            toolInvocation(project, words.readOnlyTool).mutability()
            == ToolMutability::ReadOnly
        );

        // No such tool, and arguments the descriptor's own schema refuses.
        CHECK_FALSE(project.toolCatalogSchemaOwner.validate(
            words.absentTool,
            canonical(project, words.toolArguments)
        ).has_value());
        CHECK_FALSE(project.toolCatalogSchemaOwner.validate(
            words.mutatingTool,
            canonical(project, words.refusedToolArguments)
        ).has_value());
    }

    TEST_CASE("a schema owner cannot answer for a schema its registration never named")
    {
        auto const project = projectUnderTest(ProjectRole::UnderTest);

        // Every authority takes the exact bytes it answers for, and the hash in
        // the registration is what decides. A validator for some other catalog,
        // journal or reconcile schema therefore has nowhere to attach, however
        // permissive it is.
        CHECK_FALSE(
            ProjectToolCatalogSchemaOwner::create(
                project.registration,
                "not-the-tool-catalog",
                [](std::string_view, std::string_view) -> Result<ToolDescriptor>
                {
                    return ToolDescriptor{
                        .toolVersion = "1",
                        .mutability  = ToolMutability::ReadOnly,
                    };
                }
            ).has_value()
        );
        CHECK_FALSE(
            ProjectReconcileSchemaOwner::create(
                project.registration,
                "not-the-reconcile-manifest",
                [](std::string_view) -> Result<ReconcileDisposition>
                {
                    return ReconcileDisposition::Confirmed;
                }
            ).has_value()
        );
        CHECK_FALSE(
            ProjectJournalSchemaOwner::create(
                project.registration,
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
        auto const project = projectUnderTest(ProjectRole::UnderTest);
        auto const foreign = projectUnderTest(ProjectRole::Foreign);
        REQUIRE(foreign.registration.hash() != project.registration.hash());

        // A second registration mints its own documents perfectly well. What it
        // cannot do is have them accepted anywhere the first one is named.
        auto const foreignInvocation = toolInvocation(
            foreign,
            foreign.vocabulary.mutatingTool
        );
        CHECK(
            foreignInvocation.projectRegistrationHash()
            == foreign.registration.hash()
        );
        CHECK(
            foreignInvocation.projectRegistrationHash()
            != project.registration.hash()
        );

        auto const foreignEntry = journalEntry(
            foreign,
            foreign.vocabulary.confirmedEntry
        );
        CHECK(
            foreignEntry.projectRegistrationHash()
            != project.registration.hash()
        );

        // The plugin is bound the same way: a handle registered under one
        // registration answers only for that one.
        auto const plugin = loadPlugin(project);
        CHECK(plugin.projectRegistrationHash() == project.registration.hash());
        CHECK(plugin.pluginHash() == project.registration.pluginHash());
    }

    TEST_CASE("a ProjectPlugin cannot be registered against foreign bytes")
    {
        auto const project = projectUnderTest(ProjectRole::UnderTest);
        auto const foreign = projectUnderTest(ProjectRole::Foreign);

        auto registrar = ProjectPluginRegistrar{};

        // The registrar hashes the bytes it is handed and compares them with
        // the plugin_hash the registration pinned, so the other project's
        // plugin cannot be loaded under this registration.
        CHECK_FALSE(registrar.registerPlugin(
            project.registration,
            foreign.pluginBytes,
            project.artifactBlobs,
            project.schemaOwner
        ).has_value());

        // Nor can a schema owner bound to the other registration stand in for
        // this one.
        CHECK_FALSE(registrar.registerPlugin(
            project.registration,
            project.pluginBytes,
            project.artifactBlobs,
            foreign.schemaOwner
        ).has_value());

        CHECK(registrar.registerPlugin(
            project.registration,
            project.pluginBytes,
            project.artifactBlobs,
            project.schemaOwner
        ).has_value());

        // Startup-only: the same exact registration cannot be replaced.
        CHECK_FALSE(registrar.registerPlugin(
            project.registration,
            project.pluginBytes,
            project.artifactBlobs,
            project.schemaOwner
        ).has_value());
    }
}
