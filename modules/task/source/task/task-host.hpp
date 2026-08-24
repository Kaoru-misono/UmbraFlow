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
#include <script/tool-runtime.hpp>

#include <trace/recorder.hpp>

#include <json/value.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    class OperatorTaskHost;
}

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

    // The live ports one exploration front end drives, and the ceilings it
    // drives them under.
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

    // Composition binds the one VM-facing Tool Runtime to this session's own
    // context exactly once. This factory is not a dispatch seam: callers never
    // choose between protocols, and both exploration and scoped Project VMs
    // receive script::ToolRuntimeInvoke.
    using ToolRuntimeBinder =
        std::move_only_function<script::ToolRuntimeInvoke(TaskContext&)>;

    // What one exploration session needs that is a property neither of the
    // desktop nor of the ledgered session it runs inside: the Tool Runtime its
    // chunks call through, where its authoring writes may land, when it stops,
    // and the ceilings its VM answers to.
    //
    // `projectRoot` is stated rather than read off the generation, and that is
    // one of the two reasons this type exists. A generation opened from an
    // installed RuntimeArtifact is rooted at the Operator's content-addressed
    // store, while an exploration session's project reads and writes are
    // confined to the PROJECT directory it is editing; taking the generation's
    // root would point authoring writes at the CAS.
    //
    // `bindToolRuntime` is the other. A session that could not reach a Tool Runtime
    // could do nothing at all, so it is required rather than optional, and it
    // is bound by the ledgered caller that admitted the run after this session's
    // own context exists -- for the same reason the recorder and the engine
    // session are.
    //
    // `cancellation` is the caller's process-level stop token rather than the
    // generation's, because the process that opened the session is the one a
    // person interrupts.
    struct ExplorationSessionSpec final
    {
        std::string           projectId{};
        std::filesystem::path projectRoot{};
        std::filesystem::path tracePath{};

        ToolRuntimeBinder bindToolRuntime{};

        std::stop_token cancellation{};

        uint32                     maximumReadsPerCycle{32};
        uint32                     maximumCropsPerCycle{8};
        uint64                     memoryQuotaBytes{};
        MonotonicInstant::Duration maxScriptRuntime{k_defaultMaxScriptRuntime};
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
        bool explorationClaimed{};
        bool runtimeModelBound{};
    };

    // One kind of generation. Every one carries a verified RuntimeArtifact and
    // the project root that artifact was opened from; what varies between two
    // generations is whether the artifact's hash is SEALED -- whether a closing
    // record in the ledger attests that those bytes are final.
    //
    // A sealed generation is finalized into a generation-owned
    // RuntimeModelBinding and can be observed and acted through. An unsealed
    // one is a directory somebody is still editing: its bytes can change under
    // the reader, so a binding to them would attest to something that is not a
    // fact, and installBinding refuses it. That is the whole difference. There
    // is no annotation generation and no runtime generation, because there is
    // no annotation phase and no runtime phase -- what a session may DO is its
    // Tool closure's answer and never a property of the Host object it holds.
    //
    // Control-target ruling: one TaskHost is permanently bound to the first
    // controlled target whose fence it adopts. It never carries per-target
    // fences or per-target Receipt authority. A production owner that needs a
    // different controlled target owns a different TaskHost for that target.
    class TaskHost final
    {
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

        // Constructible only inside Host. The trusted Runtime parser supplies the
        // RuntimeModel generation it reads and its complete asset closure through
        // the private native surface; no public table is accepted as proof of
        // parser execution.
        struct TrustedRuntimeFinalize final
        {
            uint64                   parserFormat{};
            ContentHash              semanticHash;
            std::vector<std::string> assetReferences{};
            DeclaredRuntimeUi        declaredUi{};
            ProjectFingerprint       fingerprint;
        };

        // PixelPoint has no default state, so every point-bearing intent
        // construction supplies its point.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct TrustedClickInput final
        {
            PixelPoint point;
        };

        // KeyName has no default state either, for its own reason: create() is
        // the only producer of one.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct TrustedKeyInput final
        {
            KeyName key;
        };

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct TrustedDragInput final
        {
            PixelPoint                 start;
            PixelPoint                 end;
            MonotonicInstant::Duration travel{};
        };

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct TrustedHoldInput final
        {
            PixelPoint                 point;
            MonotonicInstant::Duration duration{};
        };

        struct TrustedScrollInput final
        {
            int32 notches{};
        };

        // A move carries the same one point a click does and is a type of its
        // own rather than the same one: what separates them is which engine
        // verb the Receipt authorized, and a sum whose two alternatives had the
        // same type could not say which.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct TrustedMoveInput final
        {
            PixelPoint point;
        };

        // What one Receipt authorizes the Host to deliver, and the whole input
        // vocabulary a declaration may grant. A sum type because exactly one
        // shape is true of any Receipt: a click and a move name one point, a
        // keystroke names a key and no point, a scroll names a detent count and
        // no point, a hold names a point and how long the button stays down,
        // and a drag names both endpoints plus the project's declared travel
        // duration.
        using TrustedReceiptInput = std::variant<
            TrustedClickInput,
            TrustedKeyInput,
            TrustedDragInput,
            TrustedHoldInput,
            TrustedScrollInput,
            TrustedMoveInput
        >;

        // No in-class initializer for the input: no alternative has a default
        // state, so the variant has none either and every construction site
        // supplies it.
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
        friend class operator_runtime::OperatorTaskHost;

        class Generation;
        class RuntimeNativeState;

        std::vector<std::unique_ptr<Generation>> m_generations{};
        std::vector<PendingReceipt>              m_receipts{};

        uint64 m_nextGenerationValue{1};
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

        // Production selection: the supplied context holds at most one open
        // cycle, and a cycle carries at most one unconsumed Receipt. The
        // production owner therefore asks for that Receipt by context rather
        // than receiving an opaque Host token from a caller.
        [[nodiscard]]
        auto deliver(
            DispatchAuthority authority,
            TaskContext& context
        ) -> Result<HostDeliveryReport>;

        // Resolves `uiTarget` ON THE FRAME `context` IS HOLDING, authorizes
        // `action` on the Binding that resolved, and delivers the Receipt that
        // mint produced under `authority`. It opens no frame of its own and is
        // refused by name -- "no open observation frame" -- when the caller
        // holds none.
        //
        // The three steps are ONE operation because the observation frame is
        // what joins them. A Receipt is measured on the frame its cycle holds
        // and the input is posted into that same frame, so anything able to run
        // between the mint and the delivery would be able to aim at one frame
        // and post into another.
        //
        // Binding to the caller's frame is what makes the delivered input
        // attributable to the frame the caller measured on: it IS that frame,
        // by frame identity, rather than a second capture tied back to the
        // first by the (runtime generation, ui target, action) triple alone. The
        // triple still holds -- `authority.uiTarget` carries it to the Host and
        // deliver() refuses a Receipt whose intent names another target -- but
        // it is now a second agreement about one frame instead of the only
        // thread between two
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        //
        // Err means nothing was posted, exactly as for deliver(): everything
        // ahead of the engine call refuses without consuming, and every failure
        // past it is reported inside the returned HostDeliveryReport.
        [[nodiscard]]
        auto deliverUiAction(
            DispatchAuthority authority,
            TaskContext& context,
            std::string_view uiTarget,
            std::string_view action
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

        // Opens the artifact a project directory holds right now, deriving its
        // root hash from the manifest bytes on disk.
        //
        // The hash is therefore SELF-derived and nothing has sealed it: these
        // are bytes somebody may still be editing. The generation is otherwise
        // an ordinary one -- it carries the model, and a session over it reads
        // that model like any other -- but installBinding refuses it, so it can
        // never become a RuntimeModelBinding and nothing downstream can attest
        // to bytes no closing record covers. A session that means to RUN the
        // model runs the sealed one and edits this directory beside it; the
        // divergence between the two is what the parentage chain records.
        //
        // `artifactRoot` is where this project keeps the artifact its own
        // declaration names; the caller reads that declaration, because this
        // Host reads no project document.
        [[nodiscard]]
        auto openUnsealedProject(
            std::filesystem::path const& projectRoot,
            std::filesystem::path const& artifactRoot,
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
        // a vocabulary of its own.
        //
        // It accepts only a SEALED artifact: a hash a closing record in the
        // ledger attests to. An unsealed one is refused naming the hash and the
        // record that is missing, because a binding is an assertion about bytes
        // and an in-flight directory has none to assert.
        [[nodiscard]]
        auto runtimeModelBinding(GenerationId generation)
            -> Result<RuntimeModelBinding>;

        // Opens an observation frame on a finalized generation: it captures one
        // frame, resolves the state on it, and RETURNS WITH THE CYCLE STILL
        // OPEN. A generation with no binding is refused, which is every
        // unsealed one.
        //
        // WHOEVER OPENS A FRAME OWNS CLOSING IT, on every exit path, by calling
        // disengageObservationFrame below. The frame is left open because every
        // measurement taken inside it must read THE SAME frame; a verb that
        // captured its own would measure a moving screen across several
        // captures, which silently changes what those verbs mean
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        // An engage that FAILS leaves nothing open, so only a successful one
        // creates the obligation.
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
        auto engageObservationFrame(GenerationId generation, TaskContext& context)
            -> Result<UiObservationSnapshot>;

        // Closes the observation frame `context` holds and reports whether there
        // was one. Idempotent, and the only close there is: a frame closed twice
        // and a frame that was never opened are the same no-op, because the
        // caller that runs this unconditionally on every exit path cannot know
        // which of the two it is looking at.
        auto disengageObservationFrame(TaskContext& context) noexcept -> bool;

        // Latches this generation's exploration front end and hands back a
        // session over the recorder and engine session the CALLER built.
        //
        // Neither is built here, and that is the point: both carry the ledgered
        // identity of the session an exploration run was admitted under -- the
        // Operator's session id and SessionManifest hash in the trace stream,
        // and the pinned RuntimeModel's fingerprint in the engine -- and this
        // Host has no way to learn either. An exploration session that minted
        // them for itself was a second door onto the desktop
        // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md
        // V5); service::ProductLifecycle::startExplorationSession is the only
        // production caller of this, and it is the one door.
        [[nodiscard]]
        auto startExplorationSession(
            GenerationId generation,
            std::unique_ptr<trace::TraceRecorder> recorder,
            engine::EngineSession session,
            ExplorationSessionSpec spec
        ) -> Result<std::unique_ptr<ExplorationSession>>;

        [[nodiscard]] auto cancel(GenerationId generation) -> Status;
        [[nodiscard]] auto queryTask(GenerationId generation) -> Result<TaskStatus>;
        [[nodiscard]] auto pause(GenerationId generation) -> Status;
        [[nodiscard]] auto resume(GenerationId generation) -> Status;
        [[nodiscard]] auto subscribeEvents(ITaskEventSink& sink) -> Status;
    };
}
