#pragma once

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <annotation/recognition.hpp>
#include <annotation/resource.hpp>

#include <domain/error.hpp>
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

    // The one evidence stream every layer writes into. It replaces the former
    // engine-trace/v1 and task-trace/v1, which had no join key at all: the task
    // stream named no frame, the engine stream carried no run or generation id,
    // and neither wrote a timestamp, so the three layers could not be strung into
    // one ordered run. All three are closed here -- every line carries the run and
    // generation id plus a monotonic sequence, engine.* lines carry the frame
    // identity, and task.native_call carries the retained observation the verb was
    // handed, which is what still names the frame when a host-side guard fails the
    // verb before the engine ever sees it. The version is emitted first on every
    // line so a downstream consumer can reject a line it does not understand.
    inline constexpr auto k_traceSchema = std::string_view{"umbraflow-trace/v1"};

    // The single JSON member holding every field that may legitimately differ
    // between two runs of the same task at the same seed. It exists so the wall
    // clock can be recorded without making a trace unreproducible: a golden
    // comparison runs stripNonGoldenFields over each line first, and everything
    // outside `meta` is therefore part of the reproducible record.
    inline constexpr auto k_nonGoldenMember = std::string_view{"meta"};

    // WHAT THIS STREAM IS, AND WHAT IT IS NOT.
    //
    // It is an AUDIT LOG: what a run did, in order, with enough identity on each
    // line to attribute it. It is NOT a replay log. Replaying a run rests on the
    // seed recorded in run.started plus the observation sequence a test supplies
    // to its fake frame source -- never on a production trace. Nothing here
    // records a frame's pixels, a page's full anchor evidence, or the arguments
    // of a project call, so no reader can reconstruct a run from these lines and
    // nobody should build a replayer that tries. The task.* and framework.*
    // events below are the part most likely to be mistaken for one, because they
    // read like a program transcript; they are a record of the framework's own
    // decisions, kept so a failure can be explained after the fact.
    //
    // Every event is host-authoritative in the sense that only the host writes a
    // line and only the host stamps it. The run.*, engine.* and task.* events
    // additionally cannot be REQUESTED from Luau at all. The framework.* events
    // can: the trusted Luau framework asks for one through the private `emit`
    // primitive, and the host validates the request against the state machine in
    // stream-validator.hpp before any of it reaches a sink. A project script can
    // request none of them -- it cannot name the primitive.
    //
    // This enum and the four outcome enums below deliberately leave 0 without an
    // enumerator, and the members holding them carry no in-class initializer. A
    // trace is an audit record, so the one failure mode it must not have is a
    // construction site that omits a discriminator and silently records a
    // plausible success. Aggregate initialization value-initializes an omitted
    // scoped-enum member whether or not a default member initializer exists, so
    // dropping the initializer alone cannot make that a compile error; leaving 0
    // outside the domain makes it a loud one instead, at the serializer's
    // UF_UNREACHABLE, before the bogus line reaches a sink.
    //
    // framework.subtask_entered / subtask_exited are P1 and deliberately absent:
    // cross-file reuse does not exist yet, so a subtask event could only ever be
    // written by nothing.
    // Which front-end drove the run this line belongs to.
    //
    // It exists because the capability surface has two consumers at the same
    // level -- the trusted Luau framework a task runs on, and an operator sending
    // commands from outside -- and without the attribution no reader of a trace
    // can answer "did the task do this, or did the operator". That question is
    // asked of every line, so the answer is part of the STAMP rather than of the
    // event: TraceRecorder carries one value for the whole run and writes it onto
    // every line, so no emitter can forget it and none can claim the other
    // front-end's work.
    //
    // It is also what makes the two mutually exclusive rather than merely
    // documented. TaskHost latches one of these per generation the first time a
    // front-end drives it and refuses the other, and the latched value is what it
    // hands the recorder -- so a stream's attribution and the exclusion that
    // produced it are the same fact and cannot disagree.
    enum class FrontEnd : uint8
    {
        // A project task running on the trusted Luau framework, driven by
        // `umbra-flow run`.
        Task = 1,

        // An operator sending commands from outside the process, driven by
        // `umbra-flow drive`.
        Operator,
    };

    enum class TraceEventKind : uint8
    {
        RunStarted = 1,
        RunResourcesValidated,
        RunFinished,
        EngineObserved,
        EnginePageResolved,
        EngineActionFound,
        EngineActionAuthorized,
        EngineActionRejected,
        EngineActionDelivered,
        EngineKeyDelivered,
        EngineObservationInvalidated,
        TaskNativeCall,
        FrameworkStepStarted,
        FrameworkStepFinished,
        FrameworkRetryAttempt,
        FrameworkRetryBackoff,
        FrameworkInterruptMatched,
        FrameworkInterruptHandled,
        FrameworkInterruptExhausted,
        FrameworkSettled,
    };

    // How one page-resolution attempt ended. Resolved / Unknown / Ambiguous were
    // three separate event kinds in engine-trace/v1; Stopped and Failed carried
    // the kinds RecognitionStopped and Failure, which named the stage only by
    // where they happened to sit in the stream. Folding all five into the outcome
    // of engine.page_resolved keeps every distinction and adds one the old schema
    // could not express: which stage a stop or failure came from.
    enum class PageResolution : uint8
    {
        Resolved = 1,
        Unknown,
        Ambiguous,
        Stopped,
        Failed,
    };

    // How one action-target search ended, on the same reasoning as PageResolution:
    // Found / Absent were separate kinds, Stopped and Failed were the stage-blind
    // RecognitionStopped and Failure.
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

    // One trace record, without the identity the recorder stamps on it. Related
    // fields are grouped into small optional sub-structs so a call site sets one
    // group rather than a dozen loose optionals; the groups are a C++ shape only,
    // and serializeTraceEvent flattens them into one JSON object. This is a
    // transport aggregate: build it with designated initializers and set only the
    // groups the event carries.
    struct TraceEvent final
    {
        // The result of one page-resolution attempt. pageId is present only for
        // Resolved; scores carry the evidence behind whichever outcome it was.
        struct Page final
        {
            // One evaluated page's contribution to the attempt: the page the
            // resolver considered, whether it survived as a candidate, and the
            // required anchor that scored worst against its own ceiling. The
            // worst required anchor is what a non-resolution turns on, so it is
            // what "why did my page not resolve" actually asks for, while the
            // full per-anchor evidence stays off the wire because a trace line is
            // read rather than queried. A page rejected by a forbidden anchor
            // still reports its required anchors, so candidate=false with a
            // passing score means a forbidden anchor hit.
            //
            // The anchor triple is absent only when the page declares no required
            // anchor. Nothing defaults: PageId has no default constructor, and
            // omitting `candidate` would assert a page was ruled out.
            struct Score final
            {
                annotation::PageId                      pageId;
                bool                                    candidate;
                std::optional<annotation::RecognizerId> worstAnchor{};
                std::optional<uint64>                   worstAnchorSad{};
                std::optional<uint64>                   worstAnchorMaximumSad{};
            };

            PageResolution                    outcome;
            std::optional<annotation::PageId> pageId{};
            std::vector<Score>                scores{};
        };

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

        // What this run is: the addressed task, the bytes it was compiled from,
        // the trusted framework and Luau compiler those bytes ran on, and the seed
        // its RNG draws from. Every member is always supplied together on
        // run.started, which is the only place a run's identity is written down --
        // so two runs against different framework builds must not produce the same
        // line.
        //
        // The plan's remaining two run.started fields, the resource snapshot hash
        // and the config digest, are deliberately absent: task::TaskHost now owns
        // the run and still computes neither, and recording an empty string for
        // them would be a false attribution rather than a missing one.
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
            std::vector<std::string> recognizers{};
            std::vector<std::string> pages{};
        };

        // One call from the script layer into the host capability surface, with
        // the argument identity the primitive was handed. The identity is what
        // makes a native call joinable without relying on position: a host-side
        // guard -- the cycle-ledger lookup behind cycle_page, cycle_find,
        // cycle_click and cycle_close -- fails the call before the engine is
        // reached, so its task.native_call is the only line the failure produces
        // and nothing else would say which cycle the script tried to use.
        // cycle_open mints its own ordinal rather than receiving one, so it
        // carries none. The recognizer a find was handed travels on
        // TraceEvent::recognizerId.
        struct NativeCall final
        {
            std::string       verb{};
            NativeCallOutcome outcome;

            // The ordinal of the cycle whose ticket the primitive was handed.
            std::optional<uint64> cycleOrdinal{};

            // The cycle ordinal the hit handed to cycle_click carries. A hit is
            // usable only while the cycle that found it is still the open one, so
            // the two ordinals agree on every delivered click and differ exactly
            // when a hit from a spent cycle was refused -- which is that
            // refusal's entire content, so both reach the wire.
            std::optional<uint64> hitCycleOrdinal{};

            // The pause a settle declared, in whole milliseconds. A settle
            // reaches no engine verb, so this line is the only evidence it
            // happened, and the duration is part of the replay input rather than
            // a detail: a run that paused two seconds and one that paused ten are
            // different runs. Absent on every other verb.
            std::optional<uint64> durationMillis{};
        };

        // One framework semantic event's payload. The framework asks for these
        // through `emit`, so this is the only group whose contents originate
        // outside the host -- which is exactly why every field of it is checked
        // at the request boundary (see stream-validator.hpp) rather than trusted.
        //
        // One struct serves all eight framework kinds rather than eight, because
        // the fields are the same two questions asked of different scopes: what
        // is this scope called, and how many of something does it count. Which
        // members a kind carries is fixed by the validator, so an absent member
        // is a refused event rather than a silently empty line.
        struct Framework final
        {
            // A step's name or an interrupt's id. Length, character set and the
            // total open-step budget are enforced before the event is admitted;
            // an over-budget label is REJECTED, never truncated, because a
            // truncated name silently addresses a different step.
            std::string label{};

            // The retry attempt this pass is, and the total the policy declared.
            // Both are present on framework.retry_attempt and absent everywhere
            // else.
            std::optional<uint64> attempt{};
            std::optional<uint64> attempts{};

            // The pause a framework.retry_backoff or framework.settled declared,
            // in whole milliseconds.
            std::optional<uint64> durationMillis{};
        };

        TraceEventKind kind;

        // The capture this event belongs to. Present on every engine.* event, and
        // the join key that lets a reader tie an engine event to the frame it
        // observed. It is annotation's own FrameIdentity rather than a trace-local
        // copy of its three fields: the identity has no default constructor, so a
        // site that leaves any part of it out does not compile, and there is one
        // definition of "which frame" instead of two that can drift.
        std::optional<annotation::FrameIdentity> frame{};

        std::optional<Page>       page{};
        std::optional<Action>     action{};
        std::optional<Run>        run{};
        std::optional<Resources>  resources{};
        std::optional<NativeCall> nativeCall{};
        std::optional<Framework>  framework{};

        // Fields that cut across the groups above: the recognizer a page stop, an
        // action search or an authorization refusal names; why a recognition
        // search stopped early; how the run ended; and the failure detail any
        // event may carry.
        std::optional<annotation::RecognizerId> recognizerId{};
        std::optional<SadSearchStopReason>      stopReason{};
        std::optional<RunOutcome>               runOutcome{};
        std::optional<AutomationErrorKind>      errorKind{};
        std::optional<std::string>              message{};
        std::optional<Point<ClientSpace>>       clickClient{};

        // The key one engine.key_delivered posted, as the target's UI prints it.
        // It is the whole content of that event: a keystroke names no coordinate,
        // so there is no clickClient to record and the name is the only thing that
        // distinguishes one delivered key from another.
        std::optional<KeyName> key{};
    };

    // A TraceEvent with the run identity stamped onto it. Only TraceRecorder can
    // build one, which is what makes the stamp unforgettable: no emitter can
    // reach a sink without going through the recorder that owns the sequence
    // counter, the run id, and the generation id.
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

        // The framework step scope that was open when this event was written,
        // outermost first, as the validation state machine knew it. It is part
        // of the STAMP rather than of the event, for the same reason the sequence
        // is: an emitter does not get to say which steps it is inside. That is
        // what makes design section 12's "every task.native_call falls inside the
        // step scope open at the time" true by construction -- the scope is read
        // off the state machine at the instant the line is written, so a call can
        // neither claim a step that is not open nor omit one that is.
        [[nodiscard]]
        auto openSteps() const noexcept UF_LIFETIME_BOUND
            -> std::span<std::string const>;

        [[nodiscard]] auto sequence() const noexcept -> uint64;
        [[nodiscard]] auto runId() const noexcept -> TaskRunId;
        [[nodiscard]] auto generationId() const noexcept -> GenerationId;

        // Which front-end drove the run this line belongs to. Part of the stamp
        // for the reason the sequence is: an emitter does not get to say who it
        // is. See FrontEnd.
        [[nodiscard]] auto frontEnd() const noexcept -> FrontEnd;

        // Milliseconds since the Unix epoch, read from the system clock when the
        // recorder stamped the event. It is the sole content of the non-golden
        // `meta` member; see k_nonGoldenMember.
        [[nodiscard]] auto wallClockUnixMillis() const noexcept -> int64;
    };

    // Serializes one stamped event to a single-line JSON object. Pure and
    // I/O-free: the field order is fixed, the schema version is emitted first and
    // the non-golden `meta` member last, so the output is a stable golden line
    // once stripNonGoldenFields has run over it.
    [[nodiscard]]
    auto serializeTraceEvent(StampedTraceEvent const& event) -> std::string;

    // Removes the non-golden `meta` member from one serialized line, returning
    // the remainder unchanged. This is the documented way to compare traces: two
    // runs of the same task at the same seed produce byte-identical lines only
    // after the wall clock has been stripped. A line that is not a JSON object,
    // or that carries no top-level `meta` member, is returned unchanged.
    [[nodiscard]]
    auto stripNonGoldenFields(std::string_view line) -> std::string;
}
