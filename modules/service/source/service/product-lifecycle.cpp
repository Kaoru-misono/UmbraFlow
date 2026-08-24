#include "product-lifecycle.hpp"

#include <deployment/project-deployment.hpp>
#include <deployment/project-directory.hpp>

#include <operator/agent-profile.hpp>
#include <operator/effective-plan.hpp>
#include <operator/manifest.hpp>
#include <operator/policy.hpp>
#include <operator/project-generation.hpp>
#include <operator/project-plugin.hpp>
#include <operator/project-tool-dispatch.hpp>
#include <operator/snapshot-reference.hpp>
#include <operator/tool-actor-adapters.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-executor.hpp>
#include <operator/tool-root-producer.hpp>

#include <script/scoped-tool-program.hpp>

#include <task/platform/confined-file.hpp>
#include <task/runtime-model-file.hpp>
#include <task/task-host.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
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
        // The one spelling of OP:`UnboundedCeiling`. It is compared against the
        // bytes rather than re-derived, because the schema already refused
        // every other string.
        constexpr auto k_unboundedCeilingMarker = std::string_view{"unbounded"};

        // What an AgentProfile refusal names as the bytes' origin. They arrive
        // on the start request rather than as a file this module opens --
        // whoever read them owns the path and names it in its own refusal --
        // and the one refusal verifyExact spells an origin into is the hash
        // mismatch, which cannot fire from here because the manifest it is
        // compared against was derived from these exact bytes. What can fire
        // from here is the profile's own schema and its non-zero-ceiling rule,
        // and neither names an origin.
        constexpr auto k_agentProfileOrigin = std::string_view{
            "the AgentProfile bytes this session was started with"
        };

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

        // How long one minted observation authority may be presented for.
        // CALIBRATION: thirty seconds is a placeholder well above the time an
        // actor needs to interpret one observation and act on it, and well
        // below any run budget.
        constexpr auto k_observationAuthorityMillis = uint64{30'000};

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

        // One ceiling out of an AgentBudget document the definition below has
        // already accepted. Present is settled by then, which is why it is not
        // re-tested here: the definition requires all five members, so a branch
        // for an absent one would be a branch nothing could reach.
        //
        // The definition admits two spellings of a ceiling and this reads both:
        // an integer, and the "unbounded" marker an Operator writes when it
        // grants a session that no ceiling binds. The marker is read into
        // k_unboundedBudget so that the rest of the system sees one number
        // shape; what it is NOT is an absence, a default, or a number a reader
        // has to recognise as magic in the bytes.
        //
        // What the definition does not settle is the top -- it bounds only the
        // minimum -- and a canonical JSON number is a double, so a stated
        // ceiling past what the ledger's own INTEGER column holds is the one
        // thing left to refuse.
        [[nodiscard]]
        auto budgetCeiling(
            json::Value const& budget,
            std::string_view member
        ) -> Result<uint64>
        {
            auto const* const p_stated = budget.find(member);
            UF_CHECK(p_stated != nullptr);

            if (p_stated->kind() == json::ValueKind::String)
            {
                UF_CHECK(p_stated->string() == k_unboundedCeilingMarker);
                return operator_runtime::k_unboundedBudget;
            }

            auto const ceiling = checkedIntegralCast<uint64>(p_stated->number());
            if (!ceiling || *ceiling > operator_runtime::k_unboundedBudget)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "AgentProfile states a {} past what this Operator "
                        "counts in",
                        member
                    )
                );
            }
            return *ceiling;
        }

        // The production reader of the AgentProfile bytes a SessionManifest
        // attests to. It is the callback AgentProfile::verifyExact names as a
        // trusted deployment one, and it reaches no plugin code and no business
        // VM: it compiles the published Operator protocol schema this module
        // already hashes into every SessionManifest and judges the bytes by
        // that document's own OP:`AgentBudget` definition.
        //
        // The definition is the whole reason a caller cannot widen its own
        // ceiling quietly. additionalProperties is closed and every ceiling is
        // required, so the five numbers this returns are exactly the five the
        // attested bytes state -- and changing one changes agent_profile_hash,
        // which changes session_manifest_hash, which changes every
        // decision_basis_hash the session goes on to compose.
        //
        // The returned callable owns its compiled schema, and the bytes that
        // schema was compiled from name the generated catalog's
        // process-lifetime static storage, so it borrows nothing from this
        // scope.
        [[nodiscard]]
        auto agentProfileValidator(
            framework_schema::FrameworkSchemaDocument const& operatorSchema
        ) -> Result<operator_runtime::AgentProfileValidator>
        {
            UF_TRY_VALUE(
                schema,
                json::Schema::compile(json::Schema::Document{
                    .label      = operatorSchema.relativePath,
                    .exactBytes = operatorSchema.exactBytes,
                })
            );
            return [schema](
                       std::string_view exactProfileJcs
                   ) -> Result<operator_runtime::AgentBudget>
            {
                UF_TRY_VALUE(document, json::parse(exactProfileJcs));
                UF_TRY(schema.validateDefinition("AgentBudget", document));

                UF_TRY_VALUE(
                    toolCalls,
                    budgetCeiling(document, "maximum_tool_calls")
                );
                UF_TRY_VALUE(
                    mutations,
                    budgetCeiling(document, "maximum_mutations")
                );
                UF_TRY_VALUE(
                    observations,
                    budgetCeiling(document, "maximum_observations")
                );
                UF_TRY_VALUE(
                    elapsed,
                    budgetCeiling(document, "maximum_elapsed_ms")
                );
                UF_TRY_VALUE(
                    riskUnits,
                    budgetCeiling(document, "maximum_risk_units")
                );
                return operator_runtime::AgentBudget{
                    .maximumToolCalls     = toolCalls,
                    .maximumMutations     = mutations,
                    .maximumObservations  = observations,
                    .maximumElapsedMillis = elapsed,
                    .maximumRiskUnits     = riskUnits,
                };
            };
        }

        // The Tool Runtime seam a release upgrade compiles its generation
        // against. An upgrade session holds no lease, no controller binding and
        // no observation authority, so it has nothing a Tool call could be
        // admitted under; it registers a generation only to prove the shipped
        // closure compiles under the environment the upgrade would run it in.
        //
        // It refuses rather than being absent, because a scoped program with no
        // Tool Runtime is a pure program wearing the wrong type. The refusal is
        // a value this seam always answers with and never a branch on which
        // generation is running.
        [[nodiscard]]
        auto quiescentToolRuntime() -> script::ToolRuntimeInvoke
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
                    "a release upgrade session dispatches no Tool call"
                );
            };
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

        // The registrar is NOT held. registerGeneration returns the handle by
        // value and the handle owns its own state through a shared_ptr, so
        // keeping the registrar alive anchors nothing: measured 2026-08-14, the
        // field was written once and never read, and findExact -- the only
        // thing its map serves -- has no production caller. Holding it also
        // gave Impl a std::map, whose move this standard library does not
        // declare noexcept, which was the sole reason two types here could
        // throw while being constructed.
        operator_runtime::OperatorTaskHost        operatorHost;
        operator_runtime::OperatorPolicyAuthority policyAuthority;
        static_assert(
            std::is_nothrow_move_constructible_v<
                operator_runtime::ControlLease
            >,
            "ControlLease must transfer into its RAII owner without failure"
        );
        std::optional<operator_runtime::ControlLease> activeLease{};

        // The three values start() cannot have before this object exists,
        // because each of them needs a stable address for this one.
        //
        // The dispatcher's Framework provider is a callable bound to THIS Impl,
        // and the compiled tool closure holds the dispatcher's seam for as long
        // as the generation lives -- so the generation cannot be registered
        // until the dispatcher exists, and the dispatcher cannot be built until
        // its provider has something to point at. Provisioning then needs the
        // registered generation's closure, and the session pin needs the
        // provisioned instance, so the controller binding is the last of the
        // four rather than the first.
        //
        // Each is written exactly once, by start(), before anything can reach
        // it: the accessors below refuse rather than answer for an unfinished
        // lifecycle, on the same terms as controlLease().
        std::optional<operator_runtime::ProjectGenerationHandle> loadedGeneration{};
        std::optional<operator_runtime::ProjectToolDispatcher>   toolDispatcher{};
        std::optional<operator_runtime::ToolStartCatalog>        startCatalog{};
        std::optional<operator_runtime::ControllerBinding>       boundController{};

        // One adapter per actor class, each with its own root-request contexts.
        // They are per-lifecycle rather than per-call because an actor's call
        // ordinals belong to the seam that issues them: a producer rebuilt per
        // call would hand every start of one root the ordinal 1.
        operator_runtime::AgentToolAdapter         agentAdapter{};
        operator_runtime::HumanToolAdapter         humanAdapter{};
        operator_runtime::ProjectAutomationAdapter automationAdapter{};

        GenerationId              generation;
        LifecycleAccess           access;
        task::RuntimeModelBinding runtimeModel;
        uint64                    installedGeneration;
        std::string               sessionId;
        ContentHash               sessionManifestHash;

        // What Tool Runtime protocol this incarnation implements, derived once
        // from this release's own bytes. It is not the Framework Tool catalog
        // hash, which is what it used to be: that hash covers Framework Tool
        // descriptors and nothing else, so the state vocabulary, the identity
        // preimages, the durable record and the canonical-form contract could
        // every one of them change without moving it, and the equality a resume
        // performs against the stored value was named for a property it could
        // not observe.
        ContentHash               toolRuntimeProtocolIdentity;

        // Operator admission of this run's starts at the top of a run: the one
        // producer that mints a root request identity from the authenticated
        // binding, assigns a root-positioned call's ordinal, and derives what a
        // mutating start proposes. It is the same producer every actor adapter
        // holds, so this lifecycle is one more caller of the funnel rather than
        // a second spelling of it.
        operator_runtime::ToolRootProducer rootProducer{};

        // The run's observation authority: the only mint of an observation
        // reference and the only route from one back to a resolved observation.
        // It is run-scoped because every binding it records is a coordinate of
        // this run, so a reference that outlives this lifecycle is inert --
        // nothing else can resolve one.
        operator_runtime::SnapshotObservationAuthority observations{};

        Impl(
            deployment::LoadedProject ownedLoaded,
            std::size_t ownedDeploymentIndex,
            operator_runtime::OperatorTaskHost ownedOperatorHost,
            operator_runtime::OperatorPolicyAuthority ownedPlanAuthority,
            GenerationId ownedGeneration,
            LifecycleAccess ownedAccess,
            task::RuntimeModelBinding ownedRuntimeModel,
            uint64 ownedInstalledGeneration,
            std::string ownedSessionId,
            ContentHash ownedSessionManifestHash,
            ContentHash ownedToolRuntimeProtocolIdentity
        )
            : loaded{std::move(ownedLoaded)}
            , deploymentIndex{ownedDeploymentIndex}
            , operatorHost{std::move(ownedOperatorHost)}
            , policyAuthority{std::move(ownedPlanAuthority)}
            , generation{ownedGeneration}
            , access{ownedAccess}
            , runtimeModel{std::move(ownedRuntimeModel)}
            , installedGeneration{ownedInstalledGeneration}
            , sessionId{std::move(ownedSessionId)}
            , sessionManifestHash{ownedSessionManifestHash}
            , toolRuntimeProtocolIdentity{ownedToolRuntimeProtocolIdentity}
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
            UF_TRY_VALUE(lease, operatorHost.acquireLease(controller()));
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

        [[nodiscard]] auto controller() const
            -> operator_runtime::ControllerBinding const&
        {
            UF_CHECK(boundController.has_value());
            return *boundController;
        }

        [[nodiscard]] auto generationHandle() const
            -> operator_runtime::ProjectGenerationHandle const&
        {
            UF_CHECK(loadedGeneration.has_value());
            return *loadedGeneration;
        }

        [[nodiscard]] auto dispatcher()
            -> operator_runtime::ProjectToolDispatcher&
        {
            UF_CHECK(toolDispatcher.has_value());
            return *toolDispatcher;
        }

        [[nodiscard]] auto catalog() const
            -> operator_runtime::ToolStartCatalog const&
        {
            UF_CHECK(startCatalog.has_value());
            return *startCatalog;
        }

        [[nodiscard]] auto deployment() -> deployment::LoadedDeployment&
        {
            return loaded.deployments[deploymentIndex];
        }

        // What names this run inside the Tool Runtime. Every hash is read from
        // what this lifecycle already holds -- the pinned session manifest, the
        // installed artifact, and the compiled generation's own scoped
        // environment -- so no actor can state any of it.
        [[nodiscard]] auto executionIdentity() const
            -> operator_runtime::ToolExecutionIdentity
        {
            return operator_runtime::ToolExecutionIdentity{
                .runIdentity                 = sessionManifestHash,
                .frameworkReleaseIdentity    = runtimeModel.artifactRootHash(),
                .toolRuntimeProtocolIdentity = toolRuntimeProtocolIdentity,
                .environmentIdentity         = generationHandle().environmentIdentity(),
            };
        }

        // The context of the call currently running under this lifecycle. A
        // ToolProvider is stored for as long as the compiled generation lives
        // and is handed nothing but a call coordinate, so it cannot carry the
        // TaskContext of the call it is answering; the Tool Runtime is
        // single-threaded by contract -- one scoped run is synchronous on the
        // thread that dispatched it -- so the running call's context is
        // unambiguous while it is set. It is a borrow of a caller's object for
        // exactly the extent of one call, established and withdrawn by
        // ActiveContext below and by nothing else.
        task::TaskContext* p_activeContext{};

        class ActiveContext final
        {
            Impl* m_owner;

        public:
            ActiveContext(Impl& owner, task::TaskContext& context) noexcept
                : m_owner{&owner}
            {
                UF_ASSERT(m_owner->p_activeContext == nullptr);
                m_owner->p_activeContext = &context;
            }

            ActiveContext(ActiveContext const&) = delete;
            auto operator=(ActiveContext const&) -> ActiveContext& = delete;
            ActiveContext(ActiveContext&&) = delete;
            auto operator=(ActiveContext&&) -> ActiveContext& = delete;

            ~ActiveContext() noexcept { m_owner->p_activeContext = nullptr; }
        };

        [[nodiscard]] auto activeContext() const -> task::TaskContext&
        {
            UF_CHECK(p_activeContext != nullptr);
            return *p_activeContext;
        }

        // The Framework provider surface. One function per Tool the Framework
        // answers, all reached from one dispatch below, and every one of them
        // handed nothing but the immutable call position the Coordinator
        // already crossed the durable dispatch boundary for.
        [[nodiscard]]
        auto observe(task::TaskContext& context) -> Result<ProductObservation>;

        [[nodiscard]]
        auto answerFrameworkTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        [[nodiscard]]
        auto answerObserveTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        [[nodiscard]]
        auto answerStatusTool()
            -> Result<operator_runtime::ToolCallCompletion>;

        // Section 6's input-authority boundary. It resolves the observation the
        // call was issued against -- spending it, once -- against this run's own
        // controlled target, Project registration, RuntimeArtifact, Host
        // generation and issuing coordinate, never against anything the
        // arguments state, and judges the named snapshot-local semantic target
        // and UI action on the observation's own bounds before delivery, then
        // posts it through the one Host delivery seam and records the
        // classification the ledger derived from what the Host reported.
        [[nodiscard]]
        auto answerSemanticInputTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        // The one place an admitted request becomes a durable answer,
        // whichever transport built it. A Tool this generation bound runs on
        // the dispatcher's scoped program; every other admitted name is a
        // Framework Tool this Impl answers itself.
        [[nodiscard]]
        auto runAdmitted(
            operator_runtime::ToolAdmissionRequest const& request,
            task::TaskContext& context
        ) -> Result<operator_runtime::ToolCallReplay>;
    };

    ProductLifecycle::ProductLifecycle(std::unique_ptr<Impl> implementation)
        : m_impl{std::move(implementation)}
    {
    }

    ProductLifecycle::ProductLifecycle(ProductLifecycle&&) noexcept = default;

    ProductLifecycle::~ProductLifecycle() = default;


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
        // A started lifecycle holds the lease it acquires below, so it starts
        // writable and becomes read-only only when it gives that lease up.
        auto const access = LifecycleAccess::Writable;

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

        UF_TRY_VALUE(operatorSchema, publishedSchema(k_operatorSchemaPath));
        UF_TRY_VALUE(operatorSchemaHash, hashOf(operatorSchema.exactBytes));
        UF_TRY_VALUE(
            policyBytes,
            operator_runtime::operatorPolicyArtifact(
                start.runtimeDirectory,
                operatorSchemaHash
            )
        );
        UF_TRY_VALUE(policyHash, hashOf(policyBytes));

        // The manifest attests to whatever profile bytes this start carries,
        // and to the absent-profile document when it carries none. The hash is
        // derived from the bytes rather than stated beside them, which is what
        // makes a widened ceiling change the session identity every later
        // decision is composed against.
        UF_TRY_VALUE(agentProfileHash, hashOf(start.agentProfileJcs));

        auto const project = operator_runtime::ProjectIdentity{selected.generation};
        UF_TRY_VALUE(
            sessionManifest,
            operator_runtime::SessionManifest::create(
                operator_runtime::SessionManifestSpec{
                    .runtimeModelArtifactRootHash = binding.artifactRootHash(),
                    .operatorProtocolSchemaHash   = operatorSchemaHash,
                    .projectRegistrationHash      = project.hash(),
                    .policyArtifactHash           = policyHash,
                    .agentProfileHash             = agentProfileHash,
                }
            )
        );

        // Verified against the manifest that just attested to it, before the
        // pin that needs it. One path for every controller kind: there is no
        // session without a declared budget, so there is nothing here to branch
        // on and nothing for the ledger to reconcile between a kind and a
        // presence.
        UF_TRY_VALUE(validate, agentProfileValidator(operatorSchema));
        UF_TRY_VALUE(
            agentProfile,
            operator_runtime::AgentProfile::verifyExact(
                sessionManifest,
                std::filesystem::path{k_agentProfileOrigin},
                start.agentProfileJcs,
                validate
            )
        );
        UF_TRY_VALUE(
            policyAuthority,
            operator_runtime::OperatorPolicyAuthority::create(
                project,
                sessionManifest,
                binding,
                operatorSchema.exactBytes,
                policyBytes
            )
        );
        UF_TRY_VALUE(
            sessionId,
            internalSessionId(
                sessionManifest.hash(),
                start.authenticatedControllerId,
                start.controlledTargetId
            )
        );

        // Derived here rather than at each call site: it is a fact about the
        // release, so computing it once per lifecycle is both cheaper and the
        // only shape in which a caller cannot supply one.
        UF_TRY_VALUE(
            toolRuntimeProtocolIdentity,
            deployment::currentToolRuntimeProtocolIdentity()
        );

        // The lifecycle is allocated before the generation it drives, because
        // the generation's Tool Runtime seam is this lifecycle's dispatcher and
        // the dispatcher's Framework provider is this lifecycle's own. See the
        // four late-bound members on Impl for why the order cannot be reversed.
        auto implementation = std::make_unique<Impl>(
            std::move(loaded),
            deploymentIndex,
            std::move(operatorHost),
            std::move(policyAuthority),
            generation,
            access,
            binding,
            installedGeneration,
            std::move(sessionId),
            sessionManifest.hash(),
            toolRuntimeProtocolIdentity
        );
        auto& deployed = implementation->deployment();

        // The provider is bound to the Impl rather than to any handle onto it.
        // Impl is heap-allocated and neither copyable nor movable, and the
        // dispatcher that stores this callable is a member of that same object,
        // so the pointer cannot outlive what it names.
        auto* const p_implementation = implementation.get();
        UF_TRY_VALUE(
            dispatcher,
            operator_runtime::ProjectToolDispatcher::create(
                implementation->operatorHost.coordinator(),
                implementation->observations,
                implementation->policyAuthority,
                [p_implementation](
                    operator_runtime::ToolCallPositionIdentity const& call
                ) -> Result<operator_runtime::ToolCallCompletion>
                { return p_implementation->answerFrameworkTool(call); }
            )
        );
        implementation->toolDispatcher.emplace(std::move(dispatcher));

        auto registrar = operator_runtime::ProjectGenerationRegistrar{};
        UF_TRY_VALUE(
            loadedGeneration,
            registrar.registerGeneration(
                deployed.generation,
                deployed.toolCatalogSchemaOwner,
                operator_runtime::ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = deployed.toolClosure.entryModule,
                    .modules     = deployed.toolClosure.modules,
                },
                deployed.projectResources,
                implementation->dispatcher().toolRuntimeSeam()
            )
        );
        implementation->loadedGeneration.emplace(std::move(loadedGeneration));
        UF_TRY_VALUE(
            startCatalog,
            operator_runtime::ToolStartCatalog::create(
                deployed.toolCatalogSchemaOwner
            )
        );
        implementation->startCatalog.emplace(std::move(startCatalog));

        auto& store = implementation->operatorHost.coordinator();
        UF_TRY(store.registerProject(project));
        UF_TRY_VALUE(
            projectInstanceKey,
            internalProjectInstanceKey(project.hash(), start.controlledTargetId)
        );
        UF_TRY(store.provisionProjectInstance(project, projectInstanceKey));
        UF_TRY(store.pinSession(
            operator_runtime::SessionPin{
                .sessionId                 = implementation->sessionId,
                .authenticatedControllerId = start.authenticatedControllerId,
                .idempotencyNamespace      = start.authenticatedControllerId,
                .projectRegistrationHash   = project.hash(),
                .controllerCapabilities    = start.controllerCapabilities,
                .controlledTargetId        = start.controlledTargetId,
                .projectInstanceKey        = projectInstanceKey,
                .mode = access == LifecycleAccess::Writable
                    ? operator_runtime::SessionMode::Write
                    : operator_runtime::SessionMode::Read,
                .kind       = start.kind,
                .worldScope = start.worldScope,
            },
            sessionManifest,
            agentProfile
        ));
        UF_TRY_VALUE(controller, store.bindController(implementation->sessionId));
        implementation->boundController.emplace(std::move(controller));

        // Once acquireControl succeeds, no fallible ownership transfer remains.
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
            .pluginId             = m_impl->generationHandle().pluginId(),
            .registrationHash     = m_impl->generationHandle().projectRegistrationHash(),
            .runtimeModel         = m_impl->runtimeModel,
            .installedGeneration  = m_impl->installedGeneration,
            .sessionId            = m_impl->sessionId,
            .sessionManifestHash  = m_impl->sessionManifestHash,
        };
    }


    auto ProductLifecycle::observe(task::TaskContext& context)
        -> Result<ProductObservation>
    {
        return m_impl->observe(context);
    }

    auto ProductLifecycle::Impl::observe(task::TaskContext& context)
        -> Result<ProductObservation>
    {
        UF_TRY_VALUE(
            observation,
            operatorHost.host().observe(generation, context)
        );
        UF_TRY_VALUE(
            snapshot,
            operatorHost.coordinator().createSnapshot(
                controlLease(),
                deployment().generation,
                deployment().toolCatalogSchemaOwner,
                deployment().observedInstanceIdentitySchemas,
                observation
            )
        );
        return ProductObservation{
            .snapshot = std::move(snapshot),
            .ui       = std::move(observation),
        };
    }

    auto ProductLifecycle::Impl::answerObserveTool(
        operator_runtime::ToolCallPositionIdentity const& call
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(observed, observe(activeContext()));
        UF_TRY_VALUE(
            stateResolution,
            operator_runtime::CanonicalJson::parseExact(
                observed.ui.canonicalJcs()
            )
        );

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
            observations.mint(
                operator_runtime::SnapshotObservationSpec{
                    .controlledTargetId =
                        controller().controlledTargetId(),
                    .runtimeArtifactRootHash = observed.ui.artifactRootHash(),
                    .projectRegistrationHash = generationHandle().projectRegistrationHash(),
                    .frameIdentityHash       = observed.snapshot.identityHash,
                    .hostGeneration          = observed.ui.generation().value(),
                    .rootIdentity            = call.rootIdentity(),
                    .issuingParentIdentity   = call.parentIdentity(),
                    .expiresAtUnixMillis =
                        unixMillisNow() + k_observationAuthorityMillis,
                    .localSemanticTargets =
                        runtimeModel.declaredUi().uiTargets,
                    .authorizedUiActions =
                        runtimeModel.declaredUi().actions,
                }
            )
        );
        return confirmedToolResult(json::Value::ofObject({
            {"artifact_root_hash",
             json::Value::ofString(observed.ui.artifactRootHash().hex())},
            {"controlled_target_id",
             json::Value::ofString(controller().controlledTargetId())},
            {"decision_basis_hash",
             json::Value::ofString(observed.snapshot.decisionBasisHash.hex())},
            {"host_generation",
             counterMember(observed.ui.generation().value())},
            {"observation_id", json::Value::ofString(observed.ui.observationId())},
            {std::string{operator_runtime::k_observationReferenceArgument},
             reference.wire().value()},
            {"project_registration_hash",
             json::Value::ofString(generationHandle().projectRegistrationHash().hex())},
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

    auto ProductLifecycle::Impl::answerStatusTool()
        -> Result<operator_runtime::ToolCallCompletion>
    {
        // Run and call-tree status: what this run is, and whether it may still
        // mutate. It observes no frame and delivers nothing, which is why its
        // descriptor admits neither an observation nor a dispatch.
        return confirmedToolResult(json::Value::ofObject({
            {"access",
             json::Value::ofString(
                 access == LifecycleAccess::Writable
                     ? "writable"
                     : "read_only"
             )},
            {"controlled_target_id",
             json::Value::ofString(controller().controlledTargetId())},
            {"installed_generation", counterMember(installedGeneration)},
            {"session_id", json::Value::ofString(sessionId)},
        }));
    }

    auto ProductLifecycle::Impl::answerSemanticInputTool(
        operator_runtime::ToolCallPositionIdentity const& call
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
            operator_runtime::k_observationReferenceArgument
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
            .controlledTargetId      = controller().controlledTargetId(),
            .runtimeArtifactRootHash = runtimeModel.artifactRootHash(),
            .projectRegistrationHash =
                generationHandle().projectRegistrationHash(),
            .hostGeneration        = generation.value(),
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
        if (auto const refusal = observations.refuse(consumption))
        {
            return absentInputResult(
                operator_runtime::observationRefusalWireName(*refusal),
                operator_runtime::observationRefusalDiagnostic(*refusal)
            );
        }
        UF_TRY_VALUE(resolved, observations.resolve(consumption));

        // The Host delivery seam. Nothing about what to deliver is stated
        // here: the target and the action are the ones the authority resolved,
        // the lease and the generation are this run's own, and the call is the
        // coordinate the Coordinator already crossed the dispatch boundary for.
        auto delivered = operatorHost.deliverToolCallInput(
            call,
            controlLease(),
            generation,
            operator_runtime::OperatorTaskHost::ToolCallInputIntent{
                .uiTarget = resolved.localSemanticTarget(),
                .uiAction = resolved.uiAction(),
            },
            activeContext()
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

    auto ProductLifecycle::Impl::answerFrameworkTool(
        operator_runtime::ToolCallPositionIdentity const& call
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        auto const& toolName = call.toolName();
        if (toolName == k_observeTool)
        {
            return answerObserveTool(call);
        }
        if (toolName == k_statusTool)
        {
            return answerStatusTool();
        }
        if (toolName == k_semanticInputTool)
        {
            return answerSemanticInputTool(call);
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
            activeContext().settle(std::chrono::milliseconds{durationMillis});
            if (activeContext().cancellationRequested())
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
                    controller().controlledTargetId()
                )
            );
        }
        // Impossible flow rather than a refusal. Every name that reaches here
        // was admitted, and admission validates a `framework.` name against
        // FrameworkToolCatalogOwner -- which declares exactly the names above --
        // while every Project name is refused registration unless it is bound to
        // an exported entry, so runAdmitted routes it to the dispatcher instead.
        // A recoverable failure here would be a check no test could make fail.
        UF_UNREACHABLE_MSG(
            "Framework Tool Catalog declared a Tool with no provider"
        );
    }

    auto ProductLifecycle::Impl::runAdmitted(
        operator_runtime::ToolAdmissionRequest const& request,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallReplay>
    {
        auto const active = ActiveContext{*this, context};

        // Which code answers a call is decided by the name's owner and by
        // nothing else. A Tool this generation bound runs on its own scoped
        // program through the dispatcher; every other admitted name belongs to
        // the Framework namespace, which this lifecycle answers itself. That is
        // two owners of two disjoint name spaces rather than two generations of
        // one job: no value here decides which generation a project is, and the
        // binding table cannot claim a `framework` name because the reader
        // refuses a registrant whose plugin_id falls inside that namespace.
        if (
            generationHandle()
                .bindingTable()
                .entryPointFor(request.call.toolName())
                .has_value()
        )
        {
            return dispatcher().dispatch(
                generationHandle(),
                request,
                context.cancellation()
            );
        }

        auto executor = operator_runtime::ToolRuntimeExecutor{
            operatorHost.coordinator(),
        };
        auto* const p_self = this;
        auto provider = [p_self](
                            operator_runtime::ToolCallPositionIdentity const&
                                admittedCall
                        ) -> Result<operator_runtime::ToolCallCompletion>
        { return p_self->answerFrameworkTool(admittedCall); };
        return executor.invoke(request, provider);
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

        if (
            invocation.descriptor().mutability
                == operator_runtime::ToolMutability::Mutating
            && m_impl->access != LifecycleAccess::Writable
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "this lifecycle has given up its control lease, so it is "
                "read-only"
            );
        }

        // The one producer of an actor's start at the top of a run. What this
        // module supplies is a translation of its own request envelope and
        // nothing else: the caller namespace, the call ordinal, the root
        // identity and what a mutating start proposes are all the producer's,
        // so this seam cannot state any of them differently from an actor
        // adapter.
        auto const start = operator_runtime::ToolRootStart{
            .controller      = m_impl->controller(),
            .lease           = m_impl->controlLease(),
            .execution       = request.executionIdentity,
            .policyAuthority   = m_impl->policyAuthority,
            .invocation      = invocation,
            .requestKey      = std::move(request.requestKey),
            .requestPreimage = std::move(rootPreimage),
        };

        // A call whose canonical arguments carry an observation reference is
        // issued AGAINST the reference this run minted for those exact bytes.
        // Recognition is the authority's and byte equality is the whole of it,
        // so caller-authored or caller-edited observation JSON is refused here
        // -- before a durable coordinate exists for it -- rather than inside a
        // provider that would then have to explain a row nobody should have
        // been able to open.
        UF_TRY_VALUE(
            presented,
            m_impl->observations.presented(invocation.canonicalArgs())
        );
        UF_TRY_VALUE(
            admission,
            presented.has_value()
                ? m_impl->rootProducer.startAgainstObservation(start, *presented)
                : m_impl->rootProducer.start(start)
        );
        return m_impl->runAdmitted(admission, context);
    }

    auto ProductLifecycle::invokeAgentTool(
        operator_runtime::AgentToolUse const& use,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallReplay>
    {
        auto const execution = m_impl->executionIdentity();
        auto const run       = operator_runtime::ToolActorRun{
                  .controller    = m_impl->controller(),
                  .lease         = m_impl->controlLease(),
                  .execution     = execution,
                  .policyAuthority = m_impl->policyAuthority,
                  .catalog       = m_impl->catalog(),
        };
        UF_TRY_VALUE(admission, m_impl->agentAdapter.translate(run, use));
        return m_impl->runAdmitted(admission, context);
    }

    auto ProductLifecycle::invokeHumanTool(
        operator_runtime::HumanToolCommand const& command,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallReplay>
    {
        auto const execution = m_impl->executionIdentity();
        auto const run       = operator_runtime::ToolActorRun{
                  .controller    = m_impl->controller(),
                  .lease         = m_impl->controlLease(),
                  .execution     = execution,
                  .policyAuthority = m_impl->policyAuthority,
                  .catalog       = m_impl->catalog(),
        };
        UF_TRY_VALUE(admission, m_impl->humanAdapter.translate(run, command));
        return m_impl->runAdmitted(admission, context);
    }

    auto ProductLifecycle::startProjectAutomation(
        operator_runtime::ProjectAutomationStart const& start,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallReplay>
    {
        auto const execution = m_impl->executionIdentity();
        auto const run       = operator_runtime::ToolActorRun{
                  .controller    = m_impl->controller(),
                  .lease         = m_impl->controlLease(),
                  .execution     = execution,
                  .policyAuthority = m_impl->policyAuthority,
                  .catalog       = m_impl->catalog(),
        };
        UF_TRY_VALUE(
            admission,
            m_impl->automationAdapter.translate(
                run,
                m_impl->generationHandle().bindingTable(),
                start
            )
        );
        return m_impl->runAdmitted(admission, context);
    }
    auto ProductLifecycle::wait(
        operator_runtime::SubscriptionCursor after,
        uint32 maximumEvents
    ) -> Result<operator_runtime::SubscriptionRead>
    {
        return m_impl->operatorHost.coordinator().subscribe(
            m_impl->controller(),
            after,
            maximumEvents
        );
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

        auto const project = operator_runtime::ProjectIdentity{selected.generation};
        auto registrar = operator_runtime::ProjectGenerationRegistrar{};
        UF_TRY_VALUE(
            generation,
            registrar.registerGeneration(
                selected.generation,
                selected.toolCatalogSchemaOwner,
                operator_runtime::ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = selected.toolClosure.entryModule,
                    .modules     = selected.toolClosure.modules,
                },
                selected.projectResources,
                quiescentToolRuntime()
            )
        );

        UF_TRY_VALUE(operatorSchema, publishedSchema(k_operatorSchemaPath));
        UF_TRY_VALUE(operatorSchemaHash, hashOf(operatorSchema.exactBytes));
        UF_TRY_VALUE(
            policyBytes,
            operator_runtime::operatorPolicyArtifact(
                upgrade.runtimeDirectory,
                operatorSchemaHash
            )
        );
        UF_TRY_VALUE(policyHash, hashOf(policyBytes));

        // The person who typed `umbra-flow upgrade` is the operator, and this
        // is the budget they declared by typing it: unbounded, in bytes, hashed
        // into the manifest this install is recorded under. An upgrade session
        // makes no Tool call at all -- its Tool Runtime refuses every one --
        // so what these ceilings bound is nothing; what they do is say so out
        // loud instead of leaving the grant unstated.
        UF_TRY_VALUE(
            upgradeProfileHash,
            hashOf(operator_runtime::k_unboundedAgentProfileJcs)
        );

        UF_TRY_VALUE(
            sessionManifest,
            operator_runtime::SessionManifest::create(
                operator_runtime::SessionManifestSpec{
                    .runtimeModelArtifactRootHash = upgrade.artifactRootHash,
                    .operatorProtocolSchemaHash   = operatorSchemaHash,
                    .projectRegistrationHash      = project.hash(),
                    .policyArtifactHash           = policyHash,
                    .agentProfileHash             = upgradeProfileHash,
                }
            )
        );
        UF_TRY(coordinator.registerProject(project));
        UF_TRY_VALUE(
            projectInstanceKey,
            internalProjectInstanceKey(
                project.hash(),
                k_upgradeTargetId
            )
        );
        UF_TRY(coordinator.provisionProjectInstance(project, projectInstanceKey));
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
            .projectRegistrationHash   = project.hash(),
            .controllerCapabilities    = upgrade.controllerCapabilities,
            .controlledTargetId        = std::string{k_upgradeTargetId},
            .projectInstanceKey        = projectInstanceKey,
            .mode                      = operator_runtime::SessionMode::Read,
            // A person at a terminal, and stated rather than derived from
            // anything a caller passed. `umbra-flow upgrade` is somebody
            // deciding that this release goes into this production root; there
            // is no actor flag on that verb and no principal behind it but the
            // operator running it. The kind is load-bearing even here, because
            // it is what makes the session that records the release one that
            // may stand behind an approval.
            .kind       = operator_runtime::ControllerKind::Human,
            .worldScope = worldScope,
        };
        UF_TRY_VALUE(validate, agentProfileValidator(operatorSchema));
        UF_TRY_VALUE(
            upgradeProfile,
            operator_runtime::AgentProfile::verifyExact(
                sessionManifest,
                std::filesystem::path{k_agentProfileOrigin},
                operator_runtime::k_unboundedAgentProfileJcs,
                validate
            )
        );
        if (active)
        {
            UF_TRY(coordinator.upgradeRuntimeArtifactAndPinSession(
                installation,
                pin,
                sessionManifest,
                upgradeProfile
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
            UF_TRY(coordinator.pinSession(pin, sessionManifest, upgradeProfile));
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
