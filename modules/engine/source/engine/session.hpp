#pragma once

#include "ports.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <ocr/engine.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <vision/template-match.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::engine
{
    namespace detail
    {
        class EngineSessionIdentity;
    }

    class EngineSession;

    // The largest region one single-line read may hand the OCR engine, in
    // pixels. A ceiling on cost, not on meaning: recognising a strip runs in
    // single-digit milliseconds only while the strip is a strip, and a caller
    // asking to read the whole screen as one line is asking for a detector pass
    // that layout does not run. A 1600x900 target's widest single label
    // measured well under this.
    inline constexpr auto k_maximumSingleLineReadPixels = uint64{1600} * 200U;

    // The largest region one block read may hand the OCR engine, in pixels. A
    // separate ceiling because a block read is asked about a panel on purpose:
    // detection resizes its input to a 960-pixel longest side before the model
    // sees it, so a block read's cost grows with the number of LINES rather than
    // with area, and the caller's own line budget is what bounds it. 4096x4096 is
    // above every desktop this project binds to, and refuses an arithmetic
    // mistake rather than tuning anything.
    inline constexpr auto k_maximumBlockReadPixels = uint64{4096} * 4096U;

    // The longest one capture may block before observe() gives up on it. A
    // conservative placeholder awaiting a real-machine soak: the WGC adapter's
    // own stall fuse is a second, and a compositor silent for twice that will not
    // produce a frame this cycle either. This is the only place the bound is
    // spelled.
    inline constexpr auto k_defaultCaptureTimeout = MonotonicInstant::Duration{
        std::chrono::seconds{2}
    };

    // How many times observe() attempts a capture again after one stalled. A
    // stall is transient in practice -- the same frame that stalled a task run
    // came back on the next attempt in an exploration session -- so treating the
    // first one as the end of the generation ended a run that had already
    // delivered 57 actions (docs/TODO.md, 2026-08-03). Riding it out is the
    // developer's ruling of the same date.
    //
    // Bounded because the other reading of a stall is real: a window that was
    // minimized or destroyed composites nothing and never will, and a session
    // that retried forever would sit on a capture device saying nothing. Five is
    // the ceiling the ruling named. Every attempt carries its own capture
    // deadline, so the worst case is six times k_defaultCaptureTimeout spent
    // before the stall is reported -- which is why the count is small.
    inline constexpr auto k_maximumCaptureStallRetries = 5U;

    // The read-only configuration a session captures once at construction. A
    // transport aggregate: build it with designated initializers. The live
    // fingerprint has no default state and must be supplied at every site.
    struct EngineSessionConfig final
    {
        ProjectFingerprint liveFingerprint;

        // The geometry the page model this session serves was authored at,
        // supplied rather than read off a loaded project because the engine loads
        // none -- the model is layer two's, stated at the top of its own project
        // file (docs/plans/2026-07-31-script-owned-page-model.md 4). The click
        // edge's compatibility refusal and the raw match's consult this.
        ProjectFingerprint projectFingerprint;

        uint64                     maximumPixelComparisons{};
        MonotonicInstant::Duration recognitionTimeout{};

        // Read only when the frame source reports TargetWorld::Live, and
        // shortened to k_defaultMaxActionFrameAge when it asks for longer. Over
        // a recorded target there is no deadline to set, so this is not the
        // knob that decides whether a session may act on old frames -- the
        // frame source is.
        //
        // It is independent of recognitionTimeout on purpose. Recognition and
        // action have different budgets because a cycle may legitimately read
        // without acting, so a recognition that finishes outside this bound is
        // a successful observation whose result cannot be clicked, and the
        // refusal names that at the click.
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};
        MonotonicInstant::Duration captureTimeout{k_defaultCaptureTimeout};

        std::stop_token cancellation{};
    };

    // Where one raw template match landed on one frame, what it scored, and the
    // ceiling that score is read against. It carries no element and no page: a
    // template loaded by the script layer belongs to neither. Whether the score
    // counts as a hit is the caller's judgement and deliberately not decided
    // here.
    //
    // No in-class initializers: PixelRect and PixelPoint have no default state,
    // so every construction site supplies both.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct MatchFound final
    {
        PixelRect  matchedRect;
        PixelPoint clickPixel;

        uint64 sadScore{};
        uint64 maximumSad{};
    };

    // One line of text read off one frame, with where it was read and how sure
    // the engine was. Reading is the one capability that fails open -- a
    // rectangle pointed at the wrong place returns plausible text rather than
    // nothing -- so the confidence is what lets a caller refuse a bad read.
    struct TextReading final
    {
        std::string text{};

        // Where this line is, in FRAME pixels: the rectangle the detector
        // measured under Block, and the one the caller asked about under
        // SingleLine, where nothing was located.
        PixelRect rect;

        // Basis points, as ocr::TextLine reports it.
        uint32 confidenceBp{};
    };

    // A single-use, move-only handle over one captured frame, vended only by
    // EngineSession::observe. The session identity token owns no state and is
    // never dereferenced; it follows a moved EngineSession and lets every
    // operation reject a handle vended by a different session without retaining a
    // borrow into the session object. Consuming by value makes the handle typed
    // single-use, and the invalidated flag fences any surviving alias, including
    // a moved-from one, with StaleObservation.
    class Observation final
    {
        friend class EngineSession;

        Frame                                                m_frame;
        ObservationLease                                     m_lease;
        FrameIdentity                                        m_frameIdentity;
        std::shared_ptr<detail::EngineSessionIdentity const> m_sessionIdentity;
        bool                                                 m_invalidated{false};

        Observation(
            Frame frame,
            ObservationLease lease,
            FrameIdentity frameIdentity,
            std::shared_ptr<detail::EngineSessionIdentity const> sessionIdentity
        ) noexcept;

    public:
        Observation(Observation const&) = delete;
        Observation(Observation&& other) noexcept;
        auto operator=(Observation const&) -> Observation& = delete;
        auto operator=(Observation&& other) noexcept -> Observation&;

        ~Observation() = default;

        // Which capture this handle holds: the only fact about the frame that
        // leaves the engine without spending the observation. It names the
        // frame rather than describing it, so no caller can read a pixel or a
        // measurement through it -- and a caller that has to record WHICH
        // target generation an authorization was taken against, as the Operator
        // snapshot does, has no other source for it.
        [[nodiscard]] auto frameIdentity() const noexcept -> FrameIdentity;
    };

    // One rectangle of one frame's pixels, copied out of the observation that
    // held them; the primitive above it is loaded in the exploration
    // environment only, never for a business script
    // (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
    //
    // The pixels are BGRA8, packed, no row padding: stride is width * 4. A Gray8
    // frame is widened rather than refused -- a capture format the caller did not
    // choose must not change what a verb can answer.
    struct CroppedRegion final
    {
        // The frame these pixels came from; the crop's trace line joins to the
        // capture through it. No default constructor, so a site that leaves it
        // out does not compile.
        FrameIdentity frame;

        uint32 width{};
        uint32 height{};

        std::vector<std::byte> pixels{};
    };

    // The record of one delivered click: the frame it was authorized against and
    // the client-space point posted to the sink.
    struct ActReceipt final
    {
        FrameId            frameId;
        Point<ClientSpace> clickPoint;
    };

    // The record of one delivered keystroke: the frame whose observation it spent
    // and the key posted to the sink. A separate receipt rather than an
    // ActReceipt with an invented coordinate, because a keystroke names none.
    struct KeyReceipt final
    {
        FrameId frameId;
        KeyName key;
    };

    // The record of one delivered wheel scroll: the frame whose observation it
    // spent and the detent count posted to the sink. No point, for KeyReceipt's
    // reason; signed, because direction is half of what was delivered.
    //
    // No in-class initializer for the frame: FrameId has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ScrollReceipt final
    {
        FrameId frameId;
        int32   notches{};
    };

    // The record of one delivered long press: the frame it was authorized
    // against, the client-space point the button went down at, and how long it
    // stayed down. The hold is the only thing separating this receipt from an
    // ActReceipt for the same coordinate.
    //
    // No in-class initializers for the frame or the point: FrameId and Point
    // have no default state, so every construction site supplies both.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct LongPressReceipt final
    {
        FrameId            frameId;
        Point<ClientSpace> pressPoint;

        MonotonicInstant::Duration hold{};
    };

    // The record of one delivered pointer move: the frame it was authorized
    // against and the client-space point the pointer was sent to. Shaped like an
    // ActReceipt because a move is authorized like a click; what it does not
    // carry is any evidence of a press, because there was none.
    struct PointerMoveReceipt final
    {
        FrameId            frameId;
        Point<ClientSpace> movePoint;
    };

    // The record of one delivered drag: the frame it was authorized against, the
    // two client-space points the button went down and came up at, and how long
    // the travel between them took. Both points are here because the far one is
    // the caller's arithmetic rather than anything it measured, so a trace that
    // carries only the start cannot answer where the drag actually ended.
    //
    // No in-class initializers for the frame or the two points: FrameId and
    // Point have no default state, so every construction site supplies all three.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct DragReceipt final
    {
        FrameId            frameId;
        Point<ClientSpace> startPoint;
        Point<ClientSpace> endPoint;

        MonotonicInstant::Duration travel{};
    };

    // The recognition and action pipeline over one bound capture target.
    //
    // Trace lifetime contract: the session stores a non-owning borrow of the
    // run's trace::TraceRecorder, so that a run has one evidence stream and one
    // sequence counter. The composition root MUST keep the recorder in stable
    // storage declared before the session: construct it first, destroy it after,
    // and never relocate it while the session borrows it.
    class EngineSession final
    {
        friend class Observation;

        std::shared_ptr<detail::EngineSessionIdentity const> m_identity;
        std::unique_ptr<IFrameSource>                        m_frameSource;
        std::unique_ptr<IActionSink>                         m_actionSink;

        // Null when the composition root bound no OCR adapter -- the normal state
        // for a path that never reads text, whose weights are tens of megabytes.
        // readText refuses on its own terms rather than the session refusing to
        // exist.
        std::unique_ptr<ocr::IOcrEngine> m_ocrEngine;

        trace::TraceRecorder& m_recorder;
        EngineSessionConfig   m_config;

        EngineSession(
            std::shared_ptr<detail::EngineSessionIdentity const> identity,
            std::unique_ptr<IFrameSource> frameSource,
            std::unique_ptr<IActionSink> actionSink,
            std::unique_ptr<ocr::IOcrEngine> ocrEngine,
            trace::TraceRecorder& recorder,
            EngineSessionConfig config
        ) noexcept;

        [[nodiscard]]
        auto makeRecognitionPolicy() const -> RecognitionPolicy;

        [[nodiscard]]
        auto emit(trace::TraceEventSpec const& event) -> Status;

        // The two gates every verb taking an existing observation opens with: the
        // handle came from this session, and it has not been consumed or moved
        // from. `verb` names the caller so the refusal reads as that verb's own.
        [[nodiscard]]
        auto ensureUsable(
            Observation const& observation,
            std::string_view verb
        ) const -> Status;

        // The refusals every delivering verb opens with, in the order they
        // outrank one another: a requested stop before the sink is touched, then
        // the two handle gates. `verb` and `cancelMessage` name the caller so
        // each refusal reads as that verb's own.
        [[nodiscard]]
        auto beginDelivery(
            Observation const& observation,
            std::string_view verb,
            std::string_view cancelMessage
        ) const -> Status;

        // The input one verb that names no screen position delivers: a keystroke
        // or a wheel count. A closed pair, so the trace field naming the verb and
        // the sink call performing it follow from the alternative rather than
        // from anything a caller passes alongside it.
        using UnaimedInput = std::variant<KeyName, int32>;

        [[nodiscard]]
        static auto stampInput(
            trace::TraceEventSpec event,
            UnaimedInput input
        ) -> trace::TraceEventSpec;

        // The terminal line a refused delivery writes, so no verb can return a
        // failure that leaves its frame's stream ending at
        // engine.action_authorized. `input` is present exactly for the verbs
        // that carry one; a coordinate verb's point is already on the
        // authorization line above.
        [[nodiscard]]
        auto rejectAction(
            FrameIdentity identity,
            Error const& error,
            std::optional<UnaimedInput> input
        ) -> Status;

        // Everything a coordinate-bearing verb does between its opening refusals
        // and its own sink call: the live geometry must still be what the page
        // model was authored against, the observation's lease must still be
        // valid, the authorization is written, the point is carried into client
        // space, and the bound target instance is revalidated immediately before
        // the post. One place, because a coordinate verb short one of these
        // gates is a second and laxer path to the same window.
        [[nodiscard]]
        auto authorizeCoordinate(
            Observation const& observation,
            PixelPoint point
        ) -> Result<Point<ClientSpace>>;

        // The delivery sequence pressKey and scroll share, which is one
        // sequence: the opening refusals, the target-instance revalidation, the
        // sink post, the spent handle, `deliveredEventType` and the invalidation
        // line, with every fallible step writing engine.action_rejected before
        // it returns. It holds no fingerprint check, lease-age refusal or point
        // transform, because neither verb names a coordinate.
        [[nodiscard]]
        auto deliverUnaimed(
            Observation&& observation,
            std::string_view verb,
            std::string_view cancelMessage,
            std::string_view deliveredEventType,
            UnaimedInput input
        ) -> Result<FrameIdentity>;

        // One frame, with a stall ridden out up to k_maximumCaptureStallRetries
        // times. It lives here rather than in observe() because a caller of
        // observe() must not be able to see a stall it did not survive: either a
        // frame comes back, or the stall is the answer and the retries are spent.
        // Every retry writes an engine.capture_retried line, so a run that rode
        // one out is not indistinguishable from a run that never stalled.
        [[nodiscard]]
        auto captureRidingOutStalls() -> Result<Frame>;

        // What one OCR call produced, before readText decides what to report.
        // The trace line carries the engine identity and the duration even when
        // no line was found, so neither can be folded into the lines.
        struct ReadAttempt final
        {
            std::vector<TextReading> lines{};

            std::string engineId{};
            uint64      durationMicros{};
        };

        // One timed OCR call over `rect` of `frame`, with the region measured
        // against the pixel ceiling `layout` is charged at. `maximumLines`
        // bounds a Block read and is inert under SingleLine, which answers at
        // most one line by construction.
        [[nodiscard]]
        auto readTextOnFrame(
            Frame const& frame,
            PixelRect rect,
            ocr::TextLayout layout,
            uint32 maximumLines
        ) const -> Result<ReadAttempt>;

    public:
        EngineSession(EngineSession const&) = delete;
        EngineSession(EngineSession&&) noexcept = default;
        auto operator=(EngineSession const&) -> EngineSession& = delete;
        auto operator=(EngineSession&&) noexcept -> EngineSession& = delete;

        ~EngineSession() = default;

        // A session built without `ocrEngine` refuses readText rather than
        // pretending to read.
        [[nodiscard]]
        static auto create(
            std::unique_ptr<IFrameSource> frameSource,
            std::unique_ptr<IActionSink> actionSink,
            trace::TraceRecorder& recorder,
            EngineSessionConfig config,
            std::unique_ptr<ocr::IOcrEngine> ocrEngine = nullptr
        ) -> Result<EngineSession>;

        [[nodiscard]]
        auto observe() -> Result<Observation>;

        // Searches `searchRoi` of the frame `observation` holds for
        // `templateImage`, reporting the best position it found or nothing when
        // the region could hold no candidate at all. No catalog lookup, page,
        // appearance or threshold: all four are layer two's. A control stop --
        // comparison budget, recognition deadline, requested cancel -- is a
        // FAILURE and never a miss, because a search that stopped looking has not
        // established that the template is absent.
        [[nodiscard]]
        auto matchTemplate(
            Observation const& observation,
            GrayTemplateImage const& templateImage,
            PixelRect searchRoi
        ) -> Result<std::optional<MatchFound>>;

        // Reads the text in `rect` of the frame `observation` holds, under the
        // layout the CALLER asserts that rectangle has, and hands back one entry
        // per line with the rectangle the frame held it in.
        //
        // ocr::TextLayout::SingleLine asserts the region holds exactly one line
        // and runs no detection: cheaper, and immune to a detector that splits
        // one label in two, so it stays the right layout wherever the caller can
        // draw the rectangle. Its answer is at most one line, whose rect is the
        // one asked about because nothing was located. ocr::TextLayout::Block is
        // for the region nobody can draw inside -- a scrolling grid, an option
        // card whose body is one to three lines -- and every line it returns
        // carries the rect the DETECTOR measured, in frame pixels.
        //
        // Finding no lines is an empty vector; a failure means the read could
        // not be attempted. A Block region holding more than `maximumLines`
        // FAILS with RecognitionIncomplete and reads none of them, because a
        // caller handed the first n lines would conclude its target is absent
        // from a region nobody finished looking at; the refusal costs one
        // detection pass rather than one recognition per line, and `maximumLines`
        // is inert under SingleLine. See ocr::ReadSpec::maximumLines.
        //
        // A Block line may carry EMPTY text -- the detector found text and the
        // recogniser failed to read it -- which keeps the returned count equal to
        // the recognitions performed, as a caller charging a budget needs.
        [[nodiscard]]
        auto readText(
            Observation const& observation,
            PixelRect rect,
            ocr::TextLayout layout,
            uint32 maximumLines
        ) -> Result<std::vector<TextReading>>;

        // Copies `rect` of the frame `observation` holds and hands the pixels
        // back. It does NOT spend the observation and does not touch the action
        // sink -- reading pixels changes nothing on the target, so the same cycle
        // can go on to click. It refuses only a rectangle the frame does not
        // contain.
        //
        // It emits NOTHING, alone among the verbs here: the honest line for a
        // crop is annotation.region_saved, written by the front-end layer that
        // also knows the encoded bytes and their hash. An engine.* line here
        // would record the same act twice under two names.
        [[nodiscard]]
        auto cropRegion(
            Observation const& observation,
            PixelRect rect
        ) -> Result<CroppedRegion>;

        // Delivers one click at `point`, spending `observation`.
        //
        // It writes engine.action_delivered on every stream. This layer knows
        // which target it posted to and nothing about which front end asked, so
        // it cannot say whether a Binding stood behind the coordinate; the
        // caller that does know writes that line itself, and on the exploration
        // stream it is annotation.click_delivered
        // (task/task-context.cpp, annotationActionEvent).
        //
        // It enforces: a requested stop refuses before any sink call, a foreign
        // handle is an InternalInvariant, a consumed handle is StaleObservation,
        // the live fingerprint must match the project's, the observation's lease
        // must still be valid at delivery, the bound target instance is
        // revalidated immediately before the post, and the observation is spent
        // so one frame delivers at most one input.
        //
        // It does NOT enforce that a resolved page authorises the element: there
        // is no element and no page here. Naming a bare coordinate is therefore
        // the trusted framework's privilege and never a business script's --
        // modules/task/runtime/observe.luau enforces "only click what this page
        // authorises" (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
        [[nodiscard]]
        auto clickPoint(
            Observation&& observation,
            PixelPoint point
        ) -> Result<ActReceipt>;

        // Delivers one keystroke, spending `observation`.
        //
        // Shared with clickPoint() and for its reasons: a requested stop refuses
        // before any sink call; a foreign handle is an InternalInvariant and an
        // invalidated one is StaleObservation; the bound target instance is
        // revalidated immediately before the post, closing the HWND-reuse window;
        // and the observation is spent, because a keystroke changes the screen
        // exactly as a click does.
        //
        // Deliberately NOT shared, because a keystroke names no screen position:
        //   - no fingerprint check. That asks whether a coordinate measured
        //     against this project still means what it meant, and a virtual key
        //     names no coordinate.
        //   - no lease-age refusal. A lease bounds a coordinate's shelf life;
        //     enforcing it here would push an operator to widen --max-frame-age
        //     for the whole run to get keys through, weakening every click.
        //
        // Requiring an observation at all is what orders the keystroke against a
        // cycle and lets the trace join it to a frame.
        [[nodiscard]]
        auto pressKey(
            Observation&& observation,
            KeyName key
        ) -> Result<KeyReceipt>;

        // Delivers one wheel scroll of `notches` detents, spending `observation`.
        //
        // Its authorization contract is pressKey()'s, not clickPoint()'s, for
        // pressKey()'s reason: the verb names no screen position. The fingerprint
        // check and the lease-age refusal are absent for that reason too.
        //
        // The lease still travels to the sink, which is a delivery requirement
        // rather than an authorization one -- see IActionSink::scroll, where the
        // open question about aiming a scroll is recorded.
        [[nodiscard]]
        auto scroll(
            Observation&& observation,
            int32 notches
        ) -> Result<ScrollReceipt>;

        // Delivers one long press at `point` for `hold`, spending `observation`.
        //
        // Its authorization contract is clickPoint's clause for clause, because a
        // long press names a coordinate the caller measured off this frame:
        // requested stop, foreign handle, consumed handle, live fingerprint,
        // lease validity at delivery, target-instance revalidation before the
        // post, and the spent observation. Nothing here may be looser than a
        // click -- a second and laxer path to the same window is the hole this
        // closes.
        //
        // It spends the observation deliberately: a delivered long press changes
        // the screen, so reading the result costs a fresh observation rather than
        // reusing the frame that authorized the press.
        //
        // `hold` is the caller's with no default at this layer or above; see
        // IActionSink::longPress. Bounding it belongs to the host surface a
        // script reaches, where a refusal can name what the author wrote.
        [[nodiscard]]
        auto longPress(
            Observation&& observation,
            PixelPoint point,
            MonotonicInstant::Duration hold
        ) -> Result<LongPressReceipt>;

        // Moves the pointer to `point`, pressing nothing, and spends
        // `observation`.
        //
        // Its authorization contract is clickPoint's clause for clause -- stop
        // requested, foreign handle, consumed handle, live fingerprint, lease
        // validity at delivery, target-instance revalidation before the post, and
        // the spent observation -- and NOTHING is dropped. What the relaxations in
        // pressKey and scroll rest on is that those verbs name no screen position;
        // a move names one, so every gate that asks whether a coordinate still
        // means what it meant applies here unchanged.
        //
        // It spends the observation for the reason every delivering verb does: a
        // pointer message changes what the target believes is hovered, so the
        // frame that authorized the move no longer describes the screen. The
        // caller opens a second observation for the scroll that follows, which is
        // also what keeps "one frame delivers at most one input" true.
        [[nodiscard]]
        auto movePointer(
            Observation&& observation,
            PixelPoint point
        ) -> Result<PointerMoveReceipt>;

        // Delivers one drag from `start` to `end` over `travel`, spending
        // `observation`.
        //
        // Its authorization contract is longPress's clause for clause: requested
        // stop, foreign handle, consumed handle, live fingerprint, lease validity
        // at delivery, target-instance revalidation before the post, and the
        // spent observation. Every one of those is a fact about the FRAME, so it
        // is asked once for the drag rather than once per endpoint -- see the
        // definition for why asking twice would write two authorizations for one
        // delivered act.
        //
        // What `end` gets is the per-point half: it is converted like `start`,
        // and refused before any authorization is written if that conversion
        // fails. Its client-area bound is checked at the layer that knows the
        // live client size, before the button goes down; see IActionSink::drag.
        //
        // It spends the observation for longPress's reason, and more plainly: a
        // drag is what moves the thing being looked at, so the frame that
        // authorized it describes a screen that no longer exists.
        //
        // `travel` is the caller's with no default at this layer or above; see
        // IActionSink::drag. Bounding it belongs to the host surface a script
        // reaches, where a refusal can name what the author wrote.
        [[nodiscard]]
        auto drag(
            Observation&& observation,
            PixelPoint start,
            PixelPoint end,
            MonotonicInstant::Duration travel
        ) -> Result<DragReceipt>;
    };
}
