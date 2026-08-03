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
#include <vector>

namespace uf::engine
{
    namespace detail
    {
        class EngineSessionIdentity;
    }

    class EngineSession;

    // The largest region one cycle_read may hand the OCR engine, in pixels. A
    // ceiling on cost, not on meaning: recognising a strip runs in single-digit
    // milliseconds only while the strip is a strip, and a script asking to read
    // the whole screen is asking for a detector pass this adapter does not run.
    // A 1600x900 target's widest single label measured well under this.
    inline constexpr auto k_maximumReadPixels = uint64{1600} * 200U;

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
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};
        MonotonicInstant::Duration captureTimeout{k_defaultCaptureTimeout};

        std::stop_token cancellation{};
    };

    // Where one raw template match landed on one frame, what it scored, and the
    // ceiling that score is read against. It carries no element and no page: a
    // template loaded by the script layer belongs to neither. Whether the score
    // counts as a hit is the caller's judgement and deliberately not decided
    // here.
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

        // The region that was read, in frame pixels, as the caller asked for it.
        // A read has no score to locate it by.
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

    };

    // One rectangle of one frame's pixels, copied out of the observation that
    // held them. Every other verb here hands back an answer about the frame; this
    // one hands back the evidence, because its caller is the exploration
    // front-end, where an agent with no model yet must be able to look at the
    // screen (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2). The
    // primitive above it is loaded in that environment only, never for a business
    // script.
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
    struct ScrollReceipt final
    {
        FrameId frameId;
        int32   notches{};
    };

    // The record of one delivered long press: the frame it was authorized
    // against, the client-space point the button went down at, and how long it
    // stayed down. The hold is the only thing separating this receipt from an
    // ActReceipt for the same coordinate.
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

    // The recognition and action pipeline over one bound capture target.
    //
    // Trace lifetime contract: the session stores a non-owning borrow of the
    // run's trace::TraceRecorder, so that a run has one evidence stream and one
    // sequence counter. `task::TaskHost::startTask` holds the recorder in a
    // std::unique_ptr local declared before the session, and the recorder is
    // non-movable. Any other owner MUST reproduce both -- construct the recorder
    // before the session, destroy it after, never relocate it.
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
        auto emit(trace::TraceEvent const& event) -> Status;

        // The two gates every verb taking an existing observation opens with: the
        // handle came from this session, and it has not been consumed or moved
        // from. `verb` names the caller so the refusal reads as that verb's own.
        [[nodiscard]]
        auto ensureUsable(
            Observation const& observation,
            std::string_view verb
        ) const -> Status;

        // What one OCR call produced, before readText decides whether it is a
        // reading or only a trace line. The trace line carries the engine
        // identity and the duration even when no text was found, so neither can
        // be folded into the optional reading.
        struct ReadAttempt final
        {
            std::optional<TextReading> line{};

            std::string engineId{};
            uint64      durationMicros{};
        };

        // readTextLines' equivalent. Separate rather than a vector bolted onto the
        // one above: a single-line read has at most one answer by contract, and a
        // container would make every reader of that path ask how many.
        struct BlockReadAttempt final
        {
            std::vector<TextReading> lines{};

            std::string engineId{};
            uint64      durationMicros{};
        };

        [[nodiscard]]
        auto readTextOnFrame(
            Frame const& frame,
            PixelRect rect
        ) const -> Result<ReadAttempt>;

        [[nodiscard]]
        auto readTextLinesOnFrame(
            Frame const& frame,
            PixelRect rect,
            uint32 maximumLines
        ) const -> Result<BlockReadAttempt>;

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

        // Reads the text in `rect` of the frame `observation` holds. The caller
        // asserts the region holds ONE line: the adapter this project ships runs
        // no detection, so a caller that does not know the line count gets a
        // refusal rather than a silent single-line read of several. Finding no
        // text is an empty optional; a failure means the read could not be
        // attempted.
        [[nodiscard]]
        auto readText(
            Observation const& observation,
            PixelRect rect
        ) -> Result<std::optional<TextReading>>;

        // Finds every line of text inside `rect` on the frame `observation`
        // holds and reads each one, with its own rectangle in FRAME pixels.
        //
        // A second layout and not a replacement: readText stays the right verb
        // wherever the caller can draw the rectangle, being cheaper and immune to
        // a detector that splits one label in two. This one is for the region
        // nobody can draw inside -- a continuously scrolling grid, where a name's
        // position is a fact about the frame rather than about the model.
        //
        // A region holding more than `maximumLines` FAILS with
        // RecognitionIncomplete and reads none of them, because a caller handed
        // the first n lines would conclude its target is absent from a region
        // nobody finished looking at. The refusal costs one detection pass rather
        // than one recognition per line; see ocr::ReadSpec::maximumLines.
        //
        // Finding no lines is an empty vector. A returned line may carry EMPTY
        // text -- the detector found text and the recogniser failed to read it --
        // which keeps the returned count equal to the recognitions performed, as
        // a caller charging a budget needs.
        [[nodiscard]]
        auto readTextLines(
            Observation const& observation,
            PixelRect rect,
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
        // Which line the delivery is written under is decided here and nowhere
        // else: engine.action_delivered on a task or operator stream,
        // annotation.click_delivered on the exploration stream, where the caller
        // is an agent naming a coordinate with no element and no page behind it.
        // The stream validator refuses the other spelling on each stream
        // (docs/plans/2026-08-01-agent-front-end-and-exploration.md 1).
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
        // means what it meant applies here unchanged. That the verb presses
        // nothing shortens the list of what a mis-aimed one can do, not the list
        // of what makes it mis-aimed.
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
    };
}
