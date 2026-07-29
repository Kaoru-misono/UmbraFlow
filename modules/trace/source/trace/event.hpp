#pragma once

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <annotation/recognition.hpp>
#include <annotation/resource.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <optional>
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

    // Every event the host writes. All of them are host-authoritative: a script
    // or the Luau framework can never request one. The framework.* semantic
    // events arrive with the validation state machine in a later stage and are
    // deliberately absent here.
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
        EngineObservationInvalidated,
        TaskNativeCall,
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
        // cycle_open and wait_for_page mint their own ordinal rather than
        // receiving one, so they carry none. The recognizer a find was handed
        // travels on TraceEvent::recognizerId.
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
    };

    // A TraceEvent with the run identity stamped onto it. Only TraceRecorder can
    // build one, which is what makes the stamp unforgettable: no emitter can
    // reach a sink without going through the recorder that owns the sequence
    // counter, the run id, and the generation id.
    class StampedTraceEvent final
    {
        friend class TraceRecorder;

        TraceEvent   m_event;
        uint64       m_sequence;
        TaskRunId    m_runId;
        GenerationId m_generationId;
        int64        m_wallClockUnixMillis;

        StampedTraceEvent(
            TraceEvent event,
            uint64 sequence,
            TaskRunId runId,
            GenerationId generationId,
            int64 wallClockUnixMillis
        );

    public:
        [[nodiscard]]
        auto event() const noexcept UF_LIFETIME_BOUND -> TraceEvent const&;

        [[nodiscard]] auto sequence() const noexcept -> uint64;
        [[nodiscard]] auto runId() const noexcept -> TaskRunId;
        [[nodiscard]] auto generationId() const noexcept -> GenerationId;

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
