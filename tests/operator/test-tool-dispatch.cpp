#include <operator/agent-profile.hpp>
#include <operator/controller.hpp>
#include <operator/ledger.hpp>
#include <operator/project-plugin.hpp>
#include <operator/project-tool-dispatch.hpp>
#include <operator/project-tool-program.hpp>
#include <operator/tool-actor-adapters.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-executor.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-root-producer.hpp>

#include <deployment/project-deployment.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include "project-fixture.hpp"
#include "unsafe/operator-database-probe.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Stage 3 of the unified Tool handler cut: the dispatch executor, from an
// admitted call to the terminal durable row that answers it.
//
// What these cases have to prove is the part of H4 that no successful dispatch
// shows. That the issuing context is keyed on the parent's own durable position
// and is FRESH on every entry, numbering from 1 whether this is a first
// dispatch or a re-entry after a crash. That a terminal parent's handler never
// runs at all. That a handler left mid-flight by a dead process re-enters, meets
// its recorded children without executing them, and executes only the first
// position beyond history. That a handler which issues something else is stopped
// with the field that diverged named. And that a fenced-out incarnation cannot
// re-enter and cannot write.
//
// Everything here is production-unreachable: nothing in ProductLifecycle builds
// a dispatcher, and the Framework Tools these runs reach are answered by a
// provider this file installs.
namespace uf::operator_runtime
{
    namespace
    {
        using test_support::hashOf;
        using test_support::TemporaryDirectory;

        template <typename T>
        [[nodiscard]]
        auto failureText(Result<T> const& result) -> std::string
        {
            return result.has_value()
                ? std::string{}
                : std::string{result.error().message()};
        }

        constexpr auto k_pluginId    = std::string_view{"dispatch.project"};
        constexpr auto k_handlerTool = std::string_view{"dispatch.project.handler"};
        constexpr auto k_leafTool    = std::string_view{"dispatch.project.leaf"};
        constexpr auto k_echoTool    = std::string_view{"dispatch.project.echo"};
        constexpr auto k_mutatingTool = std::string_view{"dispatch.project.deliver"};

        // The mutating COMPOSED call: a Project Tool the catalog declares
        // mutating, bound to the same handler entry the read-only one is. Two
        // catalog names over one entry is what lets one authored body be
        // dispatched under both mutabilities, which is the whole of what F5
        // separates -- the discriminator is what answers a call, not what the
        // call is declared to do.
        constexpr auto k_mutatingHandlerTool =
            std::string_view{"dispatch.project.mutate"};

        // The mutating COMPOSED call whose handler raises. It is a second
        // catalog name over its own entry rather than a flag on the handler
        // above, because a Project's answer is opaque now: the only way a
        // composed call fails cleanly is that its own code said so.
        constexpr auto k_refusingHandlerTool =
            std::string_view{"dispatch.project.refuse"};
        constexpr auto k_auditTool   = std::string_view{"framework.audit.record"};

        // The mutating LEAF: a Framework Tool answered by provider code that
        // reaches the world directly. It is the one shape a crash mid-dispatch
        // cannot account for.
        constexpr auto k_inputTool =
            std::string_view{"framework.input.coordinate"};
        constexpr auto k_inputArguments =
            std::string_view{R"({"action":"click","x":1,"y":2})"};
        constexpr auto k_projectEffectType = std::string_view{"dispatch.write"};
        constexpr auto k_inputEffectType =
            std::string_view{"framework.input.deliver"};
        constexpr auto k_effectScopeKind = std::string_view{"controlled_target"};
        constexpr auto k_sessionId   = std::string_view{"dispatch-session"};
        constexpr auto k_controllerId = std::string_view{"dispatch-controller"};
        constexpr auto k_targetId    = std::string_view{"dispatch-target"};
        constexpr auto k_instanceKey = std::string_view{"dispatch-instance"};
        constexpr auto k_otherTargetId = std::string_view{"dispatch-other-target"};
        constexpr auto k_otherInstanceKey =
            std::string_view{"dispatch-other-instance"};

        // One ProjectInstance per actor of the four-way adapter comparison.
        // A unique index admits exactly one active write session per project
        // instance, so three simultaneously pinned actors are three instances;
        // they share one controlled target, which is what keeps the lease
        // exclusive between them and the admitted target identical across them.
        constexpr auto k_agentInstanceKey =
            std::string_view{"dispatch-agent-instance"};
        constexpr auto k_humanInstanceKey =
            std::string_view{"dispatch-human-instance"};
        constexpr auto k_boundMillis = uint64{60'000};

        constexpr auto k_toolCatalogBytes = std::string_view{
            R"({"schema":"umbraflow-tool-catalog/v1","plugin":"dispatch.project"})"
        };
        // The tool closure: five exported entries, six bound Tools.
        //
        // The entries are named for what they answer rather than for a
        // position in a pipeline, because a bound entry has no position: the
        // binding table is the only thing that says which Tool reaches which
        // one, and two Tools may reach the same entry. `handle` proves exactly
        // that -- the read-only and the mutating catalog names are bound to it
        // together.
        //
        // The scoped modules are required inside the handler bodies rather
        // than at module scope. Nothing forces that now that the two closures
        // are separate types, but it keeps each entry's capability visible at
        // the line that spends it.
        //
        // `handle` is the entry under test. It issues one child per name its
        // arguments list, in order, and answers with what each child was
        // recorded as, so its child sequence is a deterministic function of the
        // canonical arguments on its own durable row -- which is the whole of
        // what makes fresh-from-1 re-derivation safe.
        constexpr auto k_toolSource = std::string_view{R"LUAU(
return {
    plugin_id = "dispatch.project",

    handle = function(input)
        local tools = require("@umbraflow/tools")
        local states = {}
        for index = 1, #input.children do
            local name = input.children[index]
            local answer
            if name == "framework.audit.record" then
                answer = tools.call(name, { record = { step = index } })
            else
                answer = tools.call(name, { step = index })
            end
            states[index] = tools.state(answer)
        end
        return { states = states }
    end,

    leaf = function(input)
        return { leaf = input.step }
    end,

    echo = function(input)
        return { echo = input }
    end,

    deliver = function(input)
        return { delivered = input.step }
    end,

    refuse = function(_input)
        error("the fixture handler refused")
    end,
}
)LUAU"};

        [[nodiscard]]
        auto modulesOf(std::string_view source) -> std::vector<ProjectModuleBlob>
        {
            auto blobs = std::vector<ProjectModuleBlob>{};
            blobs.emplace_back(ProjectModuleBlob{
                .name   = "main",
                .source = std::string{source},
            });
            return blobs;
        }

        [[nodiscard]]
        auto toolModules() -> std::vector<ProjectModuleBlob>
        {
            return modulesOf(k_toolSource);
        }

        [[nodiscard]]
        auto boundEntries() -> std::vector<ProjectToolBinding>
        {
            // JCS-ordered by tool name, which is the order the loader writes
            // and the only order a registration may state.
            return {
                ProjectToolBinding{
                    .toolName   = std::string{k_mutatingTool},
                    .entryPoint = "deliver",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_echoTool},
                    .entryPoint = "echo",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_handlerTool},
                    .entryPoint = "handle",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_leafTool},
                    .entryPoint = "leaf",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_mutatingHandlerTool},
                    .entryPoint = "handle",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_refusingHandlerTool},
                    .entryPoint = "refuse",
                },
            };
        }

        // What the tool closure states it exports: the sorted, unique union of
        // the entry points its bindings name, written out rather than derived
        // from boundEntries() -- a declaration computed from the table it is
        // joined against would be the table compared with itself.
        [[nodiscard]]
        auto exportedToolEntries() -> std::vector<std::string>
        {
            return {"deliver", "echo", "handle", "leaf", "refuse"};
        }

        [[nodiscard]]
        auto declaredDescriptor(ChildEffectDeclaration declaration)
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
                    .maximumObservations  = 0U,
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
        auto handlerChildDeclaration() -> ChildEffectDeclaration
        {
            return ChildEffectDeclaration{
                .childToolNames = {
                    std::string{k_mutatingTool},
                    std::string{k_leafTool},
                    std::string{k_auditTool},
                },
                .maximumChildSurface    = ToolSurface::Semantic,
                .maximumChildMutability = ToolMutability::Mutating,
                .maximumChildRisk       = Risk::Medium,
                .maximumChildCalls      = 8U,
            };
        }

        [[nodiscard]]
        auto projectEffectPayloadSchemaHash() -> ContentHash
        {
            return hashOf("dispatch-effect-payload-schema");
        }

        [[nodiscard]]
        auto declaredTool(std::string_view name) -> ToolCatalogEntry
        {
            if (name == k_handlerTool)
            {
                return ToolCatalogEntry{
                    .name       = std::string{name},
                    .descriptor = declaredDescriptor(handlerChildDeclaration()),
                };
            }
            if (name == k_mutatingHandlerTool || name == k_refusingHandlerTool)
            {
                auto descriptor = declaredDescriptor(handlerChildDeclaration());
                descriptor.effectBounds = {
                    EffectBound{
                        .namespacedType    = std::string{k_projectEffectType},
                        .scopeKind         = std::string{k_effectScopeKind},
                        .payloadSchemaHash = projectEffectPayloadSchemaHash(),
                        .maximumRisk       = Risk::Medium,
                    },
                };
                descriptor.mutability  = ToolMutability::Mutating;
                descriptor.idempotency = ToolIdempotency::DeliverySafe;
                return ToolCatalogEntry{
                    .name       = std::string{name},
                    .descriptor = std::move(descriptor),
                };
            }
            if (name == k_mutatingTool)
            {
                // A mutating leaf that declares a bound, so the effect set a
                // scoped child proposes for it is derived rather than empty.
                // What then decides its admission is the admitted root
                // envelope, which is the ceiling the seam cannot see.
                auto descriptor         = declaredDescriptor({});
                descriptor.effectBounds = {
                    EffectBound{
                        .namespacedType    = std::string{k_projectEffectType},
                        .scopeKind         = std::string{k_effectScopeKind},
                        .payloadSchemaHash = projectEffectPayloadSchemaHash(),
                        .maximumRisk       = Risk::Medium,
                    },
                };
                descriptor.mutability  = ToolMutability::Mutating;
                descriptor.idempotency = ToolIdempotency::DeliverySafe;
                return ToolCatalogEntry{
                    .name       = std::string{name},
                    .descriptor = std::move(descriptor),
                };
            }
            return ToolCatalogEntry{
                .name       = std::string{name},
                .descriptor = declaredDescriptor({}),
            };
        }

        [[nodiscard]]
        auto declaredToolNames() -> std::vector<std::string>
        {
            return {
                std::string{k_mutatingTool},
                std::string{k_echoTool},
                std::string{k_handlerTool},
                std::string{k_leafTool},
                std::string{k_mutatingHandlerTool},
                std::string{k_refusingHandlerTool},
            };
        }

        [[nodiscard]]
        auto projectEffect(std::string_view controlledTargetId) -> ProposedEffect
        {
            return ProposedEffect{
                .namespacedType    = std::string{k_projectEffectType},
                .risk              = Risk::Low,
                .scopeKind         = std::string{k_effectScopeKind},
                .scopeKey          = std::string{controlledTargetId},
                .payloadSchemaHash = projectEffectPayloadSchemaHash(),
                .opaqueProjectPayload = R"({"value":1})",
            };
        }

        [[nodiscard]]
        auto frameworkInputEffect(std::string_view controlledTargetId)
            -> ProposedEffect
        {
            auto const catalog = FrameworkToolCatalogOwner::create();
            REQUIRE(catalog.has_value());
            auto const descriptor = catalog->describe(k_inputTool);
            REQUIRE(descriptor.has_value());
            REQUIRE(descriptor->effectBounds.size() == 1U);
            auto const& bound = descriptor->effectBounds.front();
            return ProposedEffect{
                .namespacedType    = bound.namespacedType,
                .risk              = Risk::Low,
                .scopeKind         = bound.scopeKind,
                .scopeKey          = std::string{controlledTargetId},
                .payloadSchemaHash = bound.payloadSchemaHash,
                .opaqueProjectPayload = R"({"value":1})",
            };
        }

        // The seam a registration made only to provision from answers with.
        // A scoped program is required to hold one -- a scoped program with no
        // Tool Runtime is a pure program wearing the wrong type -- and this
        // one refuses every call, because a setup-time registration holds no
        // lease, controller or observation authority for a call to be admitted
        // under. It is one value, never a branch on anything.
        [[nodiscard]]
        auto refusingToolRuntime() -> script::ToolRuntimeInvoke
        {
            return [](
                       std::string_view,
                       json::Value const&,
                       script::ToolCallCoordinate const&,
                       std::stop_token
                   ) -> Result<json::Value>
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "this registration dispatches no Tool call"
                );
            };
        }

        [[nodiscard]]
        auto entryPointArray(std::vector<std::string> const& entries) -> json::Value
        {
            auto items = std::vector<json::Value>{};
            items.reserve(entries.size());
            for (auto const& entry : entries)
            {
                items.emplace_back(json::Value::ofString(entry));
            }
            return json::Value::ofArray(std::move(items));
        }

        [[nodiscard]]
        auto closureValue(ProjectClosureClaims const& closure) -> json::Value
        {
            return json::Value::ofObject({
                {"exported_entry_points",
                 entryPointArray(closure.exportedEntryPoints)},
                {"module_manifest_hash",
                 json::Value::ofString(closure.moduleManifestHash.hex())},
            });
        }

        [[nodiscard]]
        auto generationJcs(ProjectGenerationClaims const& claims) -> std::string
        {
            auto bindings = std::vector<json::Value>{};
            for (auto const& binding : claims.projectToolBindings)
            {
                bindings.emplace_back(json::Value::ofObject({
                    {"entry_point", json::Value::ofString(binding.entryPoint)},
                    {"tool_name", json::Value::ofString(binding.toolName)},
                }));
            }
            return json::canonicalBytes(json::Value::ofObject({
                {"observed_instance_identity_schema_hashes",
                 json::Value::ofArray({})},
                {"plugin_environment_hash",
                 json::Value::ofString(claims.pluginEnvironmentHash.hex())},
                {"plugin_id", json::Value::ofString(claims.pluginId)},
                {"project_registration_format",
                 json::Value::ofNumber(
                     static_cast<double>(claims.projectRegistrationFormat)
                 )},
                {"project_resources", json::Value::ofArray({})},
                {"project_tool_bindings", json::Value::ofArray(std::move(bindings))},
                {"tool_catalog_hash",
                 json::Value::ofString(claims.toolCatalogHash.hex())},
                {"tool_closure", closureValue(claims.toolClosure)},
            }));
        }

        [[nodiscard]]
        auto manifestHashOf(std::vector<ProjectModuleBlob> const& modules)
            -> ContentHash
        {
            auto const hash = derivePluginModuleManifestHash("main", modules);
            REQUIRE(hash.has_value());
            return *hash;
        }

        // This fixture's registration generation, stated in full and read back
        // through the same reader shape a deployment uses. The declared entry
        // set is the one thing stated rather than derived: every digest is
        // taken over the bytes it describes.
        [[nodiscard]]
        auto verifiedGeneration() -> VerifiedProjectGeneration
        {
            auto const environmentHash = currentProjectPluginEnvironmentHash();
            REQUIRE(environmentHash.has_value());
            auto claims = ProjectGenerationClaims{
                .projectRegistrationFormat = k_projectGenerationFormat,
                .pluginId                  = std::string{k_pluginId},
                .toolClosure               = ProjectClosureClaims{
                    .moduleManifestHash  = manifestHashOf(toolModules()),
                    .exportedEntryPoints = exportedToolEntries(),
                },
                .pluginEnvironmentHash                = *environmentHash,
                .toolCatalogHash                      = hashOf(k_toolCatalogBytes),
                .projectResources                     = {},
                .observedInstanceIdentitySchemaHashes = {},
                .projectToolBindings                  = boundEntries(),
            };
            auto const exactJcs = generationJcs(claims);
            auto generation     = ProjectGeneration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                [exactJcs = exactJcs, claims = std::move(claims)](
                    std::string_view candidate
                ) -> Result<ProjectGenerationClaims>
                {
                    if (candidate != exactJcs)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "dispatch fixture generation is not exact JCS"
                        );
                    }
                    return claims;
                }
            );
            REQUIRE(generation.has_value());
            return *std::move(generation);
        }

        // What one dispatch fixture counted. The Framework provider is its one
        // witness that a branch actually ran: it runs once per Framework child
        // that actually EXECUTED rather than replayed. Nothing counts a Project
        // entry's invocations, because the framework never touches a Project's
        // answer.
        struct RunLog final
        {
            uint32 frameworkExecutions{0};
        };

        [[nodiscard]]
        auto frameworkProvider(std::shared_ptr<RunLog> log) -> ToolProvider
        {
            return [log = std::move(log)](
                       ToolCallPositionIdentity const& call
                   ) -> Result<ToolCallCompletion>
            {
                ++log->frameworkExecutions;
                UF_TRY_VALUE(
                    recorded,
                    CanonicalJson::parseExact(json::canonicalBytes(
                        json::Value::ofObject({
                            {"recorded", json::Value::ofString(call.toolName())},
                        })
                    ))
                );
                return ToolCallCompletion::confirmed(std::move(recorded));
            };
        }

        [[nodiscard]]
        auto toolCatalogOwner(
            VerifiedProjectGeneration const& registration
        ) -> ProjectToolCatalogSchemaOwner
        {
            auto owner = ProjectToolCatalogSchemaOwner::create(
                registration,
                k_toolCatalogBytes,
                []() -> Result<std::vector<ToolCatalogEntry>>
                {
                    auto entries = std::vector<ToolCatalogEntry>{};
                    for (auto const& name : declaredToolNames())
                    {
                        entries.emplace_back(declaredTool(name));
                    }
                    return entries;
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
            REQUIRE(owner.has_value());
            return *std::move(owner);
        }

        // One incarnation of the Operator over one runtime directory. It is
        // built in place and never moved after the dispatcher borrows its
        // store: the dispatcher reaches the coordinator on every child call.
        //
        // The observation and plan authorities are members for the same
        // reason: a dispatcher borrows the first and holds the second, and
        // both belong to the run rather than to a case. Building them here
        // once per incarnation is also what keeps every case's dispatcher the
        // same dispatcher.
        //
        // No in-class initializer for the plan authority: it has no default
        // state, so every incarnation must construct one.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct Incarnation final
        {
            OperatorCoordinator store;
            ContentHash         artifactRootHash;
            SessionManifest     manifest;
            ControllerBinding   controller;
            ControlLease        lease;

            OperatorPolicyAuthority policyAuthority;

            SnapshotObservationAuthority observations{};
        };

        // The two effect types this fixture's mutating Tools propose: the
        // Project's own, and the one a Framework input Tool declares. A rule
        // that named neither would decide every mutating admission by the
        // artifact's default deny, which is not what any case here is about.
        // The AgentProfile every incarnation's manifest attests to. It exists
        // because an Agent is one of the four producers the adapter cases
        // below compare, and pinSession requires a verified profile for
        // exactly the kinds whose ControllerProfile says budgets are required.
        // A Script or Human session is pinned against the same manifest and
        // passes no profile at all.
        [[nodiscard]]
        auto agentProfileBytes() -> std::string
        {
            return test_support::agentProfileBytes(
                test_support::k_unconstrainedAgentBudget
            );
        }

        [[nodiscard]]
        auto policyBytes() -> std::string
        {
            auto const types = std::vector<std::string>{
                std::string{k_projectEffectType},
                std::string{k_inputEffectType},
            };
            return conformance::policyArtifactBytes(hashOf("operator"), types);
        }

        // The authority a mutating admission is judged under, built the way a
        // deployment builds one: this fixture's own registration, the session
        // manifest it pins, and the RuntimeModel the Host parsed out of the
        // artifact that manifest names. Nothing here can state a policy the
        // session does not already hold, because admission compares the two.
        //
        // The Host is local: RuntimeModelBinding owns a share of the artifact
        // it was parsed from, so the authority outlives the host that produced
        // it.
        [[nodiscard]]
        auto policyAuthorityFor(
            OperatorCoordinator& store,
            VerifiedProjectGeneration const& registration,
            SessionManifest const& manifest,
            ContentHash const& artifactRootHash
        ) -> OperatorPolicyAuthority
        {
            auto installed =
                store.openActiveInstalledRuntimeArtifact(artifactRootHash);
            REQUIRE_MESSAGE(installed.has_value(), failureText(installed));
            auto observation = conformance::activateObservationHost(
                *std::move(installed),
                test_support::umbraflowProbeFrame(),
                FrameId{301}
            );
            auto const runtimeModel =
                observation.host->runtimeModelBinding(observation.generation);
            REQUIRE(runtimeModel.has_value());
            auto authority = OperatorPolicyAuthority::create(
                registration,
                manifest,
                *runtimeModel,
                "operator",
                policyBytes()
            );
            REQUIRE_MESSAGE(authority.has_value(), failureText(authority));
            return *std::move(authority);
        }

        // Pins a session and takes the lease. A restart clears every lease and
        // deactivates every session, so a second incarnation pins its own --
        // which is exactly section 5.4's "a continuation may bind a newly
        // active session epoch, lease, and fence".
        auto openSession(
            OperatorCoordinator& store,
            VerifiedProjectGeneration const& registration,
            SessionManifest const& manifest,
            std::string_view sessionId,
            std::string_view targetId     = k_targetId,
            std::string_view instanceKey  = k_instanceKey,
            ControllerKind kind           = ControllerKind::Script,
            std::string_view controllerId = k_controllerId
        ) -> std::pair<ControllerBinding, ControlLease>
        {
            auto const worldScope = ObservedInstanceWorldScope::run(
                std::string{targetId},
                1
            );
            REQUIRE(worldScope.has_value());

            // A profile is required for exactly the kinds whose
            // ControllerProfile says budgets are required, and refused for the
            // others, so the kind decides this rather than the caller.
            auto profile = std::optional<AgentProfile>{};
            if (controllerProfile(kind).budgetsRequired)
            {
                auto verified = AgentProfile::verifyExact(
                    manifest,
                    "agent-profile.json",
                    agentProfileBytes(),
                    test_support::agentProfileValidator()
                );
                REQUIRE_MESSAGE(verified.has_value(), failureText(verified));
                profile = *std::move(verified);
            }
            auto const pinned = store.pinSession(
                SessionPin{
                    .sessionId                 = std::string{sessionId},
                    .authenticatedControllerId = std::string{controllerId},
                    .idempotencyNamespace      = std::string{controllerId},
                    .projectRegistrationHash   = registration.hash(),
                    .controllerCapabilities    = {
                        std::string{conformance::k_operateCapability},
                    },
                    .controlledTargetId = std::string{targetId},
                    .projectInstanceKey = std::string{instanceKey},
                    .mode               = SessionMode::Write,
                    .kind               = kind,
                    .worldScope         = *worldScope,
                },
                manifest,
                profile
            );
            REQUIRE_MESSAGE(pinned.has_value(), failureText(pinned));
            auto controller = store.bindController(std::string{sessionId});
            REQUIRE_MESSAGE(controller.has_value(), failureText(controller));
            auto lease = store.acquireLease(*controller);
            REQUIRE_MESSAGE(lease.has_value(), failureText(lease));
            return {*std::move(controller), *std::move(lease)};
        }

        [[nodiscard]]
        auto firstIncarnation(
            std::filesystem::path const& path,
            VerifiedProjectGeneration const& registration
        ) -> Incarnation
        {
            auto const release = test_support::runtimeRelease(path / "release");
            auto storeResult   = OperatorCoordinator::open(path / "production");
            REQUIRE_MESSAGE(storeResult.has_value(), failureText(storeResult));
            auto store     = *std::move(storeResult);
            auto installed = store.installRuntimeArtifact(
                RuntimeArtifactInstallRequest{
                    .handoffRoot                 = release.handoffRoot,
                    .expectedReleaseManifestHash = release.releaseManifestHash,
                    .expectedInstalledGeneration = 0U,
                }
            );
            REQUIRE_MESSAGE(installed.has_value(), failureText(installed));
            auto const artifactRootHash = installed->rootHash();

            // A registration loaded at setup answers no call, so its Tool
            // Runtime seam refuses every one: no lease, controller or
            // observation authority exists yet for a scoped call to be
            // admitted under.
            auto registrar   = ProjectGenerationRegistrar{};
            auto provisioned = registrar.registerGeneration(
                registration,
                toolCatalogOwner(registration),
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = "main",
                    .modules     = toolModules(),
                },
                {},
                refusingToolRuntime()
            );
            REQUIRE_MESSAGE(provisioned.has_value(), failureText(provisioned));

            auto const manifest = test_support::sessionManifest(
                registration,
                artifactRootHash,
                hashOf(agentProfileBytes()),
                policyBytes()
            );
            REQUIRE(store.registerProject(registration).has_value());
            for (auto const instanceKey : {
                     k_instanceKey,
                     k_otherInstanceKey,
                     k_agentInstanceKey,
                     k_humanInstanceKey,
                 })
            {
                auto const instance = store.provisionProjectInstance(
                    registration,
                    std::string{instanceKey}
                );
                REQUIRE_MESSAGE(instance.has_value(), failureText(instance));
            }

            auto session = openSession(store, registration, manifest, k_sessionId);
            auto authority =
                policyAuthorityFor(store, registration, manifest, artifactRootHash);
            return Incarnation{
                .store            = std::move(store),
                .artifactRootHash = artifactRootHash,
                .manifest         = manifest,
                .controller       = std::move(session.first),
                .lease            = std::move(session.second),
                .policyAuthority  = std::move(authority),
            };
        }

        // The incarnation after a crash: the same runtime directory, opened
        // again. Every lease is cleared and every session deactivated by that
        // open, so this one authenticates the same origin principal under a new
        // session epoch and a new lease.
        [[nodiscard]]
        auto nextIncarnation(
            std::filesystem::path const& path,
            VerifiedProjectGeneration const& registration,
            ContentHash artifactRootHash,
            std::string_view sessionId
        ) -> Incarnation
        {
            auto storeResult = OperatorCoordinator::open(path / "production");
            REQUIRE_MESSAGE(storeResult.has_value(), failureText(storeResult));
            auto store          = *std::move(storeResult);
            auto const manifest = test_support::sessionManifest(
                registration,
                artifactRootHash,
                hashOf(agentProfileBytes()),
                policyBytes()
            );
            auto session = openSession(store, registration, manifest, sessionId);
            auto authority =
                policyAuthorityFor(store, registration, manifest, artifactRootHash);
            return Incarnation{
                .store            = std::move(store),
                .artifactRootHash = artifactRootHash,
                .manifest         = manifest,
                .controller       = std::move(session.first),
                .lease            = std::move(session.second),
                .policyAuthority  = std::move(authority),
            };
        }

        [[nodiscard]]
        auto loadProgram(
            VerifiedProjectGeneration const& registration,
            ProjectGenerationRegistrar& registrar,
            ProjectToolDispatcher const& dispatcher
        ) -> ProjectGenerationHandle
        {
            auto loaded = registrar.registerGeneration(
                registration,
                toolCatalogOwner(registration),
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = "main",
                    .modules     = toolModules(),
                },
                {},
                dispatcher.toolRuntimeSeam()
            );
            REQUIRE_MESSAGE(loaded.has_value(), failureText(loaded));
            return *std::move(loaded);
        }

        [[nodiscard]]
        auto rootFor(std::string_view requestKey) -> ToolRootRequestIdentity
        {
            auto preimage = CanonicalJson::parseExact(R"({"objective":"dispatch"})");
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                std::string{k_controllerId},
                std::string{requestKey},
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            return *std::move(root);
        }

        // The pinned execution identity every incarnation of one run repeats.
        // The environment member is the program's own, so a run that reached a
        // different scoped generation would diverge on it by name.
        [[nodiscard]]
        auto executionIdentity(ProjectGenerationHandle const& program)
            -> ToolExecutionIdentity
        {
            return ToolExecutionIdentity{
                .runIdentity                 = hashOf("dispatch-run"),
                .frameworkReleaseIdentity    = hashOf("dispatch-framework"),
                .toolRuntimeProtocolIdentity = hashOf("dispatch-protocol"),
                .environmentIdentity         = program.environmentIdentity(),
            };
        }

        [[nodiscard]]
        auto invocationOf(
            ProjectGenerationHandle const& program,
            std::string_view toolName,
            std::string_view exactArgumentsJcs
        ) -> ValidatedToolInvocation
        {
            auto arguments = CanonicalJson::parseExact(
                std::string{exactArgumentsJcs}
            );
            REQUIRE(arguments.has_value());
            auto invocation = program.catalog().validate(
                std::string{toolName},
                std::move(*arguments)
            );
            REQUIRE_MESSAGE(invocation.has_value(), failureText(invocation));
            return *std::move(invocation);
        }

        // The root-positioned call an actor's start would mint. Stage 4 owns
        // the producer; what matters here is that the position it hands the
        // dispatcher is an ordinary one.
        [[nodiscard]]
        auto projectRootCall(
            ProjectGenerationHandle const& program,
            ToolRootRequestIdentity const& root,
            std::string_view toolName,
            std::string_view exactArgumentsJcs
        ) -> ToolCallPositionIdentity
        {
            auto context = ToolCallIssuingContext::forRoot(
                root,
                executionIdentity(program)
            );
            auto call = context.issue(
                invocationOf(program, toolName, exactArgumentsJcs)
            );
            REQUIRE_MESSAGE(call.has_value(), failureText(call));
            return *std::move(call);
        }

        [[nodiscard]]
        auto rootCall(
            ProjectGenerationHandle const& program,
            ToolRootRequestIdentity const& root,
            std::string_view exactArgumentsJcs
        ) -> ToolCallPositionIdentity
        {
            return projectRootCall(
                program,
                root,
                k_handlerTool,
                exactArgumentsJcs
            );
        }

        [[nodiscard]]
        auto frameworkInvocation(
            std::string_view toolName,
            std::string_view exactArgumentsJcs
        ) -> ValidatedToolInvocation
        {
            auto arguments =
                CanonicalJson::parseExact(std::string{exactArgumentsJcs});
            REQUIRE(arguments.has_value());
            auto const catalog = FrameworkToolCatalogOwner::create();
            REQUIRE(catalog.has_value());
            auto invocation =
                catalog->validate(std::string{toolName}, std::move(*arguments));
            REQUIRE_MESSAGE(invocation.has_value(), failureText(invocation));
            return *std::move(invocation);
        }

        // A root-positioned call of a Framework Tool: the leaf half of the
        // composed/leaf cut, minted through the same issuing context.
        [[nodiscard]]
        auto frameworkToolCall(
            ProjectGenerationHandle const& program,
            ToolRootRequestIdentity const& root,
            std::string_view toolName,
            std::string_view exactArgumentsJcs
        ) -> ToolCallPositionIdentity
        {
            auto context = ToolCallIssuingContext::forRoot(
                root,
                executionIdentity(program)
            );
            auto call =
                context.issue(frameworkInvocation(toolName, exactArgumentsJcs));
            REQUIRE_MESSAGE(call.has_value(), failureText(call));
            return *std::move(call);
        }

        [[nodiscard]]
        auto payloadOf(ToolCallReplay const& replay) -> std::string
        {
            return replay.payload ? replay.payload->bytes() : std::string{};
        }
    } // namespace

    TEST_CASE("a dispatched Project Tool runs its bound entry and records its answer")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE_MESSAGE(dispatcher.has_value(), failureText(dispatcher));
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const root = rootFor("dispatch-happy-path");
        auto const call = rootCall(
            program,
            root,
            R"({"children":["dispatch.project.leaf","framework.audit.record","dispatch.project.leaf"]})"
        );

        auto const answered = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
        CHECK(answered->state == ToolCallState::Confirmed);
        CHECK(payloadOf(*answered) == R"({"states":["confirmed","confirmed","confirmed"]})");

        // Two nested Project handlers ran beside the parent, and the one
        // Framework child reached the provider once.
        CHECK(log->frameworkExecutions == 1U);

        // The children are at ordinals 1..3 under the parent's own position,
        // and there is no fourth. Sealing the context the handler would have
        // built is what says so: it compares what a context issued against what
        // the ledger recorded under that parent.
        auto context = ToolCallIssuingContext::forHandler(call);
        auto const first =
            context.issue(invocationOf(program, k_leafTool, R"({"step":1})"));
        REQUIRE(first.has_value());
        CHECK(first->sequence() == 1U);
        auto const firstReplay = prepared.store.replayToolCall(root, *first);
        REQUIRE_MESSAGE(firstReplay.has_value(), failureText(firstReplay));
        CHECK(firstReplay->state == ToolCallState::Confirmed);
        CHECK(payloadOf(*firstReplay) == R"({"leaf":1})");

        auto const third =
            context.issue(invocationOf(program, k_leafTool, R"({"step":3})"));
        REQUIRE(third.has_value());
        auto const skipped =
            context.issue(invocationOf(program, k_leafTool, R"({"step":2})"));
        REQUIRE(skipped.has_value());
        CHECK(context.issuedChildren() == 3U);
        CHECK(
            prepared.store.sealToolCallContext(root, context).has_value()
        );
    }

    TEST_CASE("a terminal parent returns its recorded result rather than dispatching again")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const root = rootFor("dispatch-terminal-parent");
        auto const call = rootCall(
            program,
            root,
            R"({"children":["framework.audit.record"]})"
        );

        auto const first = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(first.has_value(), failureText(first));
        CHECK(first->state == ToolCallState::Confirmed);
        CHECK(log->frameworkExecutions == 1U);

        // The second entry meets a terminal row and answers from it: the
        // recorded result comes back byte for byte, and the Framework child
        // under it is not executed a second time.
        auto const again = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(again.has_value(), failureText(again));
        CHECK(again->state == ToolCallState::Confirmed);
        CHECK(payloadOf(*again) == payloadOf(*first));
        CHECK(log->frameworkExecutions == 1U);
    }

    TEST_CASE("an admitted call whose dispatch never began dispatches against empty history")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const root = rootFor("dispatch-admitted-only");
        auto const call = rootCall(
            program,
            root,
            R"({"children":["dispatch.project.leaf"]})"
        );

        // Admission and nothing else, which is the durable state a crash
        // between admission and the dispatch boundary leaves behind.
        REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
        auto const admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            }
        );
        REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
        auto const beforeDispatch = prepared.store.replayToolCall(root, call);
        REQUIRE(beforeDispatch.has_value());
        CHECK(beforeDispatch->state == ToolCallState::Admitted);

        auto const answered = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
        CHECK(answered->state == ToolCallState::Confirmed);
        CHECK(payloadOf(*answered) == R"({"states":["confirmed"]})");
    }

    TEST_CASE("a handler killed mid-dispatch re-enters, numbers from one and executes only past history")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto const root         = rootFor("dispatch-crash-restart");
        auto artifactRootHash = std::optional<ContentHash>{};
        auto callIdentity     = std::optional<ContentHash>{};

        // The first incarnation: it crosses the dispatch boundary, records
        // children 1 and 2 exactly as the handler would issue them, and then
        // dies without ever completing its own call.
        {
            auto prepared  = firstIncarnation(temporary.path(), registration);
            auto const log = std::make_shared<RunLog>();
            artifactRootHash = prepared.artifactRootHash;

            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                prepared.observations,
                prepared.policyAuthority,
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectGenerationRegistrar{};
            auto const program =
                loadProgram(registration, registrar, *dispatcher);

            auto const call = rootCall(
                program,
                root,
                R"({"children":["dispatch.project.leaf","framework.audit.record","dispatch.project.leaf"]})"
            );
            callIdentity = call.identity();

            REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*admitted).has_value()
            );
            auto const grant = prepared.store.issueToolDelegationGrant(call);
            REQUIRE_MESSAGE(grant.has_value(), failureText(grant));

            auto context = ToolCallIssuingContext::forHandler(call);
            auto const one =
                context.issue(invocationOf(program, k_leafTool, R"({"step":1})"));
            REQUIRE(one.has_value());
            auto const first = dispatcher->dispatch(
                program,
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = *one,
                    .delegation = *grant,
                },
                std::stop_token{}
            );
            REQUIRE_MESSAGE(first.has_value(), failureText(first));

            auto arguments = CanonicalJson::parseExact(R"({"record":{"step":2}})");
            REQUIRE(arguments.has_value());
            auto const frameworkCatalog = FrameworkToolCatalogOwner::create();
            REQUIRE(frameworkCatalog.has_value());
            auto auditInvocation = frameworkCatalog->validate(
                std::string{k_auditTool},
                std::move(*arguments)
            );
            REQUIRE_MESSAGE(auditInvocation.has_value(), failureText(auditInvocation));
            auto const second = context.issue(*auditInvocation);
            REQUIRE(second.has_value());
            auto const audited = ToolRuntimeExecutor{prepared.store}.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = *second,
                    .delegation = *grant,
                },
                frameworkProvider(log)
            );
            REQUIRE_MESSAGE(audited.has_value(), failureText(audited));

            CHECK(log->frameworkExecutions == 1U);

            auto const midFlight = prepared.store.replayToolCall(root, call);
            REQUIRE(midFlight.has_value());
            CHECK(midFlight->state == ToolCallState::Dispatching);
        }

        // The second incarnation re-enters. The handler runs from the top, its
        // context is fresh and numbers from 1, children 1 and 2 meet their
        // recorded rows and execute nothing, and child 3 is the first position
        // beyond history.
        REQUIRE(artifactRootHash.has_value());
        REQUIRE(callIdentity.has_value());
        auto prepared = nextIncarnation(
            temporary.path(),
            registration,
            *artifactRootHash,
            "dispatch-session-restarted"
        );
        auto const log = std::make_shared<RunLog>();
        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const call = rootCall(
            program,
            root,
            R"({"children":["dispatch.project.leaf","framework.audit.record","dispatch.project.leaf"]})"
        );
        CHECK(call.identity() == *callIdentity);

        // The restart left this row dispatching rather than recovering it as
        // uncertain. A read-only Project call delivered nothing and is answered
        // by a bound handler, so there is nothing for a restart to be uncertain
        // about -- and a row declared uncertain here could never be re-entered.
        auto const afterRestart = prepared.store.replayToolCall(root, call);
        REQUIRE_MESSAGE(afterRestart.has_value(), failureText(afterRestart));
        CHECK(afterRestart->state == ToolCallState::Dispatching);

        auto const answered = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
        CHECK(answered->state == ToolCallState::Confirmed);
        CHECK(payloadOf(*answered) == R"({"states":["confirmed","confirmed","confirmed"]})");

        // The Framework child of ordinal 2 was recorded before the crash, so it
        // replayed: this incarnation's provider never ran. Only child 3 was new,
        // and only its handler ran beside the parent's own.
        CHECK(log->frameworkExecutions == 0U);

        // Exactly three children exist under the parent. A resumed counter
        // would have numbered this entry's first call 3 and left five.
        auto sealing = ToolCallIssuingContext::forHandler(call);
        for (auto index = 0U; index < 3U; ++index)
        {
            REQUIRE(
                sealing.issue(invocationOf(program, k_leafTool, R"({"step":9})"))
                    .has_value()
            );
        }
        CHECK(prepared.store.sealToolCallContext(root, sealing).has_value());

        auto shortContext = ToolCallIssuingContext::forHandler(call);
        REQUIRE(
            shortContext.issue(invocationOf(program, k_leafTool, R"({"step":9})"))
                .has_value()
        );
        auto const unconsumed =
            prepared.store.sealToolCallContext(root, shortContext);
        REQUIRE_FALSE(unconsumed.has_value());
        CHECK(unconsumed.error().message().contains(
            "leaving the recorded call at ordinal 2 unconsumed"
        ));
    }

    TEST_CASE("a handler that terminates leaving a recorded call unconsumed is stopped")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto const root         = rootFor("dispatch-unconsumed");
        auto artifactRootHash   = std::optional<ContentHash>{};

        // History under this parent carries two children. The handler derives
        // one from its own arguments, so the re-entry consumes ordinal 1 and
        // leaves ordinal 2 behind -- which is divergence even though every call
        // it did issue matched.
        {
            auto prepared  = firstIncarnation(temporary.path(), registration);
            auto const log = std::make_shared<RunLog>();
            artifactRootHash = prepared.artifactRootHash;

            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                prepared.observations,
                prepared.policyAuthority,
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectGenerationRegistrar{};
            auto const program =
                loadProgram(registration, registrar, *dispatcher);

            auto const call =
                rootCall(program, root, R"({"children":["dispatch.project.leaf"]})");
            REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*admitted).has_value()
            );
            auto const grant = prepared.store.issueToolDelegationGrant(call);
            REQUIRE(grant.has_value());

            auto context = ToolCallIssuingContext::forHandler(call);
            for (auto const* arguments :
                 {R"({"step":1})", R"({"step":2})"})
            {
                auto const child = context.issue(
                    invocationOf(program, k_leafTool, arguments)
                );
                REQUIRE(child.has_value());
                REQUIRE(dispatcher->dispatch(
                    program,
                    ToolAdmissionRequest{
                        .controller = prepared.controller,
                        .lease      = prepared.lease,
                        .root       = root,
                        .call       = *child,
                        .delegation = *grant,
                    },
                    std::stop_token{}
                ).has_value());
            }
        }

        REQUIRE(artifactRootHash.has_value());
        auto prepared = nextIncarnation(
            temporary.path(),
            registration,
            *artifactRootHash,
            "dispatch-session-unconsumed"
        );
        auto const log  = std::make_shared<RunLog>();
        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const call =
            rootCall(program, root, R"({"children":["dispatch.project.leaf"]})");
        auto const answered = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
        CHECK(answered->state == ToolCallState::TerminalFailure);
        CHECK(payloadOf(*answered).contains(
            "terminated after 1 calls, leaving the recorded call at ordinal 2 "
            "unconsumed"
        ));
    }

    TEST_CASE("a handler that issues a different call is stopped at the field that diverged")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto const root         = rootFor("dispatch-divergence");
        auto artifactRootHash   = std::optional<ContentHash>{};

        // A first incarnation whose recorded child 1 is NOT the call this
        // handler derives from its own arguments. That is what a
        // nondeterministic handler leaves behind, expressed exactly.
        {
            auto prepared  = firstIncarnation(temporary.path(), registration);
            auto const log = std::make_shared<RunLog>();
            artifactRootHash = prepared.artifactRootHash;

            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                prepared.observations,
                prepared.policyAuthority,
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectGenerationRegistrar{};
            auto const program =
                loadProgram(registration, registrar, *dispatcher);

            auto const call = rootCall(
                program,
                root,
                R"({"children":["dispatch.project.leaf"]})"
            );
            REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*admitted).has_value()
            );
            auto const grant = prepared.store.issueToolDelegationGrant(call);
            REQUIRE(grant.has_value());

            auto context = ToolCallIssuingContext::forHandler(call);
            auto const other =
                context.issue(invocationOf(program, k_leafTool, R"({"step":99})"));
            REQUIRE(other.has_value());
            REQUIRE(dispatcher->dispatch(
                program,
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = *other,
                    .delegation = *grant,
                },
                std::stop_token{}
            ).has_value());
        }

        REQUIRE(artifactRootHash.has_value());
        auto prepared = nextIncarnation(
            temporary.path(),
            registration,
            *artifactRootHash,
            "dispatch-session-diverged"
        );
        auto const log  = std::make_shared<RunLog>();
        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const call =
            rootCall(program, root, R"({"children":["dispatch.project.leaf"]})");
        auto const answered = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));

        // The run stopped, and the durable outcome of the call that was running
        // names the field that diverged rather than reporting an unspecified
        // replay failure.
        CHECK(answered->state == ToolCallState::TerminalFailure);
        CHECK(payloadOf(*answered).contains("canonical_args changed"));
        CHECK(payloadOf(*answered).contains("ordinal 1 under parent coordinate"));
    }

    TEST_CASE("a fenced-out incarnation can neither re-enter its dispatch nor complete it")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const root = rootFor("dispatch-zombie");
        auto const call =
            rootCall(program, root, R"({"children":["dispatch.project.leaf"]})");

        REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
        auto const admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            }
        );
        REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
        auto const zombieDispatch =
            prepared.store.beginToolCallDispatch(*admitted);
        REQUIRE_MESSAGE(zombieDispatch.has_value(), failureText(zombieDispatch));
        auto const zombieLease = prepared.lease;

        // A successor takes the target. The fence moves, and the predecessor is
        // still holding its own lease value and its own dispatch token.
        auto const takeover = prepared.store.takeoverLease(
            prepared.controller,
            "dispatch fixture takeover"
        );
        REQUIRE_MESSAGE(takeover.has_value(), failureText(takeover));
        CHECK(takeover->lease.fencingToken > zombieLease.fencingToken);

        auto const fenced = prepared.store.reenterToolCallDispatch(
            prepared.controller,
            zombieLease,
            root,
            call
        );
        REQUIRE_FALSE(fenced.has_value());
        CHECK(fenced.error().message().contains(
            "re-entry control lease was superseded"
        ));

        // The successor re-enters on the live lease, which moves the history
        // revision. From that moment the predecessor's token names no active
        // dispatch, so its terminal write is refused rather than accepted.
        auto const reentered = prepared.store.reenterToolCallDispatch(
            prepared.controller,
            takeover->lease,
            root,
            call
        );
        REQUIRE_MESSAGE(reentered.has_value(), failureText(reentered));

        auto zombiePayload = CanonicalJson::parseExact(R"({"states":["zombie"]})");
        REQUIRE(zombiePayload.has_value());
        auto const zombieWrite = prepared.store.completeToolCallDispatch(
            *zombieDispatch,
            ToolCallCompletion::confirmed(*std::move(zombiePayload))
        );
        REQUIRE_FALSE(zombieWrite.has_value());
        CHECK(zombieWrite.error().message().contains(
            "does not match the active dispatch"
        ));

        auto successorPayload =
            CanonicalJson::parseExact(R"({"states":["successor"]})");
        REQUIRE(successorPayload.has_value());
        auto const successorWrite = prepared.store.completeToolCallDispatch(
            *reentered,
            ToolCallCompletion::confirmed(*std::move(successorPayload))
        );
        REQUIRE_MESSAGE(successorWrite.has_value(), failureText(successorWrite));
        CHECK(successorWrite->state == ToolCallState::Confirmed);
    }

    TEST_CASE("re-entry is a Project handler's alone and cannot widen its admission")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const root = rootFor("dispatch-reentry-scope");
        auto const call =
            rootCall(program, root, R"({"children":["dispatch.project.leaf"]})");
        REQUIRE(prepared.store.persistToolRootRequest(root).has_value());

        SUBCASE("a call that is not dispatching cannot be re-entered")
        {
            auto const refused = prepared.store.reenterToolCallDispatch(
                prepared.controller,
                prepared.lease,
                root,
                call
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "requires a dispatching call; call"
            ));
        }

        SUBCASE("a binding and a lease that name different authority are refused")
        {
            auto foreignLease         = prepared.lease;
            foreignLease.controllerId = "someone-else";
            auto const refused        = prepared.store.reenterToolCallDispatch(
                prepared.controller,
                foreignLease,
                root,
                call
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "re-entry binding and lease name different authority"
            ));
        }

        SUBCASE("a live authority over another target cannot re-enter this call")
        {
            // A second live session for the same origin principal on a
            // different controlled target. Its binding and lease are live, so
            // the refusal below can only come from the comparison against the
            // admission attempt this re-entry claims to continue.
            REQUIRE(prepared.store.persistToolCallPosition(root, call).has_value());
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*admitted).has_value()
            );

            auto elsewhere = openSession(
                prepared.store,
                registration,
                prepared.manifest,
                "dispatch-session-elsewhere",
                k_otherTargetId,
                k_otherInstanceKey
            );
            auto const refused = prepared.store.reenterToolCallDispatch(
                elsewhere.first,
                elsewhere.second,
                root,
                call
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "different origin principal or controlled target"
            ));
        }

        SUBCASE("a read-only Framework leaf is re-entered like any other call")
        {
            // Being answered by a Framework provider is not what a re-entry
            // refuses. A read-only Tool declares no effect, so running its
            // provider again delivers nothing twice and its interrupted
            // dispatch is ordinary replay.
            auto const frameworkCall = frameworkToolCall(
                program,
                root,
                k_auditTool,
                R"({"record":{"step":1}})"
            );
            REQUIRE(
                prepared.store.persistToolCallPosition(root, frameworkCall)
                    .has_value()
            );
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = frameworkCall,
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*admitted).has_value()
            );
            auto const reentered = prepared.store.reenterToolCallDispatch(
                prepared.controller,
                prepared.lease,
                root,
                frameworkCall
            );
            REQUIRE_MESSAGE(reentered.has_value(), failureText(reentered));
        }

        SUBCASE("a mutating Framework leaf is never re-entered")
        {
            auto const inputCall =
                frameworkToolCall(program, root, k_inputTool, k_inputArguments);
            REQUIRE(
                prepared.store.persistToolCallPosition(root, inputCall)
                    .has_value()
            );
            auto const effects = std::vector{frameworkInputEffect(k_targetId)};
            auto const authority = prepared.policyAuthority;
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = inputCall,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = effects,
                    },
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*admitted).has_value()
            );
            auto const refused = prepared.store.reenterToolCallDispatch(
                prepared.controller,
                prepared.lease,
                root,
                inputCall
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "is a mutating leaf answered directly by a provider"
            ));
        }

        SUBCASE("a mutating Framework leaf cannot report terminal failure")
        {
            // The completion-time half of the same key, and it is refused on
            // the same conjunction rather than on mutability alone: only for a
            // leaf could "it failed" be asserting an absence nothing observed.
            auto const inputCall =
                frameworkToolCall(program, root, k_inputTool, k_inputArguments);
            REQUIRE(
                prepared.store.persistToolCallPosition(root, inputCall)
                    .has_value()
            );
            auto const authority = prepared.policyAuthority;
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = inputCall,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = std::vector{frameworkInputEffect(k_targetId)},
                    },
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            auto const dispatch =
                prepared.store.beginToolCallDispatch(*admitted);
            REQUIRE_MESSAGE(dispatch.has_value(), failureText(dispatch));
            auto const error =
                CanonicalJson::parseExact(R"({"error":"the sink refused"})");
            REQUIRE(error.has_value());
            auto const refused = prepared.store.completeToolCallDispatch(
                *dispatch,
                ToolCallCompletion::terminalFailure(*error)
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("must report possible"));
        }

        SUBCASE("a mutating composed call fails terminally and sets no barrier")
        {
            // The mirror of the two leaf subcases above, and the whole of what
            // keying completion on the composition rather than on mutability
            // buys. This handler fails CLEANLY: its own code raised, no child
            // is unaccounted for, and its declared mutability is Mutating.
            // Classifying that `possible` would set a target-wide mutation
            // barrier only an Operator reconciliation can lift, over an effect
            // that was never the frame's -- its whole effect surface is
            // children, and children carry their own classification.
            auto const failingRoot =
                rootFor("dispatch-composed-clean-failure");
            auto const failing = projectRootCall(
                program,
                failingRoot,
                k_refusingHandlerTool,
                R"({"children":[]})"
            );
            auto const authority = prepared.policyAuthority;
            auto const effects   = std::vector{projectEffect(k_targetId)};
            auto const answered  = dispatcher->dispatch(
                program,
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = failingRoot,
                    .call       = failing,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = effects,
                    },
                },
                std::stop_token{}
            );
            REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
            CHECK(answered->state == ToolCallState::TerminalFailure);
            CHECK(payloadOf(*answered).contains("the fixture handler refused"));

            // The target is still free. A `possible` row would refuse this
            // admission until reconciliation lifted it, so this is the
            // over-refusal itself rather than a proxy for it.
            auto const nextRoot = rootFor("dispatch-composed-after-failure");
            auto const nextCall = projectRootCall(
                program,
                nextRoot,
                k_mutatingHandlerTool,
                R"({"children":[]})"
            );
            REQUIRE(
                prepared.store.persistToolRootRequest(nextRoot).has_value()
            );
            auto const readmitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = nextRoot,
                    .call       = nextCall,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = effects,
                    },
                }
            );
            REQUIRE_MESSAGE(readmitted.has_value(), failureText(readmitted));
        }

        SUBCASE("a mutating Framework leaf's provider failure is recorded uncertain")
        {
            // The executor's half, keyed on the same conjunction. A failing
            // provider here could have moved the world with no row saying so,
            // so its failure becomes uncertainty rather than a terminal claim
            // -- and this is the one shape for which that conversion is right.
            auto const inputCall =
                frameworkToolCall(program, root, k_inputTool, k_inputArguments);
            auto const authority = prepared.policyAuthority;
            auto const answered = ToolRuntimeExecutor{prepared.store}.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = inputCall,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = std::vector{frameworkInputEffect(k_targetId)},
                    },
                },
                [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "the input sink did not prove delivery"
                    );
                }
            );
            REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
            CHECK(answered->state == ToolCallState::Possible);
        }
    }

    TEST_CASE("a dispatcher with no Framework Tool provider is refused")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto prepared = firstIncarnation(temporary.path(), registration);

        auto const refused = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            ToolProvider{}
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "requires a Framework Tool provider"
        ));
    }

    TEST_CASE("the scoped seam resolves its run from the durable coordinate alone")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        SUBCASE("a coordinate no live run is anchored on is refused")
        {
            // The program is the same one the dispatcher drives, entered
            // without a dispatch. One seam serves every run of a registration,
            // so a position it holds no run for is the only thing that can tell
            // it this call belongs to nothing.
            auto const torn = program.invokeBoundTool(
                k_handlerTool,
                json::Value::ofObject({
                    {"children",
                     json::Value::ofArray({
                         json::Value::ofString(std::string{k_leafTool}),
                     })},
                }),
                script::ScopedRunRequest{
                    .parentPosition = hashOf("not-a-live-position"),
                    .cancellation   = std::stop_token{},
                }
            );
            REQUIRE_FALSE(torn.has_value());
            CHECK(torn.error().message().contains(
                "no live issuing context is anchored on the durable position"
            ));
        }

        SUBCASE("a mutating child is produced and judged by the root envelope")
        {
            // The seam refuses no mutating child of its own any more: it
            // derives the effect set from the CHILD'S OWN descriptor bounds
            // and hands the call to admission, which is the only place that
            // holds the ceilings. Here the parent is a read-only root, so its
            // admitted attempt carries no effect envelope at all and the
            // child's derived effect lands outside it. The refusal therefore
            // names the envelope rather than the seam -- and a seam that had
            // proposed nothing would have been refused for having no envelope
            // to judge, which is a different sentence.
            auto const root = rootFor("dispatch-mutating-child");
            auto const call = rootCall(
                program,
                root,
                R"({"children":["dispatch.project.deliver"]})"
            );
            auto const answered = dispatcher->dispatch(
                program,
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                },
                std::stop_token{}
            );
            REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
            CHECK(answered->state == ToolCallState::TerminalFailure);
            CHECK(payloadOf(*answered).contains(
                "Child Tool effect dispatch.write on dispatch-target is "
                "outside the admitted root effect envelope"
            ));
        }
    }

    // F5's pin. A mutating Project Tool is answered by a bound handler, so
    // every effect it has is a child call carrying its own durable
    // classification -- which is why its interrupted dispatch is replayed
    // rather than declared uncertain. What must be true is exactly two things:
    // nothing the first incarnation already did happens a second time, and the
    // call still reaches a terminal answer.
    TEST_CASE("a mutating Project handler killed mid-dispatch repeats no effect and completes")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto const root         = rootFor("dispatch-mutating-crash");
        constexpr auto k_children = std::string_view{
            R"({"children":["dispatch.project.leaf","framework.audit.record","dispatch.project.leaf"]})"
        };
        auto artifactRootHash = std::optional<ContentHash>{};
        auto callIdentity     = std::optional<ContentHash>{};

        // The first incarnation crosses the dispatch boundary, records children
        // 1 and 2 exactly as the handler would issue them -- child 2 reaching
        // the Framework provider, which is the effect that must not happen
        // twice -- and then dies without answering its own call.
        {
            auto prepared  = firstIncarnation(temporary.path(), registration);
            auto const log = std::make_shared<RunLog>();
            artifactRootHash = prepared.artifactRootHash;

            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                prepared.observations,
                prepared.policyAuthority,
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectGenerationRegistrar{};
            auto const program =
                loadProgram(registration, registrar, *dispatcher);

            auto const call = projectRootCall(
                program,
                root,
                k_mutatingHandlerTool,
                k_children
            );
            callIdentity = call.identity();
            REQUIRE(call.descriptor().mutability == ToolMutability::Mutating);

            REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
            auto const authority = prepared.policyAuthority;
            auto const effects  = std::vector{projectEffect(k_targetId)};
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = effects,
                    },
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*admitted).has_value()
            );
            auto const grant = prepared.store.issueToolDelegationGrant(call);
            REQUIRE_MESSAGE(grant.has_value(), failureText(grant));

            auto context = ToolCallIssuingContext::forHandler(call);
            auto const one =
                context.issue(invocationOf(program, k_leafTool, R"({"step":1})"));
            REQUIRE(one.has_value());
            REQUIRE_MESSAGE(
                dispatcher->dispatch(
                    program,
                    ToolAdmissionRequest{
                        .controller = prepared.controller,
                        .lease      = prepared.lease,
                        .root       = root,
                        .call       = *one,
                        .delegation = *grant,
                    },
                    std::stop_token{}
                ).has_value(),
                "child 1 did not reach a terminal row"
            );

            auto const second = context.issue(
                frameworkInvocation(k_auditTool, R"({"record":{"step":2}})")
            );
            REQUIRE(second.has_value());
            auto const audited = ToolRuntimeExecutor{prepared.store}.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = *second,
                    .delegation = *grant,
                },
                frameworkProvider(log)
            );
            REQUIRE_MESSAGE(audited.has_value(), failureText(audited));
            CHECK(log->frameworkExecutions == 1U);

            auto const midFlight = prepared.store.replayToolCall(root, call);
            REQUIRE(midFlight.has_value());
            CHECK(midFlight->state == ToolCallState::Dispatching);
        }

        REQUIRE(artifactRootHash.has_value());
        REQUIRE(callIdentity.has_value());
        auto prepared = nextIncarnation(
            temporary.path(),
            registration,
            *artifactRootHash,
            "dispatch-session-mutating-restart"
        );
        auto const log  = std::make_shared<RunLog>();
        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const call =
            projectRootCall(program, root, k_mutatingHandlerTool, k_children);
        CHECK(call.identity() == *callIdentity);

        // The restart left a MUTATING row dispatching. That is F5: what decides
        // is not the mutability but that a bound handler answers this call, so
        // there is no effect of its own for a restart to be uncertain about.
        auto const afterRestart = prepared.store.replayToolCall(root, call);
        REQUIRE_MESSAGE(afterRestart.has_value(), failureText(afterRestart));
        CHECK(afterRestart->state == ToolCallState::Dispatching);

        auto const answered = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
        CHECK(answered->state == ToolCallState::Confirmed);
        CHECK(payloadOf(*answered) == R"({"states":["confirmed","confirmed","confirmed"]})");

        // No duplicate effect: the Framework child recorded before the crash
        // replayed, so this incarnation's provider never ran. Completion: only
        // child 3 was new, and the parent answered.
        CHECK(log->frameworkExecutions == 0U);

        // And the target is free again, which a call left uncertain would not
        // have allowed: the barrier a dispatching mutation holds is released by
        // the terminal row the re-entry wrote.
        auto const nextRoot = rootFor("dispatch-mutating-after-restart");
        auto const nextCall = projectRootCall(
            program,
            nextRoot,
            k_mutatingHandlerTool,
            R"({"children":[]})"
        );
        REQUIRE(prepared.store.persistToolRootRequest(nextRoot).has_value());
        auto const authority = prepared.policyAuthority;
        auto const effects   = std::vector{projectEffect(k_targetId)};
        auto const readmitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = nextRoot,
                .call       = nextCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .policyAuthority = authority,
                    .effects         = effects,
                },
            }
        );
        REQUIRE_MESSAGE(readmitted.has_value(), failureText(readmitted));
    }

    TEST_CASE("a mutating handler that re-derives a different call is stopped at the field that diverged")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto const root         = rootFor("dispatch-mutating-divergence");
        constexpr auto k_children =
            std::string_view{R"({"children":["dispatch.project.leaf"]})"};
        auto artifactRootHash = std::optional<ContentHash>{};

        {
            auto prepared  = firstIncarnation(temporary.path(), registration);
            auto const log = std::make_shared<RunLog>();
            artifactRootHash = prepared.artifactRootHash;

            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                prepared.observations,
                prepared.policyAuthority,
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectGenerationRegistrar{};
            auto const program =
                loadProgram(registration, registrar, *dispatcher);

            auto const call = projectRootCall(
                program,
                root,
                k_mutatingHandlerTool,
                k_children
            );
            REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
            auto const authority = prepared.policyAuthority;
            auto const effects  = std::vector{projectEffect(k_targetId)};
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = effects,
                    },
                }
            );
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*admitted).has_value()
            );
            auto const grant = prepared.store.issueToolDelegationGrant(call);
            REQUIRE(grant.has_value());

            // Recorded child 1 is NOT the call this handler derives from its
            // own arguments, which is what a nondeterministic handler leaves.
            auto context = ToolCallIssuingContext::forHandler(call);
            auto const other =
                context.issue(invocationOf(program, k_leafTool, R"({"step":99})"));
            REQUIRE(other.has_value());
            REQUIRE(dispatcher->dispatch(
                program,
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = *other,
                    .delegation = *grant,
                },
                std::stop_token{}
            ).has_value());
        }

        REQUIRE(artifactRootHash.has_value());
        auto prepared = nextIncarnation(
            temporary.path(),
            registration,
            *artifactRootHash,
            "dispatch-session-mutating-diverged"
        );
        auto const log  = std::make_shared<RunLog>();
        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const call =
            projectRootCall(program, root, k_mutatingHandlerTool, k_children);
        auto const answered = dispatcher->dispatch(
            program,
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
            },
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));

        // The frame was refused loudly and the field that diverged is inside
        // the durable row. The classification is TERMINAL FAILURE, and that is
        // the completion-time half of the same cut: this handler is COMPOSED,
        // so every effect it could have caused is one of its own recorded
        // children and each of those already carries its own classification.
        // Nothing about a refused frame is uncertain.
        CHECK(answered->state == ToolCallState::TerminalFailure);
        CHECK(payloadOf(*answered).contains("canonical_args changed"));
        CHECK(payloadOf(*answered).contains("ordinal 1 under parent coordinate"));

        // And no barrier was set, which is the half a `possible` got wrong: it
        // froze mutation for the whole target over an effect that was never
        // this frame's, and only a reconciliation carrying fresh evidence
        // about something that never happened could have lifted it.
        auto const nextRoot = rootFor("dispatch-mutating-after-refusal");
        REQUIRE(prepared.store.persistToolRootRequest(nextRoot).has_value());
        auto const nextCall = projectRootCall(
            program,
            nextRoot,
            k_mutatingHandlerTool,
            R"({"children":[]})"
        );
        REQUIRE(
            prepared.store.persistToolCallPosition(nextRoot, nextCall).has_value()
        );
        auto const authorityAfter = prepared.policyAuthority;
        auto const readmitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = nextRoot,
                .call       = nextCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .policyAuthority = authorityAfter,
                    .effects         = std::vector{projectEffect(k_targetId)},
                },
            }
        );
        REQUIRE_MESSAGE(readmitted.has_value(), failureText(readmitted));
    }

    // The classification pin: one restart, three interrupted dispatches, and
    // only the mutating leaf declared uncertain.
    TEST_CASE("a restart classifies the mutating leaf uncertain and leaves every other dispatch re-enterable")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto const composedRoot = rootFor("dispatch-restart-composed");
        auto const readLeafRoot = rootFor("dispatch-restart-read-leaf");
        auto const inputRoot    = rootFor("dispatch-restart-input-leaf");
        auto artifactRootHash = std::optional<ContentHash>{};
        auto composedIdentity = std::optional<ContentHash>{};
        auto readLeafIdentity = std::optional<ContentHash>{};
        auto inputIdentity    = std::optional<ContentHash>{};

        {
            auto prepared  = firstIncarnation(temporary.path(), registration);
            auto const log = std::make_shared<RunLog>();
            artifactRootHash = prepared.artifactRootHash;

            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                prepared.observations,
                prepared.policyAuthority,
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectGenerationRegistrar{};
            auto const program =
                loadProgram(registration, registrar, *dispatcher);
            auto const authority = prepared.policyAuthority;

            auto const composed = projectRootCall(
                program,
                composedRoot,
                k_mutatingHandlerTool,
                R"({"children":[]})"
            );
            composedIdentity = composed.identity();
            REQUIRE(
                prepared.store.persistToolRootRequest(composedRoot).has_value()
            );
            auto const composedEffects = std::vector{projectEffect(k_targetId)};
            auto const composedAdmitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = composedRoot,
                    .call       = composed,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = composedEffects,
                    },
                }
            );
            REQUIRE_MESSAGE(
                composedAdmitted.has_value(),
                failureText(composedAdmitted)
            );
            REQUIRE(prepared.store.beginToolCallDispatch(*composedAdmitted)
                        .has_value());

            auto const readLeaf = frameworkToolCall(
                program,
                readLeafRoot,
                k_auditTool,
                R"({"record":{"step":1}})"
            );
            readLeafIdentity = readLeaf.identity();
            REQUIRE(
                prepared.store.persistToolRootRequest(readLeafRoot).has_value()
            );
            auto const readAdmitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = readLeafRoot,
                    .call       = readLeaf,
                }
            );
            REQUIRE_MESSAGE(readAdmitted.has_value(), failureText(readAdmitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*readAdmitted).has_value()
            );

            // The mutating leaf runs on a second controlled target, because one
            // target admits one live mutation and this case is about what a
            // restart says rather than about the barrier.
            auto elsewhere = openSession(
                prepared.store,
                registration,
                prepared.manifest,
                "dispatch-session-input",
                k_otherTargetId,
                k_otherInstanceKey
            );
            auto const inputCall = frameworkToolCall(
                program,
                inputRoot,
                k_inputTool,
                k_inputArguments
            );
            inputIdentity = inputCall.identity();
            REQUIRE(
                prepared.store.persistToolRootRequest(inputRoot).has_value()
            );
            auto const inputEffects =
                std::vector{frameworkInputEffect(k_otherTargetId)};
            auto const inputAdmitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = elsewhere.first,
                    .lease      = elsewhere.second,
                    .root       = inputRoot,
                    .call       = inputCall,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .policyAuthority = authority,
                        .effects         = inputEffects,
                    },
                }
            );
            REQUIRE_MESSAGE(inputAdmitted.has_value(), failureText(inputAdmitted));
            REQUIRE(
                prepared.store.beginToolCallDispatch(*inputAdmitted).has_value()
            );
        }

        REQUIRE(artifactRootHash.has_value());
        auto prepared = nextIncarnation(
            temporary.path(),
            registration,
            *artifactRootHash,
            "dispatch-session-restart-classification"
        );
        auto const log  = std::make_shared<RunLog>();
        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            prepared.observations,
            prepared.policyAuthority,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectGenerationRegistrar{};
        auto const program =
            loadProgram(registration, registrar, *dispatcher);

        auto const composed = projectRootCall(
            program,
            composedRoot,
            k_mutatingHandlerTool,
            R"({"children":[]})"
        );
        CHECK(composed.identity() == *composedIdentity);
        auto const composedReplay =
            prepared.store.replayToolCall(composedRoot, composed);
        REQUIRE_MESSAGE(composedReplay.has_value(), failureText(composedReplay));
        CHECK(composedReplay->state == ToolCallState::Dispatching);

        auto const readLeaf = frameworkToolCall(
            program,
            readLeafRoot,
            k_auditTool,
            R"({"record":{"step":1}})"
        );
        CHECK(readLeaf.identity() == *readLeafIdentity);
        auto const readReplay =
            prepared.store.replayToolCall(readLeafRoot, readLeaf);
        REQUIRE_MESSAGE(readReplay.has_value(), failureText(readReplay));
        CHECK(readReplay->state == ToolCallState::Dispatching);

        auto const inputCall =
            frameworkToolCall(program, inputRoot, k_inputTool, k_inputArguments);
        CHECK(inputCall.identity() == *inputIdentity);
        auto const inputReplay =
            prepared.store.replayToolCall(inputRoot, inputCall);
        REQUIRE_MESSAGE(inputReplay.has_value(), failureText(inputReplay));
        CHECK(inputReplay->state == ToolCallState::Possible);
        REQUIRE(inputReplay->payload.has_value());
        CHECK(
            inputReplay->payload->bytes()
            == R"({"reason":"operator_restart_after_dispatch_started"})"
        );

        // The restart said `possible` and nothing more. Proven absence is
        // reached only by asking, and asking is a thing only a running Operator
        // with live authority can do.
        auto elsewhere = openSession(
            prepared.store,
            registration,
            prepared.manifest,
            "dispatch-session-input-restarted",
            k_otherTargetId,
            k_otherInstanceKey
        );
        auto explanation = CanonicalJson::parseExact(
            R"({"reason":"the target never received the click"})"
        );
        auto evidence = CanonicalJson::parseExact(
            R"({"snapshot_ref":"post-restart-capture"})"
        );
        REQUIRE(explanation.has_value());
        REQUIRE(evidence.has_value());
        auto queried  = uint32{0};
        auto resolved = prepared.store.reconcileMutatingToolCall(
            elsewhere.first,
            elsewhere.second,
            inputRoot,
            inputCall,
            [&queried, &explanation, &evidence](
                ToolCallPositionIdentity const& subject
            )
            {
                ++queried;
                CHECK(subject.toolName() == k_inputTool);
                return ToolCallReconciliation::provenAbsent(
                    *explanation,
                    *evidence
                );
            }
        );
        REQUIRE_MESSAGE(resolved.has_value(), failureText(resolved));
        CHECK(resolved->state == ToolCallState::ProvenAbsent);
        CHECK(queried == 1U);
    }

    // The four-way semantic fixture of `caller independence is structural`,
    // which is that ruling's experiment E1.
    //
    // The structural half of the ruling is already stated, and is stated by
    // refusing to compile: tests/operator/test-tool-admission-funnel.cpp
    // asserts that nothing downstream of admission -- the coordinate, the
    // grant, the admitted call, the capability to dispatch -- is constructible
    // outside the runtime, so a producer cannot assemble a second path to
    // authority. What that half is blind to is what a producer puts INTO the
    // request. An adapter that canonicalised arguments differently, or that
    // mapped its transport principal to a subtly different actor, would pass
    // every private constructor and open a call that is legitimately admitted
    // with the wrong meaning.
    //
    // So these two cases drive one Tool, with one set of arguments, through
    // every producer there is: the Agent adapter, the human Workbench/CLI
    // adapter, the Project automation adapter, and the fourth producer the
    // ruling names -- one Tool calling another, which the scoped seam has
    // always been. They compare canonical argument bytes, admission outcome,
    // durable row attributes and result.
    //
    // What may differ is actor identity and profile material: the principal,
    // its kind, the session and lease it acts under, the root request those
    // hang from, and the budget snapshot its profile requires. A root request
    // is keyed inside its own caller's idempotency namespace, so two actors are
    // two namespaces and two starts of one Tool are two addresses BY
    // CONSTRUCTION. That is the actor-identity axis rather than a divergence,
    // and it is why the comparisons below are of every stored attribute except
    // the address itself.
    namespace
    {
        // The three principals of the comparison. They are three because the
        // point is that the actor varies: a fixture that gave all three the
        // same principal would compare one actor with itself and pass whatever
        // an adapter did with the other two.
        constexpr auto k_agentPrincipal = std::string_view{"adapter-agent"};
        constexpr auto k_humanPrincipal = std::string_view{"adapter-human"};

        constexpr auto k_adapterObjectiveText =
            std::string_view{R"({ "objective" :  "adapter" })"};

        // What one producer's start is kept as. The request is kept whole
        // rather than projected, because ToolAdmissionRequest is copyable on
        // purpose: what a case compares is the value that was admitted.
        //
        // No in-class initializer for the request: three of its four members
        // have no default state, so a start must come from construction.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct ProducedStart final
        {
            std::string          principal{};
            ControllerKind       kind{ControllerKind::Script};
            ToolAdmissionRequest request;
            std::string          payload{};
        };

        // The prepared world all three adapters act in. Every member is a
        // call-scoped borrow owned by the case; nothing here is stored.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct AdapterWorld final
        {
            Incarnation&                       prepared;
            VerifiedProjectGeneration const& registration;
            ProjectToolDispatcher&             dispatcher;
            ProjectGenerationHandle const&    program;
            ToolStartCatalog const&            catalog;
            OperatorPolicyAuthority const&       authority;
            ToolExecutionIdentity const&       execution;
        };

        // One Tool call, in the two shapes the transports carry it in: the
        // structured value a model's tool-use block and a Project's own start
        // document deliver, and the text a person types. The text is
        // deliberately not canonical -- its spacing is exactly what exact
        // canonical form refuses -- so a human adapter that passed its bytes
        // through would open a different coordinate, and one that demanded
        // canonical bytes would refuse the command outright.
        struct AdapterCall final
        {
            std::string requestKey{};
            std::string toolName{};
            json::Value arguments{};
            std::string argumentsText{};
        };

        [[nodiscard]]
        auto adapterObjective() -> json::Value
        {
            return json::Value::ofObject({
                {"objective", json::Value::ofString("adapter")},
            });
        }

        [[nodiscard]]
        auto stepArguments() -> json::Value
        {
            return json::Value::ofObject({
                {"step", json::Value::ofNumber(1)},
            });
        }

        [[nodiscard]]
        auto childrenArguments() -> json::Value
        {
            return json::Value::ofObject({
                {"children",
                 json::Value::ofArray({
                     json::Value::ofString(std::string{k_leafTool}),
                 })},
            });
        }

        // Runs one produced start through the dispatcher and keeps what it
        // produced.
        [[nodiscard]]
        auto recordStart(
            AdapterWorld& world,
            std::string principal,
            ControllerKind kind,
            Result<ToolAdmissionRequest> produced
        ) -> ProducedStart
        {
            REQUIRE_MESSAGE(produced.has_value(), failureText(produced));
            auto const replay = world.dispatcher.dispatch(
                world.program,
                *produced,
                std::stop_token{}
            );
            REQUIRE_MESSAGE(replay.has_value(), failureText(replay));
            CHECK(replay->state == ToolCallState::Confirmed);
            return ProducedStart{
                .principal = std::move(principal),
                .kind      = kind,
                .request   = *std::move(produced),
                .payload   = payloadOf(*replay),
            };
        }

        // One Tool driven through all three actor adapters in turn.
        //
        // The lease is exclusive per controlled target, so the three take it in
        // turn -- which is what a handover between a Project run, an Agent and
        // a person on one target actually is, and what keeps the controlled
        // target, the registration and the policy identical across the three.
        // The Project automation actor goes first because it is the session the
        // incarnation already pinned and already holds the lease on; the target
        // is left unleased when this returns.
        [[nodiscard]]
        auto driveThreeActors(AdapterWorld& world, AdapterCall const& call)
            -> std::vector<ProducedStart>
        {
            auto starts = std::vector<ProducedStart>{};

            auto automation = ProjectAutomationAdapter{};
            starts.emplace_back(recordStart(
                world,
                std::string{k_controllerId},
                ControllerKind::Script,
                automation.translate(
                    ToolActorRun{
                        .controller      = world.prepared.controller,
                        .lease           = world.prepared.lease,
                        .execution       = world.execution,
                        .policyAuthority = world.authority,
                        .catalog         = world.catalog,
                    },
                    world.program.bindingTable(),
                    ProjectAutomationStart{
                        .requestKey    = call.requestKey,
                        .objective     = adapterObjective(),
                        .entryToolName = call.toolName,
                        .arguments     = call.arguments,
                    }
                )
            ));
            REQUIRE(
                world.prepared.store.releaseLease(world.prepared.lease)
                    .has_value()
            );

            auto agent        = AgentToolAdapter{};
            auto agentSession = openSession(
                world.prepared.store,
                world.registration,
                world.prepared.manifest,
                "adapter-agent-session",
                k_targetId,
                k_agentInstanceKey,
                ControllerKind::Agent,
                k_agentPrincipal
            );
            starts.emplace_back(recordStart(
                world,
                std::string{k_agentPrincipal},
                ControllerKind::Agent,
                agent.translate(
                    ToolActorRun{
                        .controller      = agentSession.first,
                        .lease           = agentSession.second,
                        .execution       = world.execution,
                        .policyAuthority = world.authority,
                        .catalog         = world.catalog,
                    },
                    AgentToolUse{
                        .requestKey = call.requestKey,
                        .objective  = adapterObjective(),
                        .toolName   = call.toolName,
                        .arguments  = call.arguments,
                    }
                )
            ));
            REQUIRE(
                world.prepared.store.releaseLease(agentSession.second).has_value()
            );

            auto human        = HumanToolAdapter{};
            auto humanSession = openSession(
                world.prepared.store,
                world.registration,
                world.prepared.manifest,
                "adapter-human-session",
                k_targetId,
                k_humanInstanceKey,
                ControllerKind::Human,
                k_humanPrincipal
            );
            starts.emplace_back(recordStart(
                world,
                std::string{k_humanPrincipal},
                ControllerKind::Human,
                human.translate(
                    ToolActorRun{
                        .controller      = humanSession.first,
                        .lease           = humanSession.second,
                        .execution       = world.execution,
                        .policyAuthority = world.authority,
                        .catalog         = world.catalog,
                    },
                    HumanToolCommand{
                        .requestKey    = call.requestKey,
                        .objectiveText = std::string{k_adapterObjectiveText},
                        .toolName      = call.toolName,
                        .argumentsText = call.argumentsText,
                    }
                )
            ));
            REQUIRE(
                world.prepared.store.releaseLease(humanSession.second).has_value()
            );
            return starts;
        }

        // Everything two producers must agree on in the coordinate itself. The
        // root identity, the parent coordinate and the call identity are absent
        // because they are the address a start hangs from, and two actors
        // address two roots by construction.
        auto checkSameCoordinateShape(
            ToolCallPositionIdentity const& left,
            ToolCallPositionIdentity const& right
        ) -> void
        {
            CHECK(left.canonicalArgs() == right.canonicalArgs());
            CHECK(left.canonicalArgsHash() == right.canonicalArgsHash());
            CHECK(left.toolName() == right.toolName());
            CHECK(left.toolVersion() == right.toolVersion());
            CHECK(left.sequence() == right.sequence());
            CHECK(left.observationReference() == right.observationReference());
            CHECK(
                toolEffectComposition(left.provider())
                == toolEffectComposition(right.provider())
            );
            auto const& leftExecution  = left.executionIdentity();
            auto const& rightExecution = right.executionIdentity();
            CHECK(leftExecution.runIdentity == rightExecution.runIdentity);
            CHECK(
                leftExecution.frameworkReleaseIdentity
                == rightExecution.frameworkReleaseIdentity
            );
            CHECK(
                leftExecution.toolRuntimeProtocolIdentity
                == rightExecution.toolRuntimeProtocolIdentity
            );
            CHECK(
                leftExecution.environmentIdentity
                == rightExecution.environmentIdentity
            );
            CHECK(left.descriptor().mutability == right.descriptor().mutability);
            CHECK(left.descriptor().surface == right.descriptor().surface);
            CHECK(left.descriptor().idempotency == right.descriptor().idempotency);
        }

        // The stored attributes of one call position, minus its address. Read
        // back out of the file the Operator wrote rather than off the value the
        // producer built, because what a later run replays is the row.
        [[nodiscard]]
        auto storedPositionAttributes(
            test_support::OperatorDatabaseProbe const& database,
            std::string const& callIdentity
        ) -> std::vector<std::string>
        {
            auto const rows = database.readRows(
                "SELECT tool_name, tool_version, canonical_args, "
                "canonical_args_hash, provider_kind, "
                "coalesce(project_registration_hash, ''), tool_catalog_hash, "
                "call_sequence, run_identity, framework_release_identity, "
                "tool_runtime_protocol_identity, environment_identity, "
                "coalesce(observation_reference_hash, '') "
                "FROM tool_call_positions WHERE call_identity='"
                    + callIdentity + "'"
            );
            REQUIRE(rows.size() == 1U);
            return rows.front();
        }

        // What admission concluded, minus everything an actor's identity
        // reaches. The budget snapshot is deliberately absent: an Agent is the
        // one kind whose profile requires ceilings, so its snapshot differs
        // from a Script's or a Human's, and that is the profile material E1
        // puts on the same axis as the actor.
        [[nodiscard]]
        auto storedAdmissionVerdict(
            test_support::OperatorDatabaseProbe const& database,
            std::string const& callIdentity
        ) -> std::vector<std::string>
        {
            auto const rows = database.readRows(
                "SELECT controlled_target_id, project_registration_hash, "
                "policy_hash, coalesce(effect_envelope, ''), "
                "coalesce(effect_envelope_hash, ''), "
                "coalesce(required_approvals, ''), "
                "coalesce(approval_tokens, '') "
                "FROM tool_admission_attempts WHERE call_identity='"
                    + callIdentity + "'"
            );
            REQUIRE(rows.size() == 1U);
            return rows.front();
        }

        // Who admission recorded as the origin of one call, and whether it
        // stood on a handler's delegation. This is the actor axis itself, so a
        // case states what it expects rather than comparing producers.
        [[nodiscard]]
        auto storedOrigin(
            test_support::OperatorDatabaseProbe const& database,
            std::string const& callIdentity
        ) -> std::vector<std::string>
        {
            auto const rows = database.readRows(
                "SELECT origin_principal_id, origin_principal_kind, "
                "coalesce(delegation_grant_id, '') "
                "FROM tool_admission_attempts WHERE call_identity='"
                    + callIdentity + "'"
            );
            REQUIRE(rows.size() == 1U);
            return rows.front();
        }
    } // namespace

    TEST_CASE(
        "four producers translate one read-only Tool call into one admission"
    )
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto databasePath  = std::filesystem::path{};
        auto starts        = std::vector<ProducedStart>{};
        auto childIdentity = std::string{};
        auto childPayload  = std::string{};

        {
            auto prepared = firstIncarnation(temporary.path(), registration);
            databasePath  = prepared.store.databasePath();
            auto const log  = std::make_shared<RunLog>();
            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                prepared.observations,
                prepared.policyAuthority,
                frameworkProvider(log)
            );
            REQUIRE_MESSAGE(dispatcher.has_value(), failureText(dispatcher));
            auto registrar = ProjectGenerationRegistrar{};
            auto const program =
                loadProgram(registration, registrar, *dispatcher);
            auto const catalog =
                ToolStartCatalog::create(toolCatalogOwner(registration));
            REQUIRE_MESSAGE(catalog.has_value(), failureText(catalog));
            auto const authority = prepared.policyAuthority;
            auto const execution = executionIdentity(program);
            auto world           = AdapterWorld{
                          .prepared     = prepared,
                          .registration = registration,
                          .dispatcher   = *dispatcher,
                          .program      = program,
                          .catalog      = *catalog,
                          .authority    = authority,
                          .execution    = execution,
            };

            starts = driveThreeActors(
                world,
                AdapterCall{
                    .requestKey = "adapter-read-only",
                    .toolName   = std::string{k_leafTool},
                    .arguments  = stepArguments(),
                    .argumentsText = R"({ "step":  1 })",
                }
            );

            // The fourth producer: one Tool calling another. A Project
            // automation start of the handler entry is the same start as any
            // other -- what makes an entry startable is that the actor is
            // admitted to start it -- and the call this case compares is the
            // child that entry's handler issued from inside its own run.
            auto automation  = ProjectAutomationAdapter{};
            auto const lease = prepared.store.acquireLease(prepared.controller);
            REQUIRE_MESSAGE(lease.has_value(), failureText(lease));
            auto const handler = automation.translate(
                ToolActorRun{
                    .controller      = prepared.controller,
                    .lease           = *lease,
                    .execution       = execution,
                    .policyAuthority = authority,
                    .catalog         = *catalog,
                },
                program.bindingTable(),
                ProjectAutomationStart{
                    .requestKey    = "adapter-child",
                    .objective     = adapterObjective(),
                    .entryToolName = std::string{k_handlerTool},
                    .arguments     = childrenArguments(),
                }
            );
            REQUIRE_MESSAGE(handler.has_value(), failureText(handler));
            auto const answered = dispatcher->dispatch(
                program,
                *handler,
                std::stop_token{}
            );
            REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
            CHECK(answered->state == ToolCallState::Confirmed);

            // Naming the child is not a second mint. The position factory is
            // private to the issuing context, so walking a context to the same
            // ordinal under the same durable parent is the only way to name a
            // call at all -- and the coordinate it re-derives is the one the
            // handler's own context assigned.
            auto context = ToolCallIssuingContext::forHandler(handler->call);
            auto const child = context.issue(
                invocationOf(program, k_leafTool, R"({"step":1})")
            );
            REQUIRE_MESSAGE(child.has_value(), failureText(child));
            childIdentity = child->identity().hex();
            auto const childReplay =
                prepared.store.replayToolCall(handler->root, *child);
            REQUIRE_MESSAGE(childReplay.has_value(), failureText(childReplay));
            CHECK(childReplay->state == ToolCallState::Confirmed);
            childPayload = payloadOf(*childReplay);
            checkSameCoordinateShape(starts.front().request.call, *child);

            // Two statements about what an adapter resolves, neither of which
            // any comparison above can make, and both about translation alone
            // -- a start is a value until something dispatches it.
            //
            // A Tool name is owned by its namespace, so the name decides which
            // catalog validates it. An adapter that preferred one catalog would
            // either be unable to name a Framework Tool at all or unable to
            // name every Project Tool compared above.
            auto const probeRun = ToolActorRun{
                .controller      = prepared.controller,
                .lease           = *lease,
                .execution       = execution,
                .policyAuthority = authority,
                .catalog         = *catalog,
            };
            auto probe           = AgentToolAdapter{};
            auto const frameworkStart = probe.translate(
                probeRun,
                AgentToolUse{
                    .requestKey = "adapter-framework",
                    .objective  = adapterObjective(),
                    .toolName   = std::string{k_auditTool},
                    .arguments  = json::Value::ofObject({
                        {"record", json::Value::ofObject({})},
                    }),
                }
            );
            REQUIRE_MESSAGE(
                frameworkStart.has_value(),
                failureText(frameworkStart)
            );
            CHECK(frameworkStart->call.toolName() == k_auditTool);
            CHECK(
                toolEffectComposition(frameworkStart->call.provider())
                == ToolEffectComposition::DirectLeaf
            );

            // And a Project starts entries its own registration bound. The
            // refusal is about whose entry it is rather than about what sort of
            // entry it is: the Framework's audit Tool is another party's,
            // whatever a Project might want to do with it. The arguments are
            // the ones the Agent start above was admitted with, so the only
            // thing left to refuse this is the binding table.
            auto const foreignStart = automation.translate(
                probeRun,
                program.bindingTable(),
                ProjectAutomationStart{
                    .requestKey    = "adapter-foreign",
                    .objective     = adapterObjective(),
                    .entryToolName = std::string{k_auditTool},
                    .arguments     = json::Value::ofObject({
                        {"record", json::Value::ofObject({})},
                    }),
                }
            );
            CHECK_FALSE(foreignStart.has_value());

            // A second start under the SAME root request advances that root's
            // own ordinal rather than reopening it. A producer that built a
            // context per start would hand this one the ordinal the handler
            // above already occupies, and it would land on that call's durable
            // row with different canonical arguments -- which is the aliasing
            // the private position factory exists to prevent, and the reason
            // the producer owns one context per root.
            auto const second = automation.translate(
                probeRun,
                program.bindingTable(),
                ProjectAutomationStart{
                    .requestKey    = "adapter-child",
                    .objective     = adapterObjective(),
                    .entryToolName = std::string{k_leafTool},
                    .arguments     = json::Value::ofObject({
                        {"step", json::Value::ofNumber(2)},
                    }),
                }
            );
            REQUIRE_MESSAGE(second.has_value(), failureText(second));
            CHECK(second->call.sequence() == 2U);
            CHECK(second->root.identity() == handler->root.identity());
            auto const secondReplay = dispatcher->dispatch(
                program,
                *second,
                std::stop_token{}
            );
            REQUIRE_MESSAGE(secondReplay.has_value(), failureText(secondReplay));
            CHECK(secondReplay->state == ToolCallState::Confirmed);
            CHECK(payloadOf(*secondReplay) == R"({"leaf":2})");
        }

        REQUIRE(starts.size() == 3U);

        // The actor axis is real rather than vacuous: three principals, three
        // kinds, three roots.
        auto principals = std::vector<std::string>{};
        auto kinds      = std::vector<ControllerKind>{};
        auto roots      = std::vector<std::string>{};
        for (auto const& start : starts)
        {
            CHECK(start.request.controller.controllerId() == start.principal);
            CHECK(start.request.controller.kind() == start.kind);
            CHECK(start.request.isRootPositioned());
            CHECK_FALSE(start.request.delegation.has_value());
            CHECK_FALSE(start.request.mutation.has_value());
            CHECK(start.request.requiredMutability() == ToolMutability::ReadOnly);
            principals.emplace_back(start.principal);
            kinds.emplace_back(start.kind);
            roots.emplace_back(start.request.root.identity().hex());
            checkSameCoordinateShape(
                starts.front().request.call,
                start.request.call
            );
            CHECK(start.payload == R"({"leaf":1})");
        }
        std::ranges::sort(principals);
        std::ranges::sort(roots);
        CHECK(std::ranges::adjacent_find(principals) == principals.end());
        CHECK(std::ranges::adjacent_find(roots) == roots.end());
        CHECK(kinds[0] != kinds[1]);
        CHECK(kinds[1] != kinds[2]);
        CHECK(kinds[0] != kinds[2]);
        CHECK(childPayload == R"({"leaf":1})");

        // The Operator claims its database exclusively, so the rows are read
        // after the store above has been destroyed.
        auto const database   = test_support::OperatorDatabaseProbe{databasePath};
        auto const attributes = storedPositionAttributes(
            database,
            starts.front().request.call.identity().hex()
        );
        auto const verdict = storedAdmissionVerdict(
            database,
            starts.front().request.call.identity().hex()
        );
        for (auto const& start : starts)
        {
            auto const identity = start.request.call.identity().hex();
            CHECK(storedPositionAttributes(database, identity) == attributes);
            CHECK(storedAdmissionVerdict(database, identity) == verdict);
            CHECK(
                storedOrigin(database, identity)
                == std::vector<std::string>{
                    start.principal,
                    std::string{controllerKindWireName(start.kind)},
                    "",
                }
            );
        }
        CHECK(storedPositionAttributes(database, childIdentity) == attributes);
        CHECK(storedAdmissionVerdict(database, childIdentity) == verdict);

        // The fourth producer's one difference, and the only one: it stands on
        // a handler's delegation grant rather than on the run's own authority.
        auto const childOrigin = storedOrigin(database, childIdentity);
        CHECK(childOrigin[0] == std::string{k_controllerId});
        CHECK(childOrigin[1] == controllerKindWireName(ControllerKind::Script));
        CHECK_FALSE(childOrigin[2].empty());
    }

    TEST_CASE(
        "three actor adapters propose one mutation and are admitted identically"
    )
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto databasePath = std::filesystem::path{};
        auto starts       = std::vector<ProducedStart>{};

        {
            auto prepared = firstIncarnation(temporary.path(), registration);
            databasePath  = prepared.store.databasePath();
            auto const log  = std::make_shared<RunLog>();
            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                prepared.observations,
                prepared.policyAuthority,
                frameworkProvider(log)
            );
            REQUIRE_MESSAGE(dispatcher.has_value(), failureText(dispatcher));
            auto registrar = ProjectGenerationRegistrar{};
            auto const program =
                loadProgram(registration, registrar, *dispatcher);
            auto const catalog =
                ToolStartCatalog::create(toolCatalogOwner(registration));
            REQUIRE_MESSAGE(catalog.has_value(), failureText(catalog));
            auto const authority = prepared.policyAuthority;
            auto const execution = executionIdentity(program);
            auto world           = AdapterWorld{
                          .prepared     = prepared,
                          .registration = registration,
                          .dispatcher   = *dispatcher,
                          .program      = program,
                          .catalog      = *catalog,
                          .authority    = authority,
                          .execution    = execution,
            };

            starts = driveThreeActors(
                world,
                AdapterCall{
                    .requestKey = "adapter-mutation",
                    .toolName   = std::string{k_mutatingHandlerTool},
                    .arguments  = childrenArguments(),
                    .argumentsText =
                        R"({ "children" : ["dispatch.project.leaf"] })",
                }
            );
        }

        REQUIRE(starts.size() == 3U);

        // The proposal is the producer's, derived from the descriptor's own
        // effect bounds and the target this actor holds the lease on. No
        // adapter states one, so all three propose the same effect at the same
        // risk under the same authority -- and none of them presents an
        // approval, because an approval is a human decision another door mints
        // and a policy that required one would refuse these admissions.
        auto const& expected = *starts.front().request.mutation;
        for (auto const& start : starts)
        {
            CHECK(start.request.requiredMutability() == ToolMutability::Mutating);
            REQUIRE(start.request.mutation.has_value());
            auto const& mutation = *start.request.mutation;
            CHECK(mutation.approvals.empty());
            CHECK(
                mutation.policyAuthority.projectRegistrationHash()
                == expected.policyAuthority.projectRegistrationHash()
            );
            CHECK(
                mutation.policyAuthority.policyHash()
                == expected.policyAuthority.policyHash()
            );
            REQUIRE(mutation.effects.size() == 1U);
            REQUIRE(expected.effects.size() == 1U);
            auto const& proposed = mutation.effects.front();
            auto const& first    = expected.effects.front();
            CHECK(proposed.namespacedType == first.namespacedType);
            CHECK(proposed.risk == first.risk);
            CHECK(proposed.scopeKind == first.scopeKind);
            CHECK(proposed.scopeKey == first.scopeKey);
            CHECK(proposed.payloadSchemaHash == first.payloadSchemaHash);
            CHECK(proposed.opaqueProjectPayload == first.opaqueProjectPayload);
            checkSameCoordinateShape(
                starts.front().request.call,
                start.request.call
            );
            CHECK(start.payload == R"({"states":["confirmed"]})");
        }

        auto const database   = test_support::OperatorDatabaseProbe{databasePath};
        auto const attributes = storedPositionAttributes(
            database,
            starts.front().request.call.identity().hex()
        );
        auto const verdict = storedAdmissionVerdict(
            database,
            starts.front().request.call.identity().hex()
        );

        // A mutating admission stored a real envelope rather than the absence a
        // read-only one stores, which is what makes the equality below a
        // statement about three evaluated envelopes.
        CHECK_FALSE(verdict[3].empty());
        CHECK_FALSE(verdict[4].empty());
        for (auto const& start : starts)
        {
            auto const identity = start.request.call.identity().hex();
            CHECK(storedPositionAttributes(database, identity) == attributes);
            CHECK(storedAdmissionVerdict(database, identity) == verdict);
            CHECK(
                storedOrigin(database, identity)
                == std::vector<std::string>{
                    start.principal,
                    std::string{controllerKindWireName(start.kind)},
                    "",
                }
            );
        }
    }
}
