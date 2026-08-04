#pragma once

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::trace
{
    class TraceRecorder;

    // The one evidence stream every layer writes into. Every line carries the run
    // and generation id plus a monotonic sequence, engine.* lines carry the frame
    // identity, and task.native_call carries the retained observation the verb was
    // handed -- which is what still names the frame when a host-side guard fails
    // the verb before the engine sees it. The version is emitted first on every
    // line so a consumer can reject a line it does not understand; removing a
    // member is a wire change and bumps it.
    inline constexpr auto k_traceSchema = std::string_view{"umbraflow-trace/v4"};

    // The single JSON member holding every field that may legitimately differ
    // between two runs of the same task at the same seed. A golden comparison
    // runs stripNonGoldenFields over each line first, so everything outside
    // `meta` is part of the reproducible record.
    inline constexpr auto k_nonGoldenMember = std::string_view{"meta"};

    // An audit log, not a replay log: nothing here records a frame's pixels, a
    // page's anchor evidence, or the arguments of a project call. Replay rests on
    // the seed in run.started plus the observation sequence a test hands its fake
    // frame source.
    //
    // Only the host writes and stamps a line. run.*, engine.* and task.* cannot
    // be requested from Luau at all; framework.* can, through the private `emit`
    // primitive, which the host validates against stream-validator.hpp's state
    // machine before any of it reaches a sink. A project script can name no
    // primitive that requests one.
    //
    // This enum and the outcome enums below leave 0 without an enumerator, and
    // the members holding them carry no in-class initializer. Aggregate
    // initialization value-initializes an omitted scoped-enum member regardless
    // of a default member initializer, so only leaving 0 outside the domain makes
    // an omitted discriminator loud -- at the serializer's UF_UNREACHABLE, before
    // a plausible-looking success line reaches a sink.
    //
    // framework.subtask_entered / subtask_exited are P1 and deliberately absent.

    // Which front-end drove the run this line belongs to. Part of the STAMP
    // rather than of the event: TraceRecorder carries one value for the whole run
    // and writes it onto every line, so no emitter can forget it or claim another
    // front-end's work. It is also the exclusion -- TaskHost latches one per
    // generation, refuses every other, and hands the latched value to the
    // recorder.
    enum class FrontEnd : uint8
    {
        // A project task running on the trusted Luau framework, driven by
        // `umbra-flow run`.
        Task = 1,

        // An agent driving a target in order to measure it, and to write down
        // what it measured: `umbra-flow explore` running one Luau chunk per
        // queue line against a live target. It loads a project, latches this
        // front-end on the generation exactly as the other two do, and consumes
        // the same capability surface plus the privileged verbs
        // (docs/plans/2026-08-01-agent-front-end-and-exploration.md).
        Annotation,

        // A run that measures screens and delivers no input at all: the
        // falsification matrix `umbra-flow check` walks, and the offline
        // trace-replay checker planned beside it. Delivering nothing is the
        // whole boundary against the other two, which both act on a target, and
        // it is why one name covers both of those runs.
        //
        // A measuring run tries every page it cares about against one frame, so
        // it stands on no page in the sense a task does. A reader rebuilding the
        // pages a run walked out of framework.page_resolved therefore excludes
        // this front-end
        // (docs/plans/2026-08-04-state-layer-and-policy-slots.md 4.2).
        Check,
    };

    // The wire spelling of one front-end, and the only place either is spelled
    // -- task::TaskHost names one outside a trace line too, when it refuses a
    // second front-end on a generation.
    [[nodiscard]]
    auto frontEndWireName(FrontEnd frontEnd) noexcept -> std::string_view;

    enum class TraceEventKind : uint8
    {
        RunStarted = 1,
        RunResourcesValidated,
        RunFinished,
        EngineObserved,
        EngineActionFound,
        EngineTextRead,
        EngineActionAuthorized,
        EngineActionRejected,
        EngineActionDelivered,
        EngineKeyDelivered,

        // One delivered wheel scroll, its own kind rather than an
        // engine.action_delivered carrying a delta; additive.
        EngineScrollDelivered,

        // One delivered long press: pointer down at a point, held, released;
        // its own kind, additive.
        EngineLongPressDelivered,

        // One delivered pointer move: the cursor message arrives at a point
        // and no button changes state; additive, and it carries its point on
        // `clickClient`, the member the long press already shares with the
        // click.
        EnginePointerMoveDelivered,

        EngineObservationInvalidated,

        // One capture that stalled and is being attempted again. Additive, and
        // its own kind rather than a rejection line: a stall the engine rode out
        // did not fail the run, and a reader counting failures must not have to
        // subtract the ones that were survived. A run with no such line stalled
        // never; a run with five of them was one attempt from ending.
        EngineCaptureRetried,
        TaskNativeCall,
        FrameworkStepStarted,
        FrameworkStepFinished,
        FrameworkRetryAttempt,
        FrameworkRetryBackoff,
        FrameworkInterruptMatched,
        FrameworkInterruptHandled,
        FrameworkInterruptExhausted,
        FrameworkSettled,

        // Which page one resolution concluded the frame was on, carried in
        // Framework::label. Additive, and written only where a resolution
        // SUCCEEDS: a caller tries every page it cares about against one frame,
        // so recording the refusals would give one line per page tried rather
        // than the sequence of pages the run believed it walked. That sequence
        // is what an offline trace-replay check reads
        // (docs/plans/2026-08-04-state-layer-and-policy-slots.md 4.2).
        //
        // The one framework.* kind admitted on every stream, because it claims
        // no framework structure and the exploration front-end resolves against
        // the same page model; see stream-validator.hpp.
        FrameworkPageResolved,

        // The two verbs the exploration front-end has and no other does,
        // spelled `annotation.*` because neither names an element or a page
        // (docs/plans/2026-08-01-agent-front-end-and-exploration.md 1).
        AnnotationClickDelivered,
        AnnotationRegionSaved,
    };

    // How one action-target search ended. Stopped and Failed say only that much;
    // TraceEvent::stopReason and errorKind carry which reason it was.
    enum class ActionSearch : uint8
    {
        Found = 1,
        Absent,
        Stopped,
        Failed,
    };

    // The result summary of one native call from the script layer. Succeeded and
    // Empty both completed without a failure; Empty is the Tier A completed miss,
    // kept distinct so a trace shows a search that ran and found nothing apart
    // from one that found its target.
    enum class NativeCallOutcome : uint8
    {
        Succeeded = 1,
        Empty,
        Failed,
    };

    // How a run ended. Completed is a clean script return; Failed is an uncaught
    // automation error or the script's own error; Cancelled is the single cancel
    // source spending the generation.
    enum class RunOutcome : uint8
    {
        Completed = 1,
        Failed,
        Cancelled,
    };

    // One trace record, without the identity the recorder stamps on it. The
    // optional sub-structs are a C++ shape only -- serializeTraceEvent flattens
    // them into one JSON object. A transport aggregate: build it with designated
    // initializers and set only the groups the event carries.
    struct TraceEvent final
    {
        // The result of one action-target search. The score pair is present
        // whenever the search ran to completion (Found or Absent); matchedRect
        // only for Found.
        struct Action final
        {
            ActionSearch             outcome;
            std::optional<uint64>    sadScore{};
            std::optional<uint64>    maximumSad{};
            std::optional<PixelRect> matchedRect{};
        };

        // What one text read produced. Reading is the one capability with no
        // score to explain itself, so every member is evidence: see the
        // annotation-model plan, section 4.2.7.
        struct Reading final
        {
            // One line a BLOCK read located inside the region, with the
            // rectangle the frame itself put it at. A single-line read is asked
            // about a rectangle and answers about that same rectangle, so `rect`
            // below carries both facts; a block read answers with lines the
            // frame located inside its region, and a click aimed at one of them
            // can only be audited against that line's own rectangle.
            struct Line final
            {
                std::string text{};

                // In frame pixels, never relative to the region, matching what
                // ocr::TextLine promises and what the click is delivered in.
                PixelRect rect;

                uint32 confidenceBp{};
            };

            std::string text{};

            PixelRect rect;

            uint32 confidenceBp{};

            // Which recogniser produced the text, engine and model together. A
            // different model spells the same pixels differently, so a line that
            // does not name it cannot be replayed.
            std::string engineId{};

            // Microseconds rather than milliseconds: a single-line read measured
            // 2-13 ms, so whole milliseconds would round most reads to one or two
            // values and lose the distribution the budget is set from. For a
            // block read it is the whole call, detection plus every recognition
            // it caused; the recogniser reports no per-line timing, so the lines
            // below carry no duration of their own.
            uint64 durationMicros{};

            // The lines a block read found, empty for a single-line read. Empty
            // is not serialized at all; which verb was called is already on the
            // task.native_call line.
            std::vector<Line> lines{};
        };

        // What this run is: the addressed task, the bytes it was compiled from,
        // the trusted framework and Luau compiler those bytes ran on, and the
        // seed its RNG draws from. Every member is supplied together on
        // run.started, the only place a run's identity is written down, so two
        // runs against different framework builds cannot produce the same line.
        //
        // The plan's resource snapshot hash and config digest are deliberately
        // absent: task::TaskHost computes neither, and an empty string would be a
        // false attribution rather than a missing one.
        struct Run final
        {
            std::string projectId{};
            std::string taskName{};
            std::string sourceHash{};
            std::string frameworkVersion{};
            std::string frameworkHash{};
            std::string luauVersion{};
            uint64      seed{};
        };

        // The validated resource closure of the run. Both lists are sorted before
        // emission, so an unordered container's iteration order can never reach
        // the wire.
        struct Resources final
        {
            std::vector<std::string> elements{};
            std::vector<std::string> pages{};
        };

        // One call from the script layer into the host capability surface, with
        // the argument identity the primitive was handed -- which is what makes
        // a native call joinable without relying on position. The cycle-ledger
        // lookup behind cycle_match, cycle_read, cycle_click and cycle_close
        // fails a call before the engine is reached, so its task.native_call is
        // the only line the failure produces. cycle_open mints its own ordinal
        // rather than receiving one, so it carries none.
        struct NativeCall final
        {
            std::string       verb{};
            NativeCallOutcome outcome;

            // The ordinal of the cycle whose ticket the primitive was handed.
            std::optional<uint64> cycleOrdinal{};

            // The cycle ordinal carried by the hit handed to cycle_click. A hit
            // is usable only while the cycle that found it is still open, so the
            // two ordinals agree on every delivered click and differ exactly when
            // a hit from a spent cycle was refused -- which is that refusal's
            // entire content, so both reach the wire.
            std::optional<uint64> hitCycleOrdinal{};

            // The pause a settle declared, in whole milliseconds. A settle
            // reaches no engine verb, so this line is the only evidence it
            // happened, and a run that paused two seconds and one that paused ten
            // are different runs. Carried by `cycle_long_press` too, where it is
            // the hold the caller named; absent on every other verb.
            std::optional<uint64> durationMillis{};

            // The project file a project_read or project_write named, as the
            // script spelled it: the confined relative name and never the
            // resolved absolute path, because the name is what the script asked
            // for and therefore what a refusal is about.
            std::optional<std::string> resourceName{};

            // How many bytes the call read or wrote, and the SHA-256 of those
            // bytes -- the same task over different bytes is a different run, and
            // nothing else in the stream says so. Present on project_read,
            // project_write and template_load.
            std::optional<uint64>      byteCount{};
            std::optional<std::string> contentHash{};
        };

        // One framework semantic event's payload. The framework asks for these
        // through `emit`, so this is the only group whose contents originate
        // outside the host, and every field of it is checked at the request
        // boundary (see stream-validator.hpp) rather than trusted. One struct
        // serves all eight framework kinds; which members a kind carries is fixed
        // by the validator, so an absent member is a refused event rather than a
        // silently empty line.
        struct Framework final
        {
            // A step's name, an interrupt's id, or the page a resolution
            // concluded on. Length, character set and the total open-step budget
            // are enforced before the event is admitted; an over-budget label is
            // rejected, never truncated, because a truncated name silently
            // addresses a different step.
            std::string label{};

            // The retry attempt this pass is, and the total the policy declared.
            // Both present on framework.retry_attempt and absent everywhere else.
            std::optional<uint64> attempt{};
            std::optional<uint64> attempts{};

            // The pause a framework.retry_backoff or framework.settled declared,
            // in whole milliseconds.
            std::optional<uint64> durationMillis{};
        };

        // What one exploration verb touched. Present on the two annotation.*
        // kinds and nowhere else. The point and the rect are FRAME pixels -- what
        // the agent believed it was aiming at -- while a delivered click also
        // records the client point it was posted at on TraceEvent::clickClient,
        // which is what the desktop received.
        struct Annotation final
        {
            std::optional<PixelPoint> point{};
            std::optional<PixelRect>  rect{};

            // The SHA-256 of the PNG a crop produced. The bytes went to the agent
            // rather than into this stream, so the hash is the only thing tying a
            // file the agent later wrote to the frame it came from.
            std::optional<std::string> contentHash{};

            // How many bytes that PNG was.
            std::optional<uint64> byteCount{};
        };

        TraceEventKind kind;

        // The capture this event belongs to, present on every engine.* event and
        // the join key tying an engine event to the frame it observed. Domain's
        // own FrameIdentity rather than a trace-local copy of its three fields:
        // the identity has no default constructor, so a site that leaves part of
        // it out does not compile.
        std::optional<FrameIdentity> frame{};

        std::optional<Action>     action{};
        std::optional<Reading>    reading{};
        std::optional<Run>        run{};
        std::optional<Resources>  resources{};
        std::optional<NativeCall> nativeCall{};
        std::optional<Framework>  framework{};
        std::optional<Annotation> annotation{};

        // Fields that cut across the groups above. A template is named by the
        // SHA-256 of the bytes the script layer loaded, because that is the only
        // name it has: it belongs to no catalog element, and no element identity
        // reaches this wire at all.
        std::optional<std::string> templateHash{};

        std::optional<SadSearchStopReason> stopReason{};
        std::optional<RunOutcome>          runOutcome{};
        std::optional<AutomationErrorKind> errorKind{};
        std::optional<std::string>         message{};
        std::optional<Point<ClientSpace>>  clickClient{};

        // The key one engine.key_delivered posted, as the target's UI prints it.
        // A keystroke names no coordinate, so there is no clickClient and the
        // name is that event's whole content.
        std::optional<KeyName> key{};

        // The detent count one engine.scroll_delivered posted, positive away from
        // the operator and negative toward them. That event's whole content for
        // `key`'s reason: a scroll names no coordinate the verb chose.
        std::optional<int32> wheelNotches{};

        // Which attempt one engine.capture_retried is retrying, counted from the
        // first that stalled. That event's whole content: the frame it wanted
        // does not exist, so there is no FrameIdentity to join it on, and the
        // number against the ceiling is what says how close the run came to
        // ending.
        std::optional<uint64> captureAttempt{};

        // How long one engine.long_press_delivered held the button down, in whole
        // milliseconds -- half of what that event records, the point being the
        // other half, because the hold is the entire difference between a long
        // press and the click at the same coordinate.
        std::optional<uint64> holdMillis{};
    };

    // A TraceEvent with the run identity stamped onto it. Only TraceRecorder can
    // build one, so no emitter reaches a sink without going through the recorder
    // that owns the sequence counter, the run id, and the generation id.
    class StampedTraceEvent final
    {
        friend class TraceRecorder;

        TraceEvent               m_event;
        std::vector<std::string> m_openSteps;
        uint64                   m_sequence;
        TaskRunId                m_runId;
        GenerationId             m_generationId;
        FrontEnd                 m_frontEnd;
        int64                    m_wallClockUnixMillis;

        StampedTraceEvent(
            TraceEvent event,
            std::vector<std::string> openSteps,
            uint64 sequence,
            TaskRunId runId,
            GenerationId generationId,
            FrontEnd frontEnd,
            int64 wallClockUnixMillis
        );

    public:
        [[nodiscard]]
        auto event() const noexcept UF_LIFETIME_BOUND -> TraceEvent const&;

        // The framework step scope open when this event was written, outermost
        // first, read off the validation state machine rather than supplied by
        // the emitter. That is what makes design section 12's "every
        // task.native_call falls inside the step scope open at the time" true by
        // construction: a call can neither claim a step that is not open nor omit
        // one that is.
        [[nodiscard]]
        auto openSteps() const noexcept UF_LIFETIME_BOUND
            -> std::span<std::string const>;

        [[nodiscard]] auto sequence() const noexcept -> uint64;
        [[nodiscard]] auto runId() const noexcept -> TaskRunId;
        [[nodiscard]] auto generationId() const noexcept -> GenerationId;

        // See FrontEnd: part of the stamp for the reason the sequence is.
        [[nodiscard]] auto frontEnd() const noexcept -> FrontEnd;

        // Milliseconds since the Unix epoch, read when the recorder stamped the
        // event. The sole content of the non-golden `meta` member.
        [[nodiscard]] auto wallClockUnixMillis() const noexcept -> int64;
    };

    // Serializes one stamped event to a single-line JSON object. Pure and
    // I/O-free: field order is fixed, the schema version is emitted first and the
    // non-golden `meta` member last, so the output is a stable golden line once
    // stripNonGoldenFields has run over it.
    [[nodiscard]]
    auto serializeTraceEvent(StampedTraceEvent const& event) -> std::string;

    // Removes the non-golden `meta` member from one serialized line. Two runs of
    // the same task at the same seed produce byte-identical lines only after the
    // wall clock is stripped. A line that is not a JSON object, or that carries
    // no top-level `meta` member, is returned unchanged.
    [[nodiscard]]
    auto stripNonGoldenFields(std::string_view line) -> std::string;
}
