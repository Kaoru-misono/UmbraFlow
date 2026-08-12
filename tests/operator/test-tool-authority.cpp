// What the Tool Catalog descriptor bounds and what the PolicyArtifact rules.
//
// Every case here is about one clause that had no owner before U8: the
// descriptor's effect, UI-action, workflow, timeout and idempotency bounds, and
// the policy that decides whether a plan may be authorised at all and by whom.
// The p03 offer side is asserted in contract-product-p03, where the requirement
// it serves is traced.

#include <operator/effective-plan.hpp>
#include <operator/ledger.hpp>
#include <operator/policy.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-invocation.hpp>

#include <deployment/project-deployment.hpp>

#include "project-fixture.hpp"

#include <core/error/result.hpp>

#include <json/repository-path.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        using test_support::freezePlanFor;
        using test_support::mintStepFor;
        using test_support::prepareStore;
        using test_support::proposedOperation;
        using test_support::TemporaryDirectory;

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

        // A session manifest and plan authority over a rewritten policy. The
        // manifest is pinned to the rewritten bytes, because an authority whose
        // policy the manifest does not name cannot be built at all.
        [[nodiscard]]
        auto authorityUnder(
            test_support::PreparedStore& prepared,
            std::string_view exactPolicyBytes
        ) -> Result<OperatorPlanAuthority>
        {
            auto runtimeModel = prepared.observation.host->runtimeModelBinding(
                prepared.observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            return conformance::planAuthority(
                prepared.project.registration,
                test_support::sessionManifest(
                    prepared.project.registration,
                    prepared.runtimeArtifactRootHash,
                    test_support::hashOf("agent"),
                    exactPolicyBytes
                ),
                *runtimeModel,
                "operator",
                exactPolicyBytes,
                test_support::k_fixtureUiAction
            );
        }

        [[nodiscard]]
        auto catalogWithTimeoutAction(
            test_support::PreparedStore const& prepared,
            TimeoutAction action
        ) -> Result<ProjectToolCatalogSchemaOwner>
        {
            UF_TRY_VALUE(
                descriptor,
                prepared.project.toolCatalogSchemaOwner.describe("command-1")
            );
            descriptor.timeout.onTimeout = action;
            return ProjectToolCatalogSchemaOwner::create(
                prepared.project.registration,
                prepared.project.toolCatalogBytes,
                [descriptor]() -> Result<std::vector<ToolCatalogEntry>>
                {
                    return std::vector<ToolCatalogEntry>{
                        ToolCatalogEntry{
                            .name       = "command-1",
                            .descriptor = descriptor,
                        },
                    };
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
        }

        [[nodiscard]]
        auto authorityWithTimeoutAction(
            test_support::PreparedStore& prepared,
            TimeoutAction action
        ) -> Result<OperatorPlanAuthority>
        {
            auto runtimeModel = prepared.observation.host->runtimeModelBinding(
                prepared.observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            return OperatorPlanAuthority::create(
                prepared.project.registration,
                prepared.manifest,
                *runtimeModel,
                "operator",
                test_support::policyArtifactBytes(),
                deployment::readPlanProposal,
                [action](ValidatedDocument const& intent) -> Result<StepIntentClaims>
                {
                    UF_TRY_VALUE(claims, deployment::readStepIntent(intent));
                    claims.timeout.onTimeout = action;
                    return claims;
                }
            );
        }
    }

    TEST_CASE("the schema this Operator applies is the published policy document")
    {
        // A schema file with no call applying it is a check that cannot fail,
        // and a second copy of one that drifts is worse. The Operator carries
        // the bytes because a runtime has no repository to read; this is what
        // keeps the two one document.
        constexpr auto k_published =
            std::string_view{"schema/umbraflow-policy-v1.schema.json"};
        auto const root = json::repositoryRoot(k_published);
        REQUIRE_FALSE(root.empty());
        auto stream = std::ifstream{root / k_published, std::ios::binary};
        auto const published = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{},
        };
        REQUIRE_FALSE(published.empty());
        CHECK(published == policyArtifactSchemaBytes());
    }

    TEST_CASE("a plan is bounded by its own tool's descriptor")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const declared =
            prepared.project.toolCatalogSchemaOwner.describe("command-1");
        REQUIRE(declared.has_value());

        // The bound the plugin's ordinary proposal sits inside: one effect
        // type, one scope kind, one payload schema, medium risk at most.
        REQUIRE(declared->effectBounds.size() == 1U);
        CHECK(declared->effectBounds.front().namespacedType == "fixture.write");
        CHECK(declared->effectBounds.front().scopeKind == "instance");
        CHECK(declared->effectBounds.front().maximumRisk == Risk::High);
        CHECK(declared->uiActionBounds == std::vector<std::string>{"fixture.step"});

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        CHECK(frozen->limits.maximumSteps == declared->limits.maximumSteps);
    }

    TEST_CASE("an effect outside the descriptor's bounds is refused")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The descriptor of the tool the Operation names, with one clause of
        // the bound moved and nothing else. Each rewrite makes exactly one of
        // effectWithinBounds' three refusals the reason.
        auto const declared =
            prepared.project.toolCatalogSchemaOwner.describe("command-1");
        REQUIRE(declared.has_value());
        auto const effect = ProposedEffect{
            .namespacedType = "fixture.write",
            .risk           = Risk::Medium,
            .scopeKind      = "instance",
            .scopeKey       = "alpha",
            .payloadSchemaHash    = declared->effectBounds.front().payloadSchemaHash,
            .opaqueProjectPayload = "{\"value\":1}",
        };
        CHECK(effectWithinBounds(*declared, effect).has_value());

        auto unknownType           = effect;
        unknownType.namespacedType = "fixture.forged";
        CHECK_FALSE(effectWithinBounds(*declared, unknownType).has_value());

        auto otherScope      = effect;
        otherScope.scopeKind = "account";
        CHECK_FALSE(effectWithinBounds(*declared, otherScope).has_value());

        auto tooRisky = effect;
        tooRisky.risk = Risk::Critical;
        CHECK_FALSE(effectWithinBounds(*declared, tooRisky).has_value());

        auto otherPayload              = effect;
        otherPayload.payloadSchemaHash = test_support::hashOf("not-the-payload-schema");
        CHECK_FALSE(effectWithinBounds(*declared, otherPayload).has_value());
    }

    TEST_CASE("a plan allowing a UI action its tool does not bound is refused")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // Every plan this plugin proposes allows the same step key. stray-action
        // bounds a different one, so its own descriptor is the only thing that
        // differs between the refusal below and the freeze after it.
        auto const stray =
            prepared.project.toolCatalogSchemaOwner.describe("stray-action");
        REQUIRE(stray.has_value());
        CHECK(
            stray->uiActionBounds
            == std::vector<std::string>{
                std::string{test_support::k_unboundUiAction},
            }
        );

        auto const proposed = proposedOperation(prepared, "request-1", "stray-action");
        auto const refused  = freezePlanFor(prepared, proposed);
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            automationErrorKind(refused.error()) == AutomationErrorKind::ActionRejected
        );
    }

    TEST_CASE("a step may claim no more safety than its tool declares")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The plugin answers next_step with one document naming delivery_safe.
        // strict-delivery declares it cannot be redelivered, so that same
        // document is refused for it and accepted for command-1.
        auto const strict =
            prepared.project.toolCatalogSchemaOwner.describe("strict-delivery");
        REQUIRE(strict.has_value());
        CHECK(strict->idempotency == ToolIdempotency::NonIdempotent);
        CHECK_FALSE(
            deliveryClassWithin(DeliveryClass::DeliverySafe, strict->idempotency)
        );

        auto const proposed = proposedOperation(prepared, "request-1", "strict-delivery");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        CHECK_FALSE(mintStepFor(prepared, frozen->operation).has_value());
    }

    TEST_CASE("a step may allow no more elapsed time than its tool declares")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const brief =
            prepared.project.toolCatalogSchemaOwner.describe("brief-timeout");
        REQUIRE(brief.has_value());
        CHECK(brief->timeout.maximumElapsedMillis == 1'000U);

        auto const proposed = proposedOperation(prepared, "request-1", "brief-timeout");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        CHECK_FALSE(mintStepFor(prepared, frozen->operation).has_value());
    }

    TEST_CASE("each timeout action is enforced as the tool's exact next action")
    {
        struct TimeoutCase final
        {
            std::string_view name{};
            TimeoutAction   declared{TimeoutAction::Stop};
            TimeoutAction   different{TimeoutAction::Stop};
        };
        constexpr auto k_cases = std::array{
            TimeoutCase{"reobserve", TimeoutAction::Reobserve, TimeoutAction::Reconcile},
            TimeoutCase{"reconcile", TimeoutAction::Reconcile, TimeoutAction::Stop},
            TimeoutCase{"stop", TimeoutAction::Stop, TimeoutAction::Reobserve},
        };

        for (auto const& timeoutCase : k_cases)
        {
            CAPTURE(timeoutCase.name);
            auto temporary = TemporaryDirectory{};
            auto prepared  = prepareStore(temporary.path());
            auto catalog = catalogWithTimeoutAction(
                prepared,
                timeoutCase.declared
            );
            REQUIRE(catalog.has_value());
            auto matchingAuthority = authorityWithTimeoutAction(
                prepared,
                timeoutCase.declared
            );
            REQUIRE(matchingAuthority.has_value());
            auto differentAuthority = authorityWithTimeoutAction(
                prepared,
                timeoutCase.different
            );
            REQUIRE(differentAuthority.has_value());

            auto const proposed = proposedOperation(prepared, "request-1", "command-1");
            auto const frozen   = prepared.store.freezePlan(
                proposed.operationId,
                proposed.revision,
                prepared.lease,
                prepared.plugin,
                *catalog,
                *matchingAuthority
            );
            REQUIRE(frozen.has_value());

            auto const refused = prepared.store.mintNextStep(
                frozen->operation.operationId,
                frozen->operation.revision,
                prepared.lease,
                prepared.plugin,
                *catalog,
                *differentAuthority
            );
            REQUIRE_MESSAGE(
                !refused.has_value(),
                "mintStep must refuse a step whose on_timeout differs from its tool"
            );
            CHECK_MESSAGE(
                refused.error().message().contains("on_timeout"),
                "the timeout-action refusal must name on_timeout"
            );

            CHECK_MESSAGE(
                prepared.store.mintNextStep(
                    frozen->operation.operationId,
                    frozen->operation.revision,
                    prepared.lease,
                    prepared.plugin,
                    *catalog,
                    *matchingAuthority
                ).has_value(),
                "mintStep must accept each declared on_timeout value"
            );
        }
    }

    TEST_CASE("required_approvals is the ruled approver set, not a risk flag")
    {
        // Two plans under the same policy, each on a store of its own because a
        // controlled target holds one unfinished mutating Operation at a time.
        // The medium-risk one carries no approver and the high-risk one carries
        // the capability the matching rule named -- a name no risk level could
        // have produced, which is the whole difference from a derived 0/1.
        auto routineRoot  = TemporaryDirectory{};
        auto routineStore = prepareStore(routineRoot.path());
        auto const routine = freezePlanFor(
            routineStore,
            proposedOperation(routineStore, "request-1", "command-1")
        );
        REQUIRE(routine.has_value());
        CHECK(routine->risk == Risk::Medium);
        CHECK(routine->requiredApprovals.empty());
        CHECK(routine->operation.state == OperationState::Ready);

        auto elevatedRoot  = TemporaryDirectory{};
        auto elevatedStore = prepareStore(elevatedRoot.path());
        auto const elevated = freezePlanFor(
            elevatedStore,
            proposedOperation(elevatedStore, "request-1", "approval-plan")
        );
        REQUIRE(elevated.has_value());
        CHECK(elevated->risk == Risk::High);
        CHECK(
            elevated->requiredApprovals
            == std::vector<std::string>{
                std::string{conformance::k_approveCapability},
            }
        );
        CHECK(elevated->operation.state == OperationState::AwaitingApproval);

        // The policy that ruled it is recorded, and it is the artifact the
        // session manifest pinned rather than anything a caller named.
        CHECK(elevated->policyHash == elevatedStore.planAuthority.policyHash());
    }

    TEST_CASE("an approver outside the ruled set cannot approve")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "approval-plan");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        REQUIRE(mintStepFor(prepared, frozen->operation).has_value());

        auto request = ApprovalRequest{
            .operationId         = proposed.operationId,
            .lease               = prepared.lease,
            .approverPrincipal   = "human-1",
            .approverCapability  = "some-other-capability",
            .expiresAtUnixMillis = 4'000'000'000'000U,
        };
        CHECK_FALSE(
            prepared.store
                .issueApproval(request, AuthorityDecisionId{"decision-1"})
                .has_value()
        );

        // The same call with the capability the plan's own required_approvals
        // names goes through, so the refusal above is the membership test and
        // not something else about the request.
        request.approverCapability = frozen->requiredApprovals.front();
        CHECK(
            prepared.store
                .issueApproval(request, AuthorityDecisionId{"decision-2"})
                .has_value()
        );
    }

    TEST_CASE("policy denies what no rule allows")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // Both rules moved onto an effect type this project never proposes, so
        // nothing speaks about fixture.write at all and the artifact's
        // unknown_effect_decision decides. Nothing else about the world moves.
        auto const elsewhere = rewrittenPolicy(
            R"("effect_types":["fixture.write"])",
            R"("effect_types":["fixture.unheard"])"
        );
        auto const authority = authorityUnder(prepared, elsewhere);
        REQUIRE(authority.has_value());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const refused  = prepared.store.freezePlan(
            proposed.operationId,
            proposed.revision,
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            *authority
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            automationErrorKind(refused.error()) == AutomationErrorKind::ActionRejected
        );

        // The identical Operation freezes under the unrewritten policy, so the
        // refusal is the rule's and not the Operation's.
        CHECK(freezePlanFor(prepared, proposed).has_value());
    }

    TEST_CASE("a rule speaks only to a controller holding its capabilities")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // Both rules require a capability this session does not hold, so
        // neither matches and the default deny decides. The session's set is
        // the ledger's own column and no caller states it.
        auto const foreign = rewrittenPolicy(
            R"("required_controller_capabilities":["operate"])",
            R"("required_controller_capabilities":["authoring"])"
        );
        auto const authority = authorityUnder(prepared, foreign);
        REQUIRE(authority.has_value());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        CHECK_FALSE(prepared.store.freezePlan(
            proposed.operationId,
            proposed.revision,
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            *authority
        ).has_value());
        CHECK(freezePlanFor(prepared, proposed).has_value());
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
}
