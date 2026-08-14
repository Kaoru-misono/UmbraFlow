#include "product-lifecycle.hpp"

#include <deployment/project-deployment.hpp>
#include <deployment/project-directory.hpp>

#include <operator/effective-plan.hpp>
#include <operator/manifest.hpp>
#include <operator/policy.hpp>
#include <operator/project-plugin.hpp>

#include <task/platform/confined-file.hpp>
#include <task/runtime-model-file.hpp>
#include <task/task-host.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace uf::service
{
    namespace
    {
        constexpr auto k_operatorSchemaPath = std::string_view{
            "schema/umbraflow-operator-v1.schema.json"
        };
        constexpr auto k_noAgentProfile = std::string_view{"null"};

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
    }

    struct ProductLifecycle::Impl final
    {
        deployment::LoadedProject loaded;
        std::size_t               deploymentIndex{};

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
        operator_runtime::ControlLease          lease;

        GenerationId              generation;
        LifecycleAccess           access{LifecycleAccess::ReadOnly};
        task::RuntimeModelBinding runtimeModel;
        uint64                    installedGeneration{};

        std::vector<operator_runtime::RecoveredUncertainDispatch> recoveries{};

        bool shutdown{};

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

    auto ProductLifecycle::operator=(ProductLifecycle&&) noexcept
        -> ProductLifecycle& = default;

    ProductLifecycle::~ProductLifecycle() = default;

    auto lifecycleAccessAfterRestart(
        std::span<operator_runtime::RecoveredUncertainDispatch const> recoveries
    ) noexcept -> LifecycleAccess
    {
        return recoveries.empty()
            ? LifecycleAccess::Writable
            : LifecycleAccess::ReadOnly;
    }

    auto offeredProductTools(
        operator_runtime::OperatorCoordinator& coordinator,
        operator_runtime::ControllerBinding const& controller,
        operator_runtime::ProjectToolCatalogSchemaOwner const& catalog
    ) -> Result<std::vector<operator_runtime::OfferedTool>>
    {
        return coordinator.availableTools(controller, catalog);
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
                selected.pluginBytes,
                selected.artifactBlobs,
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
                .kind = operator_runtime::ControllerKind::Human,
            },
            sessionManifest,
            std::nullopt
        ));
        UF_TRY_VALUE(controller, store.bindController(sessionId));
        UF_TRY_VALUE(lease, operatorHost.acquireLease(controller));
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

        auto implementation = std::make_unique<Impl>(Impl{
            .loaded              = std::move(loaded),
            .deploymentIndex     = deploymentIndex,
            .plugin              = std::move(plugin),
            .operatorHost        = std::move(operatorHost),
            .planAuthority       = std::move(planAuthority),
            .controller          = std::move(controller),
            .lease               = std::move(lease),
            .generation          = generation,
            .access              = access,
            .runtimeModel        = binding,
            .installedGeneration = installedGeneration,
            .recoveries          = std::move(recoveries),
            .shutdown            = false,
        });
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
            m_impl->lease,
            m_impl->plugin,
            m_impl->deployment().toolCatalogSchemaOwner,
            observation
            )
        );
        return ProductObservation{
            .snapshot = std::move(snapshot),
            .ui       = std::move(observation),
        };
    }

    auto ProductLifecycle::offeredTools()
        -> Result<std::vector<operator_runtime::OfferedTool>>
    {
        return offeredProductTools(
            m_impl->operatorHost.coordinator(),
            m_impl->controller,
            m_impl->deployment().toolCatalogSchemaOwner
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
                m_impl->lease,
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
                m_impl->lease,
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
                m_impl->lease,
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

    auto ProductLifecycle::shutdown() -> Status
    {
        if (m_impl->shutdown)
        {
            return ok();
        }
        UF_TRY(m_impl->operatorHost.coordinator().releaseLease(m_impl->lease));
        m_impl->shutdown = true;
        m_impl->access   = LifecycleAccess::ReadOnly;
        return ok();
    }
}
