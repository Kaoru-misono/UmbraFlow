#pragma once

#include <task/cycle-ledger.hpp>
#include <task/page-model-file.hpp>
#include <task/ui-observation.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>
#include <engine/session.hpp>

#include <ocr/engine.hpp>

#include <script/engine.hpp>

#include <trace/recorder.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    inline constexpr auto k_defaultMaxScriptRuntime = script::k_defaultMaxRuntime;

    // A cycle can hold at most one unconsumed Receipt, so this also bounds
    // how many observation cycles may be left un-acted upon at once.
    inline constexpr auto k_maximumPendingReceipts = std::size_t{64};

    class ExplorationSession;
    class ITaskEventSink;
    class TaskContext;
    struct CycleTicket;
    struct TaskHostTestAccess;

    enum class TaskRunOutcome : uint8
    {
        Completed,
        Cancelled,
        Failed,
    };

    struct TaskHostConfig final
    {
        std::stop_token externalCancellation{};

        // Receipt freshness is independent of the engine's frame lease. Both
        // must hold in deliver(): this bounds time spent carrying authority,
        // while EngineSession bounds the captured frame itself.
        MonotonicInstant::Duration maximumReceiptAge{k_defaultMaxActionFrameAge};
    };

    // Phase 1 uses live ports only for privileged Annotation. There is no
    // business task or production input configuration until Operator exists.
    struct TaskRunConfig final
    {
        std::unique_ptr<engine::IFrameSource> frameSource{};
        std::unique_ptr<engine::IActionSink>  actionSink{};
        std::unique_ptr<ocr::IOcrEngine>      ocrEngine{};

        ProjectFingerprint liveFingerprint;

        uint64                     maximumPixelComparisons{};
        MonotonicInstant::Duration recognitionTimeout{};
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};
        MonotonicInstant::Duration maxScriptRuntime{k_defaultMaxScriptRuntime};
        uint32                     maximumReadsPerCycle{32};
        uint32                     maximumCropsPerCycle{8};
        uint64                     memoryQuotaBytes{};
        std::filesystem::path      tracePath{};
    };

    struct TaskRunReport final
    {
        std::filesystem::path tracePath{};
        std::optional<Error>  failure{};

        [[nodiscard]] auto outcome() const noexcept -> TaskRunOutcome;
    };

    [[nodiscard]]
    auto closeRunBracket(
        trace::TraceRecorder& recorder,
        TaskRunReport report,
        std::optional<AutomationErrorKind> terminal,
        std::string_view terminalMessage
    ) -> TaskRunReport;

    struct TaskStatus final
    {
        bool cancellationRequested{};
        bool annotationClaimed{};
        bool runtimeModelBound{};
    };

    // Host owns the two deliberately separate generation kinds:
    //
    // - Runtime generations carry one verified RuntimeArtifact and may be
    //   privately finalized into one generation-owned RuntimeModelBinding.
    // - Annotation generations carry only an authoring directory and can start
    //   the privileged screenshot-bearing ExplorationSession.
    //
    // A generation can never change kind. Production therefore has no route to
    // authoring files or screenshots, and Annotation cannot masquerade as a
    // deployment artifact.
    class TaskHost final
    {
        enum class GenerationKind : uint8
        {
            Runtime,
            Annotation,
        };

        struct DeliveryAuthority final
        {
            uint64 hostNonce{};
            uint64 fence{};
        };

        // Private nested type: ordinary C++ and every script/plugin value can
        // neither name nor construct a Receipt. A copy carries only an opaque
        // lookup token; all proof remains in Host-owned storage.
        class Receipt final
        {
            friend class TaskHost;

            // The in-repo harness is already a friend of TaskHost, so it can read
            // both fields a Receipt holds; withholding construction from it would
            // withhold nothing.
            friend struct TaskHostTestAccess;

            uint64 m_hostNonce;
            uint64 m_ordinal;

            Receipt(uint64 hostNonce, uint64 ordinal) noexcept;

        public:
            Receipt(Receipt const&) = default;
            Receipt(Receipt&&) noexcept = default;
            auto operator=(Receipt const&) -> Receipt& = default;
            auto operator=(Receipt&&) noexcept -> Receipt& = default;
            ~Receipt() = default;
        };

        // Constructible only inside Host. The trusted Runtime parser supplies its
        // fixed schema identity and complete asset closure through the private
        // native surface; no public table is accepted as proof of parser execution.
        struct TrustedRuntimeFinalize final
        {
            ContentHash              parserSchemaHash;
            ContentHash              semanticHash;
            std::vector<std::string> assetReferences{};
        };

        struct TrustedReceiptIntent final
        {
            std::string stateIdentity{};
            std::string surface{};
            std::string uiTarget{};
            std::string binding{};
            std::string variant{};
            std::string action{};
            std::string proofLocator{};
            PixelPoint  point;
        };

        struct PendingReceipt final
        {
            uint64                     ordinal{};
            GenerationId               generation;
            ContentHash                artifactRootHash;
            ContentHash                semanticHash;
            CycleTicket                cycle;
            std::optional<uint64>      evidenceCycleOrdinal{};
            TrustedReceiptIntent       intent;
            MonotonicInstant           mintedAt;
            MonotonicInstant::Duration maximumAge{};
            uint64                     fence{};
        };

        friend struct TaskHostTestAccess;

        class Generation;
        class RuntimeNativeState;

        std::vector<std::unique_ptr<Generation>> m_generations{};
        std::vector<PendingReceipt>              m_receipts{};

        uint64 m_nextGenerationValue{1};
        uint64 m_nextRunValue{1};
        uint64 m_nextReceiptOrdinal{1};
        uint64 m_nextObservationOrdinal{1};
        uint64 m_hostNonce;
        uint64 m_fence{1};

        [[nodiscard]] auto findGeneration(GenerationId id) noexcept -> Generation*;
        [[nodiscard]] auto requireGeneration(GenerationId id) -> Result<Generation*>;

        [[nodiscard]]
        auto finalizeRuntimeModel(
            GenerationId generation,
            TrustedRuntimeFinalize trusted
        ) -> Status;

        [[nodiscard]]
        auto runtimePrivateCapabilities(GenerationId generation)
            -> script::PrivateCapabilityInstaller;

        [[nodiscard]]
        auto runtimeAssetBytes(
            GenerationId generation,
            std::string_view relativePath
        ) -> Result<std::vector<std::byte>>;

        [[nodiscard]]
        auto activeRuntimeContext(GenerationId generation) -> Result<TaskContext*>;

        [[nodiscard]]
        auto runTrustedRuntime(
            GenerationId generation,
            TaskContext& context,
            std::string_view source,
            std::string_view chunkName
        ) -> Result<script::ScriptValue>;

        [[nodiscard]] auto bootTrustedRuntime(GenerationId generation) -> Status;

        [[nodiscard]]
        auto mintClickReceipt(
            GenerationId generation,
            TaskContext& context,
            CycleTicket cycle,
            std::optional<uint64> evidenceCycleOrdinal,
            TrustedReceiptIntent intent
        ) -> Result<Receipt>;

        [[nodiscard]] auto deliveryAuthority() const noexcept -> DeliveryAuthority;

        [[nodiscard]]
        // The context is supplied at delivery rather than remembered from
        // minting: a Receipt that stored a TaskContext* would be a borrow of
        // caller-owned state with no contract keeping it alive. Nothing is
        // lost by asking, because requireReceiptCycle already proves the
        // supplied context is the one that minted this Receipt -- no other
        // context holds that cycle.
        auto deliver(
            DeliveryAuthority authority,
            Receipt const& receipt,
            TaskContext& context
        ) -> Result<engine::ActReceipt>;

        [[nodiscard]] auto takeover() -> Status;

    public:
        TaskHost();

        TaskHost(TaskHost const&) = delete;
        TaskHost(TaskHost&&) = delete;
        auto operator=(TaskHost const&) -> TaskHost& = delete;
        auto operator=(TaskHost&&) -> TaskHost& = delete;

        ~TaskHost();

        // Production entry: accepts only an artifact minted by the
        // production-owned installed-generation CAS. An authoring handoff or a
        // merely verified filesystem path is not activation authority.
        [[nodiscard]]
        auto activateRuntimeArtifact(
            InstalledRuntimeArtifact installed,
            TaskHostConfig const& config = {}
        ) -> Result<GenerationId>;

        // Authoring entry: deliberately does not inspect or bind a deployment
        // artifact. It is the only generation kind accepted by Annotation.
        [[nodiscard]]
        auto openAnnotationProject(
            std::filesystem::path const& projectRoot,
            TaskHostConfig const& config = {}
        ) -> Result<GenerationId>;

        [[nodiscard]]
        auto runtimeModelBytes(GenerationId generation)
            -> Result<std::vector<std::byte>>;

        // Runs one observation cycle on a Runtime generation and returns what
        // the trusted resolver concluded. Annotation generations are refused:
        // the kinds never convert, and a production snapshot must not be able
        // to reach authoring files.
        //
        // The context is supplied rather than remembered, for deliver()'s
        // reason: a Host that stored a TaskContext* would be holding a borrow of
        // caller-owned state with no contract keeping it alive, and
        // activeRuntimeContext answers only while a trusted chunk is already
        // running -- which is inside this call, not before it. Nothing is lost
        // by asking, because the observation is minted from what the chunk
        // measured through THIS context and no other context holds that cycle.
        //
        // The generation observed is deliberately not a parameter: a snapshot
        // whose target generation the caller named would certify a world the
        // Host never saw.
        [[nodiscard]]
        auto observe(GenerationId generation, TaskContext& context)
            -> Result<UiObservationSnapshot>;

        [[nodiscard]]
        auto startExplorationSession(
            GenerationId generation,
            TaskRunConfig config
        ) -> Result<std::unique_ptr<ExplorationSession>>;

        [[nodiscard]] auto cancel(GenerationId generation) -> Status;
        [[nodiscard]] auto queryTask(GenerationId generation) -> Result<TaskStatus>;
        [[nodiscard]] auto pause(GenerationId generation) -> Status;
        [[nodiscard]] auto resume(GenerationId generation) -> Status;
        [[nodiscard]] auto subscribeEvents(ITaskEventSink& sink) -> Status;
    };
}
