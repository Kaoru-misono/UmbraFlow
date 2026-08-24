#include "task-host.hpp"

#include "exploration-session.hpp"
#include "framework-bundle.hpp"
#include "platform/confined-file.hpp"
#include "runtime-model-file.hpp"
#include "script-bindings.hpp"
#include "task-context.hpp"
#include "ui-observation.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <core/utility/variant-match.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::task
{
    namespace
    {
        // The whole of TaskHost::observe that runs inside the VM. It resolves
        // one state and canonicalizes it; the Host receives an opaque document
        // and interprets no member of it.
        //
        // What it serializes is the resolution's CONTENT, never its occasion.
        // `resolution.resolve_state` stamps each resolved state with an id drawn
        // from a module-level counter, so a document carrying that id would put
        // the number of preceding resolutions inside state_resolution_hash,
        // inside decision_basis_hash, and inside every frozen plan -- and two
        // identical readings of one unchanged world would then demand separate
        // approvals. The projection is therefore the kind, the ordered surface
        // stack a resolved state carries, the readings that state reports, and
        // the reason the other kinds failed; `candidates`, `conflicts` and
        // `evidence` are excluded with it, because evidence ids are drawn from
        // the same kind of counter.
        //
        // `readings` appears exactly when a Surface stack does, and is the empty
        // array when the resolved model declares no reads: an unresolved state
        // has no Surface to attribute a reading to, and §6.2 of the consumer
        // design puts "UI surface resolved" ahead of every read for that reason.
        // Its length is otherwise decided by the model rather than by what the
        // Host managed to answer, because a Reader that found nothing and one
        // that could not decide are both reported with their outcome instead of
        // being dropped.
        // It is inside this document rather than beside it, which is what puts
        // it inside state_resolution_hash and therefore inside decision_basis_hash
        // without a second member having to be remembered.
        //
        // `kind` is always present, which is what keeps the document an object:
        // jcs encodes an empty table as `[]`, so an envelope that could be empty
        // would sometimes be an array. Absent members are nil rather than null,
        // which Lua drops, so an unresolved stack and a resolved reason simply
        // do not appear.
        //
        // The cycle is deliberately left open. The Host reads the frame identity
        // the resolution was taken against straight off the ledger and sweeps
        // the cycle itself; closing here would release the frame before the one
        // fact the snapshot still needs from it could be read.
        constexpr auto k_observeSource = std::string_view{R"lua(
            local cycle = observe.open(project.load_project())
            local state = cycle:resolve_state()
            local reason = state.reason
            if reason == nil and type(state.conflicts) == "table" then
                local conflict = state.conflicts[1]
                if conflict ~= nil then reason = conflict.kind end
            end
            local readings = nil
            if state.kind == "resolved_state" then
                readings = cycle:resolve_readings(state)
            end
            return jcs.encode({
                kind = state.kind,
                ordered_surface_stack = state.ordered_surface_stack,
                readings = readings,
                reason = reason,
                diagnostic = state.diagnostic,
            })
        )lua"};

        // The whole of TaskHost::deliverUiAction that runs inside the VM. It
        // BINDS TO THE FRAME ITS CALLER IS HOLDING, resolves the state on it,
        // resolves the named ui target's Binding and asks the resolver to
        // authorize the named action on it; the Receipt that mint produces is
        // Host-private storage the chunk never sees, and the frame is left open
        // so the delivery that follows posts into the frame the placement was
        // measured on.
        //
        // It used to open a frame of its own, and that was the drift this
        // binding deletes: the caller's coordinates and this measurement then
        // came from two captures of a screen that moves between them, while the
        // answer named one. With no frame open it refuses BY NAME -- observe.current
        // raises "no open observation frame" -- rather than quietly taking a
        // capture nobody asked for
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        //
        // The two names are interpolated rather than passed as arguments
        // because a trusted chunk has no argument channel, and that is safe
        // exactly because deliverUiAction admits only names its own generation's
        // model declares: model.luau accepts an id only as
        // ^[a-z][a-z0-9._:-]*$, so a declared name cannot carry a quote and an
        // undeclared one never reaches this format call.
        [[nodiscard]]
        auto authorizeUiActionSource(
            std::string_view uiTarget,
            std::string_view action
        ) -> std::string
        {
            return std::format(
                R"lua(
            local cycle = observe.current(project.load_project())
            local state = cycle:resolve_state()
            local binding = cycle:resolve_binding(state, "{}")
            local receipt, reason = cycle:authorize(binding, "{}")
            if receipt == nil then error(reason) end
            return 1
        )lua",
                uiTarget,
                action
            );
        }

        [[nodiscard]]
        auto runFinishedEvent(TaskRunReport const& report) -> trace::TraceEventSpec
        {
            auto outcome = std::string{"completed"};
            switch (report.outcome())
            {
            case TaskRunOutcome::Completed:
                outcome = "completed";
                break;
            case TaskRunOutcome::Cancelled:
                outcome = "cancelled";
                break;
            case TaskRunOutcome::Failed:
                outcome = "failed";
                break;
            }

            auto fields = std::vector<trace::TraceField>{};
            fields.emplace_back(
                trace::TraceField{
                    .name  = "outcome",
                    .value = std::move(outcome),
                }
            );
            if (report.failure.has_value())
            {
                auto const kind = automationErrorKind(*report.failure)
                    .value_or(AutomationErrorKind::InternalInvariant);
                fields.emplace_back(
                    trace::TraceField{
                        .name  = "error_kind",
                        .value = std::string{automationErrorWireName(kind)},
                    }
                );
            }
            return trace::TraceEventSpec{
                .eventType = "run.finished",
                .audit     = trace::AuditMetadata{.actor = "host"},
                .payload   = trace::TypedTracePayload{
                    .fields = std::move(fields),
                },
            };
        }

        [[nodiscard]]
        auto canonicalProjectRoot(std::filesystem::path const& root)
            -> Result<std::filesystem::path>
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(root, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot inspect project root {}: {}", root.string(), error.message())
                );
            }
            if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project root must be a real directory, not a link"
                );
            }
            auto canonical = std::filesystem::canonical(root, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot canonicalize project root {}: {}",
                        root.string(),
                        error.message()
                    )
                );
            }
            if (canonical.filename().empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project root must have a non-empty directory name"
                );
            }
            return canonical;
        }
    }

    auto TaskRunReport::outcome() const noexcept -> TaskRunOutcome
    {
        if (!failure.has_value())
        {
            return TaskRunOutcome::Completed;
        }
        if (automationErrorKind(*failure) == AutomationErrorKind::Cancelled)
        {
            return TaskRunOutcome::Cancelled;
        }
        return TaskRunOutcome::Failed;
    }

    auto closeRunBracket(
        trace::TraceRecorder& recorder,
        TaskRunReport report,
        std::optional<AutomationErrorKind> terminal,
        std::string_view terminalMessage
    ) -> TaskRunReport
    {
        if (!report.failure.has_value() && terminal.has_value())
        {
            report.failure = fail(*terminal, std::string{terminalMessage}).error();
        }
        auto closed = recorder.emit(runFinishedEvent(report));
        if (!report.failure.has_value() && !closed)
        {
            report.failure = std::move(closed).error();
        }
        return report;
    }

    class TaskHost::Generation final
    {
        struct ExternalStopBridge final
        {
            std::stop_source* p_target{};

            auto operator()() const noexcept -> void
            {
                if (p_target != nullptr)
                {
                    static_cast<void>(p_target->request_stop());
                }
            }
        };

        GenerationId                                 m_id;
        std::filesystem::path                        m_root;
        std::string                                  m_projectId;
        std::shared_ptr<RuntimeArtifactHandle const> m_artifact;

        // The installed generation whose durable pin seals this artifact's
        // hash, when one does. Absent means the hash was derived from bytes on
        // disk and nothing attests that they are final -- see installBinding.
        std::optional<uint64>                      m_sealedAt;
        std::shared_ptr<RuntimeModelBinding const> m_binding{};
        std::optional<script::Engine>              m_runtimeVm{};
        MonotonicInstant::Duration                 m_maximumReceiptAge{};

        std::stop_source                       m_stop{};
        std::stop_callback<ExternalStopBridge> m_externalStop;
        bool                                   m_explorationClaimed{};
        TaskContext*                           m_pRuntimeContext{};

    public:
        Generation(
            GenerationId id,
            std::filesystem::path root,
            std::string projectId,
            std::shared_ptr<RuntimeArtifactHandle const> artifact,
            std::optional<uint64> sealedAt,
            TaskHostConfig const& config
        )
            : m_id{id}
            , m_root{std::move(root)}
            , m_projectId{std::move(projectId)}
            , m_artifact{std::move(artifact)}
            , m_sealedAt{sealedAt}
            , m_maximumReceiptAge{config.maximumReceiptAge}
            , m_externalStop{
                  config.externalCancellation,
                  ExternalStopBridge{&m_stop},
              }
        {
        }

        Generation(Generation const&) = delete;
        Generation(Generation&&) = delete;
        auto operator=(Generation const&) -> Generation& = delete;
        auto operator=(Generation&&) -> Generation& = delete;
        ~Generation() = default;

        [[nodiscard]] auto id() const noexcept -> GenerationId { return m_id; }

        // Whether a closing record seals this artifact's hash, and which
        // installed generation it was recorded at.
        [[nodiscard]] auto sealedAt() const noexcept -> std::optional<uint64>
        {
            return m_sealedAt;
        }

        [[nodiscard]]
        auto root() const noexcept -> std::filesystem::path const& { return m_root; }

        [[nodiscard]]
        auto projectId() const noexcept -> std::string const& { return m_projectId; }

        [[nodiscard]]
        auto artifact() const noexcept
            -> std::shared_ptr<RuntimeArtifactHandle const> const&
        {
            return m_artifact;
        }

        [[nodiscard]]
        auto binding() const noexcept
            -> std::shared_ptr<RuntimeModelBinding const> const&
        {
            return m_binding;
        }

        [[nodiscard]] auto maximumReceiptAge() const noexcept
            -> MonotonicInstant::Duration
        {
            return m_maximumReceiptAge;
        }

        // A binding is an assertion about bytes, so it may only be installed
        // over bytes something has sealed. An unsealed artifact's hash was
        // derived from a directory that may still be edited, and a binding to
        // it would be an assertion the framework cannot make good on.
        [[nodiscard]]
        auto installBinding(std::shared_ptr<RuntimeModelBinding const> binding) -> Status
        {
            if (!m_sealedAt)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "RuntimeModel artifact {} carries no closing record, so "
                        "no binding can attest to it",
                        m_artifact->rootHash().hex()
                    )
                );
            }
            if (m_binding)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "this generation is already finalized"
                );
            }
            m_binding = std::move(binding);
            return ok();
        }

        [[nodiscard]] auto installRuntimeVm(script::Engine vm) -> Status
        {
            if (!m_binding || m_runtimeVm.has_value())
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "trusted Runtime VM can be installed exactly once after finalize"
                );
            }
            m_runtimeVm = std::move(vm);
            return ok();
        }

        [[nodiscard]] auto bindRuntimeContext(TaskContext& context) -> Status
        {
            if (!m_binding || !m_runtimeVm.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "trusted Runtime execution requires a finalized generation"
                );
            }
            if (m_pRuntimeContext != nullptr)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "trusted Runtime execution is already active"
                );
            }
            m_pRuntimeContext = &context;
            return ok();
        }

        auto unbindRuntimeContext(TaskContext& context) noexcept -> void
        {
            UF_CHECK(m_pRuntimeContext == &context);
            m_pRuntimeContext = nullptr;
        }

        [[nodiscard]] auto runtimeContext() const noexcept -> TaskContext*
        {
            return m_pRuntimeContext;
        }

        [[nodiscard]] auto runtimeVm() noexcept -> script::Engine*
        {
            return m_runtimeVm.has_value() ? &*m_runtimeVm : nullptr;
        }

        // One exploration front end per generation. It is a latch on the
        // front end and not on what the session may do: a session's powers are
        // its Tool closure's answer, and this only says that two front ends
        // cannot drive one generation's ledger, template store and trace at
        // once.
        [[nodiscard]] auto claimExplorationFrontEnd() -> Status
        {
            if (m_explorationClaimed)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "this generation already has an exploration front end"
                );
            }
            m_explorationClaimed = true;
            return ok();
        }

        [[nodiscard]] auto cancellation() const noexcept -> std::stop_token
        {
            return m_stop.get_token();
        }

        auto cancel() noexcept -> void
        {
            static_cast<void>(m_stop.request_stop());
        }

        [[nodiscard]] auto status() const noexcept -> TaskStatus
        {
            return TaskStatus{
                .cancellationRequested = m_stop.stop_requested(),
                .explorationClaimed    = m_explorationClaimed,
                .runtimeModelBound     = static_cast<bool>(m_binding),
            };
        }
    };

    TaskHost::Receipt::Receipt(uint64 hostNonce, uint64 ordinal) noexcept
        : m_hostNonce{hostNonce}
        , m_ordinal{ordinal}
    {
    }

    TaskHost::TaskHost()
        : m_hostNonce{mintHandleGeneration()}
    {
    }

    TaskHost::~TaskHost() = default;

    auto TaskHost::findGeneration(GenerationId id) noexcept -> Generation*
    {
        auto const found = std::ranges::find(m_generations, id, [](auto const& value)
        {
            return value->id();
        });
        return found == m_generations.end() ? nullptr : found->get();
    }

    auto TaskHost::requireGeneration(GenerationId id) -> Result<Generation*>
    {
        auto* const p_generation = findGeneration(id);
        if (p_generation == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("no Host generation {} exists", id.value())
            );
        }
        return p_generation;
    }

    auto TaskHost::finalizeRuntimeModel(
        GenerationId generation,
        TrustedRuntimeFinalize trusted
    ) -> Status
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto const& artifact = p_generation->artifact();
        // The one place the trusted parser's own generation meets the artifact's.
        // loadRuntimeArtifact has already held the artifact against
        // k_runtimeModelFormat, so this fires exactly when model.luau and
        // runtime-model-file.hpp disagree -- a build where the parser reads a
        // different RuntimeModel generation than the Host was told to expect.
        // Both numbers are named because neither is visible from the other file.
        if (trusted.parserFormat != artifact->runtimeModelFormat())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "trusted Runtime parser reads RuntimeModel format {} and the "
                    "artifact states format {}",
                    trusted.parserFormat,
                    artifact->runtimeModelFormat()
                )
            );
        }

        auto expectedAssets = artifact->assetPaths();
        if (
            !std::ranges::is_sorted(trusted.assetReferences)
            || std::ranges::adjacent_find(trusted.assetReferences)
                != trusted.assetReferences.end()
            || trusted.assetReferences != expectedAssets
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "trusted Runtime parser asset closure does not match the artifact"
            );
        }

        // The declared vocabulary is taken as given, unlike the asset closure
        // checked above. There is nothing to check it against: the parser is the
        // only reader of RuntimeModel semantics in the system, so a Host that
        // re-derived these identifiers would be the second reader this design
        // exists to prevent. Its consumers compare membership, which is
        // insensitive to order and repetition, so no ordering rule is asserted
        // here either -- one nothing could violate would be a check that cannot
        // fail.
        //
        // The declared geometry travels on the same terms and for the same
        // reason. It is already a ProjectFingerprint by the time it arrives, so
        // the only refusal it could carry -- a zero extent or DPI -- has been
        // spent at the native seam that built it.
        auto binding = std::make_shared<RuntimeModelBinding const>(
            RuntimeModelBinding{
                generation,
                artifact,
                trusted.semanticHash,
                std::move(trusted.declaredUi),
                trusted.fingerprint,
            }
        );
        return p_generation->installBinding(std::move(binding));
    }

    auto TaskHost::runtimeAssetBytes(
        GenerationId generation,
        std::string_view relativePath
    ) -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto const& binding = p_generation->binding();
        if (!binding)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "runtime assets require a finalized generation"
            );
        }
        return p_generation->artifact()->fileBytes(relativePath);
    }

    auto TaskHost::activeRuntimeContext(
        GenerationId generation
    ) -> Result<TaskContext*>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto* const p_context = p_generation->runtimeContext();
        if (p_context == nullptr)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "trusted Runtime observation is unavailable outside Host execution"
            );
        }
        return p_context;
    }

    auto TaskHost::runTrustedRuntime(
        GenerationId generation,
        TaskContext& context,
        std::string_view source,
        std::string_view chunkName
    ) -> Result<script::ScriptValue>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        UF_TRY(p_generation->bindRuntimeContext(context));
        auto guard = scopeExit(
            [p_generation, &context]() noexcept
            {
                p_generation->unbindRuntimeContext(context);
            }
        );
        auto* const p_vm = p_generation->runtimeVm();
        UF_CHECK(p_vm != nullptr);
        return p_vm->runValue(source, chunkName);
    }

    auto TaskHost::bootTrustedRuntime(GenerationId generation) -> Status
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto vm = script::Engine::create(
            script::EngineConfig{
                .cancellation      = p_generation->cancellation(),
                .frameworkModules           = frameworkScriptModules(),
                .installHostTables          = scriptHostTableInstaller(),
                .installPrivateCapabilities = runtimePrivateCapabilities(generation),
                .projectGlobals             = scriptProjectGlobals(),
                .frameworkProjectGlobals    = runtimeProjectGlobals(),
                .classifyRaisedError        = scriptRaisedErrorClassifier(),
            }
        );
        if (!vm)
        {
            return std::unexpected{std::move(vm).error()};
        }
        UF_TRY_VALUE(
            loaded,
            vm->runNumber("project.load_project(); return 1", "runtime-artifact-finalize")
        );
        if (loaded != 1.0 || !p_generation->binding())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "trusted Runtime parser returned without finalizing its artifact"
            );
        }
        return p_generation->installRuntimeVm(*std::move(vm));
    }

    auto TaskHost::mintReceipt(
        GenerationId generation,
        TaskContext& context,
        CycleTicket cycle,
        std::optional<uint64> evidenceCycleOrdinal,
        TrustedReceiptIntent intent
    ) -> Result<Receipt>
    {
        if (m_fence.fencingToken == 0)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Host has no control fence to mint against"
            );
        }

        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto const& binding = p_generation->binding();
        if (!binding)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Receipt minting requires a privately finalized generation"
            );
        }
        UF_TRY(context.requireReceiptCycle(cycle, evidenceCycleOrdinal));

        // The cycle alone identifies the duplicate: a CycleTicket names one
        // observation of one generation, so two contexts cannot share one.
        auto const duplicate = std::ranges::find_if(
            m_receipts,
            [cycle](PendingReceipt const& pending)
            {
                return pending.cycle.generation == cycle.generation
                    && pending.cycle.ordinal == cycle.ordinal;
            }
        );
        if (duplicate != m_receipts.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this observation cycle already has an unconsumed Host Receipt"
            );
        }
        // A Receipt that outlived its freshness bound or its fence can never be
        // delivered, so it is swept before the ceiling is applied. Without this
        // the ceiling is a wedge rather than a bound: nothing but a successful
        // delivery removes an entry, and Phase 1 publishes no production
        // deliverer, so a runtime that authorizes once per cycle would reach
        // the limit and then be refused forever.
        auto const now = MonotonicInstant::now();
        std::erase_if(
            m_receipts,
            [this, now](PendingReceipt const& pending) noexcept
            {
                return pending.fencingToken != m_fence.fencingToken
                    || now.saturatingDurationSince(pending.mintedAt) > pending.maximumAge;
            }
        );
        if (m_receipts.size() >= k_maximumPendingReceipts)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Host has too many undelivered Receipts"
            );
        }
        if (m_nextReceiptOrdinal == std::numeric_limits<uint64>::max())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Host Receipt ordinal space is exhausted"
            );
        }

        auto const receipt = Receipt{m_hostNonce, m_nextReceiptOrdinal};
        ++m_nextReceiptOrdinal;
        m_receipts.emplace_back(
            PendingReceipt{
                .ordinal    = receipt.m_ordinal,
                .generation = generation,
                .artifactRootHash       = binding->artifactRootHash(),
                .semanticHash           = binding->semanticHash(),
                .cycle                = cycle,
                .evidenceCycleOrdinal = evidenceCycleOrdinal,
                .intent               = std::move(intent),
                .mintedAt             = MonotonicInstant::now(),
                .maximumAge             = p_generation->maximumReceiptAge(),
                .fencingToken           = m_fence.fencingToken,
            }
        );
        return receipt;
    }

    auto TaskHost::deliver(
        DispatchAuthority authority,
        Receipt const& receipt,
        TaskContext& context
    ) -> Result<HostDeliveryReport>
    {
        if (
            receipt.m_hostNonce != m_hostNonce
            || authority.controlledTargetId != m_fence.controlledTargetId
            || authority.sessionEpoch != m_fence.sessionEpoch
            || authority.fencingToken != m_fence.fencingToken
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "delivery authority is foreign or fenced"
            );
        }

        auto const found = std::ranges::find(m_receipts, receipt.m_ordinal, &PendingReceipt::ordinal);
        if (found == m_receipts.end())
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "Host Receipt is unknown, stale, or already consumed"
            );
        }
        if (authority.runtimeGeneration != found->generation)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "delivery authority names another runtime generation"
            );
        }
        if (authority.uiTarget != found->intent.uiTarget)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "delivery receipt names a model target the reserved step's "
                "observed instance does not"
            );
        }

        // Linearization point: once valid Host authority presents a known token,
        // no later failure can make the same authorization deliverable again.
        // Everything above refuses without consuming; everything below reports.
        auto pending = std::move(*found);
        m_receipts.erase(found);

        // Synchronous and non-escaping: every use returns the value it builds,
        // so the captured authority is moved from at most once.
        auto report = [&authority, ordinal = pending.ordinal](
            DeliveryOutcome outcome,
            std::string reason,
            std::optional<DeliveredInput> delivered
        ) -> HostDeliveryReport
        {
            return HostDeliveryReport{
                std::move(authority),
                outcome,
                std::move(reason),
                ordinal,
                delivered
            };
        };

        auto const p_generation = findGeneration(pending.generation);
        auto const* p_binding = p_generation == nullptr
            ? nullptr
            : p_generation->binding().get();
        if (
            pending.fencingToken != m_fence.fencingToken
            || p_binding == nullptr
            || p_binding->generation() != pending.generation
            || p_binding->artifactRootHash() != pending.artifactRootHash
            || p_binding->semanticHash() != pending.semanticHash
        )
        {
            return report(
                DeliveryOutcome::NotDelivered,
                "Host Receipt no longer matches its generation, binding, or fence",
                std::nullopt
            );
        }
        if (
            MonotonicInstant::now().saturatingDurationSince(pending.mintedAt)
            > pending.maximumAge
        )
        {
            return report(
                DeliveryOutcome::NotDelivered,
                "Host Receipt exceeded its freshness bound",
                std::nullopt
            );
        }

        auto const cycle = context.requireReceiptCycle(
            pending.cycle,
            pending.evidenceCycleOrdinal
        );
        if (!cycle.has_value())
        {
            return report(
                DeliveryOutcome::NotDelivered,
                std::format(
                    "Host Receipt cycle is not the one this context holds: {}",
                    cycle.error().message()
                ),
                std::nullopt
            );
        }

        // Past this call the input may already have reached the target, and the
        // engine's Result cannot say whether it did. clickPoint fails before the
        // sink, at the sink, and after the click landed; pressKey does the same,
        // and its post-sink case is real rather than theoretical, because a
        // press whose release did not land is reported as one error. The
        // engine's teardown puts the key back up and does not make the press
        // unhappen, so TransportUnknown is still the only honest answer for
        // every kind, and it deliberately does not prove absence. A hold and a
        // drag hold a button down for the whole of their delivery, so the
        // window in which that is true is longest for them; what closes it is
        // EngineSession::endDelivery releasing whatever was left held, on every
        // exit path, rather than anything this layer compensates for.
        //
        // Which of the six runs is the Receipt's own intent and is decided here
        // and nowhere else: an overload set over the sum, so a seventh kind
        // cannot be added without this dispatch failing to compile.
        auto delivered = matchVariant(
            pending.intent.input,
            [&context, &pending](TrustedClickInput const& click)
                -> Result<DeliveredInput>
            {
                UF_TRY_VALUE(
                    act,
                    context.deliverReceiptClick(pending.cycle, click.point)
                );
                return DeliveredInput{act};
            },
            [&context, &pending](TrustedKeyInput const& stroke)
                -> Result<DeliveredInput>
            {
                UF_TRY_VALUE(
                    pressed,
                    context.deliverReceiptKey(pending.cycle, stroke.key)
                );
                return DeliveredInput{pressed};
            },
            [&context, &pending](TrustedDragInput const& drag) -> Result<DeliveredInput>
            {
                UF_TRY_VALUE(
                    delivered,
                    context.deliverReceiptDrag(
                        pending.cycle,
                        drag.start,
                        drag.end,
                        drag.travel
                    )
                );
                return DeliveredInput{delivered};
            },
            [&context, &pending](TrustedHoldInput const& held)
                -> Result<DeliveredInput>
            {
                UF_TRY_VALUE(
                    delivered,
                    context.deliverReceiptHold(
                        pending.cycle,
                        held.point,
                        held.duration
                    )
                );
                return DeliveredInput{delivered};
            },
            [&context, &pending](TrustedScrollInput const& wheel)
                -> Result<DeliveredInput>
            {
                UF_TRY_VALUE(
                    delivered,
                    context.deliverReceiptScroll(pending.cycle, wheel.notches)
                );
                return DeliveredInput{delivered};
            },
            [&context, &pending](TrustedMoveInput const& move)
                -> Result<DeliveredInput>
            {
                UF_TRY_VALUE(
                    delivered,
                    context.deliverReceiptMove(pending.cycle, move.point)
                );
                return DeliveredInput{delivered};
            }
        );
        if (!delivered.has_value())
        {
            return report(
                DeliveryOutcome::TransportUnknown,
                std::format(
                    "Host delivery reached the engine and did not complete: {}",
                    delivered.error().message()
                ),
                std::nullopt
            );
        }
        return report(DeliveryOutcome::Delivered, {}, *std::move(delivered));
    }

    auto TaskHost::deliver(
        DispatchAuthority authority,
        TaskContext& context
    ) -> Result<HostDeliveryReport>
    {
        auto const found = std::ranges::find_if(
            m_receipts,
            [&context](PendingReceipt const& pending)
            {
                return context.requireReceiptCycle(
                    pending.cycle,
                    pending.evidenceCycleOrdinal
                ).has_value();
            }
        );
        if (found == m_receipts.end())
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "the delivery context holds no pending Host Receipt"
            );
        }
        auto const receipt = Receipt{m_hostNonce, found->ordinal};
        return deliver(std::move(authority), receipt, context);
    }

    auto TaskHost::deliverUiAction(
        DispatchAuthority authority,
        TaskContext& context,
        std::string_view uiTarget,
        std::string_view action
    ) -> Result<HostDeliveryReport>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(authority.runtimeGeneration));
        auto const& binding = p_generation->binding();
        if (!binding)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "authorized UI-action delivery requires a privately finalized "
                "generation"
            );
        }

        // Membership in what the trusted parser declared, and it is two things
        // at once: the refusal a caller naming a target or action this model
        // does not have earns, and the proof that the two names are safe to
        // interpolate into a chunk. A name that reaches the format call is a
        // name model.luau already admitted as an identifier.
        auto const& declared = binding->declaredUi();
        if (std::ranges::find(declared.uiTargets, uiTarget) == declared.uiTargets.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "this generation's RuntimeModel declares no ui target {}",
                    uiTarget
                )
            );
        }
        if (std::ranges::find(declared.actions, action) == declared.actions.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "this generation's RuntimeModel declares no UI action {}",
                    action
                )
            );
        }

        // NO SWEEP HERE. This function opens no frame any more -- it binds to
        // the one its caller is holding -- so releasing one would release a
        // frame it does not own, at a moment its owner never named. A successful
        // delivery spends the frame, and a refused one leaves it exactly as it
        // found it, for the call that opened it to close on its own exit.
        auto const generation = authority.runtimeGeneration;
        UF_TRY(runTrustedRuntime(
            generation,
            context,
            authorizeUiActionSource(uiTarget, action),
            "runtime-authorize"
        ));
        return deliver(std::move(authority), context);
    }

    auto TaskHost::adoptControlFence(ControlFence fence) -> Status
    {
        if (fence.controlledTargetId.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "a control fence must name the target it fences"
            );
        }
        if (fence.fencingToken <= m_fence.fencingToken)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "a control fence at or below the adopted one cannot re-arm this Host"
            );
        }
        if (
            m_fence.fencingToken != 0
            && fence.controlledTargetId != m_fence.controlledTargetId
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this Host is already fenced to another controlled target"
            );
        }

        m_fence = std::move(fence);
        return ok();
    }

    auto TaskHost::activateRuntimeArtifact(
        InstalledRuntimeArtifact installed,
        TaskHostConfig const& config
    ) -> Result<GenerationId>
    {
        if (config.maximumReceiptAge < MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Host Receipt freshness bound must not be negative"
            );
        }
        if (!installed.m_artifact || installed.m_installedGeneration == 0U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Host requires a valid installed RuntimeArtifact"
            );
        }
        auto artifact = std::move(installed.m_artifact);
        auto const sealedAt = installed.m_installedGeneration;
        auto const id = GenerationId{m_nextGenerationValue};
        ++m_nextGenerationValue;
        m_generations.emplace_back(
            std::make_unique<Generation>(
                id,
                artifact->root(),
                artifact->root().filename().string(),
                std::move(artifact),
                // The seal. An InstalledRuntimeArtifact exists only where the
                // ledger answered for this exact hash, so carrying the
                // installed generation here is carrying the closing record's
                // own coordinate rather than a flag this function chose.
                std::optional{sealedAt},
                config
            )
        );
        auto rollback = scopeExit(
            [this, id]() noexcept
            {
                auto const found = std::ranges::find(
                    m_generations,
                    id,
                    [](auto const& value)
                    {
                        return value->id();
                    }
                );
                UF_CHECK(found != m_generations.end());
                m_generations.erase(found);
            }
        );
        UF_TRY(bootTrustedRuntime(id));
        rollback.release();
        return id;
    }

    auto TaskHost::openUnsealedProject(
        std::filesystem::path const& projectRoot,
        std::filesystem::path const& artifactRoot,
        TaskHostConfig const& config
    ) -> Result<GenerationId>
    {
        if (config.maximumReceiptAge < MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Host Receipt freshness bound must not be negative"
            );
        }
        UF_TRY_VALUE(root, canonicalProjectRoot(projectRoot));

        // Self-derived, and that is the point: the hash comes from the bytes
        // that are on disk at this instant, so it names what is there and
        // attests to nothing about what will be there next. loadRuntimeArtifact
        // still verifies the whole closure against that hash, so the artifact
        // is internally consistent -- it is simply not sealed, and no
        // RuntimeModelBinding will be built over it.
        UF_TRY_VALUE(confined, task_platform::ConfinedRoot::open(artifactRoot));
        UF_TRY_VALUE(
            manifestBytes,
            confined.readFile(
                k_runtimeArtifactManifestFileName,
                k_maximumRuntimeManifestBytes
            )
        );
        UF_TRY_VALUE(rootHash, sha256(manifestBytes));
        UF_TRY_VALUE(artifact, loadRuntimeArtifact(artifactRoot, rootHash));

        auto const id = GenerationId{m_nextGenerationValue};
        ++m_nextGenerationValue;
        auto projectId = root.filename().string();
        m_generations.emplace_back(
            std::make_unique<Generation>(
                id,
                std::move(root),
                std::move(projectId),
                std::make_shared<RuntimeArtifactHandle const>(std::move(artifact)),
                std::nullopt,
                config
            )
        );
        return id;
    }

    auto TaskHost::runtimeModelBytes(
        GenerationId generation
    ) -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto const bytes = p_generation->artifact()->modelBytes();
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }

    auto TaskHost::runtimeModelBinding(
        GenerationId generation
    ) -> Result<RuntimeModelBinding>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        // The sealing assertion, and the only thing this function is for
        // besides handing the value over. A binding says "this generation
        // parsed THESE bytes"; a hash nothing sealed names bytes that can still
        // move, so the assertion would be about a moving target. Both the hash
        // and the record that is missing are named, because a caller told only
        // "refused" cannot tell an unsealed directory from a parse that failed.
        if (!p_generation->sealedAt())
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                std::format(
                    "RuntimeModel artifact {} carries no closing record in the "
                    "ledger, so this Host will not bind it",
                    p_generation->artifact()->rootHash().hex()
                )
            );
        }
        auto const& binding = p_generation->binding();
        if (!binding)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "a RuntimeModel binding requires a privately finalized generation"
            );
        }
        return *binding;
    }

    auto TaskHost::engageObservationFrame(
        GenerationId generation,
        TaskContext& context
    ) -> Result<UiObservationSnapshot>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto const& binding = p_generation->binding();
        if (!binding)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "UI observation requires a privately finalized generation"
            );
        }
        if (m_nextObservationOrdinal == std::numeric_limits<uint64>::max())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Host observation ordinal space is exhausted"
            );
        }

        // k_observeSource leaves its cycle open on every path, including a
        // raise. A FAILED engage must leave nothing open -- the caller has no
        // frame to own and would have nothing to close -- so this sweeps every
        // path out of this function except the one that hands the open frame
        // over, which releases it below.
        auto sweep = scopeExit(
            [&context]() noexcept
            {
                static_cast<void>(context.sweepOpenCycle());
            }
        );
        UF_TRY_VALUE(
            resolved,
            runTrustedRuntime(generation, context, k_observeSource, "runtime-observe")
        );
        auto const* const p_document = resolved.text();
        if (p_document == nullptr)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "the trusted observation chunk returned no canonical document"
            );
        }
        auto const targetGeneration = context.openCycleTargetGeneration();
        if (!targetGeneration.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "the trusted observation chunk released its cycle before the "
                "Host could read the capture it resolved"
            );
        }

        auto observationId = std::format(
            "observation-{}-{}",
            m_hostNonce,
            m_nextObservationOrdinal
        );
        ++m_nextObservationOrdinal;
        // The frame is handed over, so it stops being this function's to close.
        sweep.release();
        return UiObservationSnapshot{
            std::move(observationId),
            generation,
            *targetGeneration,
            binding->artifactRootHash(),
            binding->semanticHash(),
            *p_document,
        };
    }

    auto TaskHost::disengageObservationFrame(TaskContext& context) noexcept -> bool
    {
        return context.sweepOpenCycle();
    }

    auto TaskHost::startExplorationSession(
        GenerationId generation,
        TaskRunConfig config
    ) -> Result<std::unique_ptr<ExplorationSession>>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        UF_TRY(p_generation->claimExplorationFrontEnd());

        auto const runId = EngineRunId{m_nextRunValue};
        ++m_nextRunValue;
        return ExplorationSession::create(
            std::move(config),
            ExplorationSession::Spec{
                .projectId    = p_generation->projectId(),
                .projectRoot  = p_generation->root(),
                .cancellation = p_generation->cancellation(),
            },
            runId,
            generation
        );
    }

    auto TaskHost::cancel(GenerationId generation) -> Status
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        p_generation->cancel();
        return ok();
    }

    auto TaskHost::queryTask(GenerationId generation) -> Result<TaskStatus>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        return p_generation->status();
    }

    auto TaskHost::pause(GenerationId generation) -> Status
    {
        UF_TRY(requireGeneration(generation));
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "pause is unavailable before Operator owns a safe boundary"
        );
    }

    auto TaskHost::resume(GenerationId generation) -> Status
    {
        UF_TRY(requireGeneration(generation));
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "resume is unavailable before Operator owns a safe boundary"
        );
    }

    auto TaskHost::subscribeEvents(ITaskEventSink& /*sink*/) -> Status
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "event subscription is unavailable before Operator exists"
        );
    }
}
