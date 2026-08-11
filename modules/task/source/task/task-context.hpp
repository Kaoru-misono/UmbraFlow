#pragma once

#include <task/cycle-answers.hpp>
#include <task/cycle-ledger.hpp>
#include <task/pixel-probe.hpp>
#include <task/project-files.hpp>
#include <task/template-store.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <trace/recorder.hpp>

#include <vision/frame-analysis.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    // The longest a single explore.settle may declare. Beyond it is a project
    // error the author can catch -- a Tier B InvalidResource, not an invariant
    // failure.
    // CALIBRATION: thirty seconds is a placeholder, far above any settle a UI
    // transition needs and far below the max-runtime budget. A longer wait is
    // written as settles with an observation between them, which is the only
    // waiting shape this host has; see TaskContext::settle.
    inline constexpr auto k_maxSettleDuration = MonotonicInstant::Duration{
        std::chrono::seconds{30}
    };

    // The longest a single long press may hold the button down. A separate and
    // much lower ceiling than the settle one: a long press leaves a pointer
    // button physically down in the target with only this host to lift it, and
    // every other input queued behind it. Beyond it is Tier B, for
    // k_maxSettleDuration's reason.
    // CALIBRATION: five seconds is a placeholder. Targets that publish a
    // long-press gesture measure it in hundreds of milliseconds.
    inline constexpr auto k_maxLongPressHold = MonotonicInstant::Duration{
        std::chrono::seconds{5}
    };

    // The longest a single drag may spend travelling. The long-press ceiling's
    // reason exactly -- the button is down for all of it -- and the same number,
    // because what is bounded is the same thing: how long this host may leave a
    // pointer button physically down in the target.
    // CALIBRATION: five seconds is a placeholder. A pan that needs more than a
    // second of travel is more likely a chunk's bug than a slow target.
    inline constexpr auto k_maxDragTravel = MonotonicInstant::Duration{
        std::chrono::seconds{5}
    };

    // How many text reads one observation cycle may charge. Its own budget
    // dimension rather than a share of the matcher's pixel-comparison pool
    // because the units do not compare: a SAD comparison is nanoseconds, a line
    // read is 2-13 ms. A block read spends this same pool -- one for its
    // detection pass, one per line located -- because those lines cost exactly
    // what a cycle_read costs.
    //
    // CALIBRATION: thirty-two, raised from the eight set when a read meant one
    // rectangle the model had drawn. The character grid this verb exists for
    // shows about twenty names at once, so one block read is twenty-one reads.
    // The ceiling is the observation lease: at 2-13 ms a read, thirty-two is at
    // most about 0.4 s of the 750 ms. Raise it here, or per run through
    // TaskContextConfig.
    inline constexpr auto k_defaultMaximumReadsPerCycle = uint32{32};

    // How many crops one observation cycle may charge. A third dimension for the
    // read budget's reason: a crop is a copy plus a PNG encode over a
    // caller-chosen rectangle, so a shared pool would let one whole-panel crop
    // starve a page's template matching. Exhaustion is RecognitionIncomplete and
    // never an empty answer -- a refused crop established nothing about the
    // screen, and this verb has no score to contradict a fail-open "no pixels".
    //
    // CALIBRATION: eight is a placeholder, above the widest measured pattern (the
    // panel, then each candidate slot inside it once) and low enough that a loop
    // cropping per poll cannot turn one cycle into megabytes of encoding. Raise
    // it here, or per run through TaskContextConfig.
    inline constexpr auto k_defaultMaximumCropsPerCycle = uint32{8};

    // The two ends a colour-keyed mask has to stay between before its counts mean
    // anything; measurements and reproductions in
    // docs/pitfalls/colour-key-annotation.md. They warn and never refuse, and the
    // share is in basis points.
    //
    // The floor holds in both directions -- too few pixels cannot measure
    // anything whichever way the key was read -- but the CEILING does not. Its
    // reasoning is that a key takes one colour, so a large mask is a solid patch
    // of that colour; a key that names the backdrop takes everything BUT one
    // colour, and a mark filling half its own bounding box is the ordinary case
    // rather than the pathological one. A diamond fills exactly half.
    inline constexpr auto k_minimumUsefulMaskPixels  = uint64{50};
    inline constexpr auto k_maximumUsefulMaskShareBp = uint64{5000};

    // The ceiling for a key that names what to remove, where the hazard is the
    // opposite one: the named colour was barely present, so almost nothing was
    // masked out and the template is effectively unmasked -- which is the failure
    // a mask exists to avoid.
    //
    // CALIBRATION: 9000 is a placeholder chosen from shape rather than measured.
    // A mark filling more than nine tenths of its own bounding box leaves no
    // backdrop worth naming, so at that point the key is trimming a rim.
    inline constexpr auto k_maximumRemovedMaskShareBp = uint64{9000};

    class TaskHost;

    // Host-side configuration for one TaskContext. The cancellation source is
    // shared with the owned EngineSession and the VM interrupt. A
    // default-constructed stop token never requests a stop, so resource-only and
    // Fake-driven paths that build a context without one are never cancelled.
    //
    // There are deliberately no wait budgets here. How long to wait for a page
    // and how often to re-observe are policy, and the wait loop is Luau now.
    struct TaskContextConfig final
    {
        std::stop_token cancellation{};

        // The directory project_read and project_write are confined to. Empty
        // leaves the context with no project files and both verbs refuse, which
        // is the right answer for a test or fake with no project on disk rather
        // than a context that would reach the working directory.
        std::filesystem::path projectRoot{};

        // See k_defaultMaximumReadsPerCycle for why this is its own dimension.
        uint32 maximumReadsPerCycle{k_defaultMaximumReadsPerCycle};

        // See k_defaultMaximumCropsPerCycle, on the same reasoning again.
        uint32 maximumCropsPerCycle{k_defaultMaximumCropsPerCycle};

    };

    // Host-owned bridge between one task VM and one EngineSession. It owns the
    // session and the ledger holding the generation's single open observation
    // cycle. The Luau binding layer reaches it through a lightuserdata upvalue and
    // never sees engine types: every engine call, and all ownership of the
    // move-only Observation, stays here in host C++.
    //
    // Lifetime contract: the caller MUST keep the TaskContext alive for at least
    // as long as the script::Engine that binds it, whose host functions hold a raw
    // pointer to it; the context is therefore non-movable so that pointer stays
    // stable. The retained Observation carries only the session's stable immutable
    // identity token, never a borrow into the session object. NOT thread-safe:
    // every method runs on the VM's owning thread.
    //
    // Trace lifetime contract: the context does NOT own a trace sink. It borrows
    // the session's trace::TraceRecorder, owned by ExplorationSession in a
    // std::unique_ptr local declared before the context and the VM so both are
    // destroyed first on every path, and non-movable so its address cannot drift
    // while the context borrows it. Any other owner MUST reproduce both
    // properties. It is the SAME recorder the owned EngineSession borrows, which
    // is what puts this context's own annotation.* lines, the engine.* events
    // the same call produced, and the host's surrounding run.started and
    // run.finished into one ordered stream.
    class TaskContext final
    {
    public:
        // What one loadTemplate produced: the ticket the script holds and the
        // content hash of the blob it came from. The hash comes back rather than
        // being recomputed because the caller's trace line needs it and the store
        // already has it.
        //
        // No in-class initializer for the hash: ContentHash has no default state.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct LoadedTemplate final
        {
            TemplateTicket ticket{};

            ContentHash hash;
        };

        // What the colour key a crop was cut under actually took. Reported
        // because the key is the one authoring decision nothing else measures: a
        // masked template that selects almost nothing, or almost everything,
        // matches every screen and looks like the healthiest element in the
        // project. The counts come out of the same colourKeyAlpha rule `probe`
        // uses, so the two verbs cannot disagree about one measurement -- but NOT
        // under the same names: the full-weight count is `selected_pixels` here
        // and `fully_selected_pixels` on probe.
        struct CropMask final
        {
            // The key AS THE HOST APPLIED IT, which is not always as the caller
            // spelled it: a caller naming no tolerance gets the verb's default.
            // It comes back so the layer that records the key in the project
            // file records the one that produced the alpha channel.
            ProbeColourKey key{};

            uint64 rectPixels{};

            // Pixels the key took at full weight, and on the antialiasing ramp.
            // The first is what the floor and the share are judged on.
            uint64 selectedPixels{};
            uint64 rampSelectedPixels{};

            // Why these counts look wrong, or empty when they do not. A hint and
            // never a refusal: see k_minimumUsefulMaskPixels.
            std::string warning{};
        };

        // What one cycleCrop produced: the PNG a script holds, and the content
        // hash of exactly those bytes. The hash comes back because the caller
        // needs it to NAME the file it is about to write -- a template asset
        // lives at assets/templates/<hex>.png and the sandbox gives Luau no hash
        // function, so recomputing it in the script layer is impossible rather
        // than merely wasteful.
        struct CroppedBlob final
        {
            std::vector<std::byte> png{};

            ContentHash hash;

            // Absent when the caller named no key rather than zeroed: "no key
            // was asked for" and "the key took nothing" are different answers.
            // The second never reaches here -- a key that takes nothing is
            // refused, because its PNG is fully transparent and every later
            // match of it aborts.
            std::optional<CropMask> mask{};
        };

    private:
        friend class TaskHost;

        engine::EngineSession m_session;
        TaskContextConfig     m_config;
        trace::TraceRecorder& m_recorder;

        CycleLedger      m_cycles{};
        CycleAnswers     m_answers{};
        TemplateStore    m_templates{};
        ProjectFileStore m_projectFiles;

        std::optional<AutomationErrorKind> m_terminal{};

        [[nodiscard]]
        auto requireReceiptCycle(
            CycleTicket ticket,
            std::optional<uint64> evidenceCycleOrdinal
        ) const -> Status;

        [[nodiscard]]
        auto deliverReceiptClick(CycleTicket ticket, PixelPoint point)
            -> Result<engine::ActReceipt>;

        // deliverReceiptClick's sibling for a Receipt that authorized a
        // keystroke. Separate verbs rather than one taking a sum, because each
        // spends the cycle's observation into a different engine verb and the
        // two return different receipts; the choice between them is made once,
        // where the Receipt's own intent is read.
        [[nodiscard]]
        auto deliverReceiptKey(CycleTicket ticket, KeyName key)
            -> Result<engine::KeyReceipt>;

    public:
        explicit TaskContext(
            engine::EngineSession session,
            trace::TraceRecorder& recorder,
            TaskContextConfig config = {}
        ) noexcept;

        TaskContext(TaskContext const&) = delete;
        TaskContext(TaskContext&&) = delete;
        auto operator=(TaskContext const&) -> TaskContext& = delete;
        auto operator=(TaskContext&&) -> TaskContext& = delete;

        ~TaskContext() = default;

        // Observes one frame and opens the generation's single observation cycle
        // over it, returning the ticket that names it. A cycle that is already
        // open fails InternalInvariant BEFORE the capture runs, so a framework
        // bug never costs a whole screenshot.
        [[nodiscard]]
        auto openCycle() -> Result<CycleTicket>;

        // Releases the frame the cycle `ticket` names retains and reports whether
        // there was one. Idempotent: closing twice, closing a ticket a click
        // already consumed, and closing another generation's ticket are all
        // no-ops. The only release path a running script can drive -- the Lua
        // collector has no part in it.
        [[nodiscard]]
        auto closeCycle(CycleTicket ticket) noexcept -> bool;

        // Searches `searchRoi` of the frame `ticket`'s cycle retains for the
        // template `templateTicket` names. An empty optional is a completed
        // search with no candidate position; a budget, deadline or cancel stop is
        // a FAILURE, never a miss.
        //
        // It requires no resolved page: the refined region, the pinned appearance
        // and the interact edge a page's reference row used to supply are the
        // caller's now, because the script-owned model puts those facts there.
        //
        // The same template searched in the same region of the same cycle reaches
        // the engine ONCE and is answered from CycleAnswers afterwards. The
        // comparison ceiling is per search rather than per cycle, so a served
        // answer takes nothing away from any other search.
        [[nodiscard]]
        auto cycleMatch(
            CycleTicket ticket,
            TemplateTicket templateTicket,
            PixelRect searchRoi
        ) -> Result<std::optional<engine::MatchFound>>;

        // Reads the text in `rect` of the frame `ticket`'s cycle retains. An
        // empty optional is a completed read that found no text.
        //
        // The read is charged against this cycle's own read budget BEFORE the
        // engine is reached, and an exhausted cycle fails RecognitionIncomplete:
        // the host stopped looking, so nothing has been established about the
        // screen, and "no text" would be a fail-open answer on the one capability
        // that has no score to contradict it.
        //
        // ONE RECTANGLE IS READ ONCE PER CYCLE. A repeat is answered from
        // CycleAnswers, which decides nothing and only hands back what the engine
        // already said about this same frame. Two consequences, each a decision:
        //
        //   - A SERVED ANSWER CHARGES NO BUDGET, and is served even once the
        //     budget is spent. That budget is calibrated in inference
        //     milliseconds against the observation lease
        //     (k_defaultMaximumReadsPerCycle), and a repeat runs no inference; a
        //     refusal here would also claim nothing had been established about a
        //     region this frame was already read at, which is false. So the
        //     budget bounds ENGINE reads per cycle, which is the cost it was
        //     always standing in for.
        //   - A SERVED ANSWER WRITES NOTHING. This verb records no line of its
        //     own on either path, so the single engine.text_read the first read
        //     produced is the whole record of this rectangle of this frame. That
        //     is the right record: a repeat hands back those same bytes and runs
        //     no inference, and a second engine line would carry a duration and
        //     an engine id no read produced.
        //     What the stream therefore does NOT hold is how many times a chunk
        //     asked. That count is a fact about the chunk's control flow rather
        //     than about the host or the target, nothing in this repository reads
        //     it back, and recovering it would need a per-primitive event this
        //     tree does not have.
        [[nodiscard]]
        auto cycleRead(
            CycleTicket ticket,
            PixelRect rect
        ) -> Result<std::optional<engine::TextReading>>;

        // Finds every line of text inside `rect` of the frame `ticket`'s cycle
        // retains and reads each one, with its own rectangle in FRAME pixels.
        //
        // cycleRead's sibling, not its replacement: cycleRead is right wherever
        // the caller drew the rectangle, this one is for a region nobody CAN draw
        // a rectangle inside because what is in it moves -- a grid that scrolls,
        // where a name's position is a fact about the frame rather than the model.
        //
        // It costs one read for the detection pass plus one for every line
        // located, out of the pool cycleRead spends, so a region holding more
        // lines than the cycle can still pay for fails RecognitionIncomplete and
        // reads NONE of them. It does NOT consume the cycle, so the same cycle
        // goes on to click one of the lines it found.
        //
        // It is memoised per cycle on cycleRead's terms, in a table of its own:
        // this verb and that one over the same rectangle are two questions with
        // two answer shapes, so a block read never serves a single-line read or
        // the reverse.
        //
        // A session whose composition root bound NO OCR adapter fails
        // UnsupportedCapability here rather than reporting no lines, and every
        // layer above must keep that a refusal: an empty list would say the
        // region holds no text, which is a claim about the screen that a host
        // unable to read text has not earned.
        [[nodiscard]]
        auto cycleReadLines(
            CycleTicket ticket,
            PixelRect rect
        ) -> Result<std::vector<engine::TextReading>>;

        // Copies `rect` of the frame `ticket`'s cycle retains, encodes it as a
        // PNG, and hands back the bytes with their content hash. It writes
        // annotation.region_saved -- the rect, the byte size and hash, and the
        // frame identity -- which is the whole record of the crop, because the
        // bytes themselves go to the agent rather than into the stream.
        //
        // Only the exploration environment may reach it: it hands raw pixels to
        // the script layer, and a business script holding pixels could decide
        // something no trace evidence could falsify. It is not installed on a run
        // VM's private surface at all, which makes the rule structural
        // (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
        // cycleCensusGrid is on the same shelf for a sharper reason than its
        // counts-only answer suggests; see its declaration.
        //
        // It does NOT spend the cycle -- reading pixels changes nothing on the
        // target. The crop budget bounds it instead, and exhaustion fails
        // RecognitionIncomplete rather than returning an empty answer.
        //
        // A `key` makes the crop a masked template: the weights it hands out
        // become the PNG's alpha channel, which decodeTemplateImage reads back as
        // the matcher's mask plane, so the template compares its glyph rather than
        // its whole rectangle (docs/pitfalls/element-choice-and-thresholds.md). A
        // key that takes NO pixel is refused here rather than persisted, because
        // its fully transparent PNG fails InternalInvariant deep in a later match
        // that no longer knows which key was chosen or over what rectangle.
        [[nodiscard]]
        auto cycleCrop(
            CycleTicket ticket,
            PixelRect rect,
            std::optional<ProbeColourKey> key
        ) -> Result<CroppedBlob>;

        // Tiles `rect` of the frame `ticket`'s cycle retains into cells of
        // `cellWidth` by `cellHeight` and reports, per cell, how many pixels
        // `key` takes at full weight and how far the cell moved between frames.
        //
        // EXPLORATION ONLY, on cycleCrop's shelf. "Counts over a grid the
        // caller drew, never a pixel" does not survive the cell size being the
        // caller's too: at one-by-one cells a count is one pixel's membership
        // of one colour, k_maximumColourGridCells still allows 32768 of them
        // per call, and no budget bounds how many calls a cycle may make -- so a
        // caller that varies the key recovers the region's pixels. It is a
        // pixel read with an extra step, and it belongs where cycleCrop is.
        //
        // Nothing is lost by that. The Runtime surface takes no caller-drawn
        // geometry at all: a plugin measures the Locators and Readers its model
        // declared and reads evidence records, so this verb has no shape to take
        // there. What it buys the exploration front end is the whole reason it
        // exists -- a chunk measuring a region cropped a PNG and probed it once
        // per rectangle, paying a whole-blob decode per probe, where one call
        // answers for every cell of the rect at once.
        //
        // It does NOT spend the cycle and charges no per-cycle budget. The crop
        // pool exists to bound an encode and the read pool to bound inference,
        // and this verb performs neither: one call is one walk over one rect of
        // one frame, and vision's k_maximumColourGridCells bounds what comes
        // back. The rect and the key are the caller's own numbers, so a
        // tolerance, a cell size or a cell count the host cannot honour is a
        // Tier B InvalidResource rather than an invariant failure.
        //
        // The per-cell spread is what probeColour reports per rect, and it stops
        // at the script boundary. One observation retains ONE frame and this
        // verb hands censusColourGrid that frame twice, so the spread across
        // them is zero at every cell; the binding layer therefore publishes
        // only the counts, because a field that always answers the same thing
        // reads as a measurement nobody took. It is NOT dropped here, so a cycle
        // that later retains more than one frame needs no second verb -- only a
        // binding that starts passing it on.
        [[nodiscard]]
        auto cycleCensusGrid(
            CycleTicket ticket,
            PixelRect rect,
            uint32 cellWidth,
            uint32 cellHeight,
            ProbeColourKey key
        ) -> Result<ColourGridReport>;

        // The six acts an exploration chunk may deliver, each spending the
        // cycle `ticket` names so one frame delivers at most one input.
        //
        // WHY THESE NEED NO RECEIPT. A Receipt exists so that a PLUGIN-issued
        // action is auditable and replayable: a model-declared action, a
        // proof_locator re-measured on this cycle, one receipt per cycle, a
        // freshness lease. That discipline is what makes an action a project
        // plugin asked for trustworthy, and none of it is weakened here,
        // because none of these verbs is reachable from a plugin environment --
        // they are installed by buildAnnotationSurface and by nothing else, and
        // only the exploration VM runs that installer
        // (task/exploration-session.hpp, task/script-bindings.hpp).
        //
        // An explore chunk is not a plugin. It is code a human or an authoring
        // agent wrote inside an environment that already holds the authoring
        // private surface, and requiring it to name a Binding is not a safety
        // property but a chicken-and-egg: an agent must REACH a screen before it
        // can model one, so a front end that can only act on screens already
        // modelled can never model the first screen. The verbs below are how the
        // model gets written in the first place; the Receipt path is how the
        // model is later acted on in production, and neither is a way into the
        // other.
        //
        // What still fences them is everything about the FRAME. The ledger
        // hands the observation over here, so the ticket dies before delivery;
        // the engine then applies its own gate unchanged -- live fingerprint,
        // lease validity, target-instance revalidation -- for every verb that
        // names a coordinate, and its narrower gate for the two that do not.
        // See engine::EngineSession for which clauses each verb answers to.
        //
        // Each writes one annotation.*_delivered line naming what the chunk
        // asked for, so no act on this surface is unrecorded; see
        // task-context.cpp for why that line is not a second spelling of the
        // engine's own.
        [[nodiscard]]
        auto cycleClickPoint(CycleTicket ticket, PixelPoint point) -> Status;

        // `hold` and `travel` have no default at any layer: how long a target
        // wants a press held is a fact about that target. This is the layer that
        // bounds them, because it is the one whose refusal can name what the
        // chunk wrote -- k_maxLongPressHold and k_maxDragTravel, refused before
        // the cycle is spent so a mistyped duration costs no frame and leaves no
        // button down.
        [[nodiscard]]
        auto cycleLongPress(
            CycleTicket ticket,
            PixelPoint point,
            MonotonicInstant::Duration hold
        ) -> Status;

        // Both points are bare coordinates. The far end is a coordinate rather
        // than an offset because an offset would have to be range-checked here
        // against a client size this layer does not know, while a coordinate
        // meets the same check the start meets.
        [[nodiscard]]
        auto cycleDrag(
            CycleTicket ticket,
            PixelPoint start,
            PixelPoint end,
            MonotonicInstant::Duration travel
        ) -> Status;

        [[nodiscard]]
        auto cycleMovePointer(CycleTicket ticket, PixelPoint point) -> Status;

        [[nodiscard]]
        auto cycleScroll(CycleTicket ticket, int32 notches) -> Status;

        // One delivered key spends its cycle exactly as a click does, because it
        // changes the screen exactly as a click does. KeyName is the single
        // definition of which key names exist and this verb opens nothing: a
        // name outside the set is refused by the same create() the Receipt path
        // calls.
        [[nodiscard]]
        auto cycleKey(CycleTicket ticket, KeyName key) -> Status;

        // Releases whatever cycle is open and reports whether there was one. NOT
        // a script verb and never installed as a primitive: see
        // CycleLedger::closeOpen for who may call it, which is a host closing a
        // bracket it owns -- the exploration session between two agent-supplied
        // chunks, and TaskHost::observe after it has read the frame identity its
        // own trusted chunk left open.
        auto sweepOpenCycle() noexcept -> bool;

        // Which target generation the open cycle's frame was captured on, or
        // nothing when no cycle is open.
        //
        // The one fact about a retained frame that leaves this context without
        // a ticket, because it is the one an Operator snapshot has to record: a
        // plan frozen against a UI observation must be able to say which
        // generation of the target that observation described. Everything else
        // about the frame stays behind a CycleTicket.
        [[nodiscard]]
        auto openCycleTargetGeneration() const noexcept
            -> std::optional<TargetGeneration>;

        // Decodes one template PNG into this generation's template store and
        // returns the ticket naming it, with the content hash of the blob for
        // the caller's trace line. Identical bytes yield the same ticket.
        [[nodiscard]]
        auto loadTemplate(
            std::span<std::byte const> pngBytes
        ) -> Result<LoadedTemplate>;

        // Reads and writes one file inside the privileged annotation directory.
        // Production RuntimeArtifact bytes never pass through this authoring seam.
        [[nodiscard]]
        auto projectRead(std::string_view name) -> Result<std::vector<std::byte>>;

        [[nodiscard]]
        auto projectWrite(
            std::string_view name,
            std::span<std::byte const> bytes
        ) -> Status;

        // Sleeps for `duration`, returning early once the run's cancel source is
        // requested. The sleep is the shared core::pollSleep, so it wakes at a
        // slice boundary once that source is requested. It backs the `settle`
        // primitive. The caller enforces the k_maxSettleDuration ceiling,
        // refuses a settle taken while a cycle is open, and re-checks
        // cancellationRequested() afterwards; the binding holds those because
        // its refusal can name what the chunk wrote.
        //
        // The ONE way this host lets time pass, and deliberately not two. A
        // deadline-bounded poll step -- sleep an interval, report whether budget
        // remains, observe, repeat -- needs a host clock to mint the deadline
        // from, which the sandbox withholds on purpose
        // (script/ffi/sandbox.cpp), and carrying the "not while a cycle is
        // open" refusal above would leave it this verb with a deadline
        // attached. A chunk that polls loops over this one and counts its
        // iterations; script::EngineConfig::maxRuntime bounds the chunk either
        // way.
        auto settle(MonotonicInstant::Duration duration) const -> void;

        // Whether the run's single cancel source has requested a stop; only the
        // time primitives use it to stop waits after cancellation.
        [[nodiscard]]
        auto cancellationRequested() const noexcept -> bool;

        // Whether an observation cycle is open, and so whether the host is still
        // holding a frame. The host-side truth a test asserts release against;
        // nothing about it involves the Lua collector.
        [[nodiscard]]
        auto hasOpenCycle() const noexcept -> bool;

        // Latches that this generation is spent, under the kind that spent it.
        // The binding layer sets it BEFORE raising, and gates every primitive on
        // it at the C guard entry, so a spent VM cannot resume authoring or
        // observation even if a script swallowed what was raised.
        //
        // Latching is idempotent and keeps the FIRST kind: a later refusal is a
        // consequence rather than a cause.
        void markTerminal(AutomationErrorKind kind) noexcept;

        [[nodiscard]]
        auto fatal() const noexcept -> bool;

        // The kind that spent the generation, or empty while it is still live.
        // The owning host reads it after the run so a generation that ended
        // terminally is still reported that way when the script caught the raise
        // and returned normally.
        [[nodiscard]]
        auto terminalKind() const noexcept -> std::optional<AutomationErrorKind>;
    };
}
