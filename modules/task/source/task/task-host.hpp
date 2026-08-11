#pragma once

#include <task/cycle-ledger.hpp>
#include <task/host-delivery.hpp>
#include <task/runtime-model-file.hpp>
#include <task/ui-observation.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
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
#include <variant>
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
            DeclaredRuntimeUi        declaredUi{};
            ProjectFingerprint       fingerprint;
        };

        // What one Receipt authorizes the Host to deliver. A sum type because
        // exactly one of the two is true of any Receipt: a click names the
        // point the model measured, a keystroke names a key and no point at
        // all. Two optional members could spell both or neither, and defaulting
        // a key's coordinate to (0,0) would put a number into the delivery path
        // that nothing measured.
        using TrustedReceiptInput = std::variant<PixelPoint, KeyName>;

        // No in-class initializer for the input: neither alternative has a
        // default state, so the variant has none either and every construction
        // site supplies it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct TrustedReceiptIntent final
        {
            std::string         stateIdentity{};
            std::string         surface{};
            std::string         uiTarget{};
            std::string         binding{};
            std::string         variant{};
            std::string         action{};
            std::string         proofLocator{};
            TrustedReceiptInput input;
        };

        // No in-class initializers for the generation, the two hashes or the
        // mint instant: GenerationId, ContentHash and MonotonicInstant have no
        // default state, so every construction site supplies all four.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
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
            uint64                     fencingToken{};
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

        // fencingToken stays 0 until a ledger fence is adopted, and minting is
        // refused until it moves. A Host the Operator has never authorized can
        // therefore mint nothing, which is a second reason production cannot
        // act, independent of deliver() being private.
        ControlFence m_fence{};

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
        auto mintReceipt(
            GenerationId generation,
            TaskContext& context,
            CycleTicket cycle,
            std::optional<uint64> evidenceCycleOrdinal,
            TrustedReceiptIntent intent
        ) -> Result<Receipt>;

        [[nodiscard]]
        // The context is supplied at delivery rather than remembered from
        // minting: a Receipt that stored a TaskContext* would be a borrow of
        // caller-owned state with no contract keeping it alive. Nothing is
        // lost by asking, because requireReceiptCycle already proves the
        // supplied context is the one that minted this Receipt -- no other
        // context holds that cycle.
        //
        // Err means nothing was consumed and there is nothing to record. Once
        // the Receipt is consumed the call cannot fail any more: a refusal past
        // that point is a fact about the world, and the ledger can record only
        // what it is told.
        auto deliver(
            DispatchAuthority authority,
            Receipt const& receipt,
            TaskContext& context
        ) -> Result<HostDeliveryReport>;

        // Raises this Host's control fence to the one the ledger now holds.
        // Strictly monotone: a fence at or below the current one is refused, so
        // a stale lease cannot re-arm a Host a takeover already fenced out. The
        // first adoption also binds this Host to one controlled target; a later
        // fence naming a different target is refused.
        //
        // Pending Receipts are deliberately left in place rather than dropped.
        // They carry the fence they were minted under, so delivering one after
        // a takeover consumes it and reports NotDelivered -- which is proof of
        // absence. Dropping them would turn the same schedule into an Err, and
        // an Err proves nothing.
        [[nodiscard]] auto adoptControlFence(ControlFence fence) -> Status;

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

        // What the trusted parser made of this generation's model: its artifact
        // root hash, its semantic hash, and the identifiers it declares. It is
        // the one way anything outside the Host learns those identifiers, and it
        // is a copy of a Host-minted value rather than a request to compute one,
        // so a caller can neither name a model the Host did not parse nor state
        // a vocabulary of its own. An Annotation generation, or a Runtime one
        // whose model has not been finalized, is refused.
        [[nodiscard]]
        auto runtimeModelBinding(GenerationId generation)
            -> Result<RuntimeModelBinding>;

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
