// Experiment E4 of the cycle SPI plan: nested authority. A Project Tool calls
// a read-only Project child, a Framework observation child and an admitted
// Framework input child, and every attack the experiment names is aimed at that
// same tree -- authority expansion, a new project, a new target, a missing
// approval, recursion, excessive depth and an exhausted root budget.
//
// The suite pins its own policy artifact because the Framework input effect is
// the one a delegated child proposes, and a policy that never speaks about it
// would deny every nested mutation for a reason the experiment is not about.

#include <operator/ledger.hpp>
#include <operator/snapshot-reference.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-invocation.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"
#include "unsafe/operator-database-probe.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
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
        using test_support::hashOf;
        using test_support::TemporaryDirectory;

        // doctest's message macro binds its message expression tighter than a
        // conditional operator, so the text is built here rather than inline.
        template <typename T>
        [[nodiscard]]
        auto failureText(Result<T> const& result) -> std::string
        {
            return result.has_value()
                ? std::string{}
                : std::string{result.error().message()};
        }

        constexpr auto k_inputEffectType = std::string_view{
            "framework.input.deliver"
        };
        constexpr auto k_inputEffectScope = std::string_view{"controlled_target"};
        constexpr auto k_coordinateInputTool = std::string_view{
            "framework.input.coordinate"
        };
        constexpr auto k_observeTool = std::string_view{
            "framework.screen.observe"
        };
        constexpr auto k_semanticInputTool = std::string_view{
            "framework.input.semantic_target"
        };
        constexpr auto k_parentTool = std::string_view{"fixture.nested.parent"};
        constexpr auto k_readOnlyChildTool = std::string_view{"fixture.nested.read"};
        constexpr auto k_boundMillis = uint64{60'000};

        // The one effect this suite's tools propose. Naming it in the policy is
        // what makes every refusal below a verdict about delegation rather than
        // the artifact's default deny.
        [[nodiscard]]
        auto nestedPolicyBytes() -> std::string
        {
            auto const types = std::vector<std::string>{
                std::string{k_inputEffectType},
            };
            // The privileged input Tool is granted no top-level surface
            // here: every case that reaches it reaches it as a delegated
            // child, and the one that presents it as a root call is the case
            // that proves a root call of it is refused.
            return conformance::policyArtifactBytes(
                hashOf("operator"),
                types,
                {}
            );
        }

        [[nodiscard]]
        auto frameworkCatalog() -> FrameworkToolCatalogOwner
        {
            auto catalog = FrameworkToolCatalogOwner::create();
            REQUIRE(catalog.has_value());
            return *std::move(catalog);
        }

        [[nodiscard]]
        auto inputPayloadSchemaHash() -> ContentHash
        {
            auto const catalog = frameworkCatalog();
            auto const descriptor = catalog.describe(k_coordinateInputTool);
            REQUIRE(descriptor.has_value());
            REQUIRE(descriptor->effectBounds.size() == 1U);
            return descriptor->effectBounds.front().payloadSchemaHash;
        }

        [[nodiscard]]
        auto readOnlyDescriptor(ChildEffectDeclaration declaration)
            -> ToolDescriptor
        {
            return ToolDescriptor{
                .toolVersion          = "1",
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .childEffects         = std::move(declaration),
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 1U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_boundMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_boundMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        [[nodiscard]]
        auto mutatingDescriptor(ChildEffectDeclaration declaration)
            -> ToolDescriptor
        {
            auto bounds = std::vector<EffectBound>{};
            bounds.emplace_back(EffectBound{
                .namespacedType    = std::string{k_inputEffectType},
                .scopeKind         = std::string{k_inputEffectScope},
                .payloadSchemaHash = inputPayloadSchemaHash(),
                .maximumRisk       = Risk::High,
            });
            return ToolDescriptor{
                .toolVersion          = "1",
                .requiredCapabilities = {},
                .effectBounds         = std::move(bounds),
                .uiActionBounds       = {},
                .childEffects         = std::move(declaration),
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 1U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_boundMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_boundMillis,
                    .onTimeout            = TimeoutAction::Reobserve,
                },
                .mutability  = ToolMutability::Mutating,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::DeliverySafe,
            };
        }

        // The Project catalog this suite drives. The reader is a trusted
        // deployment callback, so a suite states what a project's descriptors
        // declare exactly the way a deployment does; what it cannot do is make
        // the Operator believe a widened declaration, which is the whole point
        // of the cases below.
        [[nodiscard]]
        auto nestedCatalog(
            test_support::ProjectFixture const& project,
            std::vector<ToolCatalogEntry> tools
        ) -> Result<ProjectToolCatalogSchemaOwner>
        {
            return ProjectToolCatalogSchemaOwner::create(
                project.registration,
                project.toolCatalogBytes,
                [tools = std::move(tools)]() -> Result<std::vector<ToolCatalogEntry>>
                {
                    return tools;
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
        }

        [[nodiscard]]
        auto declaredCatalog(
            test_support::ProjectFixture const& project,
            ChildEffectDeclaration parentDeclaration
        ) -> ProjectToolCatalogSchemaOwner
        {
            auto tools = std::vector<ToolCatalogEntry>{
                ToolCatalogEntry{
                    .name       = std::string{k_parentTool},
                    .descriptor = mutatingDescriptor(std::move(parentDeclaration)),
                },
                ToolCatalogEntry{
                    .name       = std::string{k_readOnlyChildTool},
                    .descriptor = readOnlyDescriptor({}),
                },
            };
            auto catalog = nestedCatalog(project, std::move(tools));
            REQUIRE(catalog.has_value());
            return *std::move(catalog);
        }

        [[nodiscard]]
        auto everyChildDeclaration() -> ChildEffectDeclaration
        {
            return ChildEffectDeclaration{
                // The last name is another registration's Tool, declared on
                // purpose: a declaration only ever REQUESTS, so the case that
                // issues that child must get past the enumerated-name clause to
                // reach the one it is about, which is that a call minted by
                // another registration's catalog owner is refused for being
                // another registration's.
                .childToolNames = {
                    std::string{k_coordinateInputTool},
                    std::string{k_observeTool},
                    std::string{k_readOnlyChildTool},
                    "fixture.foreign.read",
                },
                .maximumChildSurface    = ToolSurface::Privileged,
                .maximumChildMutability = ToolMutability::Mutating,
                .maximumChildRisk       = Risk::Medium,
                .maximumChildCalls      = 4U,
            };
        }

        // An Operator whose session policy names the Framework input effect.
        // It is this suite's own rather than the shared fixture's because the
        // shared fixture pins a policy that speaks only about the project's own
        // effect type.
        [[nodiscard]]
        auto prepareNestedStore(std::filesystem::path const& path)
            -> test_support::PreparedStore
        {
            auto const policy  = nestedPolicyBytes();
            auto const release = test_support::runtimeRelease(
                path / "session-handoff"
            );
            auto storeResult = OperatorCoordinator::open(path / "production");
            REQUIRE_MESSAGE(storeResult.has_value(), failureText(storeResult));
            auto store     = *std::move(storeResult);
            auto installed = store.installRuntimeArtifact(
                RuntimeArtifactInstallRequest{
                    .handoffRoot                 = release.handoffRoot,
                    .expectedReleaseManifestHash = release.releaseManifestHash,
                    .expectedInstalledGeneration = 0U,
                }
            );
            REQUIRE(installed.has_value());
            auto const artifactRootHash    = installed->rootHash();
            auto const installedGeneration = installed->installedGeneration();
            auto const project = test_support::makeProject("fixture.nested");
            auto const manifest = test_support::sessionManifest(
                project.registration,
                artifactRootHash,
                hashOf("agent"),
                policy
            );
            auto const projectGeneration = test_support::loadGeneration(project);
            REQUIRE(store.registerProject(project.registration).has_value());
            REQUIRE(store.provisionProjectInstance(
                project.registration,
                "instance-1"
            ).has_value());
            auto const worldScope = ObservedInstanceWorldScope::run("target-1", 1);
            REQUIRE(worldScope.has_value());
            REQUIRE(store.pinSession(
                SessionPin{
                    .sessionId                 = "session-1",
                    .authenticatedControllerId = "controller-1",
                    .idempotencyNamespace      = "controller-1",
                    .projectRegistrationHash   = project.registration.hash(),
                    .controllerCapabilities    = {
                        std::string{conformance::k_operateCapability},
                    },
                    .controlledTargetId = "target-1",
                    .projectInstanceKey = "instance-1",
                    .mode               = SessionMode::Write,
                    .kind               = ControllerKind::Script,
                    .worldScope         = *worldScope,
                },
                manifest,
                std::nullopt
            ).has_value());
            auto controller = store.bindController("session-1");
            REQUIRE(controller.has_value());
            auto lease = store.acquireLease(*controller);
            REQUIRE(lease.has_value());
            auto observation = conformance::activateObservationHost(
                *std::move(installed),
                test_support::umbraflowProbeFrame(),
                FrameId{101}
            );
            auto const reading = conformance::observeOnce(observation);
            conformance::requireResolvedSurface(
                reading,
                test_support::k_fixtureUiAction.surface
            );
            auto snapshot = store.createSnapshot(
                *lease,
                project.registration,
                project.toolCatalogSchemaOwner,
                project.observedInstanceIdentitySchemas,
                reading
            );
            REQUIRE(snapshot.has_value());
            auto runtimeModel = observation.host->runtimeModelBinding(
                observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            auto policyAuthority = OperatorPolicyAuthority::create(
                project.registration,
                manifest,
                *runtimeModel,
                "operator",
                policy
            );
            REQUIRE(policyAuthority.has_value());
            return test_support::PreparedStore{
                .store                   = std::move(store),
                .generation              = projectGeneration,
                .project                 = project,
                .manifest                = manifest,
                .policyAuthority         = *std::move(policyAuthority),
                .policyArtifact          = policy,
                .controller              = *controller,
                .lease                   = *lease,
                .snapshot                = *std::move(snapshot),
                .observation             = std::move(observation),
                .runtimeArtifactRootHash = artifactRootHash,
                .installedGeneration     = installedGeneration,
            };
        }

        // An additional session on this suite's manifest. The shared fixture's
        // helper pins the shared policy, which would put this suite's sessions
        // under two different artifacts.
        [[nodiscard]]
        auto addSession(
            test_support::PreparedStore& prepared,
            ControllerKind kind,
            std::string const& sessionId,
            std::string const& projectInstanceKey,
            std::string const& controlledTargetId,
            std::optional<AgentBudget> const& budget,
            SessionMode mode = SessionMode::Write,
            std::string const& controllerId = "controller-1",
            std::vector<std::string> capabilities = {
                std::string{conformance::k_operateCapability},
            }
        ) -> ControllerBinding
        {
            REQUIRE(prepared.store.provisionProjectInstance(
                prepared.project.registration,
                projectInstanceKey
            ).has_value());

            auto manifest = prepared.manifest;
            auto profile  = std::optional<AgentProfile>{};
            if (budget)
            {
                auto const bytes = test_support::agentProfileBytes(*budget);
                manifest = test_support::sessionManifest(
                    prepared.project.registration,
                    prepared.runtimeArtifactRootHash,
                    hashOf(bytes),
                    nestedPolicyBytes()
                );
                auto verified = AgentProfile::verifyExact(
                    manifest,
                    "agent-profile.json",
                    bytes,
                    test_support::agentProfileValidator()
                );
                REQUIRE(verified.has_value());
                profile = *std::move(verified);
            }
            auto const worldScope = ObservedInstanceWorldScope::run(
                controlledTargetId,
                1
            );
            REQUIRE(worldScope.has_value());
            REQUIRE(prepared.store.pinSession(
                SessionPin{
                    .sessionId                 = sessionId,
                    .authenticatedControllerId = controllerId,
                    .idempotencyNamespace      = controllerId,
                    .projectRegistrationHash =
                        prepared.project.registration.hash(),
                    .controllerCapabilities = std::move(capabilities),
                    .controlledTargetId     = controlledTargetId,
                    .projectInstanceKey     = projectInstanceKey,
                    .mode                   = mode,
                    .kind                   = kind,
                    .worldScope             = *worldScope,
                },
                manifest,
                profile
            ).has_value());
            auto binding = prepared.store.bindController(sessionId);
            REQUIRE(binding.has_value());
            return *binding;
        }

        [[nodiscard]]
        auto rootFor(std::string_view requestKey) -> ToolRootRequestIdentity
        {
            auto preimage = CanonicalJson::parseExact(R"({"objective":"nested"})");
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                "controller-1",
                std::string{requestKey},
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            return *std::move(root);
        }

        [[nodiscard]]
        auto executionIdentity() -> ToolExecutionIdentity
        {
            return ToolExecutionIdentity{
                .runIdentity                 = hashOf("nested-run"),
                .frameworkReleaseIdentity    = hashOf("nested-framework"),
                .toolRuntimeProtocolIdentity = hashOf("nested-protocol"),
                .environmentIdentity         = hashOf("nested-environment"),
            };
        }

        [[nodiscard]]
        auto inputEffect(std::string_view controlledTargetId, Risk risk)
            -> ProposedEffect
        {
            return ProposedEffect{
                .namespacedType    = std::string{k_inputEffectType},
                .risk              = risk,
                .scopeKind         = std::string{k_inputEffectScope},
                .scopeKey          = std::string{controlledTargetId},
                .payloadSchemaHash = inputPayloadSchemaHash(),
                .opaqueProjectPayload = R"({"ui_action":"click"})",
            };
        }

        [[nodiscard]]
        auto projectCall(
            ProjectToolCatalogSchemaOwner const& catalog,
            std::string_view toolName
        ) -> ValidatedToolInvocation
        {
            auto arguments = CanonicalJson::parseExact(R"({"value":1})");
            REQUIRE(arguments.has_value());
            auto invocation = catalog.validate(
                std::string{toolName},
                std::move(*arguments)
            );
            REQUIRE_MESSAGE(invocation.has_value(), failureText(invocation));
            return *std::move(invocation);
        }

        [[nodiscard]]
        auto frameworkInvocation(
            FrameworkToolCatalogOwner const& catalog,
            std::string_view toolName,
            std::string_view exactArgumentsJcs
        ) -> ValidatedToolInvocation
        {
            auto arguments = CanonicalJson::parseExact(
                std::string{exactArgumentsJcs}
            );
            REQUIRE(arguments.has_value());
            auto invocation = catalog.validate(
                std::string{toolName},
                std::move(*arguments)
            );
            REQUIRE(invocation.has_value());
            return *std::move(invocation);
        }

        // One dispatching parent, and the grant its handler issues children on.
        struct RunningHandler final
        {
            ToolCallPositionIdentity call;
            ToolDelegationGrant      grant;
            ToolCallIssuingContext   context;
        };

        [[nodiscard]]
        auto startHandler(
            test_support::PreparedStore& prepared,
            ControllerBinding const& controller,
            ControlLease const& lease,
            ToolRootRequestIdentity const& root,
            ToolCallIssuingContext& runContext,
            ValidatedToolInvocation const& invocation,
            std::vector<ProposedEffect> const& effects
        ) -> Result<RunningHandler>
        {
            UF_TRY_VALUE(call, runContext.issue(invocation));
            UF_TRY_VALUE(
                admitted,
                prepared.store.admitToolCall(
                    ToolAdmissionRequest{
                        .controller      = controller,
                        .lease           = lease,
                        .root            = root,
                        .call            = call,
                        .policyAuthority = prepared.policyAuthority,
                        .mutation   = ToolAdmissionRequest::Mutation{
                            .effects         = effects,
                        },
                    }
                )
            );
            UF_TRY(prepared.store.beginToolCallDispatch(admitted));
            UF_TRY_VALUE(grant, prepared.store.issueToolDelegationGrant(call));
            auto context = ToolCallIssuingContext::forHandler(call);
            return RunningHandler{
                std::move(call),
                std::move(grant),
                std::move(context),
            };
        }

        // A read-only parent, for the cases that are not about the mutation
        // chain. It starts no chain at all, so several of them can run against
        // one controlled target inside one case.
        [[nodiscard]]
        auto startReadOnlyHandler(
            test_support::PreparedStore& prepared,
            ControllerBinding const& controller,
            ControlLease const& lease,
            ToolRootRequestIdentity const& root,
            ToolCallIssuingContext& runContext,
            ValidatedToolInvocation const& invocation
        ) -> Result<RunningHandler>
        {
            UF_TRY_VALUE(call, runContext.issue(invocation));
            UF_TRY_VALUE(
                admitted,
                prepared.store.admitToolCall(
                    ToolAdmissionRequest{
                        .controller      = controller,
                        .lease           = lease,
                        .root            = root,
                        .call            = call,
                        .policyAuthority = prepared.policyAuthority,
                    }
                )
            );
            UF_TRY(prepared.store.beginToolCallDispatch(admitted));
            UF_TRY_VALUE(grant, prepared.store.issueToolDelegationGrant(call));
            auto context = ToolCallIssuingContext::forHandler(call);
            return RunningHandler{
                std::move(call),
                std::move(grant),
                std::move(context),
            };
        }

        // What the durable admission row is read back by, once its store is
        // closed. Copied out as text because the identities are what the row
        // is keyed and compared by.
        struct RecordedNesting final
        {
            std::filesystem::path databasePath{};
            std::string           originPrincipalId{};
            std::string           handlerCallIdentity{};
            std::string           grantId{};
            std::string           childCallIdentity{};
        };

        auto completeReadOnlyChild(
            test_support::PreparedStore& prepared,
            ToolCallAdmission const& admission
        ) -> void
        {
            auto dispatch = prepared.store.beginToolCallDispatch(admission);
            REQUIRE(dispatch.has_value());
            auto result = CanonicalJson::parseExact(R"({"observed":true})");
            REQUIRE(result.has_value());
            auto completed = prepared.store.completeToolCallDispatch(
                *dispatch,
                ToolCallCompletion::confirmed(*result)
            );
            REQUIRE(completed.has_value());
        }
    }

    TEST_CASE("a Project Tool delegates read-only, observation and input children")
    {
        auto temporary = TemporaryDirectory{};
        auto recorded  = RecordedNesting{};
        {
            auto prepared  = prepareNestedStore(temporary.path());
            auto const catalog = declaredCatalog(
                prepared.project,
                everyChildDeclaration()
            );
            auto const framework = frameworkCatalog();

            // The origin actor is an online Agent, which is the only profile the
            // Operator restricts to semantic tools. That is what makes the last
            // assertion in this case mean something.
            auto agent = addSession(
                prepared,
                ControllerKind::Agent,
                "agent-session",
                "agent-instance",
                "target-2",
                AgentBudget{
                    .maximumToolCalls     = 8U,
                    .maximumMutations     = 4U,
                    .maximumObservations  = 4U,
                    .maximumElapsedMillis = k_boundMillis,
                    .maximumRiskUnits     = 8U,
                }
            );
            auto lease = prepared.store.acquireLease(agent);
            REQUIRE(lease.has_value());

            auto const root = rootFor("nested-happy-path");
            auto runContext = ToolCallIssuingContext::forRoot(
                root,
                executionIdentity()
            );
            auto const rootEffects = std::vector{
                inputEffect(agent.controlledTargetId(), Risk::Medium),
            };

            // Direct visibility and delegated authority are distinct: this
            // actor presenting the privileged input Tool as a root call of its
            // own is refused, while the parent's declaration reaches it below.
            // The direct attempt runs before the parent starts, because a
            // target already carrying a dispatching mutation refuses a second
            // one for that barrier rather than for the surface.
            auto inputInvocation = frameworkInvocation(
                framework,
                k_coordinateInputTool,
                R"({"action":"click","x":4,"y":9})"
            );
            auto const childEffects = std::vector{
                inputEffect(agent.controlledTargetId(), Risk::Medium),
            };
            auto const directRoot = rootFor("nested-direct-privileged");
            auto directContext = ToolCallIssuingContext::forRoot(
                directRoot,
                executionIdentity()
            );
            auto directCall = directContext.issue(inputInvocation);
            REQUIRE(directCall.has_value());
            auto direct = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = agent,
                    .lease           = *lease,
                    .root            = directRoot,
                    .call            = *directCall,
                    .policyAuthority = prepared.policyAuthority,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .effects         = childEffects,
                    },
                }
            );
            REQUIRE_FALSE(direct.has_value());
            CHECK(direct.error().message().contains(
                "does not admit this Tool surface"
            ));

            auto handler = startHandler(
                prepared,
                agent,
                *lease,
                root,
                runContext,
                projectCall(catalog, k_parentTool),
                rootEffects
            );
            REQUIRE_MESSAGE(handler.has_value(), failureText(handler));

            // A read-only Project child.
            auto projectChild = handler->context.issue(
                projectCall(catalog, k_readOnlyChildTool)
            );
            REQUIRE(projectChild.has_value());
            auto projectChildAdmission = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = agent,
                    .lease           = *lease,
                    .root            = root,
                    .call            = *projectChild,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation = handler->grant,
                }
            );
            REQUIRE_MESSAGE(projectChildAdmission.has_value(), failureText(projectChildAdmission));
            completeReadOnlyChild(prepared, *projectChildAdmission);

            // A Framework observation child.
            auto observeChild = handler->context.issue(
                frameworkInvocation(framework, k_observeTool, "{}")
            );
            REQUIRE(observeChild.has_value());
            auto observeAdmission = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = agent,
                    .lease           = *lease,
                    .root            = root,
                    .call            = *observeChild,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation = handler->grant,
                }
            );
            REQUIRE_MESSAGE(observeAdmission.has_value(), failureText(observeAdmission));
            completeReadOnlyChild(prepared, *observeAdmission);

            // The privileged Framework input child the parent's declaration
            // reaches, which the direct attempt above could not.
            auto inputChild = handler->context.issue(inputInvocation);
            REQUIRE(inputChild.has_value());
            auto inputAdmission = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = agent,
                    .lease           = *lease,
                    .root            = root,
                    .call            = *inputChild,
                    .policyAuthority = prepared.policyAuthority,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .effects         = childEffects,
                    },
                    .delegation = handler->grant,
                }
            );
            REQUIRE_MESSAGE(inputAdmission.has_value(), failureText(inputAdmission));

            recorded = RecordedNesting{
                .databasePath      = prepared.store.databasePath(),
                .originPrincipalId = agent.controllerId(),
                .handlerCallIdentity = handler->call.identity().hex(),
                .grantId             = handler->grant.grantId(),
                .childCallIdentity   = inputChild->identity().hex(),
            };
        }

        // The Operator claims its database exclusively, so the rows are read after
        // the store above has been destroyed.
        auto database = test_support::OperatorDatabaseProbe{recorded.databasePath};
        auto const rows = database.readRows(
            "SELECT origin_principal_id, execution_principal_id, "
            "delegation_grant_id FROM tool_admission_attempts "
            "WHERE call_identity='" + recorded.childCallIdentity + "'"
        );
        REQUIRE(rows.size() == 1U);
        REQUIRE(rows.front().size() == 3U);
        CHECK(rows.front()[0] == recorded.originPrincipalId);
        CHECK(rows.front()[1] == recorded.handlerCallIdentity);
        CHECK(rows.front()[2] == recorded.grantId);
        CHECK(rows.front()[1] != rows.front()[0]);
    }

    TEST_CASE("nested authority cannot be widened, moved, or repeated")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto const catalog = declaredCatalog(
            prepared.project,
            everyChildDeclaration()
        );
        auto const framework = frameworkCatalog();
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;
        auto const target      = controller.controlledTargetId();

        auto const root = rootFor("nested-attacks");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto const rootEffects = std::vector{inputEffect(target, Risk::Medium)};
        auto handler = startHandler(
            prepared,
            controller,
            lease,
            root,
            runContext,
            projectCall(catalog, k_parentTool),
            rootEffects
        );
        REQUIRE_MESSAGE(handler.has_value(), failureText(handler));
        auto inputInvocation = frameworkInvocation(
            framework,
            k_coordinateInputTool,
            R"({"action":"click","x":4,"y":9})"
        );

        SUBCASE("a child cannot propose a risk its parent may not delegate")
        {
            auto child = handler->context.issue(inputInvocation);
            REQUIRE(child.has_value());
            auto const widened = std::vector{inputEffect(target, Risk::High)};
            auto refused = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *child,
                    .policyAuthority = prepared.policyAuthority,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .effects         = widened,
                    },
                    .delegation = handler->grant,
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "may not delegate effect framework.input.deliver at high risk"
            ));
        }

        SUBCASE("a child cannot escape the admitted root effect envelope")
        {
            auto child = handler->context.issue(inputInvocation);
            REQUIRE(child.has_value());
            auto elsewhere     = inputEffect(target, Risk::Medium);
            elsewhere.scopeKey = "another-scope";
            auto const moved = std::vector{elsewhere};
            auto refused = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *child,
                    .policyAuthority = prepared.policyAuthority,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .effects         = moved,
                    },
                    .delegation = handler->grant,
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "outside the admitted root effect envelope"
            ));
        }

        SUBCASE("a child cannot name another Project's registration")
        {
            auto const foreign = test_support::makeProject("fixture.foreign");
            // The foreign registration owns its own namespace, so its Tool
            // cannot be the same name at all: what the case shows is that a
            // child minted by another registration's catalog owner is refused
            // for being another registration's, whatever it is called.
            auto foreignCatalog = nestedCatalog(
                foreign,
                {ToolCatalogEntry{
                    .name       = "fixture.foreign.read",
                    .descriptor = readOnlyDescriptor({}),
                }}
            );
            REQUIRE(foreignCatalog.has_value());
            auto child = handler->context.issue(
                projectCall(*foreignCatalog, "fixture.foreign.read")
            );
            REQUIRE(child.has_value());
            auto refused = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *child,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation = handler->grant,
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "minted for a different ProjectRegistration"
            ));
        }

        SUBCASE("a child cannot move the run to another controlled target")
        {
            auto elsewhere = addSession(
                prepared,
                ControllerKind::Script,
                "other-target-session",
                "other-target-instance",
                "target-9",
                std::nullopt
            );
            auto otherLease = prepared.store.acquireLease(elsewhere);
            REQUIRE(otherLease.has_value());
            auto child = handler->context.issue(
                projectCall(catalog, k_readOnlyChildTool)
            );
            REQUIRE(child.has_value());
            auto refused = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = elsewhere,
                    .lease           = *otherLease,
                    .root            = root,
                    .call            = *child,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation = handler->grant,
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "would change its durable run authority"
            ));
        }

        SUBCASE("a child cannot re-enter a Tool already running above it")
        {
            auto child = handler->context.issue(
                projectCall(catalog, k_parentTool)
            );
            REQUIRE(child.has_value());
            auto refused = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *child,
                    .policyAuthority = prepared.policyAuthority,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .effects         = rootEffects,
                    },
                    .delegation = handler->grant,
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "would re-enter fixture.nested.parent"
            ));
        }

        SUBCASE("a parent may issue only as many children as it declared")
        {
            auto narrowTools = std::vector<ToolCatalogEntry>{
                ToolCatalogEntry{
                    .name       = std::string{k_parentTool},
                    .descriptor = readOnlyDescriptor(ChildEffectDeclaration{
                        .childToolNames         = {std::string{k_readOnlyChildTool}},
                        .maximumChildSurface    = ToolSurface::Semantic,
                        .maximumChildMutability = ToolMutability::ReadOnly,
                        .maximumChildRisk       = Risk::ReadOnly,
                        .maximumChildCalls      = 1U,
                    }),
                },
                ToolCatalogEntry{
                    .name       = std::string{k_readOnlyChildTool},
                    .descriptor = readOnlyDescriptor({}),
                },
            };
            auto narrowOwner = nestedCatalog(
                prepared.project,
                std::move(narrowTools)
            );
            REQUIRE(narrowOwner.has_value());
            auto const& narrow = *narrowOwner;
            auto const narrowRoot = rootFor("nested-child-count");
            auto narrowContext = ToolCallIssuingContext::forRoot(
                narrowRoot,
                executionIdentity()
            );
            auto narrowHandler = startReadOnlyHandler(
                prepared,
                controller,
                lease,
                narrowRoot,
                narrowContext,
                projectCall(narrow, k_parentTool)
            );
            REQUIRE_MESSAGE(
                narrowHandler.has_value(),
                failureText(narrowHandler)
            );
            auto first = narrowHandler->context.issue(
                projectCall(narrow, k_readOnlyChildTool)
            );
            REQUIRE(first.has_value());
            auto admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = narrowRoot,
                    .call            = *first,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation = narrowHandler->grant,
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            completeReadOnlyChild(prepared, *admitted);
            auto second = narrowHandler->context.issue(
                projectCall(narrow, k_readOnlyChildTool)
            );
            REQUIRE(second.has_value());
            auto refused = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = narrowRoot,
                    .call            = *second,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation = narrowHandler->grant,
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "may issue at most 1 child calls"
            ));
        }

        SUBCASE("a child a parent never declared is refused by name")
        {
            auto child = handler->context.issue(
                frameworkInvocation(framework, "framework.workflow.wait", R"({"duration_ms":10})")
            );
            REQUIRE(child.has_value());
            auto refused = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *child,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation = handler->grant,
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "declares no child effect for framework.workflow.wait"
            ));
        }

        SUBCASE("a root-positioned call may not carry a grant, and a child must")
        {
            auto orphan = runContext.issue(
                projectCall(catalog, k_readOnlyChildTool)
            );
            REQUIRE(orphan.has_value());
            auto refusedGrant = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *orphan,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation = handler->grant,
                }
            );
            REQUIRE_FALSE(refusedGrant.has_value());
            CHECK(refusedGrant.error().message().contains(
                "carries a delegation grant for a root-positioned call"
            ));

            auto child = handler->context.issue(
                projectCall(catalog, k_readOnlyChildTool)
            );
            REQUIRE(child.has_value());
            auto refusedChild = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *child,
                    .policyAuthority = prepared.policyAuthority,
                }
            );
            REQUIRE_FALSE(refusedChild.has_value());
            CHECK(refusedChild.error().message().contains(
                "requires a delegation grant for a child call"
            ));
        }
    }

    TEST_CASE("a Tool that registered no child effect can delegate nothing")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto const silent = declaredCatalog(prepared.project, {});
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;

        auto const root = rootFor("nested-no-declaration");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto const rootEffects = std::vector{
            inputEffect(controller.controlledTargetId(), Risk::Medium),
        };
        auto call = runContext.issue(projectCall(silent, k_parentTool));
        REQUIRE(call.has_value());
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = rootEffects,
                },
            }
        );
        REQUIRE(admitted.has_value());
        REQUIRE(prepared.store.beginToolCallDispatch(*admitted).has_value());
        auto refused = prepared.store.issueToolDelegationGrant(*call);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "registered no child effect"
        ));
    }

    TEST_CASE("a read-only root compiles no envelope for a mutating child")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto tools = std::vector<ToolCatalogEntry>{
            ToolCatalogEntry{
                .name       = std::string{k_parentTool},
                .descriptor = readOnlyDescriptor(everyChildDeclaration()),
            },
        };
        auto catalog = nestedCatalog(prepared.project, std::move(tools));
        REQUIRE(catalog.has_value());
        auto const framework = frameworkCatalog();
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;

        auto const root = rootFor("nested-read-only-root");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto parent = runContext.issue(projectCall(*catalog, k_parentTool));
        REQUIRE(parent.has_value());
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *parent,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE(admitted.has_value());
        REQUIRE(prepared.store.beginToolCallDispatch(*admitted).has_value());
        auto grant = prepared.store.issueToolDelegationGrant(*parent);
        REQUIRE(grant.has_value());
        auto handlerContext = ToolCallIssuingContext::forHandler(*parent);
        auto child = handlerContext.issue(frameworkInvocation(
            framework,
            k_coordinateInputTool,
            R"({"action":"click","x":1,"y":2})"
        ));
        REQUIRE(child.has_value());
        auto const effects = std::vector{
            inputEffect(controller.controlledTargetId(), Risk::Medium),
        };
        auto refused = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *child,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
                .delegation = *grant,
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "outside the admitted root effect envelope"
        ));
    }

    TEST_CASE("a nested call tree is depth bounded")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;

        // Five distinct read-only Tools, each declaring the next as its one
        // child. Distinct names keep this case about depth and never about the
        // recursion rule.
        constexpr auto k_levels = std::array{
            std::string_view{"fixture.nested.level-1"},
            std::string_view{"fixture.nested.level-2"},
            std::string_view{"fixture.nested.level-3"},
            std::string_view{"fixture.nested.level-4"},
            std::string_view{"fixture.nested.level-5"},
        };
        auto tools = std::vector<ToolCatalogEntry>{};
        for (auto index = std::size_t{}; index < k_levels.size(); ++index)
        {
            auto declaration = ChildEffectDeclaration{};
            if (index + 1U < k_levels.size())
            {
                declaration = ChildEffectDeclaration{
                    .childToolNames = {std::string{k_levels[index + 1U]}},
                    .maximumChildSurface    = ToolSurface::Semantic,
                    .maximumChildMutability = ToolMutability::ReadOnly,
                    .maximumChildRisk       = Risk::ReadOnly,
                    .maximumChildCalls      = 1U,
                };
            }
            tools.emplace_back(ToolCatalogEntry{
                .name       = std::string{k_levels[index]},
                .descriptor = readOnlyDescriptor(std::move(declaration)),
            });
        }
        auto catalog = nestedCatalog(prepared.project, std::move(tools));
        REQUIRE(catalog.has_value());

        auto const root = rootFor("nested-depth");
        auto context = ToolCallIssuingContext::forRoot(root, executionIdentity());
        auto grant   = std::optional<ToolDelegationGrant>{};
        for (auto index = std::size_t{}; index < k_levels.size(); ++index)
        {
            auto call = context.issue(projectCall(*catalog, k_levels[index]));
            REQUIRE(call.has_value());
            auto admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *call,
                    .policyAuthority = prepared.policyAuthority,
                    .delegation      = grant,
                }
            );
            if (index + 1U == k_levels.size())
            {
                REQUIRE_FALSE(admitted.has_value());
                CHECK(admitted.error().message().contains(
                    "Tool call tree depth 5 exceeds the maximum 4"
                ));
                break;
            }
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(prepared.store.beginToolCallDispatch(*admitted).has_value());
            auto issued = prepared.store.issueToolDelegationGrant(*call);
            REQUIRE(issued.has_value());
            grant   = *std::move(issued);
            context = ToolCallIssuingContext::forHandler(*call);
        }
    }

    TEST_CASE("an exhausted root budget refuses the next child call")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto const catalog = declaredCatalog(
            prepared.project,
            everyChildDeclaration()
        );
        auto agent = addSession(
            prepared,
            ControllerKind::Agent,
            "budget-session",
            "budget-instance",
            "target-3",
            AgentBudget{
                .maximumToolCalls     = 1U,
                .maximumMutations     = 1U,
                .maximumObservations  = 1U,
                .maximumElapsedMillis = k_boundMillis,
                .maximumRiskUnits     = 4U,
            }
        );
        auto lease = prepared.store.acquireLease(agent);
        REQUIRE(lease.has_value());

        auto const root = rootFor("nested-budget");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto const rootEffects = std::vector{
            inputEffect(agent.controlledTargetId(), Risk::Medium),
        };
        auto handler = startHandler(
            prepared,
            agent,
            *lease,
            root,
            runContext,
            projectCall(catalog, k_parentTool),
            rootEffects
        );
        REQUIRE(handler.has_value());
        auto child = handler->context.issue(
            projectCall(catalog, k_readOnlyChildTool)
        );
        REQUIRE(child.has_value());
        auto refused = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = agent,
                .lease           = *lease,
                .root            = root,
                .call            = *child,
                .policyAuthority = prepared.policyAuthority,
                .delegation = handler->grant,
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "tool-call budget is exhausted"
        ));
    }

    TEST_CASE("a child arriving under a parent that is not dispatching is refused")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto tools = std::vector<ToolCatalogEntry>{
            ToolCatalogEntry{
                .name       = std::string{k_parentTool},
                .descriptor = readOnlyDescriptor(everyChildDeclaration()),
            },
            ToolCatalogEntry{
                .name       = std::string{k_readOnlyChildTool},
                .descriptor = readOnlyDescriptor({}),
            },
        };
        auto catalog = nestedCatalog(prepared.project, std::move(tools));
        REQUIRE(catalog.has_value());
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;

        auto const root = rootFor("nested-parent-moved");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto parent = runContext.issue(projectCall(*catalog, k_parentTool));
        REQUIRE(parent.has_value());
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *parent,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE(admitted.has_value());
        auto dispatch = prepared.store.beginToolCallDispatch(*admitted);
        REQUIRE(dispatch.has_value());
        auto grant = prepared.store.issueToolDelegationGrant(*parent);
        REQUIRE(grant.has_value());
        auto handlerContext = ToolCallIssuingContext::forHandler(*parent);
        auto child = handlerContext.issue(
            projectCall(*catalog, k_readOnlyChildTool)
        );
        REQUIRE(child.has_value());

        // The handler returns: its call reaches a terminal outcome, and the
        // context that could issue children with it is over.
        auto result = CanonicalJson::parseExact(R"({"handled":true})");
        REQUIRE(result.has_value());
        REQUIRE(prepared.store.completeToolCallDispatch(
            *dispatch,
            ToolCallCompletion::confirmed(*result)
        ).has_value());

        auto refused = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *child,
                .policyAuthority = prepared.policyAuthority,
                .delegation      = *grant,
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "arrived under a different parent"
        ));
        CHECK(refused.error().message().contains("is confirmed"));
    }

    TEST_CASE("a restarted context that terminates early leaves recorded calls")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto const catalog = declaredCatalog(prepared.project, {});
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;

        auto tools = std::vector<ToolCatalogEntry>{
            ToolCatalogEntry{
                .name       = std::string{k_readOnlyChildTool},
                .descriptor = readOnlyDescriptor({}),
            },
        };
        auto readCatalog = nestedCatalog(prepared.project, std::move(tools));
        REQUIRE(readCatalog.has_value());

        auto const root = rootFor("nested-unconsumed");
        auto recorded = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        for (auto index = 0; index < 2; ++index)
        {
            auto call = recorded.issue(
                projectCall(*readCatalog, k_readOnlyChildTool)
            );
            REQUIRE(call.has_value());
            auto admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = controller,
                    .lease           = lease,
                    .root            = root,
                    .call            = *call,
                    .policyAuthority = prepared.policyAuthority,
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            completeReadOnlyChild(prepared, *admitted);
        }
        CHECK(prepared.store.sealToolCallContext(root, recorded).has_value());

        // The restarted run consumes only the first recorded call and ends.
        auto restarted = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto replayed = restarted.issue(
            projectCall(*readCatalog, k_readOnlyChildTool)
        );
        REQUIRE(replayed.has_value());
        REQUIRE(prepared.store.replayToolCall(root, *replayed).has_value());
        auto sealed = prepared.store.sealToolCallContext(root, restarted);
        REQUIRE_FALSE(sealed.has_value());
        CHECK(sealed.error().message().contains(
            "terminated after 1 calls, leaving the recorded call at ordinal 2 "
            "unconsumed"
        ));

        // Section 5.3: that divergence stopped the RUN. What the record already
        // holds still rejoins, and the first position beyond it is refused --
        // which is the whole difference between stopping one call and stopping
        // the run it was in.
        auto recordedSecond = restarted.issue(
            projectCall(*readCatalog, k_readOnlyChildTool)
        );
        REQUIRE(recordedSecond.has_value());
        auto rejoined =
            prepared.store.persistToolCallPosition(root, *recordedSecond);
        REQUIRE_MESSAGE(rejoined.has_value(), failureText(rejoined));
        CHECK(rejoined->lookup == ToolIdentityLookup::Existing);
        auto beyondHistory = restarted.issue(
            projectCall(*readCatalog, k_readOnlyChildTool)
        );
        REQUIRE(beyondHistory.has_value());
        auto stopped =
            prepared.store.persistToolCallPosition(root, *beyondHistory);
        REQUIRE_FALSE(stopped.has_value());
        CHECK(stopped.error().message().contains(
            "was stopped by deterministic-replay divergence"
        ));

        // A context can only be sealed against the root it numbers under.
        auto const elsewhere = rootFor("nested-unconsumed-other-root");
        auto foreign = prepared.store.sealToolCallContext(elsewhere, restarted);
        REQUIRE_FALSE(foreign.has_value());
        CHECK(foreign.error().message().contains(
            "belongs to a different root request"
        ));
        static_cast<void>(catalog);
    }

    TEST_CASE("an observation reference is a compared attribute at its coordinate")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto const framework = frameworkCatalog();
        auto const root = rootFor("nested-observation");

        auto authority = SnapshotObservationAuthority{};
        auto reference = authority.mint(SnapshotObservationSpec{
            .controlledTargetId      = prepared.controller.controlledTargetId(),
            .runtimeArtifactRootHash = prepared.runtimeArtifactRootHash,
            .projectRegistrationHash = prepared.project.registration.hash(),
            .frameIdentityHash       = hashOf("nested-frame"),
            .hostGeneration          = 1U,
            .rootIdentity            = root.identity(),
            .issuingParentIdentity   = std::nullopt,
            .expiresAtUnixMillis     = 1U,
            .localSemanticTargets    = {"fixture.target"},
            .authorizedUiActions     = {"fixture.step"},
        });
        REQUIRE(reference.has_value());

        auto const invocation = frameworkInvocation(
            framework,
            k_observeTool,
            "{}"
        );
        auto observed = ToolCallIssuingContext::forRoot(root, executionIdentity());
        auto observedCall = observed.issueAgainstObservation(
            invocation,
            *reference
        );
        REQUIRE(observedCall.has_value());
        CHECK(
            observedCall->observationReference()
            == std::optional{reference->identity()}
        );
        REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
        REQUIRE(
            prepared.store.persistToolCallPosition(root, *observedCall).has_value()
        );

        // The same coordinate, the same tool and the same arguments, with the
        // observation dropped. The lookup key still matches, so this is a
        // compared attribute rather than a first new call.
        auto unobserved = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto unobservedCall = unobserved.issue(invocation);
        REQUIRE(unobservedCall.has_value());
        CHECK(unobservedCall->sequence() == observedCall->sequence());
        auto refused = prepared.store.persistToolCallPosition(
            root,
            *unobservedCall
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "diverged from durable history: observation_reference changed"
        ));
    }

    TEST_CASE("a child-effect declaration must state one permission, not half of two")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());

        auto const refuse = [&prepared](ChildEffectDeclaration declaration)
        {
            auto tools = std::vector<ToolCatalogEntry>{
                ToolCatalogEntry{
                    .name       = std::string{k_parentTool},
                    .descriptor = readOnlyDescriptor(std::move(declaration)),
                },
            };
            return nestedCatalog(prepared.project, std::move(tools));
        };

        auto namesNoChild = refuse(ChildEffectDeclaration{
            .childToolNames         = {},
            .maximumChildSurface    = ToolSurface::Semantic,
            .maximumChildMutability = ToolMutability::ReadOnly,
            .maximumChildRisk       = Risk::ReadOnly,
            .maximumChildCalls      = 2U,
        });
        REQUIRE_FALSE(namesNoChild.has_value());
        CHECK(namesNoChild.error().message().contains(
            "admits child calls but names no child tool"
        ));

        auto admitsNoCall = refuse(ChildEffectDeclaration{
            .childToolNames         = {std::string{k_readOnlyChildTool}},
            .maximumChildSurface    = ToolSurface::Semantic,
            .maximumChildMutability = ToolMutability::ReadOnly,
            .maximumChildRisk       = Risk::ReadOnly,
            .maximumChildCalls      = 0U,
        });
        REQUIRE_FALSE(admitsNoCall.has_value());
        CHECK(admitsNoCall.error().message().contains(
            "names a child tool but admits no child call"
        ));

        auto namesEmpty = refuse(ChildEffectDeclaration{
            .childToolNames         = {std::string{}},
            .maximumChildSurface    = ToolSurface::Semantic,
            .maximumChildMutability = ToolMutability::ReadOnly,
            .maximumChildRisk       = Risk::ReadOnly,
            .maximumChildCalls      = 1U,
        });
        REQUIRE_FALSE(namesEmpty.has_value());
        CHECK(namesEmpty.error().message().contains(
            "names an empty child tool"
        ));

        auto namesTwice = refuse(ChildEffectDeclaration{
            .childToolNames = {
                std::string{k_readOnlyChildTool},
                std::string{k_readOnlyChildTool},
            },
            .maximumChildSurface    = ToolSurface::Semantic,
            .maximumChildMutability = ToolMutability::ReadOnly,
            .maximumChildRisk       = Risk::ReadOnly,
            .maximumChildCalls      = 2U,
        });
        REQUIRE_FALSE(namesTwice.has_value());
        CHECK(namesTwice.error().message().contains(
            "names one child tool twice"
        ));
    }

    TEST_CASE("a child cannot mint the approval its own effect requires")
    {
        auto temporary            = TemporaryDirectory{};
        auto prepared             = prepareNestedStore(temporary.path());
        auto elevated             = everyChildDeclaration();
        elevated.maximumChildRisk = Risk::High;
        auto const catalog = declaredCatalog(prepared.project, elevated);
        auto const framework = frameworkCatalog();
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;
        auto const target      = controller.controlledTargetId();

        auto approver = addSession(
            prepared,
            ControllerKind::Human,
            "approver-session",
            "approver-instance",
            target,
            std::nullopt,
            SessionMode::Read,
            "approver-controller",
            {std::string{conformance::k_approveCapability}}
        );

        auto const root = rootFor("nested-approval");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto const rootEffects = std::vector{inputEffect(target, Risk::High)};
        auto parent = runContext.issue(projectCall(catalog, k_parentTool));
        REQUIRE(parent.has_value());

        // The root's own elevated effect needs an approval, which is what puts
        // a high-risk effect into the admitted envelope at all.
        auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        REQUIRE(now > 0);
        auto approval = prepared.store.issueToolApproval(
            controller,
            lease,
            approver,
            root,
            *parent,
            prepared.policyAuthority,
            rootEffects,
            ToolApprovalRequest{
                .approverCapability  = std::string{
                    conformance::k_approveCapability,
                },
                .expiresAtUnixMillis = static_cast<uint64>(now) + k_boundMillis,
            },
            AuthorityDecisionId{"nested-approval-decision"}
        );
        REQUIRE_MESSAGE(approval.has_value(), failureText(approval));
        auto const approvals = std::vector{*approval};
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *parent,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects   = rootEffects,
                    .approvals = approvals,
                },
            }
        );
        REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
        REQUIRE(prepared.store.beginToolCallDispatch(*admitted).has_value());
        auto grant = prepared.store.issueToolDelegationGrant(*parent);
        REQUIRE(grant.has_value());
        auto handlerContext = ToolCallIssuingContext::forHandler(*parent);
        auto child = handlerContext.issue(frameworkInvocation(
            framework,
            k_coordinateInputTool,
            R"({"action":"click","x":7,"y":7})"
        ));
        REQUIRE(child.has_value());
        auto refused = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *child,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = rootEffects,
                },
                .delegation = *grant,
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "requires approval before admission"
        ));
    }

    TEST_CASE("a delegation grant reaches exactly one handler invocation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto tools = std::vector<ToolCatalogEntry>{
            ToolCatalogEntry{
                .name       = std::string{k_parentTool},
                .descriptor = readOnlyDescriptor(everyChildDeclaration()),
            },
            ToolCatalogEntry{
                .name       = std::string{k_readOnlyChildTool},
                .descriptor = readOnlyDescriptor({}),
            },
        };
        auto catalog = nestedCatalog(prepared.project, std::move(tools));
        REQUIRE(catalog.has_value());
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;

        auto const root = rootFor("nested-grant-scope");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto first = startReadOnlyHandler(
            prepared,
            controller,
            lease,
            root,
            runContext,
            projectCall(*catalog, k_parentTool)
        );
        REQUIRE_MESSAGE(first.has_value(), failureText(first));

        // A grant one handler holds cannot be presented for a position another
        // handler issued, even when the same actor issued both.
        auto const siblingRoot = rootFor("nested-grant-scope-sibling");
        auto siblingRunContext = ToolCallIssuingContext::forRoot(
            siblingRoot,
            executionIdentity()
        );
        auto sibling = startReadOnlyHandler(
            prepared,
            controller,
            lease,
            siblingRoot,
            siblingRunContext,
            projectCall(*catalog, k_parentTool)
        );
        REQUIRE_MESSAGE(sibling.has_value(), failureText(sibling));
        auto siblingChild = sibling->context.issue(
            projectCall(*catalog, k_readOnlyChildTool)
        );
        REQUIRE(siblingChild.has_value());
        auto crossed = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = siblingRoot,
                .call            = *siblingChild,
                .policyAuthority = prepared.policyAuthority,
                .delegation = first->grant,
            }
        );
        REQUIRE_FALSE(crossed.has_value());
        CHECK(crossed.error().message().contains(
            "names a different parent position"
        ));

        // A context anchored on a call that is not dispatching buys nothing:
        // the position it would number children under has no grant, and the
        // ledger is what says so. That is why ToolCallIssuingContext::forHandler
        // no longer refuses an ancestry it cannot verify -- a restarted
        // dispatcher holds no enclosing context to verify against, and the two
        // refusals it used to make were checks on a caller's bookkeeping rather
        // than on the durable state a child call is admitted from.
        //
        // A handler that has not begun dispatching cannot delegate, and one
        // that never became durable cannot either.
        auto const pendingRoot = rootFor("nested-grant-scope-pending");
        auto pendingContext = ToolCallIssuingContext::forRoot(
            pendingRoot,
            executionIdentity()
        );
        auto pending = pendingContext.issue(
            projectCall(*catalog, k_parentTool)
        );
        REQUIRE(pending.has_value());
        auto undurable = prepared.store.issueToolDelegationGrant(*pending);
        REQUIRE_FALSE(undurable.has_value());
        CHECK(undurable.error().message().contains(
            "requires a durable parent call"
        ));
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = pendingRoot,
                .call            = *pending,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
        auto tooEarly = prepared.store.issueToolDelegationGrant(*pending);
        REQUIRE_FALSE(tooEarly.has_value());
        CHECK(tooEarly.error().message().contains(
            "requires a dispatching handler"
        ));
    }

    TEST_CASE("a parent delegates no surface, mutability or risk it never declared")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareNestedStore(temporary.path());
        auto const framework = frameworkCatalog();
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;
        auto const target      = controller.controlledTargetId();

        auto narrow = ChildEffectDeclaration{
            .childToolNames = {
                std::string{k_coordinateInputTool},
                std::string{k_semanticInputTool},
            },
            .maximumChildSurface    = ToolSurface::Semantic,
            .maximumChildMutability = ToolMutability::ReadOnly,
            .maximumChildRisk       = Risk::ReadOnly,
            .maximumChildCalls      = 4U,
        };
        auto narrowTools = std::vector<ToolCatalogEntry>{
            ToolCatalogEntry{
                .name       = std::string{k_parentTool},
                .descriptor = readOnlyDescriptor(std::move(narrow)),
            },
        };
        auto narrowCatalog = nestedCatalog(
            prepared.project,
            std::move(narrowTools)
        );
        REQUIRE(narrowCatalog.has_value());

        auto const root = rootFor("nested-narrow-ceilings");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );
        auto handler = startReadOnlyHandler(
            prepared,
            controller,
            lease,
            root,
            runContext,
            projectCall(*narrowCatalog, k_parentTool)
        );
        REQUIRE_MESSAGE(handler.has_value(), failureText(handler));

        auto privilegedChild = handler->context.issue(
            frameworkInvocation(
                framework,
                k_coordinateInputTool,
                R"({"action":"click","x":4,"y":9})"
            )
        );
        REQUIRE(privilegedChild.has_value());
        auto const privilegedEffects = std::vector{
            inputEffect(target, Risk::Medium),
        };
        auto refusedSurface = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *privilegedChild,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = privilegedEffects,
                },
                .delegation = handler->grant,
            }
        );
        REQUIRE_FALSE(refusedSurface.has_value());
        CHECK(refusedSurface.error().message().contains(
            "may not delegate the privileged surface of "
            "framework.input.coordinate"
        ));

        auto mutatingChild = handler->context.issue(frameworkInvocation(
            framework,
            k_semanticInputTool,
            R"({"observation_reference":{},"semantic_target":"fixture.target",)"
            R"("ui_action":"fixture.step"})"
        ));
        REQUIRE(mutatingChild.has_value());
        auto const effects = std::vector{inputEffect(target, Risk::Medium)};
        auto refusedMutability = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *mutatingChild,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
                .delegation = handler->grant,
            }
        );
        REQUIRE_FALSE(refusedMutability.has_value());
        CHECK(refusedMutability.error().message().contains(
            "may not delegate the mutating child framework.input.semantic_target"
        ));
    }

    TEST_CASE("a child effect stays at or below the risk the root envelope admits")
    {
        auto temporary            = TemporaryDirectory{};
        auto prepared             = prepareNestedStore(temporary.path());
        auto elevated             = everyChildDeclaration();
        elevated.maximumChildRisk = Risk::High;
        auto const catalog = declaredCatalog(prepared.project, elevated);
        auto const framework = frameworkCatalog();
        auto const& controller = prepared.controller;
        auto const& lease      = prepared.lease;
        auto const target      = controller.controlledTargetId();

        auto const root = rootFor("nested-envelope-risk");
        auto runContext = ToolCallIssuingContext::forRoot(
            root,
            executionIdentity()
        );

        // The root is admitted at low risk, so the envelope Operator compiled
        // for this tree admits low and nothing above it.
        auto const rootEffects = std::vector{inputEffect(target, Risk::Low)};
        auto handler = startHandler(
            prepared,
            controller,
            lease,
            root,
            runContext,
            projectCall(catalog, k_parentTool),
            rootEffects
        );
        REQUIRE_MESSAGE(handler.has_value(), failureText(handler));

        auto child = handler->context.issue(frameworkInvocation(
            framework,
            k_coordinateInputTool,
            R"({"action":"click","x":5,"y":6})"
        ));
        REQUIRE(child.has_value());
        auto const raised = std::vector{inputEffect(target, Risk::Medium)};
        auto refused = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = controller,
                .lease           = lease,
                .root            = root,
                .call            = *child,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = raised,
                },
                .delegation = handler->grant,
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "is above the low the admitted root effect envelope allows"
        ));
    }

    TEST_CASE("every Framework Tool is a leaf")
    {
        auto const catalog = frameworkCatalog();
        constexpr auto k_frameworkTools = std::array{
            std::string_view{"framework.audit.record"},
            std::string_view{"framework.input.coordinate"},
            std::string_view{"framework.input.semantic_target"},
            std::string_view{"framework.screen.observe"},
            std::string_view{"framework.workflow.status"},
            std::string_view{"framework.workflow.wait"},
        };
        for (auto const name : k_frameworkTools)
        {
            auto const descriptor = catalog.describe(name);
            REQUIRE(descriptor.has_value());
            CHECK(descriptor->childEffects.childToolNames.empty());
            CHECK(descriptor->childEffects.maximumChildCalls == 0U);
        }
    }
}
