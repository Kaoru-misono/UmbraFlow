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

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

        // The ui_target this fixture's next_step aims at, a second one the same
        // RuntimeModel declares, and arguments naming the first. Both targets
        // are declared, so requireDeclaredUi accepts either and the only thing
        // that can tell them apart at the mint is the frozen plan.
        constexpr auto k_plannedTarget   = std::string_view{"fixture.target"};
        constexpr auto k_unplannedTarget = std::string_view{"fixture.marker"};
        constexpr auto k_plannedArgs     = std::string_view{
            R"({"observed_instance_id":"fixture.target"})"
        };

        // What a step's own canonical_parameters can say about the member those
        // arguments named: the same value, another value, and nothing at all.
        // Which member holds the target is never stated here, because the
        // Operator is never told and the rule does not need to be.
        constexpr auto k_agreeingParameters = std::string_view{
            R"({"observed_instance_id":"fixture.target"})"
        };
        constexpr auto k_contradictingParameters = std::string_view{
            R"({"observed_instance_id":"fixture.marker"})"
        };

        // A parameter no argument of this command names. It is a UI action's
        // own parameter, which is the shape the step intent schema leaves the
        // project to define.
        constexpr auto k_unstatedParameters = std::string_view{
            R"({"keystroke":"enter"})"
        };

        // Arguments naming no declared ui_target at all, and a step parameter
        // contradicting one of them. Nothing about this pair is a UI target, so
        // it is what separates the argument rule from the aim rule.
        constexpr auto k_ordinaryArgs = std::string_view{R"({"value":1})"};
        constexpr auto k_ordinaryContradiction = std::string_view{
            R"({"value":9})"
        };

        // A catalog owner over this tool's own descriptor whose argument
        // validator accepts any canonical arguments. The fixture's tool
        // argument schema admits exactly {"value": 1..8}, so no Operation whose
        // arguments name the object it is about can be created through the
        // prepared catalog -- and such arguments are the whole subject of the
        // two cases below.
        [[nodiscard]]
        auto catalogAcceptingAnyArguments(
            test_support::PreparedStore const& prepared
        ) -> Result<ProjectToolCatalogSchemaOwner>
        {
            UF_TRY_VALUE(
                descriptor,
                prepared.project.toolCatalogSchemaOwner.describe("command-1")
            );
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

        // The prepared project's own plugin, behind a schema owner that judges
        // the exchanged documents' canonical form and nothing else.
        //
        // The two cases below need an Operation whose canonical_args name the
        // object the plan is about -- the tool argument the declarative
        // workflow tier calls target_argument. This fixture's tool argument
        // schema declares one integer and closes the object, so the project's
        // own deployment refuses such a call at the plan input, and that schema
        // is shared with two other suites. Substituting the document validator
        // is how a case presents a project whose catalog does declare such an
        // argument, the way catalogWithTimeoutAction substitutes an argument
        // validator. The registration, the plugin bytes and every hash the
        // Operator checks are the prepared ones and are untouched.
        [[nodiscard]]
        auto pluginJudgingCanonicalFormOnly(
            test_support::PreparedStore const& prepared
        ) -> Result<ProjectPluginHandle>
        {
            UF_TRY_VALUE(
                schemaOwner,
                ProjectSchemaOwner::create(
                    prepared.project.registration,
                    ProjectDocumentSchemaBytes{
                        .projectState       = test_support::k_projectStateSchema,
                        .projectObservation = test_support::k_projectObservationSchema,
                        .toolPrecondition   = test_support::k_toolPreconditionSchema,
                    },
                    deployment::canonicalJsonValidator(),
                    [](
                        ProjectPluginFunction,
                        ProjectDocumentDirection,
                        std::string_view
                    ) -> Status { return ok(); }
                )
            );
            auto registrar = ProjectPluginRegistrar{};
            return registrar.registerPlugin(
                prepared.project.registration,
                "main",
                {
                    ProjectPluginRegistrar::ModuleBlob{
                        .name   = "main",
                        .source = test_support::pluginSource("fixture.control"),
                    },
                },
                {},
                std::move(schemaOwner)
            );
        }

        // What one case presents to the mint: the arguments the plan is frozen
        // over, and the step-intent member a differently written plugin could
        // have filled differently. An absent substitution leaves the fixture
        // plugin's own claim in place, so each case states only the one thing
        // it is about.
        struct AimingCase final
        {
            std::string                plannedArgs{};
            std::optional<std::string> substituteParameters{};
        };

        // An authority that freezes a plan over `plannedArgs` and reads the
        // step the plugin produced as aiming at another ui_target, or as
        // restating other parameters, or both.
        //
        // Every rewrite is reader-side for authorityWithTimeoutAction's reason:
        // the readers are the seam the Operator reads the operator protocol
        // through, so rewriting one claim is how a case presents a document
        // another plugin could have produced without a second plugin.
        // canonical_args has to be rewritten because mintPlan requires the
        // proposal to restate the Operation's own arguments, and this fixture's
        // plugin proposes a fixed {"value":1} for every tool.
        [[nodiscard]]
        auto authorityAiming(
            test_support::PreparedStore& prepared,
            AimingCase aiming
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
                [plannedArgs = std::move(aiming.plannedArgs)](
                    ValidatedDocument const& proposal
                ) -> Result<PlanProposalClaims>
                {
                    UF_TRY_VALUE(claims, deployment::readPlanProposal(proposal));
                    claims.canonicalArgs = plannedArgs;
                    return claims;
                },
                [
                    substituteParameters = std::move(aiming.substituteParameters)
                ](ValidatedDocument const& intent) -> Result<StepIntentClaims>
                {
                    UF_TRY_VALUE(claims, deployment::readStepIntent(intent));
                    if (substituteParameters.has_value())
                    {
                        claims.canonicalParameters = *substituteParameters;
                    }
                    return claims;
                }
            );
        }

        // One Operation whose canonical_args name the object the plan is about,
        // submitted through the permissive catalog above.
        [[nodiscard]]
        auto operationAiming(
            test_support::PreparedStore& prepared,
            ProjectToolCatalogSchemaOwner const& catalog,
            std::string args
        ) -> StoredOperation
        {
            auto const invocation = catalog.validate(
                "command-1",
                test_support::canonical(
                    prepared.project.schemaOwner,
                    std::move(args)
                )
            );
            REQUIRE(invocation.has_value());
            auto const submitted = prepared.store.submitCommand(
                prepared.controller,
                test_support::command(prepared.snapshot, "request-1"),
                *invocation
            );
            REQUIRE(submitted.has_value());
            return submitted->operation;
        }

        // One aiming case run end to end through the ledger: an Operation over
        // its own arguments, a plan frozen from them, and the plugin's next
        // step minted by OperatorCoordinator::mintNextStep. Every refusal below
        // is one that entry produced -- no case reaches a comparison helper
        // directly, and none could, since both are file-local to the Operator.
        [[nodiscard]]
        auto mintStepAiming(
            test_support::PreparedStore& prepared,
            AimingCase aiming
        ) -> Result<PlannedStep>
        {
            auto const commandArgs = aiming.plannedArgs;
            auto const catalog     = catalogAcceptingAnyArguments(prepared);
            REQUIRE(catalog.has_value());
            auto const plugin = pluginJudgingCanonicalFormOnly(prepared);
            REQUIRE(plugin.has_value());
            auto const authority = authorityAiming(prepared, std::move(aiming));
            REQUIRE(authority.has_value());

            auto const operation = operationAiming(
                prepared,
                *catalog,
                commandArgs
            );
            auto const frozen = prepared.store.freezePlan(
                operation.operationId,
                operation.revision,
                prepared.lease,
                *plugin,
                *catalog,
                *authority
            );
            REQUIRE(frozen.has_value());
            return prepared.store.mintNextStep(
                frozen->operation.operationId,
                frozen->operation.revision,
                prepared.lease,
                *plugin,
                *catalog,
                *authority
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
                test_support::policyArtifactBytes(),
                deployment::readPlanProposal,
                deployment::readStepIntent
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
                policyBytes,
                deployment::readPlanProposal,
                deployment::readStepIntent
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "PolicyArtifact answers for an operator protocol schema this "
                "session manifest does not pin"
            ));
        }
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

    TEST_CASE("a step restating a planned argument with another value is refused")
    {
        // The exact rule: the step's canonical_parameters restate a member of
        // the plan's canonical_args with another value, and the refusal can
        // only be about the member the step restated.
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const refused = mintStepAiming(
            prepared,
            AimingCase{
                .plannedArgs          = std::string{k_plannedArgs},
                .substituteParameters = std::string{k_contradictingParameters},
            }
        );
        REQUIRE_MESSAGE(
            !refused.has_value(),
            "a step whose canonical_parameters restate a planned argument with "
            "another value must be refused"
        );
        CHECK_MESSAGE(
            operatorPlanErrorCode(refused.error())
            == OperatorPlanErrorCode::PlannedArgumentContradicted,
            "the refusal must name the exact normative code "
            "PlannedArgumentContradicted"
        );
        CHECK_MESSAGE(
            refused.error().detailCode().message() == "PlannedArgumentContradicted",
            "the emitted wire code must be exactly PlannedArgumentContradicted"
        );
    }

    TEST_CASE("a step restating a planned argument unchanged still mints")
    {
        // The positive control for the case above, and the one that makes the
        // rule falsifiable at all: a boundary that had started refusing every
        // step which restates anything would fail here.
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const minted = mintStepAiming(
            prepared,
            AimingCase{
                .plannedArgs          = std::string{k_plannedArgs},
                .substituteParameters = std::string{k_agreeingParameters},
            }
        );
        REQUIRE_MESSAGE(
            minted.has_value(),
            "a step restating a planned argument with the value the frozen plan "
            "gave it must mint"
        );
        CHECK_MESSAGE(
            k_agreeingParameters == k_plannedArgs,
            "the restated member must carry exactly the planned value"
        );
    }

    TEST_CASE("a step parameter the plan never named is outside the rule")
    {
        // The ruled direction: canonical_parameters are a UI action's own
        // parameters, and the step intent schema leaves their shape to the
        // project. A member no argument named is not a restatement of anything,
        // so there is no planned value for it to contradict and the Operator
        // has nothing to judge it against.
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const minted = mintStepAiming(
            prepared,
            AimingCase{
                .plannedArgs          = std::string{k_plannedArgs},
                .substituteParameters = std::string{k_unstatedParameters},
            }
        );
        REQUIRE_MESSAGE(
            minted.has_value(),
            "a step parameter the frozen plan's canonical_args never named must "
            "not refuse the step"
        );
        CHECK_MESSAGE(
            !k_plannedArgs.contains("keystroke"),
            "the parameter under test must be one the plan never named"
        );
    }

    TEST_CASE("the argument rule refuses a contradiction that names no ui_target")
    {
        // The refusal carries no target-specific meaning: the contradicted
        // member need not hold a ui_target at all. Reporting it as a
        // substituted target would name something the documents do not say.
        // It is a contradicted argument and nothing more.
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const refused = mintStepAiming(
            prepared,
            AimingCase{
                .plannedArgs          = std::string{k_ordinaryArgs},
                .substituteParameters = std::string{k_ordinaryContradiction},
            }
        );
        REQUIRE_MESSAGE(
            !refused.has_value(),
            "a step contradicting a planned argument must be refused even when "
            "no ui_target is involved"
        );
        CHECK_MESSAGE(
            operatorPlanErrorCode(refused.error())
            == OperatorPlanErrorCode::PlannedArgumentContradicted,
            "a contradiction naming no ui_target must still be "
            "PlannedArgumentContradicted"
        );
        CHECK_MESSAGE(
            !k_ordinaryArgs.contains(k_plannedTarget),
            "the arguments under test must name no declared ui_target"
        );
        CHECK_MESSAGE(
            !k_ordinaryArgs.contains(k_unplannedTarget),
            "the arguments under test must name no other declared ui_target"
        );
    }

    TEST_CASE("a UI-action step with no canonical_parameters at all is refused")
    {
        // The claim carries no meaning-bearing default for the reason the three
        // UI identifiers carry none: no canonical JSON value is the empty
        // string, so an empty one can only mean a reader did not fill it, and a
        // rule over an empty object would vacuously accept every step.
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const refused = mintStepAiming(
            prepared,
            AimingCase{
                .plannedArgs          = std::string{k_plannedArgs},
                .substituteParameters = std::string{},
            }
        );
        REQUIRE_MESSAGE(
            !refused.has_value(),
            "a UI-action step whose canonical_parameters are absent must be "
            "refused rather than passing the member comparison vacuously"
        );
        CHECK_MESSAGE(
            refused.error().message().contains("canonical_parameters"),
            "the refusal must name canonical_parameters"
        );
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

    TEST_CASE("ProjectPlugin registrar refuses a forged running environment")
    {
        auto const source = test_support::pluginSource("fixture.control");
        auto const project = test_support::makeProject(
            "fixture.control",
            source,
            test_support::k_projectObservationSchema,
            test_support::k_toolPreconditionSchema,
            test_support::hashOf("forged-plugin-environment")
        );
        auto registrar = ProjectPluginRegistrar{};
        auto const refused = registrar.registerPlugin(
            project.registration,
            "main",
            {
                ProjectPluginRegistrar::ModuleBlob{
                    .name   = "main",
                    .source = source,
                },
            },
            {},
            project.schemaOwner
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "environment does not match the verified registration"
        ));
    }

    TEST_CASE("module manifest identity covers bytes names and entry but not authored order")
    {
        using Module = ProjectPluginRegistrar::ModuleBlob;
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
