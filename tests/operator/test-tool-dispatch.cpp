#include <operator/ledger.hpp>
#include <operator/project-plugin.hpp>
#include <operator/project-tool-dispatch.hpp>
#include <operator/project-tool-program.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-executor.hpp>
#include <operator/tool-invocation.hpp>

#include <deployment/project-deployment.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include "project-fixture.hpp"

#include <doctest/doctest.h>

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
        constexpr auto k_reducerTool = std::string_view{"dispatch.project.reducer"};
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
        constexpr auto k_boundMillis = uint64{60'000};

        constexpr auto k_toolCatalogBytes = std::string_view{
            R"({"schema":"umbraflow-tool-catalog/v1","plugin":"dispatch.project"})"
        };
        constexpr auto k_stateSchemaBytes       = std::string_view{"state-schema"};
        constexpr auto k_observationSchemaBytes = std::string_view{"observation-schema"};
        constexpr auto k_preconditionSchemaBytes = std::string_view{"precondition"};

        // One closure, five exported entries, five bound Tools.
        //
        // The five names are the five a ProjectPlugin exports, because the
        // ledger still provisions a ProjectInstance from a pure plugin and a
        // closed-graph closure admits its declared entry set EXACTLY. So this
        // closure is registered twice against one registration: once as the
        // pure plugin the instance is provisioned from, and once as the scoped
        // program the dispatcher runs. The scoped modules are required inside
        // the handler bodies rather than at module scope, which is what lets
        // the same bytes admit under both program types -- the pure resolver
        // never sees a scoped name because the pure program never calls a
        // handler.
        //
        // `derive` is the handler under test. It issues one child per name its
        // arguments list, in order, and answers with what each child was
        // recorded as, so its child sequence is a deterministic function of the
        // canonical arguments on its own durable row -- which is the whole of
        // what makes fresh-from-1 re-derivation safe.
        constexpr auto k_projectSource = std::string_view{R"LUAU(
return {
    plugin_id = "dispatch.project",

    derive = function(input)
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

    plan = function(input)
        return { leaf = input.step }
    end,

    next_step = function(input)
        return { echo = input }
    end,

    reconcile = function(input)
        return { delivered = input.step }
    end,

    reduce = function(_input)
        return { revision = 0 }
    end,
}
)LUAU"};

        [[nodiscard]]
        auto moduleBlobs() -> std::vector<ProjectModuleBlob>
        {
            auto blobs = std::vector<ProjectModuleBlob>{};
            blobs.emplace_back(ProjectModuleBlob{
                .name   = "main",
                .source = std::string{k_projectSource},
            });
            return blobs;
        }

        [[nodiscard]]
        auto boundEntries() -> std::vector<ProjectToolBinding>
        {
            // JCS-ordered by tool name, which is the order the loader writes
            // and the only order a registration may state.
            return {
                ProjectToolBinding{
                    .toolName   = std::string{k_mutatingTool},
                    .entryPoint = "reconcile",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_echoTool},
                    .entryPoint = "next_step",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_handlerTool},
                    .entryPoint = "derive",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_leafTool},
                    .entryPoint = "plan",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_mutatingHandlerTool},
                    .entryPoint = "derive",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_reducerTool},
                    .entryPoint = "reduce",
                },
            };
        }

        [[nodiscard]]
        auto exportedEntries() -> std::vector<std::string>
        {
            return {"derive", "next_step", "plan", "reconcile", "reduce"};
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
            if (name == k_mutatingHandlerTool)
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
                auto descriptor        = declaredDescriptor({});
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
                std::string{k_reducerTool},
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

        [[nodiscard]]
        auto registrationJcs(ProjectRegistrationClaims const& claims) -> std::string
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
                {"baseline_event_type",
                 json::Value::ofString(claims.baselineEventType)},
                {"journal_event_schema_manifest_hash",
                 json::Value::ofString(claims.journalEventSchemaManifestHash.hex())},
                {"observed_instance_identity_schema_hashes",
                 json::Value::ofArray({})},
                {"plugin_environment_hash",
                 json::Value::ofString(claims.pluginEnvironmentHash.hex())},
                {"plugin_id", json::Value::ofString(claims.pluginId)},
                {"plugin_module_manifest_hash",
                 json::Value::ofString(claims.pluginModuleManifestHash.hex())},
                {"project_observation_schema_hash",
                 json::Value::ofString(claims.projectObservationSchemaHash.hex())},
                {"project_registration_format",
                 json::Value::ofNumber(
                     static_cast<double>(claims.projectRegistrationFormat)
                 )},
                {"project_resources", json::Value::ofArray({})},
                {"project_state_schema_hash",
                 json::Value::ofString(claims.projectStateSchemaHash.hex())},
                {"project_tool_bindings", json::Value::ofArray(std::move(bindings))},
                {"project_tool_precondition_schema_hash",
                 json::Value::ofString(
                     claims.projectToolPreconditionSchemaHash.hex()
                 )},
                {"reconcile_payload_schema_manifest_hash",
                 json::Value::ofString(
                     claims.reconcilePayloadSchemaManifestHash.hex()
                 )},
                {"tool_catalog_hash",
                 json::Value::ofString(claims.toolCatalogHash.hex())},
            }));
        }

        [[nodiscard]]
        auto verifiedRegistration() -> VerifiedProjectRegistration
        {
            auto const modules            = moduleBlobs();
            auto const moduleManifestHash =
                derivePluginModuleManifestHash("main", modules);
            REQUIRE(moduleManifestHash.has_value());
            auto const environmentHash = currentProjectPluginEnvironmentHash();
            REQUIRE(environmentHash.has_value());
            auto claims = ProjectRegistrationClaims{
                .projectRegistrationFormat = k_projectRegistrationFormat,
                .pluginId                  = std::string{k_pluginId},
                .pluginModuleManifestHash  = *moduleManifestHash,
                .pluginEnvironmentHash     = *environmentHash,
                .toolCatalogHash           = hashOf(k_toolCatalogBytes),
                .projectStateSchemaHash    = hashOf(k_stateSchemaBytes),
                .projectObservationSchemaHash         =
                    hashOf(k_observationSchemaBytes),
                .projectToolPreconditionSchemaHash    =
                    hashOf(k_preconditionSchemaBytes),
                .reconcilePayloadSchemaManifestHash   = hashOf("reconcile"),
                .journalEventSchemaManifestHash       = hashOf("journal"),
                .baselineEventType                    = "dispatch.baseline",
                .projectResources                     = {},
                .observedInstanceIdentitySchemaHashes = {},
                .projectToolBindings                  = boundEntries(),
            };
            auto const exactJcs = registrationJcs(claims);
            auto owner          = ProjectRegistrationSchemaOwner::create(
                [exactJcs = exactJcs, claims = std::move(claims)](
                    std::string_view candidate
                ) -> Result<ProjectRegistrationClaims>
                {
                    if (candidate != exactJcs)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "dispatch fixture registration is not exact JCS"
                        );
                    }
                    return claims;
                }
            );
            REQUIRE(owner.has_value());
            auto registration = ProjectRegistration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                *owner
            );
            REQUIRE(registration.has_value());
            return *std::move(registration);
        }

        // What one dispatch fixture counted. The result validator and the
        // Framework provider are the two witnesses that a branch actually ran:
        // the validator runs once per handler INVOCATION, and the provider runs
        // once per Framework child that actually EXECUTED rather than replayed.
        struct RunLog final
        {
            uint32 handlerAnswers{0};
            uint32 frameworkExecutions{0};
            bool   refuseResults{false};
        };

        [[nodiscard]]
        auto resultValidator(std::shared_ptr<RunLog> log) -> ToolResultValidator
        {
            return [log = std::move(log)](
                       std::string_view toolName,
                       std::string_view
                   ) -> Status
            {
                ++log->handlerAnswers;
                if (log->refuseResults)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "the answer of " + std::string{toolName}
                            + " is not the shape its result schema declares"
                    );
                }
                return ok();
            };
        }

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
        auto projectSchemaOwner(
            VerifiedProjectRegistration const& registration
        ) -> ProjectSchemaOwner
        {
            auto owner = ProjectSchemaOwner::create(
                registration,
                ProjectDocumentSchemaBytes{
                    .projectState       = k_stateSchemaBytes,
                    .projectObservation = k_observationSchemaBytes,
                    .toolPrecondition   = k_preconditionSchemaBytes,
                },
                deployment::canonicalJsonValidator(),
                [](
                    ProjectPluginFunction,
                    ProjectDocumentDirection,
                    std::string_view
                ) -> Status { return ok(); }
            );
            REQUIRE(owner.has_value());
            return *std::move(owner);
        }

        [[nodiscard]]
        auto toolCatalogOwner(
            VerifiedProjectRegistration const& registration
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
        struct Incarnation final
        {
            OperatorCoordinator store;
            ContentHash         artifactRootHash;
            SessionManifest     manifest;
            ControllerBinding   controller;
            ControlLease        lease;
        };

        // The two effect types this fixture's mutating Tools propose: the
        // Project's own, and the one a Framework input Tool declares. A rule
        // that named neither would decide every mutating admission by the
        // artifact's default deny, which is not what any case here is about.
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
        auto planAuthorityFor(
            OperatorCoordinator& store,
            VerifiedProjectRegistration const& registration,
            SessionManifest const& manifest,
            ContentHash const& artifactRootHash
        ) -> OperatorPlanAuthority
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
            auto authority = conformance::planAuthority(
                registration,
                manifest,
                *runtimeModel,
                "operator",
                policyBytes(),
                test_support::k_fixtureUiAction
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
            VerifiedProjectRegistration const& registration,
            SessionManifest const& manifest,
            std::string_view sessionId,
            std::string_view targetId    = k_targetId,
            std::string_view instanceKey = k_instanceKey
        ) -> std::pair<ControllerBinding, ControlLease>
        {
            auto const worldScope = ObservedInstanceWorldScope::run(
                std::string{targetId},
                1
            );
            REQUIRE(worldScope.has_value());
            auto const pinned = store.pinSession(
                SessionPin{
                    .sessionId                 = std::string{sessionId},
                    .authenticatedControllerId = std::string{k_controllerId},
                    .idempotencyNamespace      = std::string{k_controllerId},
                    .projectRegistrationHash   = registration.hash(),
                    .controllerCapabilities    = {
                        std::string{conformance::k_operateCapability},
                    },
                    .controlledTargetId = std::string{targetId},
                    .projectInstanceKey = std::string{instanceKey},
                    .mode               = SessionMode::Write,
                    .kind               = ControllerKind::Script,
                    .worldScope         = *worldScope,
                },
                manifest,
                std::nullopt
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
            VerifiedProjectRegistration const& registration
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

            auto registrar = ProjectPluginRegistrar{};
            auto plugin    = registrar.registerPlugin(
                registration,
                "main",
                moduleBlobs(),
                {},
                projectSchemaOwner(registration)
            );
            REQUIRE_MESSAGE(plugin.has_value(), failureText(plugin));

            auto const manifest = test_support::sessionManifest(
                registration,
                artifactRootHash,
                hashOf("agent"),
                policyBytes()
            );
            REQUIRE(store.registerProject(registration).has_value());
            for (auto const instanceKey : {k_instanceKey, k_otherInstanceKey})
            {
                auto const provisioned = store.provisionProjectInstance(
                    registration,
                    *plugin,
                    ProjectInstanceBaseline{
                        .projectInstanceKey  = std::string{instanceKey},
                        .eventId             = "",
                        .sessionManifestHash = manifest.hash(),
                        .entry               = std::nullopt,
                    }
                );
                REQUIRE_MESSAGE(provisioned.has_value(), failureText(provisioned));
            }

            auto session = openSession(store, registration, manifest, k_sessionId);
            return Incarnation{
                .store            = std::move(store),
                .artifactRootHash = artifactRootHash,
                .manifest         = manifest,
                .controller       = std::move(session.first),
                .lease            = std::move(session.second),
            };
        }

        // The incarnation after a crash: the same runtime directory, opened
        // again. Every lease is cleared and every session deactivated by that
        // open, so this one authenticates the same origin principal under a new
        // session epoch and a new lease.
        [[nodiscard]]
        auto nextIncarnation(
            std::filesystem::path const& path,
            VerifiedProjectRegistration const& registration,
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
                hashOf("agent"),
                policyBytes()
            );
            auto session = openSession(store, registration, manifest, sessionId);
            return Incarnation{
                .store            = std::move(store),
                .artifactRootHash = artifactRootHash,
                .manifest         = manifest,
                .controller       = std::move(session.first),
                .lease            = std::move(session.second),
            };
        }

        [[nodiscard]]
        auto loadProgram(
            VerifiedProjectRegistration const& registration,
            ProjectToolProgramRegistrar& registrar,
            std::shared_ptr<RunLog> const& log,
            ProjectToolDispatcher const& dispatcher
        ) -> ProjectToolProgramHandle
        {
            auto const exported = exportedEntries();
            auto loaded         = registrar.registerProject(
                registration,
                toolCatalogOwner(registration),
                "main",
                moduleBlobs(),
                {},
                exported,
                resultValidator(log),
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
        auto executionIdentity(ProjectToolProgramHandle const& program)
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
            ProjectToolProgramHandle const& program,
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
            ProjectToolProgramHandle const& program,
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
            ProjectToolProgramHandle const& program,
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
            ProjectToolProgramHandle const& program,
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
        auto const registration = verifiedRegistration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            frameworkProvider(log)
        );
        REQUIRE_MESSAGE(dispatcher.has_value(), failureText(dispatcher));
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        CHECK(log->handlerAnswers == 3U);
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

    TEST_CASE("a terminal parent returns its recorded result and never runs its handler")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedRegistration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        CHECK(log->handlerAnswers == 1U);
        CHECK(log->frameworkExecutions == 1U);

        // The second entry meets a terminal row. The recorded result comes
        // back, and neither witness moves: the handler was never invoked, so
        // its whole sub-tree is coordinate space nothing observed.
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
        CHECK(log->handlerAnswers == 1U);
        CHECK(log->frameworkExecutions == 1U);
    }

    TEST_CASE("an admitted call whose dispatch never began dispatches against empty history")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedRegistration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        CHECK(log->handlerAnswers == 2U);
    }

    TEST_CASE("a handler killed mid-dispatch re-enters, numbers from one and executes only past history")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedRegistration();
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
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectToolProgramRegistrar{};
            auto const program =
                loadProgram(registration, registrar, log, *dispatcher);

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

            CHECK(log->handlerAnswers == 1U);
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
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        CHECK(log->handlerAnswers == 2U);

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
        auto const registration = verifiedRegistration();
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
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectToolProgramRegistrar{};
            auto const program =
                loadProgram(registration, registrar, log, *dispatcher);

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
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        auto const registration = verifiedRegistration();
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
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectToolProgramRegistrar{};
            auto const program =
                loadProgram(registration, registrar, log, *dispatcher);

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
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        CHECK(log->handlerAnswers == 0U);
    }

    TEST_CASE("a fenced-out incarnation can neither re-enter its dispatch nor complete it")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedRegistration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        auto const registration = verifiedRegistration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
            auto const authority = planAuthorityFor(
                prepared.store,
                registration,
                prepared.manifest,
                prepared.artifactRootHash
            );
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = inputCall,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .planAuthority = authority,
                        .effects       = effects,
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
            auto const authority = planAuthorityFor(
                prepared.store,
                registration,
                prepared.manifest,
                prepared.artifactRootHash
            );
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = inputCall,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .planAuthority = authority,
                        .effects       = std::vector{frameworkInputEffect(k_targetId)},
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

        SUBCASE("a mutating Framework leaf's provider failure is recorded uncertain")
        {
            // The executor's half, keyed on the same conjunction. A failing
            // provider here could have moved the world with no row saying so,
            // so its failure becomes uncertainty rather than a terminal claim
            // -- and this is the one shape for which that conversion is right.
            auto const inputCall =
                frameworkToolCall(program, root, k_inputTool, k_inputArguments);
            auto const authority = planAuthorityFor(
                prepared.store,
                registration,
                prepared.manifest,
                prepared.artifactRootHash
            );
            auto const answered = ToolRuntimeExecutor{prepared.store}.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = inputCall,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .planAuthority = authority,
                        .effects       = std::vector{frameworkInputEffect(k_targetId)},
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
        auto const registration = verifiedRegistration();
        auto prepared = firstIncarnation(temporary.path(), registration);

        auto const refused = ProjectToolDispatcher::create(
            prepared.store,
            ToolProvider{}
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "requires a Framework Tool provider"
        ));
    }

    TEST_CASE("an answer the result schema refuses is a failed call rather than a recorded one")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedRegistration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log     = std::make_shared<RunLog>();
        log->refuseResults = true;

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

        auto const root = rootFor("dispatch-bad-answer");
        auto const call = rootCall(program, root, R"({"children":[]})");
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
            "is not the shape its result schema declares"
        ));
        CHECK(log->handlerAnswers == 1U);
    }

    TEST_CASE("the scoped seam resolves its run from the durable coordinate alone")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedRegistration();
        auto prepared = firstIncarnation(temporary.path(), registration);
        auto const log = std::make_shared<RunLog>();

        auto dispatcher = ProjectToolDispatcher::create(
            prepared.store,
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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

        SUBCASE("a mutating child has no producer at the scoped seam")
        {
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
                "issues read-only child calls"
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
        auto const registration = verifiedRegistration();
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
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectToolProgramRegistrar{};
            auto const program =
                loadProgram(registration, registrar, log, *dispatcher);

            auto const call = projectRootCall(
                program,
                root,
                k_mutatingHandlerTool,
                k_children
            );
            callIdentity = call.identity();
            REQUIRE(call.descriptor().mutability == ToolMutability::Mutating);

            REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
            auto const authority = planAuthorityFor(
                prepared.store,
                registration,
                prepared.manifest,
                prepared.artifactRootHash
            );
            auto const effects  = std::vector{projectEffect(k_targetId)};
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .planAuthority = authority,
                        .effects       = effects,
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
            CHECK(log->handlerAnswers == 1U);
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
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        CHECK(log->handlerAnswers == 2U);

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
        auto const authority = planAuthorityFor(
            prepared.store,
            registration,
            prepared.manifest,
            prepared.artifactRootHash
        );
        auto const effects   = std::vector{projectEffect(k_targetId)};
        auto const readmitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = nextRoot,
                .call       = nextCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = authority,
                    .effects       = effects,
                },
            }
        );
        REQUIRE_MESSAGE(readmitted.has_value(), failureText(readmitted));
    }

    TEST_CASE("a mutating handler that re-derives a different call is stopped at the field that diverged")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedRegistration();
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
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectToolProgramRegistrar{};
            auto const program =
                loadProgram(registration, registrar, log, *dispatcher);

            auto const call = projectRootCall(
                program,
                root,
                k_mutatingHandlerTool,
                k_children
            );
            REQUIRE(prepared.store.persistToolRootRequest(root).has_value());
            auto const authority = planAuthorityFor(
                prepared.store,
                registration,
                prepared.manifest,
                prepared.artifactRootHash
            );
            auto const effects  = std::vector{projectEffect(k_targetId)};
            auto const admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .planAuthority = authority,
                        .effects       = effects,
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
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
        CHECK(log->handlerAnswers == 0U);

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
        auto const authorityAfter = planAuthorityFor(
            prepared.store,
            registration,
            prepared.manifest,
            prepared.artifactRootHash
        );
        auto const readmitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = nextRoot,
                .call       = nextCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = authorityAfter,
                    .effects       = std::vector{projectEffect(k_targetId)},
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
        auto const registration = verifiedRegistration();
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
                frameworkProvider(log)
            );
            REQUIRE(dispatcher.has_value());
            auto registrar = ProjectToolProgramRegistrar{};
            auto const program =
                loadProgram(registration, registrar, log, *dispatcher);
            auto const authority = planAuthorityFor(
                prepared.store,
                registration,
                prepared.manifest,
                prepared.artifactRootHash
            );

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
                        .planAuthority = authority,
                        .effects       = composedEffects,
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
                        .planAuthority = authority,
                        .effects       = inputEffects,
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
            frameworkProvider(log)
        );
        REQUIRE(dispatcher.has_value());
        auto registrar = ProjectToolProgramRegistrar{};
        auto const program =
            loadProgram(registration, registrar, log, *dispatcher);

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
}
