#include "task-host.hpp"

#include "exploration-session.hpp"
#include "framework-bundle.hpp"
#include "page-model-file.hpp"
#include "script-bindings.hpp"
#include "task-context.hpp"
#include "ui-observation.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>

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
        // stack a resolved state carries, and the reason the other kinds
        // failed; `candidates`, `conflicts` and `evidence` are excluded with it,
        // because evidence ids are drawn from the same kind of counter.
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
            return jcs.encode({
                kind = state.kind,
                ordered_surface_stack = state.ordered_surface_stack,
                reason = reason,
            })
        )lua"};

        [[nodiscard]] auto traceSchemaHash() -> ContentHash
        {
            auto const parsed = ContentHash::parse(
                std::format("sha256:{}", trace::k_traceSchemaHash)
            );
            UF_CHECK(parsed.has_value());
            return *parsed;
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
                    .schemaHash = traceSchemaHash(),
                    .fields     = std::move(fields),
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
                    std::format("cannot inspect annotation project {}: {}", root.string(), error.message())
                );
            }
            if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "annotation project root must be a real directory, not a link"
                );
            }
            auto canonical = std::filesystem::canonical(root, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot canonicalize annotation project {}: {}",
                        root.string(),
                        error.message()
                    )
                );
            }
            if (canonical.filename().empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "annotation project root must have a non-empty directory name"
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
        GenerationKind                               m_kind;
        std::filesystem::path                        m_root;
        std::string                                  m_projectId;
        std::shared_ptr<RuntimeArtifactHandle const> m_artifact{};
        std::shared_ptr<RuntimeModelBinding const>   m_binding{};
        std::optional<script::Engine>                m_runtimeVm{};
        MonotonicInstant::Duration                   m_maximumReceiptAge{};

        std::stop_source                       m_stop{};
        std::stop_callback<ExternalStopBridge> m_externalStop;
        bool                                   m_annotationClaimed{};
        TaskContext*                           m_pRuntimeContext{};

    public:
        Generation(
            GenerationId id,
            GenerationKind kind,
            std::filesystem::path root,
            std::string projectId,
            std::shared_ptr<RuntimeArtifactHandle const> artifact,
            TaskHostConfig const& config
        )
            : m_id{id}
            , m_kind{kind}
            , m_root{std::move(root)}
            , m_projectId{std::move(projectId)}
            , m_artifact{std::move(artifact)}
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
        [[nodiscard]] auto kind() const noexcept -> GenerationKind { return m_kind; }

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

        [[nodiscard]]
        auto installBinding(std::shared_ptr<RuntimeModelBinding const> binding) -> Status
        {
            if (m_kind != GenerationKind::Runtime || !m_artifact)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "only a Runtime generation can own a RuntimeModelBinding"
                );
            }
            if (m_binding)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "this Runtime generation is already finalized"
                );
            }
            if (m_annotationClaimed)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "a claimed generation cannot be finalized"
                );
            }
            m_binding = std::move(binding);
            return ok();
        }

        [[nodiscard]] auto installRuntimeVm(script::Engine vm) -> Status
        {
            if (m_kind != GenerationKind::Runtime || !m_binding || m_runtimeVm.has_value())
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
            if (m_kind != GenerationKind::Runtime || !m_binding || !m_runtimeVm.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "trusted Runtime execution requires a finalized Runtime generation"
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

        [[nodiscard]] auto claimAnnotation() -> Status
        {
            if (m_kind != GenerationKind::Annotation)
            {
                return fail(
                    AutomationErrorKind::UnsupportedCapability,
                    "Annotation cannot open a production Runtime generation"
                );
            }
            m_annotationClaimed = true;
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
                .annotationClaimed     = m_annotationClaimed,
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
        if (p_generation->kind() != GenerationKind::Runtime || !artifact)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "trusted Runtime finalize requires a RuntimeArtifact generation"
            );
        }
        if (trusted.parserSchemaHash != artifact->runtimeModelSchemaHash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "trusted Runtime parser schema does not match the artifact"
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

        auto binding = std::make_shared<RuntimeModelBinding const>(
            RuntimeModelBinding{generation, artifact, trusted.semanticHash}
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
        if (p_generation->kind() != GenerationKind::Runtime || !binding)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "runtime assets require a finalized Runtime generation"
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
                .frameworkProjectGlobals = {
                    std::string{"jcs"},
                    std::string{"observe"},
                    std::string{"project"},
                },
                .classifyRaisedError     = scriptRaisedErrorClassifier(),
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

    auto TaskHost::mintClickReceipt(
        GenerationId generation,
        TaskContext& context,
        CycleTicket cycle,
        std::optional<uint64> evidenceCycleOrdinal,
        TrustedReceiptIntent intent
    ) -> Result<Receipt>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto const& binding = p_generation->binding();
        if (p_generation->kind() != GenerationKind::Runtime || !binding)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Receipt minting requires a privately finalized Runtime generation"
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
                return pending.fence != m_fence
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
                .fence                  = m_fence,
            }
        );
        return receipt;
    }

    auto TaskHost::deliveryAuthority() const noexcept -> DeliveryAuthority
    {
        return DeliveryAuthority{.hostNonce = m_hostNonce, .fence = m_fence};
    }

    auto TaskHost::deliver(
        DeliveryAuthority authority,
        Receipt const& receipt,
        TaskContext& context
    ) -> Result<engine::ActReceipt>
    {
        if (
            authority.hostNonce != m_hostNonce
            || authority.fence != m_fence
            || receipt.m_hostNonce != m_hostNonce
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

        // Linearization point: once valid Host authority presents a known token,
        // no later failure can make the same authorization deliverable again.
        auto pending = std::move(*found);
        m_receipts.erase(found);

        UF_TRY_VALUE(p_generation, requireGeneration(pending.generation));
        auto const& binding = p_generation->binding();
        if (
            pending.fence != m_fence
            || !binding
            || binding->generation() != pending.generation
            || binding->artifactRootHash() != pending.artifactRootHash
            || binding->semanticHash() != pending.semanticHash
        )
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "Host Receipt no longer matches its generation, binding, or fence"
            );
        }
        if (
            MonotonicInstant::now().saturatingDurationSince(pending.mintedAt)
            > pending.maximumAge
        )
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "Host Receipt exceeded its freshness bound"
            );
        }
        UF_TRY(
            context.requireReceiptCycle(
                pending.cycle,
                pending.evidenceCycleOrdinal
            )
        );
        return context.deliverReceiptClick(pending.cycle, pending.intent.point);
    }

    auto TaskHost::takeover() -> Status
    {
        if (m_fence == std::numeric_limits<uint64>::max())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Host delivery fence space is exhausted"
            );
        }
        ++m_fence;
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
        auto const id = GenerationId{m_nextGenerationValue};
        ++m_nextGenerationValue;
        m_generations.emplace_back(
            std::make_unique<Generation>(
                id,
                GenerationKind::Runtime,
                artifact->root(),
                artifact->root().filename().string(),
                std::move(artifact),
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

    auto TaskHost::openAnnotationProject(
        std::filesystem::path const& projectRoot,
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
        auto const id = GenerationId{m_nextGenerationValue};
        ++m_nextGenerationValue;
        auto projectId = root.filename().string();
        m_generations.emplace_back(
            std::make_unique<Generation>(
                id,
                GenerationKind::Annotation,
                std::move(root),
                std::move(projectId),
                nullptr,
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
        auto const& artifact = p_generation->artifact();
        if (p_generation->kind() != GenerationKind::Runtime || !artifact)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "Annotation generations do not carry RuntimeArtifact bytes"
            );
        }
        auto const bytes = artifact->modelBytes();
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }

    auto TaskHost::observe(
        GenerationId generation,
        TaskContext& context
    ) -> Result<UiObservationSnapshot>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        auto const& binding = p_generation->binding();
        if (p_generation->kind() != GenerationKind::Runtime || !binding)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "UI observation requires a privately finalized Runtime generation"
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
        // raise, so the sweep is unconditional and outranks the chunk's result.
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
        return UiObservationSnapshot{
            std::move(observationId),
            generation,
            *targetGeneration,
            binding->artifactRootHash(),
            binding->semanticHash(),
            *p_document,
        };
    }

    auto TaskHost::startExplorationSession(
        GenerationId generation,
        TaskRunConfig config
    ) -> Result<std::unique_ptr<ExplorationSession>>
    {
        UF_TRY_VALUE(p_generation, requireGeneration(generation));
        UF_TRY(p_generation->claimAnnotation());

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
