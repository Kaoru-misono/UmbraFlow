#pragma once

#include <task/cycle-ledger.hpp>
#include <task/deterministic.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <annotation/resource.hpp>
#include <annotation/recognition.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <chrono>
#include <optional>
#include <stop_token>

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

    // Host-side configuration for one TaskContext: the wait budgets a script's
    // ctx:wait_for_page falls back to when it omits them, plus the single
    // cancellation source shared with the owned EngineSession and the VM
    // interrupt. The wait defaults are conservative placeholders pending
    // calibration against the first real daily (the plan's example waits ten
    // minutes for a page); the poll cadence bounds capture churn while waiting. A
    // default-constructed stop token never requests a stop, so an unconfigured
    // context is never cancelled -- preserving the resource-only and Fake-driven
    // paths that build a context without wiring a cancel source.
    struct TaskContextConfig final
    {
        MonotonicInstant::Duration defaultWaitTimeout{
            std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::minutes{10}
            )
        };
        MonotonicInstant::Duration defaultWaitPollInterval{
            std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{500}
            )
        };
        std::stop_token cancellation{};

        // Fixed seed for this task's deterministic RNG (ctx:random). Left at the
        // stable default here; a real run gets a fresh per-run seed from
        // TaskHost::startTask, which draws it and records it in run.started so
        // the run reproduces on replay. See k_defaultRandomSeed.
        uint64          randomSeed{k_defaultRandomSeed};
    };

    // The product of ctx:wait_for_page: the observation cycle now open over
    // the frame that resolved the page, and the page itself. The wait already
    // resolved it, so the ledger holds it as that cycle's click authorization
    // evidence and the script never resolves the same frame twice.
    struct CycleWait final
    {
        CycleTicket              ticket{};
        annotation::ResolvedPage page;
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
        engine::EngineSession m_session;
        TaskContextConfig     m_config;
        DeterministicClock    m_clock{};
        DeterministicRng      m_rng;
        trace::TraceRecorder& m_recorder;

        CycleLedger m_cycles{};

        bool m_fatal{false};
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

        // Resolves the page of the frame `ticket`'s cycle retains and records it
        // in the ledger as that cycle's click authorization evidence. An empty
        // optional is Unknown or Ambiguous -- a completed resolution the engine
        // already traced, not a failure. A ticket naming no open cycle fails
        // StaleObservation.
        [[nodiscard]]
        auto cyclePage(
            CycleTicket ticket
        ) -> Result<std::optional<annotation::ResolvedPage>>;

        // Searches the frame `ticket`'s cycle retains for one action target. An
        // empty optional is a completed miss (the caller maps it to nil), not a
        // failure.
        [[nodiscard]]
        auto cycleFind(
            CycleTicket ticket,
            annotation::RecognizerId recognizerId
        ) -> Result<std::optional<engine::ActionFound>>;

        // Spends the cycle `ticket` names and delivers the click.
        //
        // It takes no page. The authorization evidence is the page that cycle
        // itself resolved, which the ledger holds, so a script cannot supply a
        // page drawn from another frame: there is no parameter to supply one
        // through, and under the one-cycle rule there is no other frame to draw
        // one from. The four-requisite authorization is therefore satisfied by
        // construction rather than checked. A cycle that resolved no page has no
        // evidence and fails ActionRejected.
        //
        // `hitCycleOrdinal` is the ordinal the hit handle carries and must be the
        // open cycle's own; anything else names a cycle that no longer exists and
        // fails StaleObservation. The cycle is spent whatever the click's
        // outcome, because act consumes the frame by rvalue.
        [[nodiscard]]
        auto cycleClick(
            CycleTicket ticket,
            uint64 hitCycleOrdinal,
            engine::ActionFound const& action
        ) -> Result<engine::ActReceipt>;

        // Polls captures until `pageId` resolves, then opens the cycle over the
        // resolving observation and records the page the wait already resolved as
        // that cycle's authorization evidence. A nullopt timeout or poll interval
        // falls back to the configured default. An already-open cycle fails
        // InternalInvariant exactly as openCycle does; a timeout is Timeout and a
        // cancellation is Cancelled, both propagating unchanged for the binding
        // layer's Tier mapping.
        [[nodiscard]]
        auto waitForPage(
            annotation::PageId pageId,
            std::optional<MonotonicInstant::Duration> timeout,
            std::optional<MonotonicInstant::Duration> pollInterval
        ) -> Result<CycleWait>;

        // Whether an observation cycle is open, and so whether the host is still
        // holding a frame. This is the host-side truth a test asserts release
        // against; nothing about it involves the Lua collector.
        [[nodiscard]]
        auto hasOpenCycle() const noexcept -> bool;

        // Latches that an unrecoverable cancellation was observed. The binding
        // layer sets this before raising its non-catchable sentinel, and gates
        // every primitive on it at the C guard entry, so a spent VM cannot resume
        // automation even if a script swallowed the sentinel. Since ctx:try is
        // pure Luau and consults nothing, this latch is the whole of the terminal
        // guarantee on the Luau side. The owning host reads it after the run to
        // tell a cancelled generation from a merely errored one.
        void markFatal() noexcept;

        [[nodiscard]]
        auto fatal() const noexcept -> bool;

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

        // The next reading of the task's logical clock, in whole milliseconds,
        // backing ctx:now(). Monotone and non-decreasing across calls, and
        // identical across runs of the same script: the clock is virtualized (a
        // fixed logical tick per read, no wall clock), so reading it advances the
        // clock -- hence non-const. See DeterministicClock for why now() is a
        // reproducible logical ordinal and not real elapsed time.
        [[nodiscard]]
        auto nowMillis() noexcept -> int64;

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
