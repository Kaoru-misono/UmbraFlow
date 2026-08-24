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

#include <task/exploration-session.hpp>
#include <task/pixel-probe.hpp>
#include <task/platform/confined-file.hpp>
#include <task/runtime-model-file.hpp>
#include <task/task-host.hpp>

#include <engine/session.hpp>

#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
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

        // What wrote an exploration session's trace stream. The stream's
        // session identity is the Operator's and arrives through the pinned
        // session; the producer is the only part of the header this module
        // names, and it is named here rather than taken from a caller because
        // an exploration session is one thing and there is one of it.
        constexpr auto k_explorationTraceProducer = std::string_view{"annotation"};

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
        constexpr auto k_deliverInputTool = std::string_view{
            "framework.input.deliver"
        };
        constexpr auto k_censusGridTool = std::string_view{
            "framework.screen.census_grid"
        };
        constexpr auto k_probeTool = std::string_view{
            "framework.screen.probe"
        };
        constexpr auto k_readLinesTool = std::string_view{
            "framework.screen.read_lines"
        };
        constexpr auto k_projectReadTool = std::string_view{
            "framework.project.read"
        };
        constexpr auto k_projectWriteTool = std::string_view{
            "framework.project.write"
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

        // The verdict a machine-aimed input records when the Framework refused
        // it BEFORE anything was captured or posted: a point outside the target
        // surface this registration declares, a key outside the closed set of
        // names, or a hold with no Tool call to lift it. Every one of them
        // precedes the capture, so every one of them is proven absence and the
        // reason names which it was.
        constexpr auto k_refusedInputVerdict = std::string_view{
            "input_refused"
        };

        // The verdict a machine-aimed input records when the delivery path was
        // entered and could not say what reached the target. It is
        // task::DeliveryOutcome::TransportUnknown's claim for a path that mints
        // no Receipt and therefore no HostDeliveryReport: EngineSession fails
        // before the sink, at the sink, and after the input has landed, and one
        // Result cannot separate the three, so every failure from the cycle
        // verb onwards is this and never proven absence.
        constexpr auto k_unknownInputTransportVerdict = std::string_view{
            "transport_unknown"
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

        // Every numeric member below was already judged by the Framework Tool
        // Catalog's own contract -- a surface pixel is a non-negative integer
        // inside uint32, a colour channel is 0..255, a tolerance is a
        // non-negative integer -- so this reads the admitted value rather than
        // re-judging it. A member the catalog did not admit cannot reach here,
        // which is why the absence is an internal invariant.
        [[nodiscard]]
        auto admittedNumber(json::Value const& arguments, std::string_view member)
            -> double
        {
            auto const* const p_member = arguments.find(member);
            UF_CHECK(p_member != nullptr);
            return p_member->number();
        }

        [[nodiscard]]
        auto admittedPixel(json::Value const& arguments, std::string_view member)
            -> uint32
        {
            return static_cast<uint32>(admittedNumber(arguments, member));
        }

        [[nodiscard]]
        auto admittedFlag(json::Value const& arguments, std::string_view member)
            -> bool
        {
            auto const* const p_member = arguments.find(member);
            UF_CHECK(p_member != nullptr);
            return p_member->boolean();
        }

        // The rectangle every measuring Tool names, on the frame its enclosing
        // observation is holding. PixelRect refuses a zero extent and an origin
        // that overflows, and that refusal is the Tool's answer rather than an
        // internal invariant: the catalog bounds each member on its own and
        // says nothing about the four together.
        [[nodiscard]]
        auto admittedRectangle(json::Value const& arguments) -> Result<PixelRect>
        {
            return PixelRect::create(
                admittedPixel(arguments, "x"),
                admittedPixel(arguments, "y"),
                admittedPixel(arguments, "width"),
                admittedPixel(arguments, "height")
            );
        }

        [[nodiscard]]
        auto admittedColourKey(json::Value const& arguments)
            -> task::ProbeColourKey
        {
            return task::ProbeColourKey{
                .red = static_cast<uint8>(
                    admittedNumber(arguments, "colour_red")
                ),
                .green = static_cast<uint8>(
                    admittedNumber(arguments, "colour_green")
                ),
                .blue = static_cast<uint8>(
                    admittedNumber(arguments, "colour_blue")
                ),
                .tolerance = static_cast<uint32>(
                    admittedNumber(arguments, "tolerance")
                ),
                .removes = admittedFlag(arguments, "removes"),
            };
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

        // Opens an observation frame and resolves the state on it, RETURNING
        // WITH THE FRAME STILL OPEN so everything measured or acted on next
        // reads the one frame. Both callers own the close: the Tool answer hands
        // it to the dispatcher, and the direct entry below closes it itself.
        [[nodiscard]]
        auto observe(task::TaskContext& context) -> Result<ProductObservation>;

        // What an observation with a BODY carries past the Tool provider
        // boundary. A provider is handed the immutable call position and
        // nothing else, so a body -- which is a closure inside the VM that
        // issued the observe, and which has to be numbered under this call's
        // own position -- travels beside that boundary rather than through it.
        //
        // Both members are borrows of the frame that started the call and
        // nothing is retained: `request` is the admitted request runAdmitted
        // holds for the whole dispatch, and `body` is consumed exactly once.
        struct ObservationBodyContext final
        {
            operator_runtime::ToolAdmissionRequest const& request;
            operator_runtime::ObservationBodyRun&         body;
        };

        [[nodiscard]]
        auto answerFrameworkTool(
            operator_runtime::ToolCallPositionIdentity const& call,
            ObservationBodyContext* p_bodyContext
        ) -> Result<operator_runtime::ToolCallCompletion>;

        [[nodiscard]]
        auto answerObserveTool(
            operator_runtime::ToolCallPositionIdentity const& call,
            ObservationBodyContext* p_bodyContext
        ) -> Result<operator_runtime::ToolCallCompletion>;

        // The three measuring Tools. Each binds BY CALL POSITION to the
        // innermost open observation frame and takes no frame handle at all, so
        // a call outside every frame is refused by name -- "no open observation
        // frame" -- rather than silently capturing one of its own, which is the
        // semantic drift the frame ruling exists to kill
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md
        // W3).
        [[nodiscard]]
        auto answerReadLinesTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        [[nodiscard]]
        auto answerProbeTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        [[nodiscard]]
        auto answerCensusGridTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        // The project's own authoring store, read and written through the
        // confined root the session was started against.
        [[nodiscard]]
        auto answerProjectReadTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        [[nodiscard]]
        auto answerProjectWriteTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        // The frame a measuring call binds to, or the refusal that names why
        // there is none.
        [[nodiscard]]
        auto measuringFrame(std::string_view toolName) -> Result<task::CycleTicket>;

        // Everything answerObserveTool does INSIDE the frame it opened. It is a
        // separate function so the frame's close is one unconditional line after
        // it rather than a line every early return has to remember, which is the
        // same reason the dispatcher's held-input release is not a scope guard.
        [[nodiscard]]
        auto observedToolResult(
            operator_runtime::ToolCallPositionIdentity const& call,
            task::TaskContext& context
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

        // The same delivery, aimed in machine terms. It posts into THE FRAME
        // ITS CALLER IS HOLDING and opens none of its own, so the point the
        // caller named is judged against the very frame it was measured on: the
        // engine's coordinate gate -- live fingerprint, lease validity,
        // target-instance revalidation, and the target-surface bound this run's
        // registration declared -- runs on that frame. A call outside every
        // frame IS its own frame and opens the one it posts into; what it never
        // does is capture a SECOND frame over one that is open, because aiming
        // at what one frame showed and posting into another is the drift an
        // observation frame exists to delete
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        //
        // It mints no observation reference and consumes none. A reference is
        // the authority to act on a target THE FRAMEWORK RESOLVED, and nothing
        // here resolved anything; what stands in its place is the Privileged
        // surface and the Operator's own privileged_surface_tools grant, which
        // is the authority saying plainly that nothing measured this point.
        [[nodiscard]]
        auto answerDeliverInputTool(
            operator_runtime::ToolCallPositionIdentity const& call
        ) -> Result<operator_runtime::ToolCallCompletion>;

        // The one place an admitted request becomes a durable answer,
        // whichever transport built it. A Tool this generation bound runs on
        // the dispatcher's scoped program; every other admitted name is a
        // Framework Tool this Impl answers itself.
        [[nodiscard]]
        auto runAdmitted(
            operator_runtime::ToolAdmissionRequest const& request,
            task::TaskContext& context,
            operator_runtime::ObservationBodyRun body
        ) -> Result<operator_runtime::ToolCallReplay>;

        // One top-of-run Framework call, admitted. It is factored out of
        // invokeFrameworkTool because the exploration seam needs the ADMITTED
        // COORDINATE as well as the outcome -- a chunk's answer names the
        // durable position its call occupied -- and a second translation of one
        // request envelope would be a second place a caller namespace or a call
        // ordinal could be stated differently.
        [[nodiscard]]
        auto admitFrameworkCall(FrameworkToolCall& request)
            -> Result<operator_runtime::ToolAdmissionRequest>;

        // One Tool call issued from an exploration chunk.
        //
        // Two routes and one rule deciding between them: a call written inside
        // an observation's body is a CHILD of that observation and goes through
        // the dispatcher's own child seam, and every other call is issued at the
        // top of this session's run. Neither is chosen by the chunk -- the chunk
        // names a Tool and an argument value, and where the call lands is
        // decided by whether a body is open.
        [[nodiscard]]
        auto issueExplorationCall(
            task::TaskContext& context,
            std::string_view toolName,
            json::Value const& arguments,
            task::ExplorationCallBody body
        ) -> Result<json::Value>;

        // The observation whose body is running, and how many children it has
        // issued so far. Both are empty outside a body; the VM is
        // single-threaded and a nested observation is refused by name, so at
        // most one body is ever open.
        std::optional<ContentHash> explorationBodyCall{};
        uint64                     explorationBodyChildren{};

        // How many top-of-run calls this session's chunks have issued. It names
        // each one's ROOT REQUEST, so every top-level exploration call is its
        // own root -- exactly as every CLI verb's single call is.
        //
        // ONE ROOT PER CALL RATHER THAN ONE PER SESSION, and the difference is
        // load-bearing. Positions under one root are a sequence, and admission
        // refuses a position whose predecessor has no terminal outcome; a call
        // refused AT admission never reaches one, so a single refused input
        // would leave every later call in that session refused for a reason
        // that has nothing to do with it. A human annotating writes independent
        // acts, and a refusal has to cost exactly the act it refused.
        uint64 explorationRequests{};

        static constexpr auto k_explorationRequestKeyPrefix = std::string_view{
            "exploration-"
        };
        static constexpr auto k_explorationRootPreimageJcs = std::string_view{
            R"({"objective":"exploration"})"
        };
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
                { return p_implementation->answerFrameworkTool(call, nullptr); }
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


    auto ProductLifecycle::startExplorationSession(
        task::TaskRunConfig config,
        std::stop_token cancellation
    ) -> Result<std::unique_ptr<task::ExplorationSession>>
    {
        auto const pinned = identity();

        // Declared before the engine session and the exploration session that
        // borrow it, and heap-allocated, because both hold its address for
        // their whole lives.
        //
        // Its stream identity is the OPERATOR's -- the session row start()
        // pinned and the SessionManifest that row was admitted against -- and
        // that is the whole of what "one door" buys the annotation trace. A
        // session that derived a stream name for itself named a session the
        // ledger had never heard of.
        UF_TRY_VALUE(sink, trace::FileTraceSink::createNew(config.tracePath));
        UF_TRY_VALUE(
            opened,
            trace::TraceRecorder::create(
                std::move(sink),
                trace::TraceStreamSpec{
                    .sessionId           = pinned.sessionId,
                    .sessionManifestHash = pinned.sessionManifestHash,
                    .producer            = std::string{k_explorationTraceProducer},
                }
            )
        );
        auto recorder = std::make_unique<trace::TraceRecorder>(std::move(opened));

        // projectFingerprint is the PINNED model's and never the live target's.
        // An exploration session used to attest its own geometry by handing the
        // live fingerprint to both members, which made the engine's
        // compatibility gate compare a value against itself. A session that
        // pins H_genesis therefore observes and crops a target of any size and
        // is refused a template search or a delivered input on it, naming the
        // mismatch -- which is the empty model being enforced faithfully rather
        // than a session pretending to have authored the screen in front of it.
        UF_TRY_VALUE(
            session,
            engine::EngineSession::create(
                std::move(config.frameSource),
                std::move(config.actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = config.liveFingerprint,
                    .projectFingerprint      = pinned.runtimeModel.fingerprint(),
                    .maximumPixelComparisons = config.maximumPixelComparisons,
                    .recognitionTimeout      = config.recognitionTimeout,
                    .maxActionFrameAge       = config.maxActionFrameAge,
                    .cancellation            = cancellation,
                },
                std::move(config.ocrEngine)
            )
        );

        // The seam every verb an exploration chunk writes goes through. It is
        // bound to the Impl rather than to this handle, on the dispatcher's
        // provider's terms: Impl is heap-allocated and neither copyable nor
        // movable, and the session that stores this callable must not outlive
        // this lifecycle -- which startExplorationSession's contract already
        // states, because the TaskContext it hands back is bound to the same
        // object.
        auto* const p_implementation = m_impl.get();
        auto toolRuntime = task::ExplorationToolInvoke{
            [p_implementation](
                task::TaskContext& callContext,
                std::string_view toolName,
                json::Value const& arguments,
                task::ExplorationCallBody body
            ) -> Result<json::Value>
            {
                return p_implementation->issueExplorationCall(
                    callContext,
                    toolName,
                    arguments,
                    std::move(body)
                );
            }
        };

        return m_impl->operatorHost.host().startExplorationSession(
            m_impl->generation,
            std::move(recorder),
            std::move(session),
            task::ExplorationSessionSpec{
                // The deployment this project declares, rather than the name of
                // the directory it happens to sit in: the trace line naming a
                // project has to survive the project being moved. The
                // registration hash would be the stronger name and cannot be
                // used -- a bare 64-character hash is what the trace refuses as
                // payload text, and the manifest hash on the stream header is
                // already the identity a reader joins on.
                .projectId            = pinned.deployment,
                .projectRoot          = pinned.projectDirectory,
                .tracePath            = std::move(config.tracePath),
                .toolRuntime          = std::move(toolRuntime),
                .cancellation         = std::move(cancellation),
                .maximumReadsPerCycle = config.maximumReadsPerCycle,
                .maximumCropsPerCycle = config.maximumCropsPerCycle,
                .memoryQuotaBytes     = config.memoryQuotaBytes,
                .maxScriptRuntime     = config.maxScriptRuntime,
            }
        );
    }

    auto ProductLifecycle::observe(task::TaskContext& context)
        -> Result<ProductObservation>
    {
        // The direct entry: one observation, taken and finished here. It is a
        // frame with an empty body -- open, resolve, close in one call -- which
        // is the degenerate case of the one rule rather than a second one, and
        // the close is a scope exit for the reason every other close is.
        auto const release = scopeExit(
            [this, &context]() noexcept
            {
                static_cast<void>(
                    m_impl->operatorHost.host().disengageObservationFrame(context)
                );
            }
        );
        return m_impl->observe(context);
    }

    auto ProductLifecycle::Impl::observe(task::TaskContext& context)
        -> Result<ProductObservation>
    {
        UF_TRY_VALUE(
            observation,
            operatorHost.host().engageObservationFrame(generation, context)
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
        operator_runtime::ToolCallPositionIdentity const& call,
        ObservationBodyContext* p_bodyContext
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        // AN OBSERVE WITH NO BODY IS A FRAME WITH AN EMPTY BODY: open, resolve,
        // close inside this one call
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        // With a body it is open, resolve, RUN THE BODY, close -- the same call
        // and the same scope, differing only in whether anything is written
        // inside it. Nothing about the ownership moves either way.
        //
        // The frame is asked for BEFORE it is taken, so a second concurrent one
        // costs no capture and earns a refusal that names what it refused,
        // rather than arriving as the Host's one-cycle invariant. Opening first
        // and asking afterwards would leave a frame nothing owns every time the
        // answer is no, which is attachHeldInput's reason exactly.
        auto* const p_context = &activeContext();
        UF_TRY(dispatcher().engageObservationFrame(
            call.identity(),
            // The close borrows the context, and the backing owner is the
            // admitted request this call runs inside: runAdmitted holds the
            // TaskContext for the whole dispatch, every scope this frame can
            // reach exits inside that dispatch, and the Tool Runtime is
            // single-threaded by contract. Nothing here outlives that.
            [p_host = &operatorHost.host(), p_context]() -> Status
            {
                static_cast<void>(p_host->disengageObservationFrame(*p_context));
                return ok();
            }
        ));

        auto answered = observedToolResult(call, *p_context);

        // THE BODY, INSIDE THE FRAME AND INSIDE THIS CALL'S OWN DISPATCH. Both
        // halves matter: the measuring calls it makes bind by position to the
        // frame opened above, and a delegation grant can only be minted from a
        // parent whose durable row is still dispatching -- which this one is,
        // because a provider runs between the dispatch boundary and the
        // terminal write.
        //
        // It runs only after the state resolved, so a body never measures a
        // frame the resolution failed on.
        if (answered.has_value() && p_bodyContext != nullptr)
        {
            // The two counters the exploration seam reads to decide that a call
            // is this observation's child, saved and restored around the body
            // rather than assigned: they are the frame's, and the frame is this
            // call's scope. The lambda is consumed inside this same call --
            // runObservationBody runs it synchronously and returns -- so the
            // pointer it carries names an object that strictly outlives it.
            auto* const p_self = this;
            auto ran = dispatcher().runObservationBody(
                generationHandle(),
                p_bodyContext->request.controller,
                p_bodyContext->request.lease,
                p_bodyContext->request.root,
                call,
                [p_self, frameCall = call.identity(),
                 inner = std::move(p_bodyContext->body)]() mutable -> Status
                {
                    auto const outerCall = std::exchange(
                        p_self->explorationBodyCall,
                        std::optional{frameCall}
                    );
                    auto const outerChildren = std::exchange(
                        p_self->explorationBodyChildren,
                        uint64{0}
                    );
                    auto ranBody = inner();
                    p_self->explorationBodyCall     = outerCall;
                    p_self->explorationBodyChildren = outerChildren;
                    return ranBody;
                }
            );
            if (!ran)
            {
                answered = std::unexpected{std::move(ran).error()};
            }
        }

        // Unconditional, and before the answer is looked at. The engage above
        // refused if anything was already open, so what closes here can only be
        // this call's own frame -- including on the path where the observation
        // itself failed and the frame it would have owned never opened.
        auto closed = dispatcher().closeObservationFrameOpenedInside(std::nullopt);
        if (!answered)
        {
            auto error = std::move(answered).error();
            if (!closed)
            {
                error.addContext(
                    "closing the observation frame this call opened also "
                    "failed: "
                        + std::string{closed.error().message()}
                );
            }
            return std::unexpected{std::move(error)};
        }
        if (!closed)
        {
            return std::unexpected{std::move(closed).error()};
        }
        return answered;
    }

    auto ProductLifecycle::Impl::observedToolResult(
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

    auto ProductLifecycle::Impl::measuringFrame(std::string_view toolName)
        -> Result<task::CycleTicket>
    {
        auto const held = activeContext().openObservationFrame();
        if (!held.has_value())
        {
            // The specific signal W3 requires, and the reason a measuring Tool
            // takes no frame handle: a verb that captured a frame of its own
            // here would answer about a screen nobody measured against, which
            // is exactly the drift an observation frame exists to delete.
            return fail(
                AutomationErrorKind::ActionRejected,
                std::string{toolName}
                    + " measures the frame its enclosing observation is "
                      "holding, and there is no open observation frame; it is "
                      "callable only inside the body of "
                    + std::string{k_observeTool}
            );
        }
        return *held;
    }

    auto ProductLifecycle::Impl::answerReadLinesTool(
        operator_runtime::ToolCallPositionIdentity const& call
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(
            arguments,
            operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
        );
        UF_TRY_VALUE(rect, admittedRectangle(arguments.value()));
        UF_TRY_VALUE(ticket, measuringFrame(k_readLinesTool));

        // Block layout and never the caller's choice: this Tool exists for the
        // region nobody can draw a rectangle inside, so a layout argument would
        // offer the caller the one answer it came here to avoid.
        UF_TRY_VALUE(
            lines,
            activeContext().cycleRead(ticket, rect, ocr::TextLayout::Block)
        );

        auto rendered = std::vector<json::Value>{};
        rendered.reserve(lines.size());
        for (auto const& line : lines)
        {
            rendered.emplace_back(json::Value::ofObject({
                {"confidence",
                 json::Value::ofNumber(
                     static_cast<double>(line.confidenceBp) / 10'000.0
                 )},
                {"height",
                 json::Value::ofNumber(static_cast<double>(line.rect.height()))},
                {"text", json::Value::ofString(line.text)},
                {"width",
                 json::Value::ofNumber(static_cast<double>(line.rect.width()))},
                {"x", json::Value::ofNumber(static_cast<double>(line.rect.x()))},
                {"y", json::Value::ofNumber(static_cast<double>(line.rect.y()))},
            }));
        }
        return confirmedToolResult(json::Value::ofObject({
            {"lines", json::Value::ofArray(std::move(rendered))},
        }));
    }

    auto ProductLifecycle::Impl::answerProbeTool(
        operator_runtime::ToolCallPositionIdentity const& call
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(
            arguments,
            operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
        );
        UF_TRY_VALUE(rect, admittedRectangle(arguments.value()));
        auto const key = admittedColourKey(arguments.value());
        UF_TRY_VALUE(ticket, measuringFrame(k_probeTool));

        // The crop is taken and then measured, and the PNG is DROPPED here: a
        // measurement crosses the Tool boundary and a frame's pixels never do.
        // Keeping a piece of the screen is framework.project.write's capture
        // arm, which is an authoring write and is judged as one.
        UF_TRY_VALUE(
            cropped,
            activeContext().cycleCrop(ticket, rect, std::nullopt)
        );
        UF_TRY_VALUE(
            report,
            task::probePngRegion(
                cropped.png,
                *PixelRect::create(0U, 0U, rect.width(), rect.height()),
                key
            )
        );

        auto members = std::vector<json::Member>{
            {"distinct_colours",
             json::Value::ofNumber(static_cast<double>(report.distinctColours))},
            {"dominant_blue",
             json::Value::ofNumber(static_cast<double>(report.dominantBlue))},
            {"dominant_green",
             json::Value::ofNumber(static_cast<double>(report.dominantGreen))},
            {"dominant_pixels",
             json::Value::ofNumber(static_cast<double>(report.dominantPixels))},
            {"dominant_red",
             json::Value::ofNumber(static_cast<double>(report.dominantRed))},
            {"rect_pixels",
             json::Value::ofNumber(static_cast<double>(report.rectPixels))},
        };
        // PixelProbeReport engages the three selection counts together or not
        // at all, so this asks for exactly what it then reads rather than
        // reading two of them on the strength of the first.
        if (
            report.fullySelectedPixels.has_value()
            && report.rampSelectedPixels.has_value()
            && report.selectedWeight.has_value()
        )
        {
            members.emplace_back(
                "fully_selected_pixels",
                json::Value::ofNumber(
                    static_cast<double>(*report.fullySelectedPixels)
                )
            );
            members.emplace_back(
                "ramp_selected_pixels",
                json::Value::ofNumber(
                    static_cast<double>(*report.rampSelectedPixels)
                )
            );
            members.emplace_back(
                "selected_weight",
                json::Value::ofNumber(
                    static_cast<double>(*report.selectedWeight)
                )
            );
        }
        return confirmedToolResult(json::Value::ofObject(std::move(members)));
    }

    auto ProductLifecycle::Impl::answerCensusGridTool(
        operator_runtime::ToolCallPositionIdentity const& call
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(
            arguments,
            operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
        );
        UF_TRY_VALUE(rect, admittedRectangle(arguments.value()));
        auto const key = admittedColourKey(arguments.value());
        UF_TRY_VALUE(ticket, measuringFrame(k_censusGridTool));
        UF_TRY_VALUE(
            report,
            activeContext().cycleCensusGrid(
                ticket,
                rect,
                admittedPixel(arguments.value(), "cell_width"),
                admittedPixel(arguments.value(), "cell_height"),
                key
            )
        );

        auto selected = std::vector<json::Value>{};
        selected.reserve(report.selectedPixels.size());
        for (auto const count : report.selectedPixels)
        {
            selected.emplace_back(
                json::Value::ofNumber(static_cast<double>(count))
            );
        }
        return confirmedToolResult(json::Value::ofObject({
            {"cell_height",
             json::Value::ofNumber(static_cast<double>(report.cellHeight))},
            {"cell_width",
             json::Value::ofNumber(static_cast<double>(report.cellWidth))},
            {"columns",
             json::Value::ofNumber(static_cast<double>(report.columns))},
            {"rows", json::Value::ofNumber(static_cast<double>(report.rows))},
            {"selected_pixels", json::Value::ofArray(std::move(selected))},
        }));
    }

    auto ProductLifecycle::Impl::answerProjectReadTool(
        operator_runtime::ToolCallPositionIdentity const& call
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(
            arguments,
            operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
        );
        UF_TRY_VALUE(path, requiredStringArgument(arguments.value(), "path"));
        UF_TRY_VALUE(bytes, activeContext().projectRead(path));

        auto text = std::string{};
        text.reserve(bytes.size());
        for (auto const value : bytes)
        {
            text.push_back(
                static_cast<char>(std::to_integer<unsigned char>(value))
            );
        }
        UF_TRY_VALUE(hash, sha256(bytes));
        return confirmedToolResult(json::Value::ofObject({
            {"content", json::Value::ofString(std::move(text))},
            {"content_hash", json::Value::ofString(hash.hex())},
            {"path", json::Value::ofString(path)},
        }));
    }

    auto ProductLifecycle::Impl::answerProjectWriteTool(
        operator_runtime::ToolCallPositionIdentity const& call
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(
            arguments,
            operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
        );
        auto const& value = arguments.value();
        UF_TRY_VALUE(action, requiredStringArgument(value, "action"));
        UF_TRY_VALUE(path, requiredStringArgument(value, "path"));

        // The bytes this call writes, from whichever of the two arms named
        // them. The capture arm takes its own frame when its caller holds none
        // -- word for word what an observe with no body and a top-level input
        // both do -- and posts into the innermost open one when there is one,
        // so a capture written from inside an observation's body keeps the
        // frame that body measured.
        auto& context = activeContext();
        auto written  = [&]() -> Result<std::vector<std::byte>>
        {
            if (action == "text")
            {
                UF_TRY_VALUE(content, requiredStringArgument(value, "content"));
                auto bytes = std::vector<std::byte>{};
                bytes.reserve(content.size());
                for (auto const character : content)
                {
                    bytes.emplace_back(
                        static_cast<std::byte>(
                            static_cast<unsigned char>(character)
                        )
                    );
                }
                return bytes;
            }
            UF_CHECK(action == "capture");
            UF_TRY_VALUE(rect, admittedRectangle(value));
            auto const inheritedFrame = context.openObservationFrame();
            if (!inheritedFrame.has_value())
            {
                UF_TRY(context.openCycle());
            }
            auto const closeOwnFrame = scopeExit(
                [&context, owned = !inheritedFrame.has_value()]() noexcept
                {
                    if (owned)
                    {
                        static_cast<void>(context.sweepOpenCycle());
                    }
                }
            );
            auto const capturing = context.openObservationFrame();
            UF_CHECK(capturing.has_value());
            UF_TRY_VALUE(
                cropped,
                context.cycleCrop(*capturing, rect, std::nullopt)
            );
            return cropped.png;
        }();
        UF_TRY_VALUE(bytes, std::move(written));

        UF_TRY(context.projectWrite(path, bytes));
        UF_TRY_VALUE(hash, sha256(bytes));
        return confirmedToolResult(json::Value::ofObject({
            {"action", json::Value::ofString(action)},
            {"content_hash", json::Value::ofString(hash.hex())},
            {"path", json::Value::ofString(path)},
            {"written_bytes",
             json::Value::ofNumber(static_cast<double>(bytes.size()))},
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

        // The frame this delivery resolves and posts on, under the rule
        // answerDeliverInputTool states in full: the innermost open one, or one
        // this call opens and closes because its caller was holding none. The
        // Host seam below resolves the Binding on the frame it finds and opens
        // none of its own, so the frame has to exist before it is reached -- and
        // when the caller IS holding one, the reference being spent names
        // exactly that frame rather than a second capture of it.
        auto& context = activeContext();
        auto const inheritedFrame = context.openObservationFrame();
        if (!inheritedFrame.has_value())
        {
            UF_TRY(context.openCycle());
        }
        auto const closeOwnFrame = scopeExit(
            [&context, owned = !inheritedFrame.has_value()]() noexcept
            {
                if (owned)
                {
                    static_cast<void>(context.sweepOpenCycle());
                }
            }
        );

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

    auto ProductLifecycle::Impl::answerDeliverInputTool(
        operator_runtime::ToolCallPositionIdentity const& call
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        UF_TRY_VALUE(
            arguments,
            operator_runtime::CanonicalJson::parseExact(call.canonicalArgs())
        );
        auto const& value = arguments.value();
        UF_TRY_VALUE(action, requiredStringArgument(value, "action"));

        // Every member below was already judged by the Framework Tool Catalog's
        // tagged contract: the tag is one of six, the arm's own members are all
        // present, a pixel is a non-negative integer inside uint32, and a key
        // is a non-empty string. What is left to this layer is what the catalog
        // cannot see -- this run's declared target surface, this run's cycle,
        // and the delivery layers' own ceilings.
        auto const integerMember = [&value](std::string_view member) -> int64
        {
            auto const* const p_member = value.find(member);
            UF_CHECK(p_member != nullptr);
            return static_cast<int64>(p_member->number());
        };
        auto const pixelMember = [&integerMember](std::string_view member)
        {
            return static_cast<uint32>(integerMember(member));
        };

        // THE SCOPE OF AN INPUT INJECTION IS THE TARGET SURFACE THIS
        // REGISTRATION DECLARED, AND NO WIDER
        // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md).
        // The surface is the RuntimeModel's own base resolution, which the
        // engine already refuses to act against a live target that does not
        // match, so a point outside it is a point on no surface this session
        // can reach.
        //
        // It is judged HERE and before the capture rather than at the sink, for
        // two reasons. The registration is what declares the bound, so this is
        // the layer that can name it. And a refusal before anything is captured
        // or posted is PROVEN ABSENCE, where the same refusal taken inside the
        // delivery path could only be reported as possible -- which would set
        // the target-wide mutation barrier over an aim that never left this
        // function.
        auto const surface    = runtimeModel.fingerprint();
        auto const offSurface = [this, surface](PixelPoint point)
            -> std::optional<std::string>
        {
            if (point.x() < surface.width() && point.y() < surface.height())
            {
                return std::nullopt;
            }
            return std::format(
                "the point ({}, {}) is outside the {}x{} target surface that "
                "registration {} declares for {}, so no input may be aimed at "
                "it",
                point.x(),
                point.y(),
                surface.width(),
                surface.height(),
                generationHandle().projectRegistrationHash().hex(),
                controller().controlledTargetId()
            );
        };

        // The one or two points this arm aims at, in the order the verb takes
        // them. An arm that aims at nothing leaves this empty, which is the
        // whole of what `key` and `scroll` have to say about position.
        auto aimed = std::vector<PixelPoint>{};
        if (value.find("x") != nullptr)
        {
            aimed.emplace_back(pixelMember("x"), pixelMember("y"));
        }
        if (value.find("to_x") != nullptr)
        {
            aimed.emplace_back(pixelMember("to_x"), pixelMember("to_y"));
        }
        for (auto const& point : aimed)
        {
            if (auto refusal = offSurface(point))
            {
                return absentInputResult(k_refusedInputVerdict, *refusal);
            }
        }

        // A key name outside the closed set is the last refusal that precedes
        // the capture, and it is KeyName's own: one definition of which names
        // exist, printing the whole set it refused against.
        auto key = std::optional<KeyName>{};
        if (action == "key")
        {
            UF_TRY_VALUE(name, requiredStringArgument(value, "key"));
            auto created = KeyName::create(name);
            if (!created)
            {
                return absentInputResult(
                    k_refusedInputVerdict,
                    created.error().message()
                );
            }
            key = *created;
        }

        auto const holding = action == "hold";
        auto&      context = activeContext();

        // A HOLD HANDS THE LIFT OVER BEFORE IT PRESSES ANYTHING.
        //
        // An engaged hold returns with the button down, so the frame that lifts
        // it is the Tool call this one was issued from, and that frame has to
        // exist. Pressing first and asking afterwards would deliver an
        // unasked-for press-and-release -- a click to the target -- every time
        // the answer was no. Asking first costs nothing: a run that engages
        // nothing closes a release that finds nothing engaged and answers ok.
        //
        // A top-level hold is what the answer is no for: the dispatcher anchors
        // no live issuing context on a root position, so there is no frame, and
        // the refusal names the position it looked for.
        if (holding)
        {
            auto attached = dispatcher().attachHeldInput(
                call.parentIdentity(),
                // The release borrows the context, and the backing owner is the
                // admitted request this call runs inside: runAdmitted holds the
                // TaskContext for the whole dispatch, the run this is handed to
                // is erased before that dispatch returns, and the Tool Runtime
                // is single-threaded by contract. Nothing here outlives that.
                [p_context = &context]() -> Status
                {
                    if (!p_context->inputEngaged())
                    {
                        return ok();
                    }
                    return p_context->disengageInput().transform([](uint64) {});
                }
            );
            if (!attached)
            {
                return absentInputResult(
                    k_refusedInputVerdict,
                    attached.error().message()
                );
            }
        }

        // THE FRAME THIS ACT LANDS ON IS THE INNERMOST OPEN ONE.
        //
        // A verb inside an observe's body posts into the frame that body is
        // holding and NEVER captures a second one over it: the point it aims at
        // was measured on that frame, so posting into another capture of a
        // screen that moved between them silently changes what the act means.
        // It is also the one sequence a Project could write that would reach the
        // Host's one-cycle invariant, which from now on only a framework bug may
        // reach.
        //
        // A call outside every frame IS its own frame: it opens the one it acts
        // in and closes it inside this same call -- word for word what an
        // observe with no body does, and the same degenerate case rather than a
        // second rule
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        auto const inheritedFrame = context.openObservationFrame();
        if (!inheritedFrame.has_value())
        {
            UF_TRY(context.openCycle());
        }
        // A frame this call opened is a frame this call closes, on every exit
        // path -- including the ones below where the act fails and the frame was
        // never spent.
        auto const closeOwnFrame = scopeExit(
            [&context, owned = !inheritedFrame.has_value()]() noexcept
            {
                if (owned)
                {
                    static_cast<void>(context.sweepOpenCycle());
                }
            }
        );
        auto const acting = context.openObservationFrame();
        UF_CHECK(acting.has_value());
        auto const ticket = *acting;

        // From here the cycle is spent, so nothing below can be reported as
        // proven absence.

        auto delivered = [&]() -> Status
        {
            if (action == "click")
            {
                return context.cycleClick(ticket, aimed.front());
            }
            if (action == "move")
            {
                return context.cycleMove(ticket, aimed.front());
            }
            if (action == "scroll")
            {
                return context.cycleScroll(
                    ticket,
                    static_cast<int32>(integerMember("notches"))
                );
            }
            if (action == "key")
            {
                return context.cycleKey(ticket, *key);
            }
            if (action == "drag")
            {
                return context.cycleDrag(
                    ticket,
                    aimed.front(),
                    aimed.back(),
                    std::chrono::milliseconds{integerMember("travel_ms")}
                );
            }

            // The press. Its lift already belongs to the Tool call above, so
            // this returns with the button down and the run is what puts it
            // back up -- on every exit path, including this one failing.
            UF_CHECK(holding);
            return context.cycleEngageHold(ticket, aimed.front());
        }();

        if (!delivered)
        {
            UF_TRY_VALUE(
                payload,
                operator_runtime::CanonicalJson::parseExact(
                    json::canonicalBytes(json::Value::ofObject({
                        {"action", json::Value::ofString(action)},
                        {"delivered", json::Value::ofBoolean(false)},
                        {"reason",
                         json::Value::ofString(
                             std::string{delivered.error().message()}
                         )},
                        {"verdict",
                         json::Value::ofString(
                             std::string{k_unknownInputTransportVerdict}
                         )},
                    }))
                )
            );
            UF_TRY_VALUE(
                evidence,
                operator_runtime::CanonicalJson::parseExact(
                    json::canonicalBytes(json::Value::ofObject({
                        {"host_delivery",
                         json::Value::ofString(
                             std::string{k_unknownInputTransportVerdict}
                         )},
                        {"posted_inputs", json::Value::ofString("unknown")},
                    }))
                )
            );
            return operator_runtime::ToolCallCompletion::possible(
                std::move(payload),
                std::move(evidence)
            );
        }

        return confirmedToolResult(json::Value::ofObject({
            {"action", json::Value::ofString(action)},
            {"controlled_target_id",
             json::Value::ofString(controller().controlledTargetId())},
            {"delivered", json::Value::ofBoolean(true)},
            // A hold returns with the button still DOWN, and the Tool call this
            // one was issued from is what lifts it. Every other verb is over
            // when it answers.
            {"held", json::Value::ofBoolean(holding)},
        }));
    }

    auto ProductLifecycle::Impl::answerFrameworkTool(
        operator_runtime::ToolCallPositionIdentity const& call,
        ObservationBodyContext* p_bodyContext
    ) -> Result<operator_runtime::ToolCallCompletion>
    {
        auto const& toolName = call.toolName();
        if (toolName == k_observeTool)
        {
            return answerObserveTool(call, p_bodyContext);
        }
        if (toolName == k_statusTool)
        {
            return answerStatusTool();
        }
        if (toolName == k_readLinesTool)
        {
            return answerReadLinesTool(call);
        }
        if (toolName == k_probeTool)
        {
            return answerProbeTool(call);
        }
        if (toolName == k_censusGridTool)
        {
            return answerCensusGridTool(call);
        }
        if (toolName == k_projectReadTool)
        {
            return answerProjectReadTool(call);
        }
        if (toolName == k_projectWriteTool)
        {
            return answerProjectWriteTool(call);
        }
        if (toolName == k_semanticInputTool)
        {
            return answerSemanticInputTool(call);
        }
        if (toolName == k_deliverInputTool)
        {
            return answerDeliverInputTool(call);
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
        task::TaskContext& context,
        operator_runtime::ObservationBodyRun body
    ) -> Result<operator_runtime::ToolCallReplay>
    {
        auto const active = ActiveContext{*this, context};

        // The outermost scope an observation frame can live inside, and
        // therefore the one that closes a frame no inner scope did.
        //
        // For a ROOT observe this is not a backstop but the whole of the rule:
        // a root call has no body, so its frame is a frame with an empty body --
        // open, resolve, close in one call, which is exactly what an observe
        // with no measurements has always been. Root and nested are one shape
        // because the frame is anchored on the observe call's own position
        // either way; only which scope exits around it differs
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        auto const inheritedFrame = dispatcher().heldObservationFrame();
        // Which code answers a call is decided by the name's owner and by
        // nothing else. A Tool this generation bound runs on its own scoped
        // program through the dispatcher; every other admitted name belongs to
        // the Framework namespace, which this lifecycle answers itself. That is
        // two owners of two disjoint name spaces rather than two generations of
        // one job: no value here decides which generation a project is, and the
        // binding table cannot claim a `framework` name because the reader
        // refuses a registrant whose plugin_id falls inside that namespace.
        auto replay = [&]() -> Result<operator_runtime::ToolCallReplay>
        {
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
            auto bodyContext = ObservationBodyContext{
                .request = request,
                .body    = body,
            };
            // The context is offered only when there IS a body: a provider that
            // received an empty one would have to decide whether an empty body
            // means "no body" or "a body that does nothing", which is the
            // absent-means-something reading this repository forbids.
            auto* const p_bodyContext = body ? &bodyContext : nullptr;
            auto provider = [p_self, p_bodyContext](
                                operator_runtime::ToolCallPositionIdentity const&
                                    admittedCall
                            ) -> Result<operator_runtime::ToolCallCompletion>
            { return p_self->answerFrameworkTool(admittedCall, p_bodyContext); };
            return executor.invoke(request, provider);
        }();

        // Unconditional, and before the answer is looked at. Not a scope guard,
        // for the reason the dispatcher's own close is not one: a guard would
        // have to swallow the close's own failure, and a Host that will not
        // release a frame is exactly what an operator has to be told about.
        auto closed = dispatcher().closeObservationFrameOpenedInside(inheritedFrame);
        if (!replay)
        {
            auto error = std::move(replay).error();
            if (!closed)
            {
                error.addContext(
                    "closing the observation frame this Tool call left open "
                    "also failed: "
                        + std::string{closed.error().message()}
                );
            }
            return std::unexpected{std::move(error)};
        }
        if (!closed)
        {
            return std::unexpected{std::move(closed).error()};
        }
        return replay;
    }

    auto ProductLifecycle::invokeFrameworkTool(
        FrameworkToolCall request,
        task::TaskContext& context
    ) -> Result<operator_runtime::ToolCallReplay>
    {
        UF_TRY_VALUE(admission, m_impl->admitFrameworkCall(request));
        return m_impl->runAdmitted(
            admission,
            context,
            std::move(request.body)
        );
    }

    auto ProductLifecycle::Impl::admitFrameworkCall(FrameworkToolCall& request)
        -> Result<operator_runtime::ToolAdmissionRequest>
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
            && access != LifecycleAccess::Writable
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
            .controller      = controller(),
            .lease           = controlLease(),
            .execution       = request.executionIdentity,
            .policyAuthority = policyAuthority,
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
            observations.presented(invocation.canonicalArgs())
        );
        return presented.has_value()
            ? rootProducer.startAgainstObservation(start, *presented)
            : rootProducer.start(start);
    }

    auto ProductLifecycle::Impl::issueExplorationCall(
        task::TaskContext& context,
        std::string_view toolName,
        json::Value const& arguments,
        task::ExplorationCallBody body
    ) -> Result<json::Value>
    {
        UF_TRY_VALUE(
            canonicalArguments,
            operator_runtime::CanonicalJson::parseExact(
                json::canonicalBytes(arguments)
            )
        );

        // INSIDE AN OBSERVATION'S BODY. The call is a child of that observation
        // and goes through the dispatcher's own child seam, which mints the
        // delegation grant from the observation's still-dispatching row, judges
        // the child against what that observation declared it may delegate, and
        // records the call under it.
        if (explorationBodyCall.has_value())
        {
            if (body)
            {
                // Refused here rather than left to the frame's own single-slot
                // refusal, because a body handed to a child call would be
                // silently dropped by the seam that carries no body at all --
                // and a body that did not run is worse than one refused.
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "an observation may not be written inside another "
                    "observation's body; at most one observation frame may be "
                    "open, and the call at "
                        + explorationBodyCall->hex() + " is holding it"
                );
            }
            ++explorationBodyChildren;
            return dispatcher().toolRuntimeSeam()(
                toolName,
                arguments,
                script::ToolCallCoordinate{
                    .parentPosition = *explorationBodyCall,
                    .childIndex     = explorationBodyChildren,
                },
                context.cancellation()
            );
        }

        // AT THE TOP OF THE RUN. One request envelope, admitted by the same
        // translation every other top-of-run call goes through.
        ++explorationRequests;
        auto call = FrameworkToolCall{
            .requestKey = std::string{k_explorationRequestKeyPrefix}
                + std::to_string(explorationRequests),
            .exactRootRequestPreimageJcs = std::string{
                k_explorationRootPreimageJcs
            },
            .executionIdentity = executionIdentity(),
            .toolName          = std::string{toolName},
            .exactArgumentsJcs = std::string{canonicalArguments.bytes()},
            .body              = std::move(body),
        };
        UF_TRY_VALUE(admission, admitFrameworkCall(call));
        auto const callIdentity = admission.call.identity();
        UF_TRY_VALUE(
            replay,
            runAdmitted(admission, context, std::move(call.body))
        );

        // The same answer shape a scoped run's Tool call gets, because it is
        // the same thing: the position the call occupied, its recorded
        // classification, and the Tool's own result and evidence when the
        // outcome carries them.
        auto members = std::vector<json::Member>{};
        members.emplace_back(
            "call_identity",
            json::Value::ofString(callIdentity.hex())
        );
        if (replay.evidence)
        {
            members.emplace_back("evidence", replay.evidence->value());
        }
        if (replay.payload)
        {
            members.emplace_back("result", replay.payload->value());
        }
        members.emplace_back(
            "state",
            json::Value::ofString(
                std::string{operator_runtime::toolCallStateWireName(replay.state)}
            )
        );
        members.emplace_back(
            "tool",
            json::Value::ofString(std::string{toolName})
        );
        return json::Value::ofObject(std::move(members));
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
        return m_impl->runAdmitted(admission, context, {});
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
        return m_impl->runAdmitted(admission, context, {});
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
        return m_impl->runAdmitted(admission, context, {});
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
