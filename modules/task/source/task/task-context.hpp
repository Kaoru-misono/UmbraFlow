#pragma once

#include <task/deterministic.hpp>
#include <task/trace.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <annotation/catalog.hpp>
#include <annotation/recognition.hpp>

#include <engine/session.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>

namespace uf::task
{
    // A monotonically increasing tag identifying one capture. Every script-facing
    // frame / outcome / page / hit handle records the sequence of the observation
    // it descends from, so the binding layer can reject a click that mixes objects
    // from two different captures before they ever reach the engine. A plain
    // integer would work; the alias documents that the value is an observation
    // identity and not an array index or a count.
    using ObservationSeq = uint64;

    // The default seed for a task's deterministic RNG (umbra:random). A real run
    // overrides it with a host-chosen seed recorded in the task trace (task-trace/
    // v1), so a run can be replayed by re-supplying the same seed; this constant
    // only gives tests and unconfigured contexts a stable, non-zero starting point.
    // Because the sandbox removed math.random, umbra:random seeded from this value
    // is a script's only randomness, so nothing outside the host can perturb the
    // sequence.
    inline constexpr auto k_defaultRandomSeed = uint64{0x9E3779B97F4A7C15};

    // Host-side configuration for one TaskContext: the wait budgets a script's
    // umbra:wait_for_page falls back to when it omits them, plus the single
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
        // Guardrail on how many observations one script may keep retained at
        // once. Every live frame handle pins a whole-frame Observation
        // (megabytes of BGRA pixels) in host memory, outside the Lua accounting
        // allocator's view, so an unbounded backlog can exhaust the host long
        // before a Lua instruction or time budget trips. Under Model B a running
        // script holds only the single frame it is inspecting, so this is a
        // guard against pathological retention, not a working limit; the binding
        // layer forces a full VM collection to reclaim dead frame handles before
        // it trips, so this bound counts only frames the script still references.
        // The conservative placeholder awaits calibration against the first real
        // daily. 0 disables the guard.
        uint64          maxLiveObservations{8};
        std::stop_token cancellation{};

        // Fixed seed for this task's deterministic RNG (umbra:random). Left at the
        // stable default here; the owning host injects a fresh per-run seed and
        // records it in the task trace so a run reproduces on replay. See
        // k_defaultRandomSeed.
        uint64          randomSeed{k_defaultRandomSeed};
    };

    // The completed product of umbra:wait_for_page after the engine's move-only
    // PageWait has been split: the resolved page (copied into the returned page
    // handle) and the sequence under which the wait's own observation is now
    // retained. The returned frame and page handles both carry this seq, so they
    // can be handed to umbra:click together -- the cross-frame guard applies to a
    // wait's paired page and frame exactly as it does to a capture's.
    struct WaitResolved final
    {
        ObservationSeq           seq;
        annotation::ResolvedPage page;
    };

    // Host-owned bridge between one task VM and one EngineSession. It owns the
    // session (moved in by the caller, who is responsible for constructing the WGC
    // / controller ports) and the live observations the running script has
    // captured but not yet consumed, each keyed by a monotonic sequence. The Luau
    // binding layer reaches this object through a lightuserdata upvalue and never
    // sees engine types: every engine call, and all ownership of the move-only
    // Observation, stays here in host C++.
    //
    // Lifetime contract: the caller MUST keep the TaskContext alive for at least
    // as long as the script::Engine (task VM) that binds it, because the VM's host
    // functions hold a raw pointer to it. The context is therefore non-movable so
    // that pointer, and the address of the owned EngineSession that every retained
    // Observation points back to, both stay stable. NOT thread-safe: every method
    // runs on the VM's owning thread.
    class TaskContext final
    {
        engine::EngineSession m_session;
        TaskContextConfig     m_config;
        DeterministicClock    m_clock{};
        DeterministicRng      m_rng;
        // The task-trace sink, or null when tracing is disabled. Owned here
        // because the context is the single object every host verb already
        // reaches, so HostCall events emit through it (emitTrace) without a
        // second lifetime to thread; the owning host emits the surrounding
        // TaskStarted / ResourcesValidated / TaskFinished through the same object.
        std::unique_ptr<TaskTraceSink> m_traceSink;
        std::map<ObservationSeq, engine::Observation> m_observations{};
        ObservationSeq m_nextSeq{1};
        bool           m_fatal{false};
        bool           m_traceFailed{false};

    public:
        explicit TaskContext(
            engine::EngineSession session,
            TaskContextConfig config = {},
            std::unique_ptr<TaskTraceSink> traceSink = nullptr
        ) noexcept;

        TaskContext(TaskContext const&) = delete;
        TaskContext(TaskContext&&) = delete;
        auto operator=(TaskContext const&) -> TaskContext& = delete;
        auto operator=(TaskContext&&) -> TaskContext& = delete;

        ~TaskContext() = default;

        // Observes a fresh frame and retains it under a new sequence, returned to
        // the binding layer to bake into the frame handle. A capture failure
        // propagates unchanged for the binding layer's Tier mapping.
        [[nodiscard]]
        auto capture() -> Result<ObservationSeq>;

        // Resolves the page of the retained observation `seq`. A sequence with no
        // live observation -- already consumed by a click, or never captured --
        // fails StaleObservation, so a frame handle used after its click reports
        // exactly the engine's own post-consume contract.
        [[nodiscard]]
        auto resolvePage(ObservationSeq seq) -> Result<annotation::PageOutcome>;

        // Searches the retained observation `seq` for one action target. An empty
        // optional is a completed miss (the caller maps it to nil), not a failure.
        [[nodiscard]]
        auto findAction(
            ObservationSeq seq,
            annotation::RecognizerId recognizerId
        ) -> Result<std::optional<engine::ActionFound>>;

        // Consumes the observation shared by `page` and `hit` and delivers the
        // click. A cross-frame mix (pageSeq != hitSeq) is rejected here, before the
        // engine, so mismatched evidence never reaches authorization. Consuming the
        // observation -- on success or failure, because act takes it by rvalue --
        // makes every later use of that frame fail StaleObservation.
        [[nodiscard]]
        auto click(
            ObservationSeq pageSeq,
            ObservationSeq hitSeq,
            annotation::ResolvedPage const& page,
            engine::ActionFound const& action
        ) -> Result<engine::ActReceipt>;

        // Polls captures until `pageId` resolves, then retains the resolving
        // observation under a fresh sequence and returns it alongside the resolved
        // page, so the binding layer can build the paired frame and page handles
        // that share that seq. A nullopt timeout or poll interval falls back to the
        // configured default. A timeout is Timeout; a cancellation is Cancelled;
        // both propagate unchanged for the binding layer's Tier mapping.
        [[nodiscard]]
        auto waitForPage(
            annotation::PageId pageId,
            std::optional<MonotonicInstant::Duration> timeout,
            std::optional<MonotonicInstant::Duration> pollInterval
        ) -> Result<WaitResolved>;

        // Drops the observation retained under `seq`, freeing the frame it pins,
        // when one is still live. A seq with no live observation -- already
        // consumed by a click, or released once already -- is a harmless no-op,
        // so a frame handle's garbage collection can release unconditionally
        // without racing click's own erase or double-freeing a consumed frame.
        // noexcept: erasing a std::map entry by key does not throw.
        void release(ObservationSeq seq) noexcept;

        // The number of observations currently retained -- captured or waited but
        // not yet consumed or released. The binding layer's frame-retention
        // guardrail reads it before retaining one more, and tests assert that a
        // dropped frame handle actually reclaimed its observation.
        [[nodiscard]]
        auto liveObservationCount() const noexcept -> uint64;

        // The configured ceiling on live observations the binding layer enforces
        // (see TaskContextConfig::maxLiveObservations). 0 means unbounded.
        [[nodiscard]]
        auto maxLiveObservations() const noexcept -> uint64;

        // True once the shared cancellation source has requested a stop. The
        // binding layer's umbra:try consults this after a protected call so a
        // cancellation is never handed back to the script as a recoverable error:
        // the same stop token that makes the engine return Cancelled also drives
        // the VM interrupt, so a requested stop means the generation is spent.
        [[nodiscard]]
        auto cancelled() const noexcept -> bool;

        // Latches that an unrecoverable cancellation was observed. The binding
        // layer sets this before raising its non-catchable sentinel, and gates
        // every later engine verb on it, so a spent VM cannot resume automation
        // even if a script swallowed the sentinel. The owning host reads it after
        // the run to tell a cancelled generation from a merely errored one.
        void markFatal() noexcept;

        [[nodiscard]]
        auto fatal() const noexcept -> bool;

        // Latches that a task-trace event could not be recorded. A verb that is
        // already failing cannot raise the sink's failure instead of its own --
        // that would let a broken sink downgrade a cancellation into an error a
        // script can catch -- so it latches here and raises its real cause. The
        // owning host reads this after the run to report that the trace is
        // incomplete.
        void latchTraceFailure() noexcept;

        [[nodiscard]]
        auto traceFailed() const noexcept -> bool;

        // Records one task-trace event through the sink installed at construction,
        // or a success no-op when tracing is disabled (null sink). A sink failure
        // returns the error so the caller can abort the operation whose evidence
        // was lost, matching the engine's trace discipline. The observation and
        // action verbs call this at their exit to emit HostCall; the owning host
        // emits TaskStarted / ResourcesValidated / TaskFinished around the run.
        [[nodiscard]]
        auto emitTrace(TaskTraceEvent const& event) -> Status;

        // The next reading of the task's logical clock, in whole milliseconds,
        // backing umbra:now(). Monotone and non-decreasing across calls, and
        // identical across runs of the same script: the clock is virtualized (a
        // fixed logical tick per read, no wall clock), so reading it advances the
        // clock -- hence non-const. See DeterministicClock for why now() is a
        // reproducible logical ordinal and not real elapsed time.
        [[nodiscard]]
        auto nowMillis() noexcept -> int64;

        // The next uniform double in [0, 1) from the task's seeded RNG, backing the
        // no-argument umbra:random(). Deterministic for a given seed and draw order.
        [[nodiscard]]
        auto nextRandomUnitDouble() noexcept -> double;

        // A uniform integer in [lowInclusive, highInclusive] from the task's seeded
        // RNG, backing umbra:random(m) and umbra:random(m, n). The mapping is
        // unbiased (rejection sampling in DeterministicRng). Precondition, enforced
        // by the binding that parses the script arguments: lowInclusive <=
        // highInclusive and both within +/-2^53, so the result is an exact integer.
        [[nodiscard]]
        auto nextRandomInRange(int64 lowInclusive, int64 highInclusive) noexcept
            -> int64;
    };
}
