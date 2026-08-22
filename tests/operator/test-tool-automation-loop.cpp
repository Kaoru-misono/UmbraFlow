#include <operator/controller.hpp>
#include <operator/ledger.hpp>
#include <operator/project-plugin.hpp>
#include <operator/project-tool-dispatch.hpp>
#include <operator/project-tool-program.hpp>
#include <operator/snapshot-reference.hpp>
#include <operator/tool-actor-adapters.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-descriptor.hpp>
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

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Experiment E2 of the cycle-SPI plan: a Project automation script that
// observes, calls Project interpretation and planning Tools, requests Framework
// input, chooses a bounded delay, observes a transition frame, repeats
// wait-observe-recognise, reconciles, and terminates -- written as an ordinary
// Luau loop over the shared SDK modules, with no hand-written external
// `advance` state machine and no Framework-imposed capture interval.
//
// The two things the loop needs that no earlier stage could give it are the
// subject here. A mutating child call must have a proposed effect set, and it
// comes from the CHILD DESCRIPTOR'S OWN bounds plus the controlled target the
// run's binding holds the lease on -- the script names a Tool and an argument
// value and states none of it. And a child call must be issuable AGAINST an
// observation, which needs the reference bytes the script holds as data to be
// recognised by the authority that mints and spends them; the script can hold
// the bytes and can never hold the authority, so the join is the one place the
// two meet.
//
// Everything here is production-unreachable. Nothing in ProductLifecycle builds
// a dispatcher or reaches the registrar, and the Framework Tools this run
// reaches are answered by the provider this file installs -- so what E2 proves
// under a test provider is the runtime's own path: admission, delegation, the
// derived effect envelope, the observation join, the durable rows and replay.
// What only the production flip can prove is that the real Framework providers
// mint and spend the same references and that a real Host delivers the input.
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

        constexpr auto k_pluginId = std::string_view{"e2.automation"};

        // The five Tools this Project declares, one per exported entry. Their
        // order is UTF-8 order of the names, which is the only order a catalog
        // and a binding table may state.
        constexpr auto k_chooseTool = std::string_view{"e2.automation.choose"};
        constexpr auto k_recogniseTool =
            std::string_view{"e2.automation.recognise"};
        constexpr auto k_reducerTool = std::string_view{"e2.automation.reduce"};
        constexpr auto k_runTool     = std::string_view{"e2.automation.run"};
        constexpr auto k_settleTool  = std::string_view{"e2.automation.settle"};

        constexpr auto k_observeTool =
            std::string_view{"framework.screen.observe"};
        constexpr auto k_inputTool =
            std::string_view{"framework.input.semantic_target"};
        constexpr auto k_waitTool = std::string_view{"framework.workflow.wait"};

        constexpr auto k_inputEffectType =
            std::string_view{"framework.input.deliver"};

        constexpr auto k_sessionId    = std::string_view{"e2-session"};
        constexpr auto k_controllerId = std::string_view{"e2-controller"};
        constexpr auto k_targetId     = std::string_view{"e2-target"};
        constexpr auto k_instanceKey  = std::string_view{"e2-instance"};
        constexpr auto k_boundMillis  = uint64{60'000};

        // The world this run's observations are bound to. The generation and
        // the two instants are constants because the provider both mints and
        // presents them, so nothing here is a clock this case depends on.
        constexpr auto k_hostGeneration      = uint64{7};
        constexpr auto k_observationExpiry   = uint64{2'000};
        constexpr auto k_presentedAtMillis   = uint64{1'000};

        // The snapshot-local vocabulary the provider publishes on each frame.
        // The first frame is mid-transition and the second has settled, which
        // is what gives the loop something to iterate over: the script does not
        // count frames, it re-reads the world.
        constexpr auto k_startTarget    = std::string_view{"e2.start"};
        constexpr auto k_loadingTarget  = std::string_view{"e2.loading"};
        constexpr auto k_readyTarget    = std::string_view{"e2.ready"};
        constexpr auto k_uiAction       = std::string_view{"e2.press"};

        constexpr auto k_toolCatalogBytes = std::string_view{
            R"({"schema":"umbraflow-tool-catalog/v1","plugin":"e2.automation"})"
        };
        constexpr auto k_stateSchemaBytes        = std::string_view{"e2-state"};
        constexpr auto k_observationSchemaBytes  = std::string_view{"e2-observation"};
        constexpr auto k_preconditionSchemaBytes = std::string_view{"e2-precondition"};

        // The automation script.
        //
        // `derive` is E2's subject and is an ordinary Luau loop: it observes,
        // asks the Project what it is looking at, asks the Project what to do,
        // posts one Framework input against the observation it just read,
        // waits for a bound it chose, and observes again. Nothing outside it
        // drives a step, nothing outside it decides when to capture, and its
        // exit condition is what the game told it rather than a counter.
        //
        // The reducer closure: `plugin_id` and one entry, compiled on the
        // pure program type. The ledger folds a new ProjectInstance's baseline
        // through it at provisioning time, and a scoped require here would be
        // a module the pure resolver rightly cannot find -- which is why the
        // fold lives in a closure of its own rather than beside the entries
        // the loop dispatches.
        constexpr auto k_reducerSource = std::string_view{R"LUAU(
return {
    plugin_id = "e2.automation",

    reduce = function(_input)
        return { revision = 0 }
    end,
}
)LUAU"};

        // The tool closure: the four entries the loop's Tools are bound to.
        //
        // The scoped modules are required inside the entry bodies rather than
        // at module scope. Nothing forces that now that this closure is
        // compiled on the scoped type alone, but it keeps each entry's
        // capability visible at the line that spends it.
        constexpr auto k_toolSource = std::string_view{R"LUAU(
return {
    plugin_id = "e2.automation",

    run = function(input)
        local results = require("@umbraflow/result")
        local screen = require("@umbraflow/screen")
        local tools = require("@umbraflow/tools")
        local workflow = require("@umbraflow/workflow")

        local trace = {}
        local traced = 0
        local delivered = false

        for _attempt = 1, input.attempts do
            local handle = results.unwrap_or(
                screen.observation(screen.observe()),
                nil
            )
            if handle == nil then
                break
            end

            local reading = results.unwrap_or(
                tools.result(tools.call("e2.automation.recognise", {
                    targets = screen.targets(handle),
                })),
                nil
            )
            traced += 1
            trace[traced] = reading.state
            if reading.state == "ready" then
                break
            end

            local step = results.unwrap_or(
                tools.result(tools.call("e2.automation.choose", {
                    actions = screen.actions(handle),
                    state = reading.state,
                })),
                nil
            )
            local posted = tools.call("framework.input.semantic_target", {
                observation_reference = screen.use(handle),
                semantic_target = step.target,
                ui_action = step.action,
            })
            delivered = workflow.delivered(posted)
            traced += 1
            trace[traced] = tools.state(posted)

            workflow.wait(step.wait_ms)
        end

        local settled = results.unwrap_or(
            tools.result(tools.call("e2.automation.settle", {
                delivered = delivered,
                trace = trace,
            })),
            nil
        )
        return { delivered = delivered, settled = settled.settled, trace = trace }
    end,

    recognise = function(input)
        local state = "transition"
        for index = 1, #input.targets do
            if input.targets[index] == "e2.ready" then
                state = "ready"
            end
        end
        return { state = state }
    end,

    choose = function(input)
        return {
            action = input.actions[1],
            target = "e2.start",
            wait_ms = 25,
        }
    end,

    -- Reconciliation, and the fixture's forged-reference probe. An actor may
    -- hand this entry a document SHAPED like an observation reference, and the
    -- script relays it exactly as it relays the one screen.observe answered
    -- with -- it has no way to tell them apart, which is the point. What
    -- separates them is whether this run's authority minted those bytes.
    settle = function(input)
        if input.observation_reference ~= nil then
            local tools = require("@umbraflow/tools")
            return tools.call("framework.input.semantic_target", {
                observation_reference = input.observation_reference,
                semantic_target = "e2.start",
                ui_action = "e2.press",
            })
        end
        return { settled = input.delivered }
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
        auto reducerModules() -> std::vector<ProjectModuleBlob>
        {
            return modulesOf(k_reducerSource);
        }

        [[nodiscard]]
        auto toolModules() -> std::vector<ProjectModuleBlob>
        {
            return modulesOf(k_toolSource);
        }

        [[nodiscard]]
        auto boundEntries() -> std::vector<ProjectToolBinding>
        {
            return {
                ProjectToolBinding{
                    .toolName   = std::string{k_chooseTool},
                    .entryPoint = "choose",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_recogniseTool},
                    .entryPoint = "recognise",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_reducerTool},
                    .entryPoint = "settle",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_runTool},
                    .entryPoint = "run",
                },
                ProjectToolBinding{
                    .toolName   = std::string{k_settleTool},
                    .entryPoint = "settle",
                },
            };
        }

        // What the tool closure states it exports: the sorted, unique union
        // of the entry points its bindings name, written out rather than
        // derived from boundEntries() -- a declaration computed from the table
        // it is joined against would be the table compared with itself.
        [[nodiscard]]
        auto exportedToolEntries() -> std::vector<std::string>
        {
            return {"choose", "recognise", "run", "settle"};
        }

        // The reducer closure's whole declared export set.
        [[nodiscard]]
        auto exportedReducerEntries() -> std::vector<std::string>
        {
            return {"reduce"};
        }

        [[nodiscard]]
        auto declaredToolNames() -> std::vector<std::string>
        {
            return {
                std::string{k_chooseTool},
                std::string{k_recogniseTool},
                std::string{k_reducerTool},
                std::string{k_runTool},
                std::string{k_settleTool},
            };
        }

        // The bound the Framework's own semantic-input Tool declares, borrowed
        // by the two Project Tools that delegate to it. A Project descriptor
        // may REQUEST that envelope -- section 3.3 -- and requesting it is what
        // makes the child's derived effect land inside the admitted root
        // envelope rather than outside it. It cannot widen anything: the
        // ceiling is still the pinned policy, and the effect a child proposes
        // is still derived from the child's own descriptor.
        [[nodiscard]]
        auto frameworkInputBound() -> EffectBound
        {
            auto const catalog = FrameworkToolCatalogOwner::create();
            REQUIRE(catalog.has_value());
            auto const descriptor = catalog->describe(k_inputTool);
            REQUIRE(descriptor.has_value());
            REQUIRE(descriptor->effectBounds.size() == 1U);
            return descriptor->effectBounds.front();
        }

        [[nodiscard]]
        auto readOnlyDescriptor() -> ToolDescriptor
        {
            return ToolDescriptor{
                .toolVersion          = "1",
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .childEffects         = {},
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

        // What the automation entry requests: the Framework Tools its loop
        // reaches, the Project Tools it composes, and the one effect it may
        // delegate. Every one of them is a request judged at admission.
        [[nodiscard]]
        auto automationDescriptor(std::vector<std::string> childToolNames)
            -> ToolDescriptor
        {
            auto descriptor           = readOnlyDescriptor();
            descriptor.effectBounds   = {frameworkInputBound()};
            descriptor.uiActionBounds = {std::string{k_inputTool}};
            descriptor.childEffects   = ChildEffectDeclaration{
                  .childToolNames         = std::move(childToolNames),
                  .maximumChildSurface    = ToolSurface::Semantic,
                  .maximumChildMutability = ToolMutability::Mutating,
                  .maximumChildRisk       = Risk::Medium,
                  .maximumChildCalls      = 16U,
            };
            descriptor.mutability  = ToolMutability::Mutating;
            descriptor.idempotency = ToolIdempotency::DeliverySafe;
            return descriptor;
        }

        [[nodiscard]]
        auto declaredTool(std::string_view name) -> ToolCatalogEntry
        {
            if (name == k_runTool)
            {
                return ToolCatalogEntry{
                    .name       = std::string{name},
                    .descriptor = automationDescriptor({
                        std::string{k_chooseTool},
                        std::string{k_recogniseTool},
                        std::string{k_settleTool},
                        std::string{k_inputTool},
                        std::string{k_observeTool},
                        std::string{k_waitTool},
                    }),
                };
            }
            if (name == k_settleTool)
            {
                // Reconciliation is mutating and may reach the same input
                // Tool, which is what makes the forged-reference probe below
                // stand on exactly one refusal: everything else the Operator
                // could have refused -- the parent declaration, the root
                // envelope, the pinned policy -- admits it.
                return ToolCatalogEntry{
                    .name       = std::string{name},
                    .descriptor = automationDescriptor({
                        std::string{k_inputTool},
                    }),
                };
            }
            return ToolCatalogEntry{
                .name       = std::string{name},
                .descriptor = readOnlyDescriptor(),
            };
        }

        // The seam a registration made only to provision from answers with.
        // A scoped program is required to hold one, and this one refuses every
        // call: setup holds no lease, controller or observation authority for
        // a call to be admitted under. One value, never a branch.
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
                {"baseline_event_type",
                 json::Value::ofString(claims.baselineEventType)},
                {"journal_event_schema_manifest_hash",
                 json::Value::ofString(claims.journalEventSchemaManifestHash.hex())},
                {"observed_instance_identity_schema_hashes",
                 json::Value::ofArray({})},
                {"plugin_environment_hash",
                 json::Value::ofString(claims.pluginEnvironmentHash.hex())},
                {"plugin_id", json::Value::ofString(claims.pluginId)},
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
                {"reducer_closure", closureValue(claims.reducerClosure)},
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

        [[nodiscard]]
        auto verifiedGeneration() -> VerifiedProjectGeneration
        {
            auto const environmentHash = currentProjectPluginEnvironmentHash();
            REQUIRE(environmentHash.has_value());
            auto claims = ProjectGenerationClaims{
                .projectRegistrationFormat = k_projectGenerationFormat,
                .pluginId                  = std::string{k_pluginId},
                .reducerClosure            = ProjectClosureClaims{
                    .moduleManifestHash  = manifestHashOf(reducerModules()),
                    .exportedEntryPoints = exportedReducerEntries(),
                },
                .toolClosure = ProjectClosureClaims{
                    .moduleManifestHash  = manifestHashOf(toolModules()),
                    .exportedEntryPoints = exportedToolEntries(),
                },
                .pluginEnvironmentHash        = *environmentHash,
                .toolCatalogHash              = hashOf(k_toolCatalogBytes),
                .projectStateSchemaHash       = hashOf(k_stateSchemaBytes),
                .projectObservationSchemaHash = hashOf(k_observationSchemaBytes),
                .projectToolPreconditionSchemaHash =
                    hashOf(k_preconditionSchemaBytes),
                .reconcilePayloadSchemaManifestHash   = hashOf("e2-reconcile"),
                .journalEventSchemaManifestHash       = hashOf("e2-journal"),
                .baselineEventType                    = "e2.baseline",
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
                            "E2 fixture generation is not exact JCS"
                        );
                    }
                    return claims;
                }
            );
            REQUIRE(generation.has_value());
            return *std::move(generation);
        }

        // The world the Framework Tools of this run answer for, and the run's
        // observation authority.
        //
        // The authority is a member rather than a separate object because the
        // provider and the dispatcher must share exactly one: the provider is
        // what mints a reference and what spends it, and the dispatcher is what
        // recognises the bytes a script hands back. A second authority beside
        // it would recognise nothing.
        //
        // No in-class initializer for the hashes: ContentHash has no default
        // state.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct AutomationWorld final
        {
            SnapshotObservationAuthority observations{};

            ContentHash artifactRootHash;
            ContentHash registrationHash;

            uint32 framesObserved{0};
            uint32 inputsPosted{0};
            uint32 waitsRun{0};

            std::vector<std::string> postedTargets{};
            std::vector<std::string> spentReferences{};
        };

        // The frame this observation publishes. The first is mid-transition and
        // the second has settled; the script never learns which is which except
        // by asking the Project to read it.
        [[nodiscard]]
        auto frameTargets(uint32 framesObserved) -> std::vector<std::string>
        {
            return {
                framesObserved == 0U
                    ? std::string{k_loadingTarget}
                    : std::string{k_readyTarget},
                std::string{k_startTarget},
            };
        }

        [[nodiscard]]
        auto confirmedResult(json::Value value) -> Result<ToolCallCompletion>
        {
            UF_TRY_VALUE(
                canonical,
                CanonicalJson::parseExact(json::canonicalBytes(std::move(value)))
            );
            return ToolCallCompletion::confirmed(std::move(canonical));
        }

        [[nodiscard]]
        auto requiredString(json::Value const& arguments, std::string_view member)
            -> std::string
        {
            auto const* const p_member = arguments.find(member);
            REQUIRE(p_member != nullptr);
            REQUIRE(p_member->kind() == json::ValueKind::String);
            return std::string{p_member->string()};
        }

        // The Framework side of this run. It mints every observation reference
        // into the run's own authority and spends one there per input, so the
        // authority the dispatcher borrows is the same object that answered the
        // observation the script is presenting.
        [[nodiscard]]
        auto frameworkProvider(std::shared_ptr<AutomationWorld> world)
            -> ToolProvider
        {
            return [world = std::move(world)](
                       ToolCallPositionIdentity const& call
                   ) -> Result<ToolCallCompletion>
            {
                if (call.toolName() == k_observeTool)
                {
                    auto const frame = world->framesObserved;
                    ++world->framesObserved;
                    UF_TRY_VALUE(
                        reference,
                        world->observations.mint(SnapshotObservationSpec{
                            .controlledTargetId      = std::string{k_targetId},
                            .runtimeArtifactRootHash = world->artifactRootHash,
                            .projectRegistrationHash = world->registrationHash,
                            .frameIdentityHash =
                                hashOf("e2-frame-" + std::to_string(frame)),
                            .hostGeneration        = k_hostGeneration,
                            .rootIdentity          = call.rootIdentity(),
                            .issuingParentIdentity = call.parentIdentity(),
                            .expiresAtUnixMillis   = k_observationExpiry,
                            .localSemanticTargets  = frameTargets(frame),
                            .authorizedUiActions   = {std::string{k_uiAction}},
                        })
                    );
                    return confirmedResult(json::Value::ofObject({
                        {std::string{k_observationReferenceArgument},
                         reference.wire().value()},
                    }));
                }
                if (call.toolName() == k_inputTool)
                {
                    UF_TRY_VALUE(
                        arguments,
                        CanonicalJson::parseExact(call.canonicalArgs())
                    );
                    auto const* const p_reference = arguments.value().find(
                        k_observationReferenceArgument
                    );
                    REQUIRE(p_reference != nullptr);

                    // Nothing about the world is read from the arguments except
                    // the two names the Project chose. The target, the artifact,
                    // the generation and the coordinate are all this run's own,
                    // which is what makes "validated against the same snapshot"
                    // structural rather than checked.
                    UF_TRY_VALUE(
                        resolved,
                        world->observations.resolve(
                            SnapshotObservationConsumption{
                                .exactReferenceJcs =
                                    json::canonicalBytes(*p_reference),
                                .controlledTargetId = std::string{k_targetId},
                                .runtimeArtifactRootHash =
                                    world->artifactRootHash,
                                .projectRegistrationHash =
                                    world->registrationHash,
                                .hostGeneration        = k_hostGeneration,
                                .rootIdentity          = call.rootIdentity(),
                                .issuingParentIdentity = call.parentIdentity(),
                                .localSemanticTarget   = requiredString(
                                    arguments.value(),
                                    "semantic_target"
                                ),
                                .uiAction = requiredString(
                                    arguments.value(),
                                    "ui_action"
                                ),
                                .presentedAtUnixMillis = k_presentedAtMillis,
                            }
                        )
                    );
                    ++world->inputsPosted;
                    world->postedTargets.emplace_back(
                        resolved.localSemanticTarget()
                    );
                    world->spentReferences.emplace_back(
                        resolved.referenceIdentity().hex()
                    );
                    return confirmedResult(json::Value::ofObject({
                        {"delivered", json::Value::ofBoolean(true)},
                        {"frame_identity_hash",
                         json::Value::ofString(
                             resolved.frameIdentityHash().hex()
                         )},
                    }));
                }
                if (call.toolName() == k_waitTool)
                {
                    ++world->waitsRun;
                    UF_TRY_VALUE(
                        arguments,
                        CanonicalJson::parseExact(call.canonicalArgs())
                    );
                    auto const* const p_duration =
                        arguments.value().find("duration_ms");
                    REQUIRE(p_duration != nullptr);
                    return confirmedResult(json::Value::ofObject({
                        {"completed", json::Value::ofBoolean(true)},
                        {"duration_ms",
                         json::Value::ofNumber(p_duration->number())},
                    }));
                }
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the E2 provider answers no Tool named " + call.toolName()
                );
            };
        }

        [[nodiscard]]
        auto projectSchemaOwner(
            VerifiedProjectGeneration const& registration
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

        [[nodiscard]]
        auto policyBytes() -> std::string
        {
            auto const types =
                std::vector<std::string>{std::string{k_inputEffectType}};
            return conformance::policyArtifactBytes(hashOf("operator"), types);
        }

        [[nodiscard]]
        auto planAuthorityFor(
            OperatorCoordinator& store,
            VerifiedProjectGeneration const& registration,
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
                FrameId{701}
            );
            auto const runtimeModel =
                observation.host->runtimeModelBinding(observation.generation);
            REQUIRE(runtimeModel.has_value());
            auto authority = OperatorPlanAuthority::create(
                registration,
                manifest,
                *runtimeModel,
                "operator",
                policyBytes()
            );
            REQUIRE_MESSAGE(authority.has_value(), failureText(authority));
            return *std::move(authority);
        }

        // One Operator over one runtime directory, with the run's authorities.
        // Built in place and never moved after the dispatcher borrows it.
        //
        // No in-class initializer for the plan authority: it has no default
        // state.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct Incarnation final
        {
            OperatorCoordinator   store;
            ContentHash           artifactRootHash;
            SessionManifest       manifest;
            ControllerBinding     controller;
            ControlLease          lease;
            OperatorPlanAuthority planAuthority;
        };

        [[nodiscard]]
        auto openIncarnation(
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

            // Provisioning needs the generation's fold and nothing else, so
            // this registration's Tool Runtime seam refuses every call.
            auto registrar  = ProjectGenerationRegistrar{};
            auto generation = registrar.registerGeneration(
                registration,
                toolCatalogOwner(registration),
                projectSchemaOwner(registration),
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = "main",
                    .modules     = reducerModules(),
                },
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = "main",
                    .modules     = toolModules(),
                },
                {},
                [](std::string_view, std::string_view) -> Status { return ok(); },
                refusingToolRuntime()
            );
            REQUIRE_MESSAGE(generation.has_value(), failureText(generation));

            auto const policy   = policyBytes();
            auto const manifest = test_support::sessionManifest(
                registration,
                artifactRootHash,
                hashOf("e2-agent-profile"),
                policy
            );
            REQUIRE(store.registerProject(registration).has_value());
            auto const provisioned = store.provisionProjectInstance(
                registration,
                *generation,
                ProjectInstanceBaseline{
                    .projectInstanceKey  = std::string{k_instanceKey},
                    .eventId             = "",
                    .sessionManifestHash = manifest.hash(),
                    .entry               = std::nullopt,
                }
            );
            REQUIRE_MESSAGE(provisioned.has_value(), failureText(provisioned));

            auto const worldScope = ObservedInstanceWorldScope::run(
                std::string{k_targetId},
                1
            );
            REQUIRE(worldScope.has_value());
            auto const pinned = store.pinSession(
                SessionPin{
                    .sessionId                 = std::string{k_sessionId},
                    .authenticatedControllerId = std::string{k_controllerId},
                    .idempotencyNamespace      = std::string{k_controllerId},
                    .projectRegistrationHash   = registration.hash(),
                    .controllerCapabilities    = {
                        std::string{conformance::k_operateCapability},
                    },
                    .controlledTargetId = std::string{k_targetId},
                    .projectInstanceKey = std::string{k_instanceKey},
                    .mode               = SessionMode::Write,
                    .kind               = ControllerKind::Script,
                    .worldScope         = *worldScope,
                },
                manifest,
                std::nullopt
            );
            REQUIRE_MESSAGE(pinned.has_value(), failureText(pinned));
            auto controller = store.bindController(std::string{k_sessionId});
            REQUIRE_MESSAGE(controller.has_value(), failureText(controller));
            auto lease = store.acquireLease(*controller);
            REQUIRE_MESSAGE(lease.has_value(), failureText(lease));
            auto authority =
                planAuthorityFor(store, registration, manifest, artifactRootHash);

            return Incarnation{
                .store            = std::move(store),
                .artifactRootHash = artifactRootHash,
                .manifest         = manifest,
                .controller       = *std::move(controller),
                .lease            = *std::move(lease),
                .planAuthority    = std::move(authority),
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
                projectSchemaOwner(registration),
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = "main",
                    .modules     = reducerModules(),
                },
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = "main",
                    .modules     = toolModules(),
                },
                {},
                [](std::string_view, std::string_view) -> Status
                { return ok(); },
                dispatcher.toolRuntimeSeam()
            );
            REQUIRE_MESSAGE(loaded.has_value(), failureText(loaded));
            return *std::move(loaded);
        }

        [[nodiscard]]
        auto executionIdentity(ProjectGenerationHandle const& program)
            -> ToolExecutionIdentity
        {
            return ToolExecutionIdentity{
                .runIdentity                 = hashOf("e2-run"),
                .frameworkReleaseIdentity    = hashOf("e2-framework"),
                .toolRuntimeProtocolIdentity = hashOf("e2-protocol"),
                .environmentIdentity         = program.environmentIdentity(),
            };
        }

        [[nodiscard]]
        auto payloadOf(ToolCallReplay const& replay) -> std::string
        {
            return replay.payload ? replay.payload->bytes() : std::string{};
        }
    } // namespace

    // E2. One automation script, one run, and the two seams that had to open
    // for it: a mutating child with a derived effect set, and a child issued
    // against the observation the script is holding.
    TEST_CASE("a Project automation loop observes, acts, waits and terminates")
    {
        auto temporary          = TemporaryDirectory{};
        auto const registration = verifiedGeneration();
        auto databasePath  = std::filesystem::path{};
        auto world         = std::shared_ptr<AutomationWorld>{};
        auto loopAnswer    = std::string{};
        auto forgedFailure = std::string{};

        {
            auto prepared = openIncarnation(temporary.path(), registration);
            databasePath  = prepared.store.databasePath();
            world         = std::make_shared<AutomationWorld>(AutomationWorld{
                        .artifactRootHash = prepared.artifactRootHash,
                        .registrationHash = registration.hash(),
            });

            auto dispatcher = ProjectToolDispatcher::create(
                prepared.store,
                world->observations,
                prepared.planAuthority,
                frameworkProvider(world)
            );
            REQUIRE_MESSAGE(dispatcher.has_value(), failureText(dispatcher));
            auto registrar     = ProjectGenerationRegistrar{};
            auto const program = loadProgram(registration, registrar, *dispatcher);
            auto const catalog =
                ToolStartCatalog::create(toolCatalogOwner(registration));
            REQUIRE_MESSAGE(catalog.has_value(), failureText(catalog));
            auto const execution = executionIdentity(program);
            auto const run       = ToolActorRun{
                      .controller    = prepared.controller,
                      .lease         = prepared.lease,
                      .execution     = execution,
                      .planAuthority = prepared.planAuthority,
                      .catalog       = *catalog,
            };

            // The actor's start. A Project starting one of its own bound
            // entries is the same translation every other caller performs: it
            // names a Tool and an argument value, and the producer mints the
            // root coordinate and derives what the start proposes.
            auto automation = ProjectAutomationAdapter{};
            auto const start = automation.translate(
                run,
                program.bindingTable(),
                ProjectAutomationStart{
                    .requestKey    = "e2-automation",
                    .objective     = json::Value::ofObject({
                        {"objective", json::Value::ofString("drive one screen")},
                    }),
                    .entryToolName = std::string{k_runTool},
                    .arguments     = json::Value::ofObject({
                        {"attempts", json::Value::ofNumber(3.0)},
                    }),
                }
            );
            REQUIRE_MESSAGE(start.has_value(), failureText(start));

            auto const answered =
                dispatcher->dispatch(program, *start, std::stop_token{});
            REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
            CHECK(answered->state == ToolCallState::Confirmed);
            loopAnswer = payloadOf(*answered);

            // The same input call, with an observation reference nothing
            // minted. It never reaches a durable coordinate: recognition is
            // byte equality against what this run's authority minted, so a
            // fabricated document is refused at the seam -- ahead of admission,
            // which would have admitted it -- the refusal is terminal for the
            // VM, and the run it was made from records the failure.
            auto const forged = automation.translate(
                run,
                program.bindingTable(),
                ProjectAutomationStart{
                    .requestKey    = "e2-forged",
                    .objective     = json::Value::ofObject({
                        {"objective", json::Value::ofString("forge one input")},
                    }),
                    .entryToolName = std::string{k_settleTool},
                    .arguments     = json::Value::ofObject({
                        {std::string{k_observationReferenceArgument},
                         json::Value::ofObject({
                             {"forged", json::Value::ofBoolean(true)},
                         })},
                    }),
                }
            );
            REQUIRE_MESSAGE(forged.has_value(), failureText(forged));
            auto const refused =
                dispatcher->dispatch(program, *forged, std::stop_token{});
            REQUIRE_MESSAGE(refused.has_value(), failureText(refused));
            CHECK(refused->state == ToolCallState::TerminalFailure);
            forgedFailure = payloadOf(*refused);
        }

        // The loop ran twice, acted once, waited once, and terminated on what
        // the second frame said rather than on its attempt counter.
        CHECK(
            loopAnswer
            == R"({"delivered":true,"settled":true,)"
               R"("trace":["transition","confirmed","ready"]})"
        );
        CHECK(world->framesObserved == 2U);
        CHECK(world->inputsPosted == 1U);
        CHECK(world->waitsRun == 1U);
        REQUIRE(world->postedTargets.size() == 1U);
        CHECK(world->postedTargets.front() == k_startTarget);
        CHECK(forgedFailure.contains(
            "unminted: these bytes are not an observation reference this "
            "Framework minted"
        ));

        auto const database = test_support::OperatorDatabaseProbe{databasePath};

        // The input call was issued AGAINST the observation, so its durable
        // position carries the reference identity the authority spent. A run
        // that had issued it as an ordinary call would have a null here and
        // would replay a different call at the same coordinate without saying
        // so.
        auto const positions = database.readRows(
            "SELECT coalesce(observation_reference_hash, '') "
            "FROM tool_call_positions WHERE tool_name='"
                + std::string{k_inputTool} + "'"
        );
        REQUIRE(positions.size() == 1U);
        REQUIRE(world->spentReferences.size() == 1U);
        CHECK(positions.front().front() == world->spentReferences.front());

        // The mutating child was admitted on an effect the CATALOG declared,
        // scoped to the target this run holds the lease on. The envelope is on
        // the child's own admission attempt, and the script that named the Tool
        // wrote none of it.
        auto const envelopes = database.readRows(
            "SELECT coalesce(attempt.effect_envelope, '') "
            "FROM tool_admission_attempts attempt "
            "JOIN tool_call_positions position "
            "ON position.call_identity=attempt.call_identity "
            "WHERE position.tool_name='"
                + std::string{k_inputTool} + "'"
        );
        REQUIRE(envelopes.size() == 1U);
        CHECK(envelopes.front().front().contains(
            R"("namespaced_type":"framework.input.deliver")"
        ));
        CHECK(envelopes.front().front().contains(
            R"("scope_key":"e2-target")"
        ));

        // Every Tool the loop reached has one terminal row, and the observation
        // the script spent is the only one either frame minted.
        auto const states = database.readRows(
            "SELECT position.tool_name, history.state "
            "FROM tool_call_positions position "
            "JOIN tool_call_history history "
            "ON history.call_identity=position.call_identity "
            "WHERE position.root_identity IN ("
            "SELECT root_identity FROM tool_call_positions WHERE tool_name='"
                + std::string{k_runTool}
                + "') ORDER BY position.call_sequence, position.tool_name"
        );
        CHECK(states.size() == 9U);
        for (auto const& row : states)
        {
            CHECK(row.back() == "confirmed");
        }
    }
}
