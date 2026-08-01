#pragma once

#include <task/cycle-ledger.hpp>
#include <task/deterministic.hpp>
#include <task/project-files.hpp>
#include <task/template-store.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <annotation/content-hash.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace uf::task
{
    // The default seed for a task's deterministic RNG (ctx:random). A real run
    // overrides it with a host-chosen seed recorded in the run.started trace
    // event, so a run can be replayed by re-supplying the same seed; this constant
    // only gives tests and unconfigured contexts a stable, non-zero starting point.
    // Because the sandbox removed math.random, ctx:random seeded from this value
    // is a script's only randomness, so nothing outside the host can perturb the
    // sequence.
    inline constexpr auto k_defaultRandomSeed = uint64{0x9E3779B97F4A7C15};

    // The longest a single ctx:settle may declare. A settle is a declarative
    // pause a task asks for, so a request beyond this ceiling is a project
    // error rather than a framework bug (design section 9 reserves the invariant
    // kind for failures a project cannot cause), and the binding refuses it as a
    // Tier B InvalidResource the author can catch and correct.
    //
    // CALIBRATION: thirty seconds is a conservative placeholder awaiting the
    // first real daily. It is deliberately far above any settle a UI transition
    // needs and far below the max-runtime budget, so it catches an author who
    // meant minutes without capping anything a real animation waits for. Waiting
    // longer than this is what ctx:wait plus a deadline is for -- that loop
    // re-observes, where a settle only sleeps.
    inline constexpr auto k_maxSettleDuration = MonotonicInstant::Duration{
        std::chrono::seconds{30}
    };

    // The floor a ctx:wait poll interval is clamped up to. Without it a
    // framework loop could ask for a zero interval and spin the observation
    // cycle as fast as captures complete, burning the instruction budget and the
    // target's CPU for no extra evidence.
    //
    // CALIBRATION: ten milliseconds is a conservative placeholder awaiting the
    // first real daily. It is low enough to be invisible next to a capture and
    // high enough that a degenerate loop cannot become a busy wait.
    inline constexpr auto k_minWaitPollInterval = MonotonicInstant::Duration{
        std::chrono::milliseconds{10}
    };

    // How many text reads one observation cycle may charge before the host
    // refuses the next one.
    //
    // OCR GETS ITS OWN BUDGET DIMENSION, and this is it. The pixel-comparison
    // budget the matcher spends is a single shared pool drawn down anchor by
    // anchor, and folding reads into it would let one read-heavy page quietly
    // starve template matching and then blame whichever anchor happened to be
    // searching when the pool ran dry. The two units are not comparable -- a SAD
    // comparison is nanoseconds, a line read is 2-13 milliseconds -- so one
    // number covering both describes neither.
    //
    // CALIBRATION: eight is a conservative placeholder awaiting the first real
    // daily. It is above every reading pattern measured so far -- the widest is
    // "read each of the candidate slots on this page once and pick one" -- and
    // low enough that a wait loop reading once per poll cannot turn a cycle into
    // a tenth of a second of inference. Raise it here, or per run through
    // TaskContextConfig, once a real page needs more.
    inline constexpr auto k_defaultMaximumReadsPerCycle = uint32{8};

    // Host-side configuration for one TaskContext: the single cancellation
    // source shared with the owned EngineSession and the VM interrupt, plus this
    // run's RNG seed. A default-constructed stop token never requests a stop, so
    // an unconfigured context is never cancelled -- preserving the
    // resource-only and Fake-driven paths that build a context without wiring a
    // cancel source.
    //
    // There are deliberately no wait budgets here. How long to wait for a page
    // and how often to re-observe are policy, and policy lives in the framework:
    // the wait loop is Luau now, so a default the framework cannot read would be
    // a default nothing applies.
    struct TaskContextConfig final
    {
        std::stop_token cancellation{};

        // Fixed seed for this task's deterministic RNG (ctx:random). Left at the
        // stable default here; a real run gets a fresh per-run seed from
        // TaskHost::startTask, which draws it and records it in run.started so
        // the run reproduces on replay. See k_defaultRandomSeed.
        uint64          randomSeed{k_defaultRandomSeed};

        // The directory project_read and project_write are confined to, which is
        // the generation's own project root. Empty leaves the context with no
        // project files at all, and both verbs then refuse -- which is the right
        // answer for a context built by a test or a fake that has no project on
        // disk, rather than a context that would reach the working directory.
        std::filesystem::path projectRoot{};

        // See k_defaultMaximumReadsPerCycle for why this is a dimension of its
        // own rather than a share of the pixel-comparison pool.
        uint32 maximumReadsPerCycle{k_defaultMaximumReadsPerCycle};
    };

    // Host-owned bridge between one task VM and one EngineSession. It owns the
    // session (moved in by the caller, who is responsible for constructing the WGC
    // / controller ports) and the ledger holding the generation's single open
    // observation cycle. The Luau binding layer reaches this object through a
    // lightuserdata upvalue and never sees engine types: every engine call, and
    // all ownership of the move-only Observation, stays here in host C++.
    //
    // Lifetime contract: the caller MUST keep the TaskContext alive for at least
    // as long as the script::Engine (task VM) that binds it, because the VM's host
    // functions hold a raw pointer to it. The context is therefore non-movable so
    // that pointer stays stable. The retained Observation carries only the engine
    // session's stable immutable identity token, never a borrow into the session
    // object. NOT thread-safe: every method runs on the VM's owning thread.
    //
    // Trace lifetime contract: the context does NOT own a trace sink. It stores a
    // non-owning borrow of the run's trace::TraceRecorder, which is owned by
    // `task::TaskHost::startTask`: that function holds the recorder in a
    // std::unique_ptr local declared before the context and the VM, so both are
    // destroyed first on the normal path and on every early return, and the
    // recorder is non-movable so its address cannot drift while the context
    // borrows it. Any other owner MUST reproduce both properties. It is the SAME
    // recorder the owned EngineSession borrows, which is what puts
    // task.native_call and the engine.* events it caused into one ordered stream.
    class TaskContext final
    {
    public:
        // What one loadTemplate produced: the ticket the script holds and the
        // content hash of the blob it came from.
        //
        // The hash is returned rather than looked up again because the caller's
        // trace line needs it and the store already computed it; recomputing a
        // SHA-256 over the same megabyte to write one line would be the kind of
        // duplicate truth this codebase keeps refusing.
        struct LoadedTemplate final
        {
            TemplateTicket ticket{};

            annotation::ContentHash hash;
        };

    private:
        engine::EngineSession m_session;
        TaskContextConfig     m_config;
        DeterministicRng      m_rng;
        trace::TraceRecorder& m_recorder;

        CycleLedger      m_cycles{};
        TemplateStore    m_templates{};
        ProjectFileStore m_projectFiles;

        std::optional<AutomationErrorKind> m_terminal{};

        bool m_traceFailed{false};

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
        // there was one to release. Idempotent: closing twice, closing a ticket a
        // click already consumed, and closing a ticket from another generation
        // are all no-ops. This is the only release path a running script can
        // drive -- the Lua collector has no part in it.
        [[nodiscard]]
        auto closeCycle(CycleTicket ticket) noexcept -> bool;

        // Searches `searchRoi` of the frame `ticket`'s cycle retains for the
        // template `templateTicket` names. An empty optional is a completed
        // search with no candidate position; a budget, deadline or cancel stop is
        // a FAILURE, never a miss.
        //
        // It requires no resolved page. The refined region, the pinned
        // appearance and the interact edge that a page's reference row used to
        // supply are the caller's now, because in the script-owned model the
        // caller is the layer that owns those facts.
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
        // engine is reached, and a cycle that has spent it fails
        // RecognitionIncomplete -- the kind an exhausted comparison budget
        // already uses, and for the same reason: the host stopped looking, so
        // nothing has been established about the screen. Reporting it as "no
        // text" would be a fail-open answer on the one capability that has no
        // score to contradict it.
        [[nodiscard]]
        auto cycleRead(
            CycleTicket ticket,
            PixelRect rect
        ) -> Result<std::optional<engine::TextReading>>;

        // Decodes one template PNG into this generation's template store and
        // returns the ticket naming it, with the content hash of the blob for
        // the caller's trace line. Identical bytes yield the same ticket.
        [[nodiscard]]
        auto loadTemplate(
            std::span<std::byte const> pngBytes
        ) -> Result<LoadedTemplate>;

        // Reads and writes one file inside the generation's project directory.
        // Neither is cycle-scoped: a page model is loaded before any observation
        // and written after one, and tying either to an open cycle would make a
        // frame a precondition for touching the disk.
        [[nodiscard]]
        auto projectRead(std::string_view name) -> Result<std::vector<std::byte>>;

        [[nodiscard]]
        auto projectWrite(
            std::string_view name,
            std::span<std::byte const> bytes
        ) -> Status;

        // Spends the cycle `ticket` names and delivers a click at `point`.
        //
        // WHO MAY REACH THIS. It is the layer-two-held privilege: the trusted
        // Luau framework binds it as a closure upvalue and the environment a
        // business task runs in never names it. That is where "a task only
        // clicks annotated elements" is now enforced, because the element and the
        // page moved up with the model. What C++ still enforces is the rest of
        // the four: the frame is this ticket's, the observation's lease is still
        // fresh, the live fingerprint matches the project, and the cycle is spent
        // exactly once.
        //
        // `hitCycleOrdinal` is the ordinal a match handle carries, or empty when
        // the caller named a bare point and there is no handle to check. When
        // present it must be the open cycle's own; anything else names a cycle
        // that no longer exists and fails StaleObservation, which is what makes
        // "the hit came from THIS frame" a check the caller cannot skip by
        // reaching for the point spelling with a stale match in hand.
        [[nodiscard]]
        auto cycleClickPoint(
            CycleTicket ticket,
            std::optional<uint64> hitCycleOrdinal,
            PixelPoint point
        ) -> Result<engine::ActReceipt>;

        // Spends the cycle `ticket` names and delivers one keystroke.
        //
        // ITS CONTRACT, and where it differs from cycleClick's. The full reasoning
        // is at engine::EngineSession::pressKey; what this layer decides is the
        // part the ledger owns.
        //
        // It requires the ticket to name the generation's OPEN cycle, and that is
        // the whole of what it requires. There is no hit ordinal, because there is
        // no hit: a keystroke names no screen position, so nothing has to have
        // been found on this frame, and no fingerprint check applies for the same
        // reason.
        //
        // Requiring the open cycle is not ceremony. It is what puts the keystroke
        // in the single-open-cycle ordering with every observation and click around
        // it, and what gives its trace line a cycle ordinal to join on, so a reader
        // can see which frame the operator or the task was looking at when it
        // pressed the key.
        //
        // It CONSUMES the cycle, exactly as a click does. A delivered keystroke
        // changes the screen, so the frame the cycle retains no longer describes the
        // target; leaving the cycle open would let a later find or click in the same
        // cycle act on a screen this key had already changed. "A delivered input
        // ends its observation" therefore holds for both verbs, which is a stricter
        // rule than the click alone needed.
        [[nodiscard]] auto cycleKey(CycleTicket ticket, KeyName key) -> Status;

        // Sleeps until `deadline`, or for `interval`, whichever comes first, and
        // reports whether budget remains afterwards -- false means the deadline
        // has passed and the caller's wait loop is over. It backs the `wait`
        // primitive, which is why it decides nothing else: the framework owns
        // what is polled between two calls, and this owns only the pause and the
        // verdict on the deadline.
        //
        // The sleep is the shared core::pollSleep, so it wakes at a slice
        // boundary once the run's cancel source is requested. The caller then
        // asks cancellationRequested() and takes the terminal path; this reports
        // budget, never cancellation.
        [[nodiscard]]
        auto waitUntil(
            MonotonicInstant deadline,
            MonotonicInstant::Duration interval
        ) const -> bool;

        // Sleeps for `duration`, returning early once the run's cancel source is
        // requested. It backs the `settle` primitive: a declarative bounded pause
        // whose length is part of the replayable record, which is why the binding
        // traces it. The caller enforces the k_maxSettleDuration ceiling and
        // re-checks cancellationRequested() afterwards.
        auto settle(MonotonicInstant::Duration duration) const -> void;

        // Whether the run's single cancel source has requested a stop.
        //
        // The observation and action primitives never need this: they reach the
        // engine, which already fails closed on the same token. The time
        // primitives do, because they reach nothing -- a sleep that ignored the
        // token would be the one place a cancelled generation could still burn
        // its whole wait -- so they consult it before pausing and again after.
        [[nodiscard]]
        auto cancellationRequested() const noexcept -> bool;

        // Whether an observation cycle is open, and so whether the host is still
        // holding a frame. This is the host-side truth a test asserts release
        // against; nothing about it involves the Lua collector.
        [[nodiscard]]
        auto hasOpenCycle() const noexcept -> bool;

        // Latches that this generation is spent, under the kind that spent it.
        // The binding layer sets it BEFORE raising, and gates every primitive on
        // it at the C guard entry, so a spent VM cannot resume automation even if
        // a script swallowed what was raised. Since ctx:try is pure Luau and
        // consults nothing, this latch is the whole of the terminal guarantee on
        // the Luau side.
        //
        // Two kinds reach it, and both need the same before-raising order for the
        // same reason. Cancelled is the host's own verdict on the generation.
        // InternalInvariant is a framework bug the trace state machine caught --
        // design section 9's rule 5 says latching first is what stops a project
        // pcall from swallowing it and driving one more primitive. Latching is
        // idempotent and keeps the FIRST kind: what spent the generation is what
        // ended it, and a later refusal is a consequence rather than a cause.
        void markTerminal(AutomationErrorKind kind) noexcept;

        [[nodiscard]]
        auto fatal() const noexcept -> bool;

        // The kind that spent the generation, or empty while it is still live.
        // The owning host reads it after the run so a generation that ended
        // terminally is still reported that way when the script caught the raise
        // and returned normally.
        [[nodiscard]]
        auto terminalKind() const noexcept -> std::optional<AutomationErrorKind>;

        // Latches that a trace event could not be recorded. A verb that is
        // already failing cannot raise the sink's failure instead of its own --
        // that would let a broken sink downgrade a cancellation into an error a
        // script can catch -- so it latches here and raises its real cause. The
        // owning host reads this after the run to report that the trace is
        // incomplete.
        void latchTraceFailure() noexcept;

        [[nodiscard]]
        auto traceFailed() const noexcept -> bool;

        // Records one event through the run's recorder, which stamps the sequence,
        // run id and generation id. A sink failure returns the error so the caller
        // can abort the operation whose evidence was lost, matching the engine's
        // trace discipline. The observation and action verbs call this at their
        // exit to emit task.native_call; the owning host emits the surrounding
        // run.started / run.resources_validated / run.finished directly on the
        // same recorder.
        [[nodiscard]]
        auto emitTrace(trace::TraceEvent const& event) -> Status;

        // The next uniform double in [0, 1) from the task's seeded RNG, backing the
        // no-argument ctx:random(). Deterministic for a given seed and draw order.
        [[nodiscard]]
        auto nextRandomUnitDouble() noexcept -> double;

        // A uniform integer in [lowInclusive, highInclusive] from the task's seeded
        // RNG, backing ctx:random(m) and ctx:random(m, n). The mapping is
        // unbiased (rejection sampling in DeterministicRng). Precondition, enforced
        // by the binding that parses the script arguments: lowInclusive <=
        // highInclusive and both within +/-2^53, so the result is an exact integer.
        [[nodiscard]]
        auto nextRandomInRange(int64 lowInclusive, int64 highInclusive) noexcept
            -> int64;
    };
}
