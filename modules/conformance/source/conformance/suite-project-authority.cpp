// What a ProjectRegistration's authorities decide, and what no caller can
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
#include <operator/tool-admission-request.hpp>
#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

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

        static_assert(!NamesReducerInput<ProjectInstanceBaseline>);
        static_assert(!NamesMutability<ToolAdmissionRequest>);
        static_assert(!NamesTool<ToolAdmissionRequest>);

        static_assert(!std::is_aggregate_v<ValidatedJournalEntryData>);
        static_assert(!std::is_aggregate_v<ValidatedToolInvocation>);
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
        REQUIRE(std::holds_alternative<ProjectToolProvider>(mutating.provider()));
        CHECK(
            std::get<ProjectToolProvider>(mutating.provider())
                    .projectRegistrationHash
            == ProjectIdentity{underTest.generation}.hash()
        );
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
        // the registration is what decides. A validator for some other catalog
        // or journal schema therefore has nowhere to attach, however permissive
        // it is.
        CHECK_FALSE(
            ProjectToolCatalogSchemaOwner::create(
                ProjectIdentity{underTest.generation},
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
            ProjectJournalSchemaOwner::create(
                ProjectIdentity{underTest.generation},
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
        REQUIRE(ProjectIdentity{foreign.generation}.hash() != ProjectIdentity{underTest.generation}.hash());

        // A second registration mints its own documents perfectly well. What it
        // cannot do is have them accepted anywhere the first one is named.
        auto const foreignInvocation = toolInvocation(
            project,
            ProjectRole::Foreign,
            vocabularyFor(project, ProjectRole::Foreign).mutatingTool
        );
        REQUIRE(
            std::holds_alternative<ProjectToolProvider>(
                foreignInvocation.provider()
            )
        );
        auto const& foreignProvider = std::get<ProjectToolProvider>(
            foreignInvocation.provider()
        );
        CHECK(
            foreignProvider.projectRegistrationHash == ProjectIdentity{foreign.generation}.hash()
        );
        CHECK(
            foreignProvider.projectRegistrationHash != ProjectIdentity{underTest.generation}.hash()
        );

        auto const foreignEntry = journalEntry(
            project,
            ProjectRole::Foreign,
            vocabularyFor(project, ProjectRole::Foreign).confirmedEntry
        );
        CHECK(
            foreignEntry.projectRegistrationHash()
            != ProjectIdentity{underTest.generation}.hash()
        );

        // The loaded generation is bound the same way: a handle registered
        // under one registration answers only for that one, and its reducer
        // closure digest is the one that registration pinned.
        auto const generation = loadGeneration(
            project,
            ProjectRole::UnderTest,
            provisioningToolRuntime()
        );
        CHECK(
            generation.projectRegistrationHash()
            == ProjectIdentity{underTest.generation}.hash()
        );
        CHECK(
            generation.reducerModuleManifestHash()
            == underTest.generation.reducerClosure().moduleManifestHash
        );
    }

    // The flip replaced the code that produces a ProjectInstance's baseline, so
    // the question every stored baseline poses is whether the new reducer
    // answers what the stored bytes say. This case asks it the only way that
    // means anything: the Journal prefix is read back OUT of the database, put
    // back through the schemas this registration pinned, and folded again --
    // and the answer is compared with the bytes the provisioning transaction
    // wrote, which nothing in this case supplied.
    //
    // It is a conformance obligation rather than a test because the subject is
    // a consumer's own store: a project directory this run was pointed at, the
    // reducer it ships, and the baseline it was provisioned with.
    TEST_CASE("a stored baseline is the fold of the Journal prefix beside it")
    {
        auto const root = TemporaryDirectory{"refold"};
        auto prepared   = prepareStore(root.path());
        auto const& underTest = deploymentFor(prepared.project, ProjectRole::UnderTest);

        auto const refolded = prepared.store.refoldProjectState(
            ProjectIdentity{underTest.generation},
            underTest.journalSchemaOwner,
            prepared.generation,
            "instance-1"
        );
        auto const refusal = (
            refolded.has_value()
                ? std::string{}
                : std::string{refolded.error().message()}
        );
        REQUIRE_MESSAGE(refolded.has_value(), refusal);

        // The prefix must have something in it. An equality over an empty
        // prefix would be satisfied by a reducer that ignored its input, so the
        // count is asserted before the bytes are.
        CHECK_MESSAGE(
            refolded->journalEventCount == 1U,
            "the provisioned instance's Journal prefix must hold its baseline "
            "event, or the equality below is over nothing"
        );
        CHECK_MESSAGE(
            refolded->refoldedCanonicalPayload == refolded->storedCanonicalPayload,
            "the ProjectState stored at provisioning must be byte-identical to "
            "the fold of the Journal prefix the database still holds"
        );
        CHECK_MESSAGE(
            refolded->refoldedStateHash == refolded->storedStateHash,
            "the stored state_hash must be the digest of the refolded bytes"
        );

        // The positive control. A reducer belonging to another registration is
        // refused rather than answering a different fold, which is what makes
        // the equality above a fact about THIS project's reducer and not about
        // any reducer at all.
        //
        // The refusal is asked for BY NAME rather than by outcome. A foreign
        // reducer also fails the stamp check further down, so "it was refused"
        // alone would stay true with the identity comparison deleted -- and a
        // check that cannot be made to fail is not a check. Naming the reducer
        // in the sentence is what makes the earlier refusal the one measured,
        // and the ordering matters: it refuses before another project's VM is
        // ever entered.
        auto const foreign = prepared.store.refoldProjectState(
            ProjectIdentity{underTest.generation},
            underTest.journalSchemaOwner,
            loadGeneration(
                prepared.project,
                ProjectRole::Foreign,
                provisioningToolRuntime()
            ),
            "instance-1"
        );
        REQUIRE_FALSE_MESSAGE(
            foreign.has_value(),
            "a refold offered another registration's reducer must be refused"
        );
        CHECK_MESSAGE(
            foreign.error().message().contains("reducer"),
            "the refusal must be the reducer identity comparison, which runs "
            "before the foreign closure is executed at all"
        );
    }

}
