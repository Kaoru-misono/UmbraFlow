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
        // Re-adding either member would reopen the hole the authorities exist
        // to close: a request-owned tool or mutability makes the mutation chain
        // opt-out. The checks go through concepts because a member lookup on a
        // concrete type is an error rather than a substitution failure.
        template <typename T>
        concept NamesMutability = requires(T value) { value.mutating; };

        template <typename T>
        concept NamesTool = requires(T value) { value.toolName; };

        static_assert(!NamesMutability<ToolAdmissionRequest>);
        static_assert(!NamesTool<ToolAdmissionRequest>);

        static_assert(!std::is_aggregate_v<ValidatedToolInvocation>);
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
            canonical(words.toolArguments)
        ).has_value());
        CHECK_FALSE(underTest.toolCatalogSchemaOwner.validate(
            words.mutatingTool,
            canonical(words.refusedToolArguments)
        ).has_value());
    }

    TEST_CASE("a schema owner cannot answer for a schema its registration never named")
    {
        auto const project    = loadedProject();
        auto const& underTest = deploymentFor(project, ProjectRole::UnderTest);

        // Every authority takes the exact bytes it answers for, and the hash in
        // the registration is what decides. A validator for some other Tool
        // declaration therefore has nowhere to attach, however permissive it
        // is.
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

        // The loaded generation is bound the same way: a handle registered
        // under one registration answers only for that one, and its closure
        // digest is the one that registration pinned.
        auto const generation = loadGeneration(
            project,
            ProjectRole::UnderTest
        );
        CHECK(
            generation.projectRegistrationHash()
            == ProjectIdentity{underTest.generation}.hash()
        );
        CHECK(
            generation.toolModuleManifestHash()
            == underTest.generation.toolClosure().moduleManifestHash
        );
    }


}
