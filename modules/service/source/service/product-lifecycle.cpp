#include "product-lifecycle.hpp"

#include <deployment/project-deployment.hpp>
#include <deployment/project-directory.hpp>

#include <operator/effective-plan.hpp>
#include <operator/manifest.hpp>
#include <operator/policy.hpp>
#include <operator/project-plugin.hpp>
#include <operator/snapshot-reference.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-executor.hpp>

#include <task/platform/confined-file.hpp>
#include <task/runtime-model-file.hpp>
#include <task/task-host.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <json/value.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

        // The Framework Tools this module answers. Every name is spelled once
        // here so the provider switch and the seam that reads an argument
        // cannot disagree about which Tool they are talking about.
        constexpr auto k_observeTool = std::string_view{"framework.screen.observe"};
        constexpr auto k_waitTool    = std::string_view{"framework.workflow.wait"};
        constexpr auto k_auditTool   = std::string_view{"framework.audit.record"};
        constexpr auto k_statusTool  = std::string_view{"framework.workflow.status"};
        constexpr auto k_semanticInputTool = std::string_view{
            "framework.input.semantic_target"
        };
        constexpr auto k_coordinateInputTool = std::string_view{
            "framework.input.coordinate"
        };

        // The member a Tool's canonical arguments carry exactly when the call
        // consumes one observation authority. Which Tools those are is the
        // catalog's statement and not this module's, so the seam reads the
        // member rather than listing the names again.
        constexpr auto k_observationArgument = std::string_view{
            "observation_reference"
        };

        // How long one minted observation authority may be presented for.
        // CALIBRATION: thirty seconds is a placeholder well above the time an
        // actor needs to interpret one observation and act on it, and well
        // below any run budget.
        constexpr auto k_observationAuthorityMillis = uint64{30'000};

        // The risk one Framework input effect is proposed at. It is at or below
        // every input descriptor's own maximumRisk, so what bounds an admission
        // is the policy the session pinned rather than a number this module
        // chose to be generous with.
        constexpr auto k_inputEffectRisk = operator_runtime::Risk::Low;

        // The empty project payload a Framework-owned effect carries. Framework
        // owns the effect type, so there is no project document to put here and
        // the member is present-and-empty rather than absent.
        constexpr auto k_frameworkEffectPayload = std::string_view{"{}"};

        [[nodiscard]]
        auto unixMillisNow() -> uint64
        {
            auto const since = std::chrono::system_clock::now().time_since_epoch();
            auto const millis =
                std::chrono::duration_cast<std::chrono::milliseconds>(since)
                    .count();
            return millis <= 0 ? uint64{0} : static_cast<uint64>(millis);
        }

        // Counters render as decimal strings for SnapshotObservationReference's
        // reason: RFC 8785 numbers are IEEE-754 doubles, so a generation or an
        // instant above 2^53 would round inside a durable Tool result.
        [[nodiscard]]
        auto counterMember(uint64 value) -> json::Value
        {
            return json::Value::ofString(std::to_string(value));
        }

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

        // What a native input that posted nothing records.
        //
        // proven_absent and not terminal_failure, and not an error either. A
        // returned error is classified `possible` for a mutating Tool, which
        // would set the target-wide mutation barrier for an input this module
        // can prove never reached a sink -- and only a reconciliation carrying
        // fresh Host evidence could lift it. Refusing on the observation's own
        // bounds, or stopping at a delivery boundary that was never crossed,
        // are both "the authorization was consumed and nothing was posted",
        // which is exactly what task::DeliveryOutcome::NotDelivered means and
        // exactly what proven_absent records.
        //
        // The evidence is mandatory and is the claim itself: no delivery ran,
        // so no input was posted. It is not a Host observation because there is
        // nothing for a Host to have observed.
        [[nodiscard]]
        auto absentInputResult(std::string_view verdict, std::string_view reason)
            -> Result<operator_runtime::ToolCallCompletion>
        {
            UF_TRY_VALUE(
                payload,
                operator_runtime::CanonicalJson::parseExact(
                    json::canonicalBytes(json::Value::ofObject({
                        {"delivered", json::Value::ofBoolean(false)},
                        {"reason", json::Value::ofString(std::string{reason})},
                        {"verdict", json::Value::ofString(std::string{verdict})},
                    }))
                )
            );
            UF_TRY_VALUE(
                evidence,
                operator_runtime::CanonicalJson::parseExact(
                    json::canonicalBytes(json::Value::ofObject({
                        {"host_delivery", json::Value::ofString("none")},
                        {"posted_inputs", counterMember(0U)},
                    }))
                )
            );
            return operator_runtime::ToolCallCompletion::provenAbsent(
                std::move(payload),
                std::move(evidence)
            );
        }

        // The verdict a delivery the Host refused before posting anything
        // records. It is not an ObservationRefusal: the observation authority
        // was resolved and spent, and what stopped the call is a refusal on the
        // delivery side of that boundary.
        constexpr auto k_refusedDeliveryVerdict = std::string_view{
            "host_delivery_refused"
        };

        // The verdict a bare-coordinate input records, and it is a decision
        // rather than a gap.
        //
        // The Host posts an input only against a Receipt, and a Receipt is the
        // Host's proof that the point it posts was MEASURED on the frame it
        // posts into: its surface, ui target, Binding, variant and proof
        // locator all come from the trusted resolver, and TaskHost::deliver
        // joins the ui target it names against the one the ledger reserved. A
        // bare coordinate has none of those. Minting it a Receipt with those
        // fields blank would be a proof of nothing, and it would turn that join
        // into a comparison of one empty string against another -- a check that
        // cannot fail, standing where the only check on aim is.
        //
        // So a bare coordinate posts nothing until it carries something a
        // Receipt can be about. What that is is the open question, and it is a
        // question about the Tool rather than about the Host: either the
        // descriptor gains the frame the point was measured on, or the point is
        // delivered under a distinct privileged authority that states plainly
        // that nothing measured it.
        constexpr auto k_unmeasuredInputVerdict = std::string_view{
            "coordinate_input_unmeasured"
        };

        [[nodiscard]]
        auto requiredStringArgument(
            json::Value const& arguments,
            std::string_view member
        ) -> Result<std::string>
        {
            auto const* const p_member = arguments.find(member);
            if (p_member == nullptr || p_member->kind() != json::ValueKind::String)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the Framework Tool Catalog admitted arguments without a "
                        + std::string{member} + " string"
                );
            }
            return std::string{p_member->string()};
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

        // One issuing context per Tool root request. Per R4 the call ordinal is
        // a monotone child index assigned exclusively by this seam, so the
        // calls one root issues have to advance one counter: a context built
        // per call would hand every call ordinal 1, and a second call would
        // then land on the first call's coordinate and inherit its recorded
        // outcome -- exactly the aliasing R4 forbids a caller from performing.
        //
        // The key is the root identity, which ToolRootRequestIdentity derives
        // from the caller namespace, the request key and the exact request
        // preimage. Two distinct root requests therefore never share a counter,
        // and a restart re-derives the same key with a fresh context that
        // numbers from 1 again, which is what makes the recorded outcomes
        // replay rather than re-execute.
        //
        // An entry is released only when this Impl is destroyed. Nothing
        // releases one earlier because this seam is never told a root is
        // finished: a request names its root and there is no termination
        // signal to seal the context on. The map is therefore bounded by the
        // number of distinct root requests one lifecycle serves.
        std::map<ContentHash, operator_runtime::ToolCallIssuingContext>
            issuingContexts{};

        // The run's observation authority: the only mint of an observation
        // reference and the only route from one back to a resolved observation.
        // It is run-scoped because every binding it records is a coordinate of
        // this run, so a reference that outlives this lifecycle is inert --
        // nothing else can resolve one.
        operator_runtime::SnapshotObservationAuthority observations{};

        // The reference values this run minted, kept because issuing a call
        // AGAINST an observation takes the reference itself and the authority
        // publishes no lookup. The authority still owns the spend: this is a
        // handle store and answers only "were these exact bytes minted here",
        // which is the one question that must be answered before a coordinate
        // exists at all.
        std::vector<operator_runtime::SnapshotObservationReference>
            mintedObservations{};

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

        // Assigns the coordinate of one root-positioned call under root,
        // opening that root's issuing context on first use. The context is
        // never handed out: it stays owned here and only the position it
        // minted leaves, so no caller can hold a counter or read the next
        // ordinal.
        [[nodiscard]]
        auto issueRootToolCall(
            operator_runtime::ToolRootRequestIdentity const& root,
            operator_runtime::ToolExecutionIdentity const& executionIdentity,
            operator_runtime::ValidatedToolInvocation const& invocation
        ) -> Result<operator_runtime::ToolCallPositionIdentity>
        {
            auto const opened = issuingContexts.find(root.identity());
            if (opened != issuingContexts.end())
            {
                return opened->second.issue(invocation);
            }
            auto const created = issuingContexts.emplace(
                root.identity(),
                operator_runtime::ToolCallIssuingContext::forRoot(
                    root,
                    executionIdentity
                )
            );
            return created.first->second.issue(invocation);
        }

        // The same assignment for a call that consumes one observation. The
        // reference is a call-scoped borrow; the coordinate copies its identity
        // and nothing is retained.
        [[nodiscard]]
        auto issueRootToolCallAgainstObservation(
            operator_runtime::ToolRootRequestIdentity const& root,
            operator_runtime::ToolExecutionIdentity const& executionIdentity,
            operator_runtime::ValidatedToolInvocation const& invocation,
            operator_runtime::SnapshotObservationReference const& observation
        ) -> Result<operator_runtime::ToolCallPositionIdentity>
        {
            auto const opened = issuingContexts.find(root.identity());
            if (opened != issuingContexts.end())
            {
                return opened->second.issueAgainstObservation(
                    invocation,
                    observation
                );
            }
            auto const created = issuingContexts.emplace(
                root.identity(),
                operator_runtime::ToolCallIssuingContext::forRoot(
                    root,
                    executionIdentity
                )
            );
            return created.first->second.issueAgainstObservation(
                invocation,
                observation
            );
        }

        // A non-owning observation of one reference this run minted, or
        // nullptr. It points into mintedObservations and stays valid until the
        // next mint; every caller here consumes it before returning.
        [[nodiscard]]
        auto findObservation(std::string_view exactReferenceJcs) const noexcept
            UF_LIFETIME_BOUND
            -> operator_runtime::SnapshotObservationReference const*
        {
            auto const found = std::ranges::find_if(
                mintedObservations,
                [exactReferenceJcs](
                    operator_runtime::SnapshotObservationReference const& minted
                ) { return minted.wire().bytes() == exactReferenceJcs; }
            );
            return found == mintedObservations.end() ? nullptr : &*found;
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

    auto ProductLifecycle::answerObserveTool(
        operator_runtime::ToolCallPositionIdentity const& call,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(observed, observe(context));
        UF_TRY_VALUE(
            stateResolution,
            operator_runtime::CanonicalJson::parseExact(
                observed.ui.canonicalJcs()
            )
        );
        auto const& deployed = m_impl->deployment();

        // The observation authority section 6 requires, bound to all six of
        // the things it names. Every binding is read from what this run holds
        // or from what the Host just resolved, and none of it is stated by a
        // caller: a consumer that could name the frame, the target or the
        // coordinate could present a reference against a world it never
        // observed.
        //
        // TODO(cpp-debt): localSemanticTargets is the RuntimeModel's declared
        // ui_target vocabulary rather than the targets THIS resolution
        // reported, because the StateResolution document publishes readings
        // only for declared Readers and this generation's models may declare
        // none. Narrowing it needs the resolution to publish the targets it
        // resolved; until it does, the reference is snapshot-scoped by its
        // frame identity and model-scoped by its target vocabulary.
        UF_TRY_VALUE(
            reference,
            m_impl->observations.mint(
                operator_runtime::SnapshotObservationSpec{
                    .controlledTargetId =
                        m_impl->controller.controlledTargetId(),
                    .runtimeArtifactRootHash = observed.ui.artifactRootHash(),
                    .projectRegistrationHash = deployed.registration.hash(),
                    .frameIdentityHash       = observed.snapshot.identityHash,
                    .hostGeneration          = observed.ui.generation().value(),
                    .rootIdentity            = call.rootIdentity(),
                    .issuingParentIdentity   = call.parentIdentity(),
                    .expiresAtUnixMillis =
                        unixMillisNow() + k_observationAuthorityMillis,
                    .localSemanticTargets =
                        m_impl->runtimeModel.declaredUi().uiTargets,
                    .authorizedUiActions =
                        m_impl->runtimeModel.declaredUi().actions,
                }
            )
        );
        auto const referenceWire = reference.wire().value();
        m_impl->mintedObservations.emplace_back(std::move(reference));

        return confirmedToolResult(json::Value::ofObject({
            {"artifact_root_hash",
             json::Value::ofString(observed.ui.artifactRootHash().hex())},
            {"controlled_target_id",
             json::Value::ofString(m_impl->controller.controlledTargetId())},
            {"decision_basis_hash",
             json::Value::ofString(observed.snapshot.decisionBasisHash.hex())},
            {"host_generation",
             counterMember(observed.ui.generation().value())},
            {"observation_id", json::Value::ofString(observed.ui.observationId())},
            {"observation_reference", referenceWire},
            {"project_registration_hash",
             json::Value::ofString(deployed.registration.hash().hex())},
            {"snapshot_identity_hash",
             json::Value::ofString(observed.snapshot.identityHash.hex())},
            {"snapshot_ref", json::Value::ofString(observed.snapshot.token)},
            {"state_resolution", stateResolution.value()},
            {"state_resolution_hash",
             json::Value::ofString(observed.ui.stateResolutionHash().hex())},
            {"target_generation",
             counterMember(observed.ui.targetGeneration().value())},
        }));
    }

    auto ProductLifecycle::answerStatusTool()
        -> Result<operator_runtime::ToolCallCompletion>
    {
        // Run and call-tree status: what this run is, and whether it may still
        // mutate. It observes no frame and delivers nothing, which is why its
        // descriptor admits neither an observation nor a dispatch.
        return confirmedToolResult(json::Value::ofObject({
            {"access",
             json::Value::ofString(
                 m_impl->access == LifecycleAccess::Writable
                     ? "writable"
                     : "read_only"
             )},
            {"controlled_target_id",
             json::Value::ofString(m_impl->controller.controlledTargetId())},
            {"installed_generation", counterMember(m_impl->installedGeneration)},
            {"session_id", json::Value::ofString(m_impl->sessionId)},
            {"unreconciled_dispatches",
             counterMember(static_cast<uint64>(m_impl->recoveries.size()))},
        }));
    }

    auto ProductLifecycle::answerSemanticInputTool(
        operator_runtime::ToolCallPositionIdentity const& call,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(
            arguments,
            operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
        );
        UF_TRY_VALUE(
            semanticTarget,
            requiredStringArgument(arguments.value(), "semantic_target")
        );
        UF_TRY_VALUE(
            uiAction,
            requiredStringArgument(arguments.value(), "ui_action")
        );
        auto const* const p_reference = arguments.value().find(
            k_observationArgument
        );
        UF_CHECK(p_reference != nullptr);

        // Everything except the two names the caller chose is read from this
        // run's own live authority. The Tool's argument schema declares exactly
        // three members, so there is no controlled target, registration,
        // artifact, generation or coordinate a caller could state here even if
        // it wanted to -- which is what makes "validated against the SAME
        // snapshot, Binding, plan, lease and fence" structural rather than
        // checked.
        auto const consumption = operator_runtime::SnapshotObservationConsumption{
            .exactReferenceJcs       = json::canonicalBytes(*p_reference),
            .controlledTargetId      = m_impl->controller.controlledTargetId(),
            .runtimeArtifactRootHash = m_impl->runtimeModel.artifactRootHash(),
            .projectRegistrationHash =
                m_impl->deployment().registration.hash(),
            .hostGeneration        = m_impl->generation.value(),
            .rootIdentity          = call.rootIdentity(),
            .issuingParentIdentity = call.parentIdentity(),
            .localSemanticTarget   = semanticTarget,
            .uiAction              = uiAction,
            .presentedAtUnixMillis = unixMillisNow(),
        };

        // The whole refusal matrix is answered once, before anything is spent,
        // and the verdict is recorded by name. A refusal spends nothing, so an
        // action refused on its bounds leaves the authority available to the
        // call that is entitled to it.
        if (auto const refusal = m_impl->observations.refuse(consumption))
        {
            return absentInputResult(
                operator_runtime::observationRefusalWireName(*refusal),
                operator_runtime::observationRefusalDiagnostic(*refusal)
            );
        }
        UF_TRY_VALUE(resolved, m_impl->observations.resolve(consumption));

        // The Host delivery seam. Nothing about what to deliver is stated
        // here: the target and the action are the ones the authority resolved,
        // the lease and the generation are this run's own, and the call is the
        // coordinate the Coordinator already crossed the dispatch boundary for.
        auto delivered = m_impl->operatorHost.deliverToolCallInput(
            call,
            m_impl->controlLease(),
            m_impl->generation,
            operator_runtime::OperatorTaskHost::ToolCallInputIntent{
                .uiTarget = resolved.localSemanticTarget(),
                .uiAction = resolved.uiAction(),
            },
            context
        );

        // An Err from the seam is a refusal that posted nothing, and that is a
        // property of the seam rather than an assumption made here: every
        // refusal ahead of the engine call -- a superseded lease, a call whose
        // row is no longer dispatching, a target or action this model does not
        // declare, a resolution that found no Binding, a Receipt the Host would
        // not mint -- returns before any input is authorized, and every failure
        // from the engine call onwards is reported inside the report instead.
        // So it is recorded as proven absence, for the reason absentInputResult
        // states: classifying it possible would set the target-wide mutation
        // barrier over an effect this run can prove never reached a sink.
        if (!delivered)
        {
            return absentInputResult(
                k_refusedDeliveryVerdict,
                delivered.error().message()
            );
        }

        // The classification is the ledger's. A provider that chose its own
        // would be choosing whether the world may be uncertain about its own
        // effect, and task::DeliveryOutcome is the only value that can prove
        // one absent.
        return operator_runtime::toolCallCompletionFor(*delivered);
    }

    auto ProductLifecycle::answerFrameworkTool(
        operator_runtime::ToolCallPositionIdentity const& call,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        auto const& toolName = call.toolName();
        if (toolName == k_observeTool)
        {
            return answerObserveTool(call, context);
        }
        if (toolName == k_statusTool)
        {
            return answerStatusTool();
        }
        if (toolName == k_semanticInputTool)
        {
            return answerSemanticInputTool(call, context);
        }
        if (toolName == k_waitTool)
        {
            UF_TRY_VALUE(
                waitArguments,
                operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
            );
            auto const* const p_duration =
                waitArguments.value().find("duration_ms");
            UF_CHECK(p_duration != nullptr);
            auto const durationMillis = static_cast<uint64>(p_duration->number());
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
                 json::Value::ofNumber(static_cast<double>(durationMillis))},
            }));
        }
        if (toolName == k_auditTool)
        {
            // The durable Tool call row IS the audit record: its canonical
            // arguments are the whole of what was recorded and its terminal
            // outcome is the whole of what happened to it. A second store
            // beside it would be a second answer to "what did this run
            // record", and only one of them would replay.
            UF_TRY_VALUE(
                auditArguments,
                operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
            );
            auto const* const p_record = auditArguments.value().find("record");
            UF_CHECK(p_record != nullptr);
            UF_TRY_VALUE(
                record,
                operator_runtime::CanonicalJson::parseExact(
                    json::canonicalBytes(*p_record)
                )
            );
            return confirmedToolResult(json::Value::ofObject({
                {"record_hash",
                 json::Value::ofString(record.contentHash().hex())},
                {"recorded", json::Value::ofBoolean(true)},
            }));
        }
        if (toolName == k_coordinateInputTool)
        {
            // Bare coordinates resolve nothing: there is no observation to
            // present and no semantic target to judge. What keeps them out of
            // an ordinary actor's hands is the descriptor's Privileged
            // surface, judged at admission against the controller profile, and
            // being a Framework Tool does not widen that.
            UF_TRY_VALUE(
                coordinateArguments,
                operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
            );
            UF_TRY_VALUE(
                action,
                requiredStringArgument(coordinateArguments.value(), "action")
            );
            // TODO(cpp-debt): a bare coordinate has no Receipt to present, so
            // it posts nothing; k_unmeasuredInputVerdict states why and what
            // the Tool would have to carry for that to change.
            return absentInputResult(
                k_unmeasuredInputVerdict,
                std::format(
                    "the bare-coordinate action {} on {} reached the delivery "
                    "boundary, and nothing measured the point it names on the "
                    "frame it would be posted into",
                    action,
                    m_impl->controller.controlledTargetId()
                )
            );
        }
        return fail(
            AutomationErrorKind::InternalInvariant,
            "Framework Tool Catalog admitted a Tool with no provider"
        );
    }

    auto ProductLifecycle::invokeFrameworkTool(
        FrameworkToolCall request,
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

        auto const mutating = invocation.descriptor().mutability
            == operator_runtime::ToolMutability::Mutating;
        if (mutating && m_impl->access != LifecycleAccess::Writable)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "recovery is unfinished, so this lifecycle is read-only"
            );
        }

        // A call whose canonical arguments carry an observation reference is
        // issued AGAINST the reference this run minted for those exact bytes.
        // Recognition is byte equality and nothing else, so caller-authored or
        // caller-edited observation JSON is refused here -- before a durable
        // coordinate exists for it -- rather than inside a provider that would
        // then have to explain a row nobody should have been able to open.
        auto const* const p_presented = invocation.canonicalArgs().value().find(
            k_observationArgument
        );
        auto issued = p_presented == nullptr
            ? m_impl->issueRootToolCall(
                  root,
                  request.executionIdentity,
                  invocation
              )
            : [this, &root, &request, &invocation, p_presented]()
                -> Result<operator_runtime::ToolCallPositionIdentity>
              {
                  auto const* const p_minted = m_impl->findObservation(
                      json::canonicalBytes(*p_presented)
                  );
                  if (p_minted == nullptr)
                  {
                      return fail(
                          AutomationErrorKind::InvalidResource,
                          std::string{
                              operator_runtime::observationRefusalDiagnostic(
                                  operator_runtime::ObservationRefusal::Unminted
                              )
                          }
                      );
                  }
                  return m_impl->issueRootToolCallAgainstObservation(
                      root,
                      request.executionIdentity,
                      invocation,
                      *p_minted
                  );
              }();
        UF_TRY_VALUE(call, std::move(issued));

        // What a mutating call proposes: one effect per bound its own
        // descriptor declares, scoped to the controlled target this run holds
        // the lease on, judged by the policy the session pinned. A read-only
        // call proposes none, and that is the whole of the difference between
        // the two admissions.
        auto mutation = std::optional<operator_runtime::ToolAdmissionRequest::Mutation>{};
        if (mutating)
        {
            auto effects = std::vector<operator_runtime::ProposedEffect>{};
            effects.reserve(invocation.descriptor().effectBounds.size());
            for (auto const& bound : invocation.descriptor().effectBounds)
            {
                effects.emplace_back(operator_runtime::ProposedEffect{
                    .namespacedType = bound.namespacedType,
                    .risk           = k_inputEffectRisk,
                    .scopeKind      = bound.scopeKind,
                    .scopeKey = m_impl->controller.controlledTargetId(),
                    .payloadSchemaHash    = bound.payloadSchemaHash,
                    .opaqueProjectPayload = std::string{k_frameworkEffectPayload},
                });
            }
            mutation.emplace(operator_runtime::ToolAdmissionRequest::Mutation{
                .planAuthority = m_impl->planAuthority,
                .effects   = std::move(effects),
                .approvals = {},
            });
        }

        auto executor = operator_runtime::ToolRuntimeExecutor{
            m_impl->operatorHost.coordinator(),
        };
        auto provider = [this, &context](
                            operator_runtime::ToolCallPositionIdentity const&
                                admittedCall
                        ) -> Result<operator_runtime::ToolCallCompletion>
        { return answerFrameworkTool(admittedCall, context); };

        // No delegation grant: these are the root-positioned calls this run's
        // own context issues, and a grant exists only for a child call under a
        // dispatching handler.
        return executor.invoke(
            operator_runtime::ToolAdmissionRequest{
                .controller = m_impl->controller,
                .lease      = m_impl->controlLease(),
                .root     = std::move(root),
                .call     = std::move(call),
                .mutation = std::move(mutation),
            },
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
