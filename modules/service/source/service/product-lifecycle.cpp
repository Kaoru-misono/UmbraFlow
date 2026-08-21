#include "product-lifecycle.hpp"

#include <deployment/project-deployment.hpp>
#include <deployment/project-directory.hpp>

#include <operator/effective-plan.hpp>
#include <operator/manifest.hpp>
#include <operator/policy.hpp>
#include <operator/project-plugin.hpp>
#include <operator/tool-executor.hpp>

#include <task/platform/confined-file.hpp>
#include <task/runtime-model-file.hpp>
#include <task/task-host.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <json/value.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace uf::service
{
    namespace
    {
        constexpr auto k_operatorSchemaPath = std::string_view{
            "schema/umbraflow-operator-v1.schema.json"
        };
        constexpr auto k_noAgentProfile = std::string_view{"null"};

        // The controller identity an upgrade authenticates as, and the target
        // every upgrade session binds. The target id is stable across upgrades
        // so the chain of upgrade sessions shares one project instance key,
        // which is what makes a later pin an upgrade of an earlier one rather
        // than a stranger -- the ledger's release-upgrade guard keys on exactly
        // that pair. A per-run target id would silently forfeit the quiescence
        // and approval checks.
        constexpr auto k_upgradeControllerId = std::string_view{"umbra-flow-upgrade"};
        constexpr auto k_upgradeTargetId     = std::string_view{"runtime-artifact"};

        [[nodiscard]]
        auto hashOf(std::string_view bytes) -> Result<ContentHash>
        {
            return sha256(std::as_bytes(std::span{bytes}));
        }

        [[nodiscard]]
        auto projectArtifactRootHash(
            std::filesystem::path const& artifactRoot
        ) -> Result<ContentHash>
        {
            UF_TRY_VALUE(root, task_platform::ConfinedRoot::open(artifactRoot));
            UF_TRY_VALUE(
                manifestBytes,
                root.readFile(
                    task::k_runtimeArtifactManifestFileName,
                    task::k_maximumRuntimeManifestBytes
                )
            );
            return sha256(manifestBytes);
        }

        [[nodiscard]]
        auto internalProjectInstanceKey(
            ContentHash const& registrationHash,
            std::string_view controlledTargetId
        ) -> Result<std::string>
        {
            auto material = registrationHash.hex();
            material.push_back('\0');
            material += controlledTargetId;
            UF_TRY_VALUE(hash, hashOf(material));
            return "project-" + hash.hex();
        }

        [[nodiscard]]
        auto internalSessionId(
            ContentHash const& manifestHash,
            std::string_view controllerId,
            std::string_view controlledTargetId
        ) -> Result<std::string>
        {
            static auto s_sequence = std::atomic<uint64>{1};
            // Built by concatenation rather than by std::format. A format string
            // is a string literal, so an embedded NUL terminates it: the earlier
            // "{}\0{}\0{}\0{}\0{}" spelling reached format as "{}" and silently
            // dropped four arguments, leaving every session on one manifest
            // sharing an id. The separator still has to be a byte that cannot
            // occur in any part, which is why it is a NUL and why it is appended
            // rather than written into a literal.
            auto material = manifestHash.hex();
            material += '\0';
            material += controllerId;
            material += '\0';
            material += controlledTargetId;
            material += '\0';
            material += std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            material += '\0';
            material += std::to_string(
                s_sequence.fetch_add(1, std::memory_order_relaxed)
            );
            UF_TRY_VALUE(hash, hashOf(material));
            return "session-" + hash.hex();
        }

        [[nodiscard]]
        auto publishedSchema(std::string_view relativePath)
            -> Result<framework_schema::FrameworkSchemaDocument>
        {
            auto const document = framework_schema::findFrameworkSchema(relativePath);
            if (!document.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "generated framework schema catalog is missing "
                        + std::string{relativePath}
                );
            }
            return *document;
        }

        [[nodiscard]]
        auto confirmedToolResult(json::Value value)
            -> Result<operator_runtime::ToolCallCompletion>
        {
            UF_TRY_VALUE(
                canonical,
                operator_runtime::CanonicalJson::parseExact(
                    json::canonicalBytes(value)
                )
            );
            return operator_runtime::ToolCallCompletion::confirmed(
                std::move(canonical)
            );
        }
    }

    struct ProductLifecycle::Impl final
    {
        deployment::LoadedProject loaded;
        std::size_t               deploymentIndex;

        // The registrar is NOT held. registerPlugin returns the handle by value
        // and the handle owns its own state through a shared_ptr, so keeping the
        // registrar alive anchors nothing: measured 2026-08-14, the field was
        // written once and never read, and findExact -- the only thing its map
        // serves -- has no production caller. Holding it also gave Impl a
        // std::map, whose move this standard library does not declare noexcept,
        // which was the sole reason two types here could throw while being
        // constructed.
        operator_runtime::ProjectPluginHandle   plugin;
        operator_runtime::OperatorTaskHost      operatorHost;
        operator_runtime::OperatorPlanAuthority planAuthority;
        operator_runtime::ControllerBinding     controller;
        static_assert(
            std::is_nothrow_move_constructible_v<
                operator_runtime::ControlLease
            >,
            "ControlLease must transfer into its RAII owner without failure"
        );
        std::optional<operator_runtime::ControlLease> activeLease{};

        GenerationId              generation;
        LifecycleAccess           access;
        task::RuntimeModelBinding runtimeModel;
        uint64                    installedGeneration;
        std::string               sessionId;
        ContentHash               sessionManifestHash;

        std::vector<operator_runtime::RecoveredUncertainDispatch> recoveries;

        Impl(
            deployment::LoadedProject ownedLoaded,
            std::size_t ownedDeploymentIndex,
            operator_runtime::ProjectPluginHandle ownedPlugin,
            operator_runtime::OperatorTaskHost ownedOperatorHost,
            operator_runtime::OperatorPlanAuthority ownedPlanAuthority,
            operator_runtime::ControllerBinding ownedController,
            GenerationId ownedGeneration,
            LifecycleAccess ownedAccess,
            task::RuntimeModelBinding ownedRuntimeModel,
            uint64 ownedInstalledGeneration,
            std::string ownedSessionId,
            ContentHash ownedSessionManifestHash,
            std::vector<operator_runtime::RecoveredUncertainDispatch> ownedRecoveries
        )
            : loaded{std::move(ownedLoaded)}
            , deploymentIndex{ownedDeploymentIndex}
            , plugin{std::move(ownedPlugin)}
            , operatorHost{std::move(ownedOperatorHost)}
            , planAuthority{std::move(ownedPlanAuthority)}
            , controller{std::move(ownedController)}
            , generation{ownedGeneration}
            , access{ownedAccess}
            , runtimeModel{std::move(ownedRuntimeModel)}
            , installedGeneration{ownedInstalledGeneration}
            , sessionId{std::move(ownedSessionId)}
            , sessionManifestHash{ownedSessionManifestHash}
            , recoveries{std::move(ownedRecoveries)}
        {
        }

        Impl(Impl const&) = delete;
        auto operator=(Impl const&) -> Impl& = delete;
        Impl(Impl&&) = delete;
        auto operator=(Impl&&) -> Impl& = delete;

        ~Impl() noexcept
        {
            try
            {
                static_cast<void>(releaseControl());
            }
            catch (...)
            {
            }
        }

        [[nodiscard]] auto acquireControl() -> Status
        {
            UF_CHECK(!activeLease.has_value());
            UF_TRY_VALUE(lease, operatorHost.acquireLease(controller));
            activeLease.emplace(std::move(lease));
            return ok();
        }

        [[nodiscard]] auto releaseControl() -> Status
        {
            if (!activeLease.has_value())
            {
                return ok();
            }
            UF_TRY(operatorHost.releaseLease(*activeLease));
            activeLease.reset();
            access = LifecycleAccess::ReadOnly;
            return ok();
        }

        [[nodiscard]] auto controlLease() const
            -> operator_runtime::ControlLease const&
        {
            UF_CHECK(activeLease.has_value());
            return *activeLease;
        }

        [[nodiscard]] auto deployment() -> deployment::LoadedDeployment&
        {
            return loaded.deployments[deploymentIndex];
        }
    };

    ProductLifecycle::ProductLifecycle(std::unique_ptr<Impl> implementation)
        : m_impl{std::move(implementation)}
    {
    }

    ProductLifecycle::ProductLifecycle(ProductLifecycle&&) noexcept = default;

    ProductLifecycle::~ProductLifecycle() = default;

    auto lifecycleAccessAfterRestart(
        std::span<operator_runtime::RecoveredUncertainDispatch const> recoveries
    ) noexcept -> LifecycleAccess
    {
        return recoveries.empty()
            ? LifecycleAccess::Writable
            : LifecycleAccess::ReadOnly;
    }

    auto ProductLifecycle::start(ProductStart const& start)
        -> Result<ProductLifecycle>
    {
        UF_TRY_VALUE(
            loaded,
            deployment::loadProductionProject(start.projectDirectory, {})
        );
        auto const deploymentIterator = std::ranges::find(
            loaded.deployments,
            loaded.primaryDeployment,
            &deployment::LoadedDeployment::name
        );
        if (deploymentIterator == loaded.deployments.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "primary deployment disappeared after production validation"
            );
        }
        auto const deploymentIndex = static_cast<std::size_t>(
            std::distance(loaded.deployments.begin(), deploymentIterator)
        );
        auto& selected = loaded.deployments[deploymentIndex];

        UF_TRY_VALUE(rootHash, projectArtifactRootHash(loaded.runtimeArtifactRoot));

        // ProductLifecycle::start is the production construction site named by
        // the class declaration. Recovery completes inside open before the
        // returned Coordinator can publish any writable surface.
        UF_TRY_VALUE(
            coordinator,
            operator_runtime::OperatorCoordinator::open(start.runtimeDirectory)
        );
        UF_TRY_VALUE(recoveries, coordinator.recoveredUncertainDispatches());
        auto const access = lifecycleAccessAfterRestart(recoveries);

        UF_TRY_VALUE(
            installed,
            coordinator.openActiveInstalledRuntimeArtifact(rootHash)
        );
        auto const installedGeneration = installed.installedGeneration();
        UF_TRY_VALUE(
            operatorHost,
            operator_runtime::OperatorTaskHost::create(
                std::move(coordinator),
                start.controlledTargetId
            )
        );
        UF_TRY_VALUE(
            generation,
            operatorHost.host().activateRuntimeArtifact(std::move(installed))
        );
        UF_TRY_VALUE(binding, operatorHost.host().runtimeModelBinding(generation));

        auto registrar = operator_runtime::ProjectPluginRegistrar{};
        UF_TRY_VALUE(
            plugin,
            registrar.registerPlugin(
                selected.registration,
                selected.pluginEntryModule,
                selected.pluginModules,
                selected.projectResources,
                selected.schemaOwner
            )
        );

        UF_TRY_VALUE(operatorSchema, publishedSchema(k_operatorSchemaPath));
        UF_TRY_VALUE(operatorSchemaHash, hashOf(operatorSchema.exactBytes));
        auto policyBytes = loaded.policyArtifactBytes.value_or(
            operator_runtime::denyAllPolicyArtifact(operatorSchemaHash)
        );
        UF_TRY_VALUE(policyHash, hashOf(policyBytes));
        UF_TRY_VALUE(noAgentProfileHash, hashOf(k_noAgentProfile));

        UF_TRY_VALUE(
            sessionManifest,
            operator_runtime::SessionManifest::create(
                operator_runtime::SessionManifestSpec{
                    .runtimeModelArtifactRootHash = binding.artifactRootHash(),
                    .operatorProtocolSchemaHash   = operatorSchemaHash,
                    .projectRegistrationHash      = selected.registration.hash(),
                    .policyArtifactHash           = policyHash,
                    .agentProfileHash             = noAgentProfileHash,
                }
            )
        );
        UF_TRY_VALUE(
            planAuthority,
            operator_runtime::OperatorPlanAuthority::create(
                selected.registration,
                sessionManifest,
                binding,
                operatorSchema.exactBytes,
                policyBytes,
                deployment::readPlanProposal,
                deployment::readStepIntent
            )
        );
        auto& store = operatorHost.coordinator();
        UF_TRY(store.registerProject(selected.registration));
        UF_TRY_VALUE(
            projectInstanceKey,
            internalProjectInstanceKey(
                selected.registration.hash(),
                start.controlledTargetId
            )
        );
        UF_TRY(store.provisionProjectInstance(
            selected.registration,
            plugin,
            operator_runtime::ProjectInstanceBaseline{
                .projectInstanceKey  = projectInstanceKey,
                .eventId             = {},
                .sessionManifestHash = sessionManifest.hash(),
                .entry               = std::nullopt,
            }
        ));
        UF_TRY_VALUE(
            sessionId,
            internalSessionId(
                sessionManifest.hash(),
                start.authenticatedControllerId,
                start.controlledTargetId
            )
        );
        UF_TRY(store.pinSession(
            operator_runtime::SessionPin{
                .sessionId                 = sessionId,
                .authenticatedControllerId = start.authenticatedControllerId,
                .idempotencyNamespace      = start.authenticatedControllerId,
                .projectRegistrationHash   = selected.registration.hash(),
                .controllerCapabilities    = start.controllerCapabilities,
                .controlledTargetId        = start.controlledTargetId,
                .projectInstanceKey        = projectInstanceKey,
                .mode = access == LifecycleAccess::Writable
                    ? operator_runtime::SessionMode::Write
                    : operator_runtime::SessionMode::Read,
                .kind       = operator_runtime::ControllerKind::Human,
                .worldScope = start.worldScope,
            },
            sessionManifest,
            std::nullopt
        ));
        UF_TRY_VALUE(controller, store.bindController(sessionId));

        // Allocate and fully construct the RAII owner before taking control.
        // Once acquireControl succeeds, no fallible ownership transfer remains.
        auto implementation = std::make_unique<Impl>(
            std::move(loaded),
            deploymentIndex,
            std::move(plugin),
            std::move(operatorHost),
            std::move(planAuthority),
            std::move(controller),
            generation,
            access,
            binding,
            installedGeneration,
            std::move(sessionId),
            sessionManifest.hash(),
            std::move(recoveries)
        );
        UF_TRY(implementation->acquireControl());
        return ProductLifecycle{std::move(implementation)};
    }

    auto ProductLifecycle::access() const noexcept -> LifecycleAccess
    {
        return m_impl->access;
    }

    auto ProductLifecycle::identity() const -> ProductIdentity
    {
        auto const& deployed = m_impl->loaded.deployments[m_impl->deploymentIndex];
        return ProductIdentity{
            .projectDirectory     = m_impl->loaded.directory,
            .runtimeArtifactRoot  = m_impl->loaded.runtimeArtifactRoot,
            .deployment           = deployed.name,
            .pluginId             = m_impl->plugin.pluginId(),
            .registrationHash     = deployed.registration.hash(),
            .runtimeModel         = m_impl->runtimeModel,
            .installedGeneration  = m_impl->installedGeneration,
            .sessionId            = m_impl->sessionId,
            .sessionManifestHash  = m_impl->sessionManifestHash,
        };
    }

    auto ProductLifecycle::recoveries() const
        -> std::vector<operator_runtime::RecoveredUncertainDispatch>
    {
        return m_impl->recoveries;
    }

    auto ProductLifecycle::observe(task::TaskContext& context)
        -> Result<ProductObservation>
    {
        UF_TRY_VALUE(
            observation,
            m_impl->operatorHost.host().observe(m_impl->generation, context)
        );
        UF_TRY_VALUE(
            snapshot,
            m_impl->operatorHost.coordinator().createSnapshot(
                m_impl->controlLease(),
                m_impl->plugin,
                m_impl->deployment().toolCatalogSchemaOwner,
                m_impl->deployment().observedInstanceIdentitySchemas,
                observation
            )
        );
        return ProductObservation{
            .snapshot = std::move(snapshot),
            .ui       = std::move(observation),
        };
    }

    auto ProductLifecycle::invokeFrameworkReadOnlyTool(
        FrameworkReadOnlyToolCall request,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallReplay>
    {
        UF_TRY_VALUE(
            rootPreimage,
            operator_runtime::CanonicalJson::parseExact(
                std::move(request.exactRootRequestPreimageJcs)
            )
        );
        UF_TRY_VALUE(
            root,
            operator_runtime::ToolRootRequestIdentity::create(
                m_impl->controller.controllerId(),
                std::move(request.requestKey),
                std::move(rootPreimage)
            )
        );
        UF_TRY_VALUE(
            arguments,
            operator_runtime::CanonicalJson::parseExact(
                std::move(request.exactArgumentsJcs)
            )
        );
        UF_TRY_VALUE(catalog, operator_runtime::FrameworkToolCatalogOwner::create());
        UF_TRY_VALUE(
            invocation,
            catalog.validate(std::move(request.toolName), std::move(arguments))
        );
        UF_TRY_VALUE(
            call,
            operator_runtime::ToolCallPositionIdentity::create(
                root,
                std::nullopt,
                request.sequence,
                request.executionIdentity,
                invocation
            )
        );

        auto executor = operator_runtime::ToolRuntimeExecutor{
            m_impl->operatorHost.coordinator(),
        };
        auto provider = [this, &context](
                            operator_runtime::ToolCallPositionIdentity const&
                                admittedCall
                        ) -> Result<operator_runtime::ToolCallCompletion>
        {
            if (admittedCall.toolName() == "framework.screen.observe")
            {
                UF_TRY_VALUE(observed, observe(context));
                UF_TRY_VALUE(
                    stateResolution,
                    operator_runtime::CanonicalJson::parseExact(
                        observed.ui.canonicalJcs()
                    )
                );
                auto const& deployed = m_impl->deployment();
                return confirmedToolResult(json::Value::ofObject({
                    {"artifact_root_hash",
                     json::Value::ofString(
                         observed.ui.artifactRootHash().hex()
                     )},
                    {"controlled_target_id",
                     json::Value::ofString(
                         m_impl->controller.controlledTargetId()
                     )},
                    {"decision_basis_hash",
                     json::Value::ofString(
                         observed.snapshot.decisionBasisHash.hex()
                     )},
                    {"host_generation",
                     json::Value::ofString(std::to_string(
                         observed.ui.generation().value()
                     ))},
                    {"observation_id",
                     json::Value::ofString(observed.ui.observationId())},
                    {"project_registration_hash",
                     json::Value::ofString(deployed.registration.hash().hex())},
                    {"snapshot_identity_hash",
                     json::Value::ofString(
                         observed.snapshot.identityHash.hex()
                     )},
                    {"snapshot_ref",
                     json::Value::ofString(observed.snapshot.token)},
                    {"state_resolution", stateResolution.value()},
                    {"state_resolution_hash",
                     json::Value::ofString(
                         observed.ui.stateResolutionHash().hex()
                     )},
                    {"target_generation",
                     json::Value::ofString(std::to_string(
                         observed.ui.targetGeneration().value()
                     ))},
                }));
            }

            if (admittedCall.toolName() == "framework.workflow.wait")
            {
                UF_TRY_VALUE(
                    waitArguments,
                    operator_runtime::CanonicalJson::parseExact(
                        admittedCall.canonicalArgs()
                    )
                );
                auto const* const p_duration =
                    waitArguments.value().find("duration_ms");
                UF_CHECK(p_duration != nullptr);
                auto const durationMillis = static_cast<uint64>(
                    p_duration->number()
                );
                context.settle(std::chrono::milliseconds{durationMillis});
                if (context.cancellationRequested())
                {
                    return fail(
                        AutomationErrorKind::Cancelled,
                        "framework.workflow.wait was cancelled"
                    );
                }
                return confirmedToolResult(json::Value::ofObject({
                    {"completed", json::Value::ofBoolean(true)},
                    {"duration_ms",
                     json::Value::ofNumber(
                         static_cast<double>(durationMillis)
                     )},
                }));
            }

            return fail(
                AutomationErrorKind::InternalInvariant,
                "Framework Tool Catalog admitted a Tool with no provider"
            );
        };
        return executor.invokeReadOnly(
            m_impl->controller,
            m_impl->controlLease(),
            root,
            call,
            provider
        );
    }

    auto ProductLifecycle::execute(
        operator_runtime::SnapshotRecord const& snapshot,
        std::string toolName,
        std::string exactArgumentsJcs,
        std::string clientRequestId,
        task::TaskContext& context
    ) -> Result<ProductExecution>
    {
        if (m_impl->access != LifecycleAccess::Writable)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "recovery is unfinished, so this lifecycle is read-only"
            );
        }
        auto& deployed = m_impl->deployment();
        UF_TRY_VALUE(
            arguments,
            deployed.schemaOwner.canonicalize(std::move(exactArgumentsJcs))
        );
        UF_TRY_VALUE(
            invocation,
            deployed.toolCatalogSchemaOwner.validate(
                std::move(toolName),
                std::move(arguments)
            )
        );
        auto const mutability = invocation.descriptor().mutability;
        UF_TRY_VALUE(
            accepted,
            m_impl->operatorHost.coordinator().submitCommand(
                m_impl->controller,
                operator_runtime::CommandRequest{
                    .snapshotToken        = snapshot.token,
                    .idempotencyNamespace = m_impl->controller.controllerId(),
                    .clientRequestId      = std::move(clientRequestId),
                },
                invocation
            )
        );
        if (accepted.operation.lookup == operator_runtime::CommandLookup::Existing)
        {
            return ProductExecution{.operation = accepted.operation};
        }
        if (mutability == operator_runtime::ToolMutability::ReadOnly)
        {
            UF_TRY_VALUE(
                completed,
                m_impl->operatorHost.coordinator().transitionOperation(
                    accepted.operation.operationId,
                    accepted.operation.revision,
                    operator_runtime::OperationSignal::ReadCompleted
                )
            );
            return ProductExecution{.operation = std::move(completed)};
        }
        UF_TRY_VALUE(
            frozen,
            m_impl->operatorHost.coordinator().freezePlan(
                accepted.operation.operationId,
                accepted.operation.revision,
                m_impl->controlLease(),
                m_impl->plugin,
                deployed.toolCatalogSchemaOwner,
                m_impl->planAuthority
            )
        );
        if (frozen.operation.state == operator_runtime::OperationState::AwaitingApproval)
        {
            return ProductExecution{.operation = std::move(frozen.operation)};
        }
        UF_TRY_VALUE(
            step,
            m_impl->operatorHost.coordinator().mintNextStep(
                frozen.operation.operationId,
                frozen.operation.revision,
                m_impl->controlLease(),
                m_impl->plugin,
                deployed.toolCatalogSchemaOwner,
                m_impl->planAuthority
            )
        );
        if (step.kind == operator_runtime::StepKind::Wait)
        {
            return ProductExecution{.operation = std::move(step.operation)};
        }
        auto const authority = operator_runtime::AuthorityDecisionId{
            "authority-" + step.operation.operationId
        };
        UF_TRY_VALUE(
            dispatched,
            m_impl->operatorHost.dispatch(
                step.operation.operationId,
                step.operation.revision,
                m_impl->controlLease(),
                m_impl->generation,
                authority,
                std::nullopt,
                context
            )
        );
        return ProductExecution{
            .operation = std::move(dispatched.operation),
            .delivery  = std::move(dispatched.delivery),
        };
    }

    auto ProductLifecycle::wait(
        operator_runtime::SubscriptionCursor after,
        uint32 maximumEvents
    ) -> Result<operator_runtime::SubscriptionRead>
    {
        return m_impl->operatorHost.coordinator().subscribe(
            m_impl->controller,
            after,
            maximumEvents
        );
    }

    auto ProductLifecycle::reconcile(
        operator_runtime::ReconciliationCommit const& commit
    ) -> Result<operator_runtime::StoredOperation>
    {
        auto result = m_impl->operatorHost.coordinator().commitReconciliation(
            m_impl->plugin,
            commit
        );
        if (!result.has_value())
        {
            return std::unexpected{result.error().clone()};
        }
        std::erase_if(
            m_impl->recoveries,
            [&commit](operator_runtime::RecoveredUncertainDispatch const& recovery)
            {
                return recovery.operationId == commit.operationId;
            }
        );
        return result;
    }

    auto reclaimRuntimeArtifacts(std::filesystem::path const& runtimeDirectory)
        -> Result<operator_runtime::ReclaimedRuntimeArtifacts>
    {
        UF_TRY_VALUE(
            coordinator,
            operator_runtime::OperatorCoordinator::open(runtimeDirectory)
        );
        return coordinator.reclaimUnreferencedRuntimeArtifacts();
    }

    auto upgradeRuntimeArtifactAndPinSession(RuntimeUpgradeStart const& upgrade)
        -> Result<RuntimeUpgradeResult>
    {
        UF_TRY_VALUE(
            loaded,
            deployment::loadProductionProject(upgrade.projectDirectory, {})
        );
        auto const deploymentIterator = std::ranges::find(
            loaded.deployments,
            loaded.primaryDeployment,
            &deployment::LoadedDeployment::name
        );
        if (deploymentIterator == loaded.deployments.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "primary deployment disappeared after production validation"
            );
        }
        auto const deploymentIndex = static_cast<std::size_t>(
            std::distance(loaded.deployments.begin(), deploymentIterator)
        );
        auto& selected = loaded.deployments[deploymentIndex];

        UF_TRY_VALUE(
            coordinator,
            operator_runtime::OperatorCoordinator::open(upgrade.runtimeDirectory)
        );

        // The generation the install compare-and-swaps against.
        // activeRuntimeArtifactPin fails exactly when no release is active,
        // and that absence is the bootstrap case the schema spells as
        // generation 0 -- the same reading the ledger's own first-install
        // tests use.
        auto const active = coordinator.activeRuntimeArtifactPin();
        auto const expectedInstalledGeneration = (
            active ? active->installedGeneration : uint64{0}
        );

        auto registrar = operator_runtime::ProjectPluginRegistrar{};
        UF_TRY_VALUE(
            plugin,
            registrar.registerPlugin(
                selected.registration,
                selected.pluginEntryModule,
                selected.pluginModules,
                selected.projectResources,
                selected.schemaOwner
            )
        );

        UF_TRY_VALUE(operatorSchema, publishedSchema(k_operatorSchemaPath));
        UF_TRY_VALUE(operatorSchemaHash, hashOf(operatorSchema.exactBytes));
        auto policyBytes = loaded.policyArtifactBytes.value_or(
            operator_runtime::denyAllPolicyArtifact(operatorSchemaHash)
        );
        UF_TRY_VALUE(policyHash, hashOf(policyBytes));
        UF_TRY_VALUE(noAgentProfileHash, hashOf(k_noAgentProfile));

        UF_TRY_VALUE(
            sessionManifest,
            operator_runtime::SessionManifest::create(
                operator_runtime::SessionManifestSpec{
                    .runtimeModelArtifactRootHash = upgrade.artifactRootHash,
                    .operatorProtocolSchemaHash   = operatorSchemaHash,
                    .projectRegistrationHash      = selected.registration.hash(),
                    .policyArtifactHash           = policyHash,
                    .agentProfileHash             = noAgentProfileHash,
                }
            )
        );
        UF_TRY(coordinator.registerProject(selected.registration));
        UF_TRY_VALUE(
            projectInstanceKey,
            internalProjectInstanceKey(
                selected.registration.hash(),
                k_upgradeTargetId
            )
        );
        UF_TRY(coordinator.provisionProjectInstance(
            selected.registration,
            plugin,
            operator_runtime::ProjectInstanceBaseline{
                .projectInstanceKey  = projectInstanceKey,
                .eventId             = {},
                .sessionManifestHash = sessionManifest.hash(),
                .entry               = std::nullopt,
            }
        ));
        UF_TRY_VALUE(
            sessionId,
            internalSessionId(
                sessionManifest.hash(),
                k_upgradeControllerId,
                k_upgradeTargetId
            )
        );
        UF_TRY_VALUE(
            worldScope,
            operator_runtime::ObservedInstanceWorldScope::run(
                std::string{k_upgradeTargetId},
                1
            )
        );
        auto const installation = operator_runtime::RuntimeArtifactInstallRequest{
            .handoffRoot                 = upgrade.handoffRoot,
            .expectedReleaseManifestHash = upgrade.expectedReleaseManifestHash,
            .expectedInstalledGeneration = expectedInstalledGeneration,
        };
        auto const pin = operator_runtime::SessionPin{
            .sessionId                 = sessionId,
            .authenticatedControllerId = std::string{k_upgradeControllerId},
            .idempotencyNamespace      = std::string{k_upgradeControllerId},
            .projectRegistrationHash   = selected.registration.hash(),
            .controllerCapabilities    = upgrade.controllerCapabilities,
            .controlledTargetId        = std::string{k_upgradeTargetId},
            .projectInstanceKey        = projectInstanceKey,
            .mode                      = operator_runtime::SessionMode::Read,
            .kind                      = operator_runtime::ControllerKind::Human,
            .worldScope                = worldScope,
        };
        if (active)
        {
            UF_TRY(coordinator.upgradeRuntimeArtifactAndPinSession(
                installation,
                pin,
                sessionManifest,
                std::nullopt
            ));
        }
        else
        {
            // A root with no release is the bootstrap: there is no predecessor
            // for the ledger's refusal rollback to restore, so the first
            // install goes through the same two public doors the ledger's own
            // first-install tests use. A pin refusal leaves the install active
            // and no session; re-running the verb then takes the upgrade path.
            UF_TRY(coordinator.installRuntimeArtifact(installation));
            UF_TRY(coordinator.pinSession(pin, sessionManifest, std::nullopt));
        }
        UF_TRY_VALUE(installed, coordinator.activeRuntimeArtifactPin());
        return RuntimeUpgradeResult{
            .installedGeneration = installed.installedGeneration,
            .artifactRootHash    = installed.artifactRootHash,
            .sessionId           = std::move(sessionId),
        };
    }

    auto approveReleaseCapabilities(
        std::filesystem::path const& runtimeDirectory,
        operator_runtime::ReleaseCapabilityApproval const& approval
    ) -> Status
    {
        UF_TRY_VALUE(
            coordinator,
            operator_runtime::OperatorCoordinator::open(runtimeDirectory)
        );
        return coordinator.approveReleaseCapabilities(approval);
    }

    auto ProductLifecycle::shutdown() -> Status
    {
        return m_impl->releaseControl();
    }
}
