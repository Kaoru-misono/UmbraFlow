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

namespace uf::engine
{
    namespace detail
    {
        class EngineSessionIdentity;
    }

    class EngineSession;

    // The largest region one cycle_read may hand the OCR engine, in pixels.
    //
    // It is a ceiling on cost rather than on meaning: a read is the one engine
    // verb whose price is set by an argument a script chose, and recognising a
    // strip runs in single-digit milliseconds only while the strip is a strip.
    // A script asking to read the whole screen is asking for a detector pass
    // this adapter does not run, so the refusal is what tells it so.
    //
    // CALIBRATION: a 1600x900 target's widest single label measured well under
    // this; it is deliberately far above any line and far below a full frame.
    inline constexpr auto k_maximumReadPixels = uint64{1600} * 200U;

    // The longest one capture may block before observe() gives up on it. It is a
    // conservative placeholder awaiting the first real daily and a real-machine
    // soak: the WGC adapter's own stall fuse is a second, and a compositor that
    // has produced nothing for twice that is not going to produce a frame this
    // cycle either. Calibrate it here -- it is the only place the bound is
    // spelled -- once measured capture latencies exist.
    inline constexpr auto k_defaultCaptureTimeout = MonotonicInstant::Duration{
        std::chrono::seconds{2}
    };

    // The read-only configuration a session captures once at construction. It is
    // a transport aggregate: build it with designated initializers. The live
    // fingerprint has no default state, so it must be supplied at every
    // construction site; every other field carries a safe in-class default.
    struct EngineSessionConfig final
    {
        ProjectFingerprint liveFingerprint;

        // The geometry the page model this session serves was authored at.
        //
        // It is supplied rather than read off a loaded project because the
        // engine no longer loads one: the model is layer two's, stated at the top
        // of its own project file, and the host reads it there
        // (docs/plans/2026-07-31-script-owned-page-model.md 4). Every guarantee
        // that used to consult the catalog's fingerprint -- the click edge's
        // compatibility refusal and the raw match's -- consults this instead, so
        // the check is unchanged and only its source moved.
        ProjectFingerprint projectFingerprint;

        uint64                     maximumPixelComparisons{};
        MonotonicInstant::Duration recognitionTimeout{};
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};
        MonotonicInstant::Duration captureTimeout{k_defaultCaptureTimeout};

        std::stop_token cancellation{};
    };

    // Where one raw template match landed on one frame, what it scored, and the
    // ceiling that score is read against.
    //
    // It carries no element and no page, because a template loaded by the script
    // layer belongs to neither -- and after the page model moved to Luau there is
    // no other shape of evidence left in this module. What it carries is a
    // rectangle this frame produced, a distance, and the maximum that distance
    // could have been, so the click that consumes it is fenced by the same
    // same-frame, lease and fingerprint checks. Whether the score counts as a hit
    // is the caller's judgement and deliberately not decided here.
    struct MatchFound final
    {
        PixelRect  matchedRect;
        PixelPoint clickPixel;

        uint64 sadScore{};
        uint64 maximumSad{};
    };

    // One line of text read off one frame, with where it was read and how sure
    // the engine was.
    //
    // The confidence is not decoration. Reading is the one capability that fails
    // open -- a rectangle pointed at the wrong place returns plausible text
    // rather than nothing -- so a caller that cannot tell "there really is text
    // here" from "the model guessed" has no way to refuse a bad read.
    struct TextReading final
    {
        std::string text{};

        // The region that was read, in frame pixels, as the caller asked for it.
        // A read has no score to locate it by, so the rectangle is the whole of
        // "where did this come from".
        PixelRect rect;

        // The model's own confidence in basis points, as ocr::TextLine reports
        // it.
        uint32 confidenceBp{};
    };

    // A single-use, move-only handle over one captured frame, vended only by
    // EngineSession::observe. It carries the frame, its lease, its frame
    // identity, and a shared immutable token identifying the session that
    // produced it.
    //
    // The token owns no session state and is never dereferenced. It follows a
    // moved EngineSession and lets every operation reject a handle vended by a
    // different session without retaining a borrow into the session object.
    // Consuming the handle by value makes it typed single-use, and the invalidated
    // flag fences any surviving alias at runtime. The move operations copy the
    // members into the destination and invalidate the source, so a moved-from
    // handle is dead exactly like a consumed one and fails StaleObservation on
    // any later use.
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

    // The record of one delivered click: the frame it was authorized against and
    // the client-space point posted to the sink.
    struct ActReceipt final
    {
        FrameId            frameId;
        Point<ClientSpace> clickPoint;
    };

    // The record of one delivered keystroke: the frame whose observation it spent
    // and the key posted to the sink. There is no point, because a keystroke has
    // none -- which is the whole reason it is a separate receipt rather than an
    // ActReceipt with an invented coordinate.
    struct KeyReceipt final
    {
        FrameId frameId;
        KeyName key;
    };

    // The recognition and action pipeline over one bound capture target.
    //
    // Trace lifetime contract: the session does NOT own its trace sink. It stores
    // a non-owning borrow of the run's trace::TraceRecorder, which is owned by
    // `task::TaskHost::startTask`: that function holds the recorder in a
    // std::unique_ptr local declared before the session it builds, so the session
    // is destroyed first on the normal path and on every early return, and the
    // recorder is non-movable so its address cannot drift while the session
    // borrows it. Any other owner MUST reproduce both properties -- construct the
    // recorder before the session, destroy it after, and never relocate it. The
    // borrow exists because the run has exactly one evidence stream and every
    // layer stamps its events through the same sequence counter, which a
    // per-session owned sink could not provide.
    class EngineSession final
    {
        friend class Observation;

        std::shared_ptr<detail::EngineSessionIdentity const> m_identity;
        std::unique_ptr<IFrameSource>                        m_frameSource;
        std::unique_ptr<IActionSink>                         m_actionSink;

        // Null when the composition root bound no OCR adapter, which is the
        // normal state for every path that does not read text: the weights are
        // tens of megabytes and loading them for a run that never reads would be
        // a cost nobody asked for. readText refuses on its own terms when it is
        // absent, rather than the session refusing to exist.
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
        // from. `verb` names the caller so the refusal still reads as that verb's
        // own, which is why these stayed spelled out per verb until there were
        // five of them.
        [[nodiscard]]
        auto ensureUsable(
            Observation const& observation,
            std::string_view verb
        ) const -> Status;

        // What one OCR call produced, before readText decides whether it is a
        // reading or only a trace line. It is nested and private because nothing
        // names it except readText: the trace line has to carry the engine
        // identity and the duration even when no text was found, so the two
        // cannot be folded into the optional reading itself.
        struct ReadAttempt final
        {
            std::optional<TextReading> line{};

            std::string engineId{};
            uint64      durationMicros{};
        };

        [[nodiscard]]
        auto readTextOnFrame(
            Frame const& frame,
            PixelRect rect
        ) const -> Result<ReadAttempt>;

    public:
        EngineSession(EngineSession const&) = delete;
        EngineSession(EngineSession&&) noexcept = default;
        auto operator=(EngineSession const&) -> EngineSession& = delete;
        auto operator=(EngineSession&&) noexcept -> EngineSession& = delete;

        ~EngineSession() = default;

        // `ocrEngine` is optional and defaults to none, so every composition root
        // that never reads text is unchanged and pays nothing. A session built
        // without one refuses readText rather than pretending to read.
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
        // the region could hold no candidate at all.
        //
        // It is the whole of what this module searches for: no catalog lookup,
        // no page, no appearance, no threshold, because all four moved to layer
        // two with the model. A control stop -- the comparison budget, the
        // recognition deadline, a requested cancel -- is a FAILURE and never a
        // miss, because a search that stopped looking has not established that
        // the template is absent.
        [[nodiscard]]
        auto matchTemplate(
            Observation const& observation,
            GrayTemplateImage const& templateImage,
            PixelRect searchRoi
        ) -> Result<std::optional<MatchFound>>;

        // Reads the text in `rect` of the frame `observation` holds.
        //
        // The region is asserted by the caller to hold ONE line: detection is the
        // expensive stage and the adapter this project ships does not run it, so
        // a caller that does not know the line count gets a refusal rather than a
        // silent single-line read of several lines. Finding no text is an empty
        // optional, which is an ordinary answer about the screen; a failure here
        // means the read could not be attempted.
        [[nodiscard]]
        auto readText(
            Observation const& observation,
            PixelRect rect
        ) -> Result<std::optional<TextReading>>;

        // Delivers one click at `point`, spending `observation`.
        //
        // WHAT IT ENFORCES, and what it deliberately does not. A requested stop
        // refuses before any sink call, a foreign handle is an
        // InternalInvariant, a consumed handle is StaleObservation, the live
        // fingerprint must match the project's, the observation's lease must
        // still be valid at delivery, the bound target instance is revalidated
        // immediately before the post, and the observation is spent so one frame
        // delivers at most one input.
        //
        // What it does NOT enforce is that a resolved page authorises the element
        // being clicked, because there is no element here and no page: both moved
        // to layer two with the model. The privilege of naming a bare coordinate
        // is therefore the trusted framework's and is never handed to a business
        // script -- modules/task/runtime/observe.luau is where "only click what
        // this page authorises" is enforced now
        // (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
        [[nodiscard]]
        auto clickPoint(
            Observation&& observation,
            PixelPoint point
        ) -> Result<ActReceipt>;

        // Delivers one keystroke, spending `observation`.
        //
        // ITS AUTHORIZATION CONTRACT, and how it differs from clickPoint()'s.
        // Stated here because this is the only place both verbs are visible at
        // once.
        //
        // Shared with clickPoint(), and for the same reasons:
        //   - a requested stop refuses before any sink call, so a cancelled run
        //     never posts input;
        //   - an observation from another session is an InternalInvariant, and an
        //     invalidated one is StaleObservation, so a handle cannot be reused or
        //     smuggled across sessions;
        //   - the bound target instance is revalidated immediately before the post,
        //     which closes the HWND-reuse window;
        //   - the observation is spent, so one observation delivers at most one
        //     input. A keystroke changes the screen exactly as a click does, so a
        //     frame that survived it would describe a target that no longer exists.
        //
        // Deliberately NOT shared, because a keystroke names no screen position:
        //   - there is no fingerprint check. That question asks whether a
        //     coordinate measured against this project still means what it meant;
        //     a virtual key names no coordinate, so there is nothing for the
        //     geometry to invalidate;
        //   - the observation's lease is not enforced. A lease bounds a
        //     coordinate's shelf life; a key's meaning does not decay with layout.
        //     Enforcing it would refuse keystrokes for a reason that cannot apply
        //     to them, and would push an operator to widen --max-frame-age for the
        //     whole run to get keys through -- weakening every click to serve a key.
        //
        // What the caller supplies instead of a detection is the requirement that
        // an observation exist at all: the keystroke is ordered against the
        // observation cycle that produced it, and the trace joins it to that
        // frame.
        [[nodiscard]]
        auto pressKey(
            Observation&& observation,
            KeyName key
        ) -> Result<KeyReceipt>;
    };
}
