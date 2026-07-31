#pragma once

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <annotation/resource.hpp>

#include <domain/detection.hpp>
#include <domain/ids.hpp>

#include <engine/ports.hpp>

#include <ocr/engine.hpp>

#include <task/task-context.hpp>

#include <trace/event.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    // The operator front-end's consumer of the capability surface, defined in
    // task/operator-session.hpp. Declared here so startOperatorSession can hand one
    // back without this header depending on that one, which depends on this.
    class OperatorSession;

    // The event stream a resident host subscribes to. Deliberately declared and
    // never defined. subscribeEvents' shape is frozen here because the verb set
    // must not change between the one-run-per-process P0 and the resident P2
    // host, while what a task event actually *is* is a P2 question this stage
    // does not answer. An incomplete type states both facts at once: the
    // signature exists, and no caller can invent a payload for it.
    class ITaskEventSink;

    // How one task run ended. Completed is a clean script return; Cancelled is
    // the single cancel source spending the generation; Failed is every other
    // automation or script failure. It is never stored alongside the failure it
    // describes -- TaskRunReport derives it from that failure -- so a report and
    // the run.finished line written from it cannot disagree.
    enum class TaskRunOutcome : uint8
    {
        Completed,
        Cancelled,
        Failed,
    };

    // Per-generation settings for loadProject. Everything here is a property of
    // the loaded project instance rather than of one run: a run's own inputs
    // live in TaskRunConfig.
    struct TaskHostConfig final
    {
        // Cancellation the host does not own: a CLI's Ctrl-C registration, a
        // resident host's shutdown signal. The generation composes it with the
        // stop source cancel() drives, so an external stop and an explicit
        // cancel() both spend the generation and neither can shadow the other. A
        // default-constructed token never requests a stop, so an unconfigured
        // generation is simply never cancelled from outside.
        std::stop_token externalCancellation{};
    };

    // One run's inputs. Unlike an ordinary Config this is a move-in ownership
    // boundary: it carries the two ports the run drives, so it is passed to
    // startTask by value and the caller's copy is left empty. The ports are the
    // caller's, because binding a live desktop target is a composition-root job
    // that modules/task must not know how to do; everything else is a tuning
    // value the run reads once.
    //
    // liveFingerprint has no default -- a run that guessed the target's geometry
    // would defeat the engine's fail-closed compatibility check -- so every
    // construction site supplies it.
    struct TaskRunConfig final
    {
        std::unique_ptr<engine::IFrameSource> frameSource{};
        std::unique_ptr<engine::IActionSink>  actionSink{};

        // The OCR adapter cycle_read runs on, or null for a run that never reads
        // text. It is null by default because the weights are tens of megabytes:
        // a run that reads nothing must not pay for them, and cycle_read refuses
        // on its own terms rather than the run refusing to start.
        std::unique_ptr<ocr::IOcrEngine> ocrEngine{};

        annotation::ProjectFingerprint liveFingerprint;

        uint64                     maximumPixelComparisons{};
        MonotonicInstant::Duration recognitionTimeout{};
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};

        // The per-cycle text-read budget. It is a separate dimension from
        // maximumPixelComparisons on purpose; see k_defaultMaximumReadsPerCycle
        // in task/task-context.hpp for why the two cannot share a pool.
        uint32 maximumReadsPerCycle{k_defaultMaximumReadsPerCycle};

        // There is no page-wait budget here. How long a task waits for a page
        // and how often it re-observes are decided by the task, in Luau, where
        // the wait loop now lives; a host-side fallback would be a value nothing
        // reads.

        // Where this run's umbraflow-trace/v2 stream is written. One run writes
        // one file; the host opens it only after the task has loaded and
        // validated, so a misspelled task name leaves no evidence file behind.
        std::filesystem::path tracePath{};
    };

    // The product of one startTask call: what ran, what it can be replayed from,
    // and how it ended.
    //
    // A report exists exactly when the run bracket was opened -- once
    // run.started is in the trace, the run happened and is describable, however
    // it ended. Everything that stops a run before that point (an unknown
    // generation, a missing or invalid task, a trace file that will not open)
    // is a Result failure instead, because there is nothing to report on.
    //
    // `failure` is empty for a clean script return and otherwise carries the
    // error that ended the run. outcome() is derived from it, so the two can
    // never contradict each other and no construction site can invent a
    // Failed-with-no-failure state.
    struct TaskRunReport final
    {
        std::string taskName{};

        // Hex SHA-256 of the task source this run compiled, as stamped into
        // run.started: the run is attributable to those exact bytes.
        std::string sourceHash{};

        // The per-run seed the deterministic RNG drew from, as stamped into
        // run.started. Re-supplying it against the same observation sequence is
        // what makes a run reproducible.
        uint64 seed{};

        std::filesystem::path tracePath{};

        std::optional<Error> failure{};

        [[nodiscard]] auto outcome() const noexcept -> TaskRunOutcome;
    };

    // What the host can say about one generation between calls. P0 runs a task
    // synchronously inside startTask, so no caller can observe a run in
    // progress: the status is what the last finished run left behind, plus
    // whether the generation has been cancelled. lastOutcome is empty until a
    // run has finished.
    struct TaskStatus final
    {
        std::optional<TaskRunOutcome> lastOutcome{};
        std::string                   taskName{};
        bool                          cancellationRequested{};
    };

    // The host API for the script layer: load a project, run its tasks, cancel
    // and query them. It owns everything a run needs that is not the desktop
    // itself -- the loaded recognition runtime, the capability surface, the run
    // and generation identities, the trace recorder, the engine session, the
    // task context and the VM -- so the same run is reachable from a CLI, from a
    // resident host, and from a test.
    //
    // The verb set is fixed. pause, resume and subscribeEvents are P2 features
    // and return the existing UnsupportedCapability kind today. That is a
    // signature commitment, not speculative generality: their shape is frozen so
    // the API surface does not change between P0 and P2, and none of them has an
    // untested branch or a line of unused machinery behind it. Do not "clean
    // them up", and do not implement pause -- the framework's observation-cycle
    // boundary is where a future pause point goes, and it does not exist yet.
    //
    // NOT thread-safe: every verb runs on the owning thread. The one exception
    // is the cancellation a generation observes, which is an std::stop_token by
    // construction and may be signalled from anywhere.
    class TaskHost final
    {
        class Generation;

        std::vector<std::unique_ptr<Generation>> m_generations{};

        uint64 m_nextGenerationValue{1};
        uint64 m_nextRunValue{1};

        // Optional, non-owning observation of one generation this host owns, or
        // null when `id` names none. The pointee lives in m_generations and is
        // never relocated -- a Generation is non-movable and is held through a
        // unique_ptr -- so the returned pointer stays valid until the host dies.
        [[nodiscard]]
        auto findGeneration(GenerationId id) noexcept -> Generation*;

    public:
        TaskHost();

        TaskHost(TaskHost const&) = delete;
        TaskHost(TaskHost&&) = delete;
        auto operator=(TaskHost const&) -> TaskHost& = delete;
        auto operator=(TaskHost&&) -> TaskHost& = delete;

        ~TaskHost();

        // Reads the published annotation project at `projectRoot`, builds its
        // recognition runtime and the script-visible capability surface, and
        // registers both as one generation. Nothing observable happens here: a
        // bad project fails before any target is bound and before any trace file
        // is opened.
        [[nodiscard]]
        auto loadProject(
            std::filesystem::path const& projectRoot,
            TaskHostConfig const& config = {}
        ) -> Result<GenerationId>;

        // Runs `taskName` from `generation`'s project to completion. The task is
        // addressed as (project, name) and is loaded and fully validated before
        // any VM exists; the host then opens the run's trace, draws the run's
        // seed, writes run.started and run.resources_validated, builds the engine
        // session over `config`'s ports, runs the script, and writes
        // run.finished. Blocks until the script returns or the generation is
        // cancelled.
        [[nodiscard]]
        auto startTask(
            GenerationId generation,
            std::string_view taskName,
            TaskRunConfig config
        ) -> Result<TaskRunReport>;

        // Binds `generation`'s project to an operator front-end and hands back the
        // session it drives. The operator is a SIBLING consumer of the same private
        // capability surface a task's Luau framework consumes, never a route into
        // Luau; see task/operator-session.hpp for what that means and what it
        // deliberately does not include.
        //
        // WHY THIS IS THE MUTUAL EXCLUSION, AND WHY IT IS HERE. A generation holds
        // the single-open-cycle ledger, so two policy sources driving one generation
        // would contend for it: a task's wait loop and an operator's commands would
        // each believe they owned the one open cycle. This is where that is made
        // impossible rather than documented. The first of startTask and
        // startOperatorSession to reach a generation LATCHES that generation's
        // front-end, and the other is refused for the life of the generation --
        // whichever order they arrive in, and however many times either is called.
        //
        // The latched value is also what the generation hands the trace recorder, so
        // a stream's attribution and the exclusion that produced it are one fact.
        // Nothing above this can weaken it: a CLI that dispatched both subcommands in
        // one process would meet this refusal, and so would a resident host.
        //
        // A second call under the SAME front-end is allowed, because a generation
        // legitimately runs several tasks in sequence and would legitimately host
        // several operator sessions; what it must never do is mix them.
        //
        // Unlike startTask this does not block: the session is returned live and the
        // caller drives it verb by verb, then calls OperatorSession::finish to close
        // the run bracket.
        [[nodiscard]]
        auto startOperatorSession(
            GenerationId generation,
            TaskRunConfig config
        ) -> Result<std::unique_ptr<OperatorSession>>;

        // Spends `generation`: the engine returns Cancelled from its next verb
        // and the VM interrupt hard-breaks the running task thread. Idempotent,
        // and safe to call while startTask is running on another thread.
        [[nodiscard]]
        auto cancel(GenerationId generation) -> Status;

        [[nodiscard]]
        auto queryTask(GenerationId generation) -> Result<TaskStatus>;

        // P2 verbs. Each returns UnsupportedCapability today; see the class
        // comment for why they exist at all.
        [[nodiscard]]
        auto pause(GenerationId generation) -> Status;

        [[nodiscard]]
        auto resume(GenerationId generation) -> Status;

        [[nodiscard]]
        auto subscribeEvents(ITaskEventSink& sink) -> Status;
    };
}
