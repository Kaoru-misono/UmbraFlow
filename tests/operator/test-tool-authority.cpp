// The Tool call identity seam, the Tool Catalog's own declarations, and the
// PolicyArtifact a session is pinned to.
//
// What the descriptor's effect, UI-action, workflow and timeout bounds meant
// for a frozen plan died with the plan: those clauses were reached only through
// freezePlan and mintNextStep, and a Project of the two-closure generation
// exports no plan or next_step for them to run. What survives here is what an
// Operation never owned -- the call identity builder, the catalog's namespace
// rule, and the artifact a plan authority is built from -- and the Tool
// Runtime's own bounds are asserted in tests/operator/test-tool-executor.cpp.
// The p03 offer side is asserted in contract-product-p03, where the requirement
// it serves is traced.

#include <operator/effective-plan.hpp>
#include <operator/ledger.hpp>
#include <operator/policy.hpp>
#include <operator/project-generation.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-invocation.hpp>

#include <deployment/project-deployment.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        using test_support::toolCallAt;

        using test_support::prepareStore;
        using test_support::TemporaryDirectory;

        template <typename Extra>
        concept AdditionalToolCallIdentityInputAccepted = requires(
            ToolCallIssuingContext& context,
            ValidatedToolInvocation const& invocation,
            Extra const& extra
        ) { context.issue(invocation, extra); };

        static_assert(
            !AdditionalToolCallIdentityInputAccepted<CanonicalJson>,
            "Tool results and other canonical outcome material must not enter "
            "call identity"
        );

        // The seam owns the child index, so there is no factory a caller can
        // hand an ordinal to at all. R4 makes that structural rather than
        // documented, and this is the compile-time reading of it.
        template <typename Ordinal>
        concept CallerSuppliedSequenceAccepted = requires(
            ToolCallIssuingContext& context,
            ValidatedToolInvocation const& invocation,
            Ordinal ordinal
        ) { context.issue(invocation, ordinal); };

        static_assert(
            !CallerSuppliedSequenceAccepted<uint64>,
            "The Tool Runtime seam assigns the child index; no caller may pass "
            "one"
        );

        // The fixture's PolicyArtifact with one clause rewritten wherever it
        // appears. Every occurrence, because both of its rules carry the same
        // selector and the same required capabilities: rewriting one would
        // leave the other matching, and the case would be about a rule it did
        // not mean. A rewrite that matched nothing would leave the artifact
        // unchanged and prove nothing, which is what the REQUIRE refuses.
        [[nodiscard]]
        auto rewrittenPolicy(
            std::string_view from,
            std::string_view to
        ) -> std::string
        {
            auto const policy = test_support::policyArtifactBytes();
            auto rewritten = std::string{};
            auto rest      = std::string_view{policy};
            auto replaced  = 0U;
            while (true)
            {
                auto const at = rest.find(from);
                if (at == std::string_view::npos)
                {
                    rewritten += rest;
                    break;
                }
                rewritten += rest.substr(0U, at);
                rewritten += to;
                rest.remove_prefix(at + from.size());
                ++replaced;
            }
            REQUIRE(replaced > 0U);
            return rewritten;
        }
    }

    // The session's operator_protocol_schema_hash is the one pinned value on
    // this seam whose two sides can be supplied independently: the manifest is
    // minted by one caller and the exact schema bytes and the PolicyArtifact
    // reach the authority as separate arguments. Both refusals are shown here
    // because the composition root that exists today derives all three from one
    // local, and a check only that root can reach would read as one no caller
    // can fail.
    TEST_CASE("a plan authority refuses an operator protocol its session did not pin")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto runtimeModel = prepared.observation.host->runtimeModelBinding(
            prepared.observation.generation
        );
        REQUIRE(runtimeModel.has_value());

        SUBCASE("the exact schema bytes must hash to what the manifest pinned")
        {
            auto const refused = OperatorPlanAuthority::create(
                prepared.project.registration,
                prepared.manifest,
                *runtimeModel,
                "a-second-operator-protocol",
                test_support::policyArtifactBytes()
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "Operator protocol schema bytes do not match the pinned session manifest"
            ));
        }

        SUBCASE("the PolicyArtifact must answer for the same operator protocol")
        {
            // The manifest pins a second protocol and the exact bytes handed in
            // are that one, so the check above is satisfied and the policy's own
            // claim is the only thing left that can disagree.
            auto const policyBytes = test_support::policyArtifactBytes();
            auto const manifest    = SessionManifest::create(SessionManifestSpec{
                .runtimeModelArtifactRootHash = runtimeModel->artifactRootHash(),
                .operatorProtocolSchemaHash = test_support::hashOf("a-second-operator-protocol"),
                .projectRegistrationHash    = prepared.project.registration.hash(),
                .policyArtifactHash         = test_support::hashOf(policyBytes),
                .agentProfileHash           = test_support::hashOf("agent"),
            });
            REQUIRE(manifest.has_value());

            auto const refused = OperatorPlanAuthority::create(
                prepared.project.registration,
                *manifest,
                *runtimeModel,
                "a-second-operator-protocol",
                policyBytes
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "PolicyArtifact answers for an operator protocol schema this "
                "session manifest does not pin"
            ));
        }
    }

    TEST_CASE("Framework and Project tools share one validated invocation type")
    {
        auto frameworkCatalog = FrameworkToolCatalogOwner::create();
        REQUIRE(frameworkCatalog.has_value());

        auto observeArguments = CanonicalJson::parseExact("{}");
        REQUIRE(observeArguments.has_value());
        auto observe = frameworkCatalog->validate(
            "framework.screen.observe",
            std::move(*observeArguments)
        );
        REQUIRE(observe.has_value());
        REQUIRE(std::holds_alternative<FrameworkToolProvider>(observe->provider()));
        CHECK(
            std::get<FrameworkToolProvider>(observe->provider()).toolCatalogHash
            == frameworkCatalog->toolCatalogHash()
        );
        CHECK(observe->descriptor().mutability == ToolMutability::ReadOnly);
        CHECK(observe->descriptor().limits.maximumObservations == 1U);

        auto waitArguments = CanonicalJson::parseExact(
            R"({"duration_ms":500})"
        );
        REQUIRE(waitArguments.has_value());
        auto wait = frameworkCatalog->validate(
            "framework.workflow.wait",
            std::move(*waitArguments)
        );
        REQUIRE(wait.has_value());
        CHECK(wait->descriptor().limits.maximumWaits == 1U);
        CHECK(wait->descriptor().timeout.maximumElapsedMillis == 60'000U);

        // An Agent is offered the Semantic Framework Tools and none of the
        // Privileged ones: raw capture, bare-coordinate input, and the
        // reconciliation transition are absent rather than present and refused.
        auto noCapabilities = std::array<std::string, 0U>{};
        auto const offered = frameworkCatalog->offeredTools(
            controllerProfile(ControllerKind::Agent),
            noCapabilities
        );
        REQUIRE(offered.size() == 5U);
        CHECK(offered[0].name == "framework.audit.record");
        CHECK(offered[1].name == "framework.input.semantic_target");
        CHECK(offered[2].name == "framework.screen.observe");
        CHECK(offered[3].name == "framework.workflow.status");
        CHECK(offered[4].name == "framework.workflow.wait");

        auto catalogMaterial = CanonicalJson::parseExact(
            frameworkCatalog->canonicalJcs()
        );
        REQUIRE(catalogMaterial.has_value());
        CHECK(
            catalogMaterial->contentHash()
            == frameworkCatalog->toolCatalogHash()
        );
    }

    TEST_CASE("Framework wait and observe arguments are exact and bounded")
    {
        auto frameworkCatalog = FrameworkToolCatalogOwner::create();
        REQUIRE(frameworkCatalog.has_value());

        struct ArgumentCase final
        {
            std::string_view tool{};
            std::string_view arguments{};
        };
        constexpr auto k_refused = std::array{
            ArgumentCase{"framework.screen.observe", R"({"duration_ms":0})"},
            ArgumentCase{"framework.workflow.wait", "{}"},
            ArgumentCase{"framework.workflow.wait", "[]"},
            ArgumentCase{"framework.workflow.wait", R"({"duration_ms":-1})"},
            ArgumentCase{"framework.workflow.wait", R"({"duration_ms":60001})"},
            ArgumentCase{"framework.workflow.wait", R"({"duration_ms":"1"})"},
            ArgumentCase{
                "framework.workflow.wait",
                R"({"duration_ms":1,"extra":true})",
            },
        };

        for (auto const& refusedCase : k_refused)
        {
            CAPTURE(refusedCase.tool);
            CAPTURE(refusedCase.arguments);
            auto arguments = CanonicalJson::parseExact(
                std::string{refusedCase.arguments}
            );
            REQUIRE(arguments.has_value());
            CHECK_FALSE(
                frameworkCatalog->validate(
                    std::string{refusedCase.tool},
                    std::move(*arguments)
                ).has_value()
            );
        }

        auto projectArguments = CanonicalJson::parseExact("{}");
        REQUIRE(projectArguments.has_value());
        CHECK_FALSE(
            frameworkCatalog->validate(
                "fixture.command",
                std::move(*projectArguments)
            ).has_value()
        );
    }

    TEST_CASE("Project Tool Catalog declares only its own namespace")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto descriptor = prepared.project.toolCatalogSchemaOwner.describe(
            prepared.project.toolName("command-1")
        );
        REQUIRE(descriptor.has_value());

        // The three ways a declared name can be one this registrant does not
        // own: the Framework's namespace, another Project's, and no namespace
        // at all. The first two are refused by the ownership rule, which names
        // the namespace the registration does own; the third never reaches it,
        // because an undotted name is not a Tool name to begin with. This
        // registration owns `fixture.control`, so that is the namespace the
        // first two refusals name.
        struct UnownedName final
        {
            std::string_view name{};
            std::string_view refusal{};
        };
        constexpr auto k_unowned = std::array{
            UnownedName{
                "framework.screen.observe",
                "Tool name framework.screen.observe is outside the namespace "
                "fixture.control its registrant owns",
            },
            UnownedName{
                "fixture.other.command-1",
                "Tool name fixture.other.command-1 is outside the namespace "
                "fixture.control its registrant owns",
            },
            UnownedName{
                "command-1",
                "Tool name is not a canonical namespaced name",
            },
        };

        for (auto const& unownedCase : k_unowned)
        {
            CAPTURE(unownedCase.name);
            auto const refused = ProjectToolCatalogSchemaOwner::create(
                prepared.project.registration,
                prepared.project.toolCatalogBytes,
                [descriptor = *descriptor, name = std::string{unownedCase.name}]()
                    -> Result<std::vector<ToolCatalogEntry>>
                {
                    return std::vector<ToolCatalogEntry>{
                        ToolCatalogEntry{
                            .name       = name,
                            .descriptor = descriptor,
                        },
                    };
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(unownedCase.refusal));
        }

        auto arguments = test_support::canonical(
            prepared.project.schemaOwner,
            R"({"value":1})"
        );
        auto invocation = prepared.project.toolCatalogSchemaOwner.validate(
            prepared.project.toolName("command-1"),
            std::move(arguments)
        );
        REQUIRE(invocation.has_value());
        REQUIRE(std::holds_alternative<ProjectToolProvider>(invocation->provider()));
        auto const& provider = std::get<ProjectToolProvider>(
            invocation->provider()
        );
        CHECK(provider.projectRegistrationHash == prepared.project.registration.hash());
        CHECK(provider.toolCatalogHash == prepared.project.registration.toolCatalogHash());
    }

    TEST_CASE("Tool root request identity is stable and detects key conflicts")
    {
        auto firstPreimage = CanonicalJson::parseExact(
            R"({"objective":"inspect"})"
        );
        auto repeatedPreimage = CanonicalJson::parseExact(
            R"({"objective":"inspect"})"
        );
        auto changedPreimage = CanonicalJson::parseExact(
            R"({"objective":"act"})"
        );
        REQUIRE(firstPreimage.has_value());
        REQUIRE(repeatedPreimage.has_value());
        REQUIRE(changedPreimage.has_value());

        auto first = ToolRootRequestIdentity::create(
            "principal-7",
            "request-9",
            std::move(*firstPreimage)
        );
        auto repeated = ToolRootRequestIdentity::create(
            "principal-7",
            "request-9",
            std::move(*repeatedPreimage)
        );
        auto conflict = ToolRootRequestIdentity::create(
            "principal-7",
            "request-9",
            std::move(*changedPreimage)
        );
        REQUIRE(first.has_value());
        REQUIRE(repeated.has_value());
        REQUIRE(conflict.has_value());
        CHECK(first->identity() == repeated->identity());
        CHECK(
            first->relationTo(*repeated)
            == RootRequestRelation::SameRequest
        );
        CHECK(first->identity() != conflict->identity());
        CHECK(
            first->relationTo(*conflict)
            == RootRequestRelation::Conflict
        );

        auto distinctPreimage = CanonicalJson::parseExact(
            R"({"objective":"inspect"})"
        );
        REQUIRE(distinctPreimage.has_value());
        auto distinct = ToolRootRequestIdentity::create(
            "principal-7",
            "request-10",
            std::move(*distinctPreimage)
        );
        REQUIRE(distinct.has_value());
        CHECK(first->identity() != distinct->identity());
        CHECK(
            first->relationTo(*distinct)
            == RootRequestRelation::Distinct
        );

        auto distinctNamespacePreimage = CanonicalJson::parseExact(
            R"({"objective":"inspect"})"
        );
        REQUIRE(distinctNamespacePreimage.has_value());
        auto distinctNamespace = ToolRootRequestIdentity::create(
            "principal-8",
            "request-9",
            std::move(*distinctNamespacePreimage)
        );
        REQUIRE(distinctNamespace.has_value());
        CHECK(first->identity() != distinctNamespace->identity());
        CHECK(
            first->relationTo(*distinctNamespace)
            == RootRequestRelation::Distinct
        );

        auto emptyNamespacePreimage = CanonicalJson::parseExact("{}");
        auto emptyKeyPreimage       = CanonicalJson::parseExact("{}");
        REQUIRE(emptyNamespacePreimage.has_value());
        REQUIRE(emptyKeyPreimage.has_value());
        CHECK_FALSE(
            ToolRootRequestIdentity::create(
                "",
                "request-9",
                std::move(*emptyNamespacePreimage)
            ).has_value()
        );
        CHECK_FALSE(
            ToolRootRequestIdentity::create(
                "principal-7",
                "",
                std::move(*emptyKeyPreimage)
            ).has_value()
        );

        CHECK(
            CallerIdempotencyNamespace::create(
                std::string(256U, 'n')
            ).has_value()
        );
        CHECK_FALSE(
            CallerIdempotencyNamespace::create(
                std::string(257U, 'n')
            ).has_value()
        );
        CHECK(
            RootRequestKey::create(std::string(256U, 'k')).has_value()
        );
        CHECK_FALSE(
            RootRequestKey::create(std::string(257U, 'k')).has_value()
        );
    }

    TEST_CASE("Tool call identity covers every caller-fixed call position input")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path(), "fixture.identity-a");
        auto rootPreimage = CanonicalJson::parseExact(
            R"({"objective":"drive"})"
        );
        REQUIRE(rootPreimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "principal-1",
            "request-1",
            std::move(*rootPreimage)
        );
        REQUIRE(root.has_value());

        auto arguments = test_support::canonical(
            prepared.project.schemaOwner,
            R"({"value":1})"
        );
        auto invocation = prepared.project.toolCatalogSchemaOwner.validate(
            prepared.project.toolName("command-1"),
            std::move(arguments)
        );
        REQUIRE(invocation.has_value());
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = test_support::hashOf("run-1"),
            .frameworkReleaseIdentity    = test_support::hashOf("framework-1"),
            .toolRuntimeProtocolIdentity = test_support::hashOf("tool-runtime-1"),
            .environmentIdentity         = test_support::hashOf("environment-1"),
        };
        auto first = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *invocation
        );
        auto repeated = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *invocation
        );
        REQUIRE(first.has_value());
        REQUIRE(repeated.has_value());
        CHECK(first->identity() == repeated->identity());

        auto movedRootPreimage = CanonicalJson::parseExact(
            R"({"objective":"drive-other"})"
        );
        REQUIRE(movedRootPreimage.has_value());
        auto movedRoot = ToolRootRequestIdentity::create(
            "principal-1",
            "request-1",
            std::move(*movedRootPreimage)
        );
        REQUIRE(movedRoot.has_value());

        struct CallerFixedCase final
        {
            std::string_view               name;
            ToolRootRequestIdentity const& root;
            ToolExecutionIdentity          executionIdentity;
        };
        auto const callerFixedCases = std::array{
            CallerFixedCase{"root", *movedRoot, execution},
            CallerFixedCase{
                "run",
                *root,
                ToolExecutionIdentity{
                    .runIdentity              = test_support::hashOf("run-2"),
                    .frameworkReleaseIdentity = execution.frameworkReleaseIdentity,
                    .toolRuntimeProtocolIdentity =
                        execution.toolRuntimeProtocolIdentity,
                    .environmentIdentity = execution.environmentIdentity,
                },
            },
            CallerFixedCase{
                "framework release",
                *root,
                ToolExecutionIdentity{
                    .runIdentity              = execution.runIdentity,
                    .frameworkReleaseIdentity = test_support::hashOf("framework-2"),
                    .toolRuntimeProtocolIdentity =
                        execution.toolRuntimeProtocolIdentity,
                    .environmentIdentity = execution.environmentIdentity,
                },
            },
            CallerFixedCase{
                "Tool Runtime protocol",
                *root,
                ToolExecutionIdentity{
                    .runIdentity              = execution.runIdentity,
                    .frameworkReleaseIdentity = execution.frameworkReleaseIdentity,
                    .toolRuntimeProtocolIdentity =
                        test_support::hashOf("tool-runtime-2"),
                    .environmentIdentity = execution.environmentIdentity,
                },
            },
            CallerFixedCase{
                "environment",
                *root,
                ToolExecutionIdentity{
                    .runIdentity              = execution.runIdentity,
                    .frameworkReleaseIdentity = execution.frameworkReleaseIdentity,
                    .toolRuntimeProtocolIdentity =
                        execution.toolRuntimeProtocolIdentity,
                    .environmentIdentity = test_support::hashOf("environment-2"),
                },
            },
        };
        for (auto const& callerFixedCase : callerFixedCases)
        {
            CAPTURE(callerFixedCase.name);
            auto moved = toolCallAt(
                callerFixedCase.root,
                nullptr,
                1U,
                callerFixedCase.executionIdentity,
                *invocation
            );
            REQUIRE(moved.has_value());
            CHECK(first->identity() != moved->identity());
        }

        auto changedArguments = test_support::canonical(
            prepared.project.schemaOwner,
            R"({"value":2})"
        );
        auto changedArgumentInvocation =
            prepared.project.toolCatalogSchemaOwner.validate(
                prepared.project.toolName("command-1"),
                std::move(changedArguments)
            );
        REQUIRE(changedArgumentInvocation.has_value());
        auto changedArgument = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *changedArgumentInvocation
        );
        REQUIRE(changedArgument.has_value());
        CHECK(first->identity() != changedArgument->identity());

        auto changedNameArguments = test_support::canonical(
            prepared.project.schemaOwner,
            R"({"value":1})"
        );
        auto changedNameInvocation =
            prepared.project.toolCatalogSchemaOwner.validate(
                prepared.project.toolName("command-2"),
                std::move(changedNameArguments)
            );
        REQUIRE(changedNameInvocation.has_value());
        auto changedName = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *changedNameInvocation
        );
        REQUIRE(changedName.has_value());
        CHECK(first->identity() != changedName->identity());

        auto const changedVersionName = prepared.project.toolName("command-1");
        auto descriptor = prepared.project.toolCatalogSchemaOwner.describe(
            changedVersionName
        );
        REQUIRE(descriptor.has_value());
        descriptor->toolVersion = "changed-version";
        auto changedVersionOwner = ProjectToolCatalogSchemaOwner::create(
            prepared.project.registration,
            prepared.project.toolCatalogBytes,
            [descriptor = std::move(*descriptor), name = changedVersionName]()
                -> Result<std::vector<ToolCatalogEntry>>
            {
                return std::vector<ToolCatalogEntry>{
                    ToolCatalogEntry{
                        .name       = name,
                        .descriptor = descriptor,
                    },
                };
            },
            [](std::string_view, std::string_view) -> Status { return ok(); }
        );
        REQUIRE(changedVersionOwner.has_value());
        auto changedVersionArguments = test_support::canonical(
            prepared.project.schemaOwner,
            R"({"value":1})"
        );
        auto changedVersionInvocation = changedVersionOwner->validate(
            changedVersionName,
            std::move(changedVersionArguments)
        );
        REQUIRE(changedVersionInvocation.has_value());
        auto changedVersion = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *changedVersionInvocation
        );
        REQUIRE(changedVersion.has_value());
        CHECK(first->identity() != changedVersion->identity());

        auto secondTemporary = TemporaryDirectory{};
        auto secondPrepared = prepareStore(
            secondTemporary.path(),
            "fixture.identity-b"
        );
        auto changedProviderArguments = test_support::canonical(
            secondPrepared.project.schemaOwner,
            R"({"value":1})"
        );
        auto changedProviderInvocation =
            secondPrepared.project.toolCatalogSchemaOwner.validate(
                secondPrepared.project.toolName("command-1"),
                std::move(changedProviderArguments)
            );
        REQUIRE(changedProviderInvocation.has_value());
        auto changedProvider = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *changedProviderInvocation
        );
        REQUIRE(changedProvider.has_value());
        CHECK(first->identity() != changedProvider->identity());

        auto secondParent = toolCallAt(
            *root,
            nullptr,
            2U,
            execution,
            *invocation
        );
        REQUIRE(secondParent.has_value());
        auto child = toolCallAt(
            *root,
            &*first,
            1U,
            execution,
            *invocation
        );
        auto movedParent = toolCallAt(
            *root,
            &*secondParent,
            1U,
            execution,
            *invocation
        );
        auto movedSequence = toolCallAt(
            *root,
            &*first,
            2U,
            execution,
            *invocation
        );
        REQUIRE(child.has_value());
        REQUIRE(movedParent.has_value());
        REQUIRE(movedSequence.has_value());
        CHECK(child->identity() != first->identity());
        CHECK(child->identity() != movedParent->identity());
        CHECK(child->identity() != movedSequence->identity());
    }

    TEST_CASE("Framework and Project calls use one validated identity builder")
    {
        auto temporary    = TemporaryDirectory{};
        auto prepared     = prepareStore(temporary.path());
        auto rootPreimage = CanonicalJson::parseExact("{}");
        REQUIRE(rootPreimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "principal-1",
            "request-1",
            std::move(*rootPreimage)
        );
        REQUIRE(root.has_value());
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = test_support::hashOf("run-1"),
            .frameworkReleaseIdentity    = test_support::hashOf("framework-1"),
            .toolRuntimeProtocolIdentity = test_support::hashOf("tool-runtime-1"),
            .environmentIdentity         = test_support::hashOf("environment-1"),
        };

        auto frameworkCatalog   = FrameworkToolCatalogOwner::create();
        auto frameworkArguments = CanonicalJson::parseExact("{}");
        REQUIRE(frameworkCatalog.has_value());
        REQUIRE(frameworkArguments.has_value());
        auto frameworkInvocation = frameworkCatalog->validate(
            "framework.screen.observe",
            std::move(*frameworkArguments)
        );
        REQUIRE(frameworkInvocation.has_value());
        auto frameworkCall = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *frameworkInvocation
        );
        REQUIRE(frameworkCall.has_value());
        CHECK(std::holds_alternative<FrameworkToolProvider>(
            frameworkCall->provider()
        ));

        auto projectArguments = test_support::canonical(
            prepared.project.schemaOwner,
            R"({"value":1})"
        );
        auto projectInvocation = prepared.project.toolCatalogSchemaOwner.validate(
            prepared.project.toolName("command-1"),
            std::move(projectArguments)
        );
        REQUIRE(projectInvocation.has_value());
        auto projectCall = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *projectInvocation
        );
        REQUIRE(projectCall.has_value());
        CHECK(std::holds_alternative<ProjectToolProvider>(
            projectCall->provider()
        ));

        // The seam owns the ordinal, so there is no out-of-range ordinal for a
        // caller to present: a fresh context starts at one and every issue
        // advances by exactly one, whether or not the calls are alike.
        auto counted = ToolCallIssuingContext::forRoot(*root, execution);
        CHECK(counted.issuedChildren() == 0U);
        auto firstIssued = counted.issue(*projectInvocation);
        REQUIRE(firstIssued.has_value());
        CHECK(firstIssued->sequence() == 1U);
        CHECK(counted.issuedChildren() == 1U);
        auto secondIssued = counted.issue(*projectInvocation);
        REQUIRE(secondIssued.has_value());
        CHECK(secondIssued->sequence() == 2U);
        CHECK(secondIssued->identity() != firstIssued->identity());
        CHECK(counted.issuedChildren() == 2U);

        // A handler's context numbers from one again, and its children are
        // parented on the handler call rather than on the run root. That is
        // what makes a replayed child cost its parent exactly one increment
        // regardless of how large its subtree was.
        auto handlerContext = ToolCallIssuingContext::forHandler(*firstIssued);
        auto handlerChild   = handlerContext.issue(*frameworkInvocation);
        REQUIRE(handlerChild.has_value());
        CHECK(handlerChild->sequence() == 1U);
        CHECK(handlerChild->parentIdentity() == firstIssued->identity());
        CHECK(counted.issuedChildren() == 2U);

        // The run's own context is anchored on the root request itself, so a
        // call it issues names a real parent coordinate rather than none.
        CHECK(firstIssued->parentIdentity() == root->identity());

        auto foreignPreimage = CanonicalJson::parseExact("{}");
        REQUIRE(foreignPreimage.has_value());
        auto foreignRoot = ToolRootRequestIdentity::create(
            "principal-1",
            "request-2",
            std::move(*foreignPreimage)
        );
        REQUIRE(foreignRoot.has_value());
        auto foreignParent = toolCallAt(
            *foreignRoot,
            nullptr,
            1U,
            execution,
            *projectInvocation
        );
        REQUIRE(foreignParent.has_value());

        // A handler context takes its root from the call it implements, so a
        // position issued under a foreign parent belongs to that foreign root
        // no matter which root the caller had in hand. The caller cannot claim
        // otherwise, and the ledger is what refuses the claim: a position is
        // persisted under the root its own parent chain names.
        auto foreignChild = toolCallAt(
            *root,
            &*foreignParent,
            1U,
            execution,
            *projectInvocation
        );
        REQUIRE(foreignChild.has_value());
        CHECK(foreignChild->rootIdentity() == foreignRoot->identity());

        auto const misfiled = prepared.store.persistToolCallPosition(
            *root,
            *foreignChild
        );
        REQUIRE_FALSE(misfiled.has_value());
        CHECK(misfiled.error().message().contains(
            "belongs to a different root request"
        ));
    }


    TEST_CASE("a policy whose priorities contradict its order is refused")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The array order decides which matching rule rules. A priority that
        // disagreed with it would be a second authority over one ordering, so
        // the artifact is refused rather than read either way.
        auto const contradictory = rewrittenPolicy(
            R"("maximum_risk":"medium","priority":20)",
            R"("maximum_risk":"medium","priority":5)"
        );
        auto const manifest = test_support::sessionManifest(
            prepared.project.registration,
            prepared.runtimeArtifactRootHash,
            test_support::hashOf("agent"),
            contradictory
        );
        CHECK_FALSE(
            VerifiedPolicyArtifact::verifyExact(manifest, contradictory).has_value()
        );

        // The unrewritten artifact verifies against the same manifest shape, so
        // the refusal is the ordering rule and not the rewrite itself.
        auto const ordered = test_support::policyArtifactBytes();
        CHECK(VerifiedPolicyArtifact::verifyExact(
            test_support::sessionManifest(
                prepared.project.registration,
                prepared.runtimeArtifactRootHash,
                test_support::hashOf("agent"),
                ordered
            ),
            ordered
        ).has_value());
    }

    TEST_CASE("a policy the session manifest does not pin cannot be verified")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The manifest pins the fixture's artifact; these are other bytes. A
        // caller-supplied hash is exactly what this refusal replaces.
        auto const other = rewrittenPolicy(
            R"("policy_id":"conformance-fixture")",
            R"("policy_id":"some-other-policy")"
        );
        CHECK_FALSE(
            VerifiedPolicyArtifact::verifyExact(prepared.manifest, other).has_value()
        );
    }

    // A registration whose plugin_environment_hash is not the digest of the
    // environment this process runs. It reaches the loader through the shared
    // fixture rather than a hand-built document, which is what makes the
    // refusal one a real deployment could meet.
    TEST_CASE("the generation registrar refuses a forged running environment")
    {
        auto const source  = test_support::reducerSource("fixture.control");
        auto const project = test_support::makeProject(
            "fixture.control",
            source,
            test_support::k_projectObservationSchema,
            test_support::k_toolPreconditionSchema,
            test_support::hashOf("forged-plugin-environment")
        );
        auto       registrar = ProjectGenerationRegistrar{};
        auto const refused   = registrar.registerGeneration(
            project.generation,
            project.toolCatalogSchemaOwner,
            project.schemaOwner,
            ProjectGenerationRegistrar::ClosureModules{
                .entryModule = "main",
                .modules     = test_support::closureModules(source),
            },
            ProjectGenerationRegistrar::ClosureModules{
                .entryModule = "main",
                .modules     = test_support::closureModules(
                    test_support::toolClosureSource("fixture.control")
                ),
            },
            {},
            [](std::string_view, std::string_view) -> Status { return ok(); },
            test_support::refusingToolRuntime()
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "environment does not match the verified generation"
        ));
    }

    TEST_CASE("module manifest identity covers bytes names and entry but not authored order")
    {
        using Module = ProjectModuleBlob;
        auto const authored = std::vector<Module>{
            Module{.name = "main", .source = "return require('./worker')"},
            Module{.name = "worker", .source = "return 1"},
        };
        auto const reordered = std::vector<Module>{authored[1], authored[0]};
        auto renamed = authored;
        renamed[1].name = "helper";
        auto changedBytes = authored;
        changedBytes[1].source = "return 2";

        auto const identity = derivePluginModuleManifestHash("main", authored);
        auto const reorderedIdentity = derivePluginModuleManifestHash("main", reordered);
        auto const renamedIdentity = derivePluginModuleManifestHash("main", renamed);
        auto const changedEntryIdentity = derivePluginModuleManifestHash("worker", authored);
        auto const changedBytesIdentity = derivePluginModuleManifestHash("main", changedBytes);
        REQUIRE(identity.has_value());
        REQUIRE(reorderedIdentity.has_value());
        REQUIRE(renamedIdentity.has_value());
        REQUIRE(changedEntryIdentity.has_value());
        REQUIRE(changedBytesIdentity.has_value());
        CHECK(*identity == *reorderedIdentity);
        CHECK(*identity != *renamedIdentity);
        CHECK(*identity != *changedEntryIdentity);
        CHECK(*identity != *changedBytesIdentity);
    }
}
