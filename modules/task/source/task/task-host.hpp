#pragma once

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <annotation/resource.hpp>

#include <domain/detection.hpp>
#include <domain/ids.hpp>

#include <engine/ports.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
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

        annotation::ProjectFingerprint liveFingerprint;

        uint64                     maximumPixelComparisons{};
        MonotonicInstant::Duration recognitionTimeout{};
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};

        // Fallbacks a script's page wait uses when it names neither. They are the
        // task layer's policy knobs, not the engine's, and are forwarded verbatim
        // to the TaskContext this run builds.
        MonotonicInstant::Duration defaultWaitTimeout{};
        MonotonicInstant::Duration defaultWaitPollInterval{};

        // Where this run's umbraflow-trace/v1 stream is written. One run writes
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
