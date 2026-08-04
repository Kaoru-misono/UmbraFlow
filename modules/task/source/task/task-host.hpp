#pragma once

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>


#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <engine/ports.hpp>

#include <ocr/engine.hpp>

#include <script/engine.hpp>

#include <task/task-context.hpp>

#include <trace/event.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    // The script layer's own ceiling on one unit of script, named here so a run
    // config carries it explicitly rather than inheriting it silently. It is an
    // alias and not a second thirty minutes: an exploration session sets no
    // maxRuntime at all and answers to the script layer's value, so the two must
    // be the same length by construction.
    inline constexpr auto k_defaultMaxScriptRuntime = script::k_defaultMaxRuntime;

    // Defined in task/exploration-session.hpp; forward-declared so this header
    // does not depend on one that depends on it.
    class ExplorationSession;

    // The event stream a resident host subscribes to. Deliberately declared and
    // never defined: subscribeEvents' shape is frozen so the verb set does not
    // change between the one-run-per-process P0 and the resident P2 host, while
    // what a task event *is* remains a P2 question. An incomplete type states
    // both at once -- the signature exists, and no caller can invent a payload.
    class ITaskEventSink;

    // How one task run ended. Completed is a clean script return; Cancelled is
    // the single cancel source spending the generation; Failed is every other
    // automation or script failure. It is never stored alongside the failure it
    // describes -- TaskRunReport derives it -- so a report and the run.finished
    // line written from it cannot disagree.
    enum class TaskRunOutcome : uint8
    {
        Completed,
        Cancelled,
        Failed,
    };

    // Per-generation settings for loadProject: properties of the loaded project
    // instance rather than of one run, whose inputs live in TaskRunConfig.
    struct TaskHostConfig final
    {
        // Cancellation the host does not own: a CLI's Ctrl-C registration, a
        // resident host's shutdown signal. The generation composes it with the
        // stop source cancel() drives, so neither can shadow the other. A
        // default-constructed token never requests a stop.
        std::stop_token externalCancellation{};
    };

    // One run's inputs. Unlike an ordinary Config this is a move-in ownership
    // boundary: it carries the two ports the run drives, so it is passed to
    // startTask by value and the caller's copy is left empty. The ports are the
    // caller's, because binding a live desktop target is a composition-root job
    // that modules/task must not know how to do.
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

        ProjectFingerprint liveFingerprint;

        uint64                     maximumPixelComparisons{};
        MonotonicInstant::Duration recognitionTimeout{};
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};

        // Wall-clock ceiling on the task script. A run is ONE unit of script --
        // the whole task is one runValue call -- so this bounds the entire run
        // rather than a step of it. Thirty minutes is the script layer's own
        // default and a real daily exceeds it: a 142-step run was cut off
        // mid-battle. `umbra-flow run --max-runtime` is how a caller raises it.
        MonotonicInstant::Duration maxScriptRuntime{k_defaultMaxScriptRuntime};

        // The per-cycle text-read budget, a separate dimension from
        // maximumPixelComparisons on purpose; see k_defaultMaximumReadsPerCycle
        // in task/task-context.hpp.
        uint32 maximumReadsPerCycle{k_defaultMaximumReadsPerCycle};

        // The per-cycle crop budget, a third dimension on the same reasoning.
        // Only an exploration session can spend it, but it lives here because a
        // run config is where every recognition and delivery bound is stated.
        uint32 maximumCropsPerCycle{k_defaultMaximumCropsPerCycle};

        // The VM's hard memory ceiling, or zero for the script layer's own
        // default. Here rather than left to that default because one caller's
        // need is a property of the FILE and not of a policy: the falsification
        // matrix accumulates one row per element per screen and renders every
        // one, so a corpus that grows walks into a ceiling sized for a business
        // task. Luau throws the instant the accounting allocator refuses and
        // never collects and retries, so the ceiling is measured against live
        // bytes plus whatever the incremental collector has not reached
        // (docs/pitfalls/embedded-vm-memory-ceiling.md) -- which is why a caller
        // sizing this from a row count leaves the collector room rather than
        // budgeting the rows alone.
        uint64 memoryQuotaBytes{};

        // There is no page-wait budget here: how long a task waits for a page and
        // how often it re-observes are decided in Luau, where the wait loop lives.

        // Where this run's umbraflow-trace/v4 stream is written. One run writes
        // one file; it is opened only after the task has loaded and validated, so
        // a misspelled task name leaves no evidence file behind.
        std::filesystem::path tracePath{};
    };

    // The product of one startTask call: what ran, what it can be replayed from,
    // and how it ended. A report exists exactly when the run bracket was opened
    // -- once run.started is in the trace, the run happened and is describable,
    // however it ended. Everything that stops a run before that point (an unknown
    // generation, a missing or invalid task, a trace file that will not open) is
    // a Result failure instead.
    //
    // `failure` is empty for a clean script return and otherwise carries the
    // error that ended the run. outcome() is derived from it, so no construction
    // site can invent a Failed-with-no-failure state.
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

        // What the task's chunk returned, rendered as one line, empty when it
        // returned nothing. A task's return is its only voice: the four facts
        // above are the host's, and the trace records every native call rather
        // than the task's own account of them, so a run that stopped at step 176
        // for a reason only the script knows had nowhere to say so.
        std::string returned{};

        std::optional<Error> failure{};

        [[nodiscard]] auto outcome() const noexcept -> TaskRunOutcome;
    };

    // Closes one session's run bracket, which is the half of finish() every
    // front-end shares. `terminal` is the kind a spent generation ended the
    // session under and is empty when it was not spent; it is folded into
    // `report` only when the session carries no failure of its own, and a lost
    // run.finished line is folded in on the same terms. `recorder` is written to
    // because writing that line is the whole operation.
    //
    // A task run does not use it: startTask has two more verdicts to fold, and
    // their order against the terminal latch is part of the contract.
    [[nodiscard]]
    auto closeRunBracket(
        trace::TraceRecorder& recorder,
        TaskRunReport report,
        std::optional<AutomationErrorKind> terminal,
        std::string_view terminalMessage
    ) -> TaskRunReport;

    // One trusted routine the host itself supplies, run through the same bracket
    // a project task runs through. Trusted precisely because its source is a
    // string literal in this binary: it is not addressed as (project, name),
    // never read from disk, and no project can supply, replace or shadow it.
    // Everything else -- the VM, the private capability surface, the trace
    // bracket, the generation, the cancellation -- is identical to a task's.
    //
    // Lifetime contract: both views must outlive the runFrameworkRoutine call.
    // Every caller in this repository satisfies that with a string literal.
    struct FrameworkRoutine final
    {
        // Labels the run in the trace and in compile diagnostics, going into
        // run.started's task name.
        std::string_view name{};

        std::string_view source{};
    };

    // What one framework routine's run produced.
    struct FrameworkRoutineReport final
    {
        TaskRunReport run{};

        // The number the routine's chunk returned, which for a routine is its
        // answer: `umbra-flow check` returns how many things the falsification
        // matrix found wrong. Zero for a run that failed before returning, where
        // the report's own failure is what says so.
        double answer{};
    };

    // What the host can say about one generation between calls. P0 runs a task
    // synchronously inside startTask, so no caller can observe a run in progress:
    // the status is what the last finished run left behind, plus whether the
    // generation has been cancelled. lastOutcome is empty until a run finishes.
    struct TaskStatus final
    {
        std::optional<TaskRunOutcome> lastOutcome{};
        std::string                   taskName{};
        bool                          cancellationRequested{};
    };

    // The host API for the script layer: load a project, run its tasks, cancel
    // and query them. It owns everything a run needs that is not the desktop
    // itself, so the same run is reachable from a CLI, from a resident host, and
    // from a test.
    //
    // The verb set is fixed. pause, resume and subscribeEvents are P2 features
    // returning UnsupportedCapability today; their shape is frozen so the API
    // surface does not change between P0 and P2, and none has an untested branch
    // or a line of unused machinery behind it. Do not "clean them up", and do not
    // implement pause -- the framework's observation-cycle boundary is where a
    // future pause point goes, and it does not exist yet.
    //
    // NOT thread-safe: every verb runs on the owning thread, except the
    // cancellation a generation observes, which is an std::stop_token by
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

        // findGeneration with the refusal every public verb below opens with, so
        // an unknown generation is named once rather than once per verb. The
        // borrow it hands back is findGeneration's, on the same terms.
        [[nodiscard]]
        auto requireGeneration(GenerationId id) -> Result<Generation*>;

    public:
        TaskHost();

        TaskHost(TaskHost const&) = delete;
        TaskHost(TaskHost&&) = delete;
        auto operator=(TaskHost const&) -> TaskHost& = delete;
        auto operator=(TaskHost&&) -> TaskHost& = delete;

        ~TaskHost();

        // Reads <projectRoot>/page-model.toml for the two things the host needs
        // before a VM exists -- the geometry the model was authored at, and the
        // element and page names a script may spell -- and registers them as one
        // generation. Nothing observable happens here: a bad project fails before
        // any target is bound and before any trace file is opened.
        //
        // It reads no generated/annotations.runtime.toml
        // (docs/plans/2026-07-31-script-owned-page-model.md 9); the model a run
        // works from is the one layer two loads for itself through project_read.
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

        // The geometry `generation`'s project was authored at, as its page model
        // states it. A live front-end measures this from the window it bound and
        // hands it back as TaskRunConfig::liveFingerprint, which is what makes the
        // engine's compatibility check mean something. A front-end whose frames
        // come from the project's OWN screens has no window to measure, and the
        // project's own fingerprint is the honest answer for it: those screens
        // were captured at that geometry.
        [[nodiscard]]
        auto projectFingerprint(
            GenerationId generation
        ) -> Result<ProjectFingerprint>;

        // How many elements `generation`'s project declares, as its page model
        // lists them. Here for the one caller that must size a run bound from the
        // FILE rather than from a policy constant: the falsification matrix holds
        // a row per measured cell, and this count times the screen count is the
        // lower bound it sizes the VM's memory from (entry/cli/check.cpp, where
        // the gap between that bound and the true row count is stated). The
        // pre-VM resource pass already reads the names, so this asks the flat
        // line scan nothing new.
        [[nodiscard]]
        auto projectElementCount(GenerationId generation) -> Result<std::size_t>;

        // Runs one of the host's own trusted routines against `generation`'s
        // project and reports how it ended and what it answered.
        //
        // It is startTask with the two steps that police an untrusted script
        // replaced: the source comes from `routine` instead of from
        // <projectRoot>/tasks/<name>.luau, and its chunk hash is taken from those
        // bytes. Everything observable is the same -- the front-end claim, the
        // resource closure pass, the run bracket, the seed, the engine session,
        // the VM's two environments -- so a routine cannot quietly enjoy a wider
        // surface than a task.
        //
        // It claims the CHECK front-end, so its stream is attributed to a run
        // that measures and delivers nothing, and a generation is never shared
        // with a task run: a check tries every page against one frame, and a
        // reader taking that for the sequence of pages a run walked would report
        // transitions no page graph covers (trace::FrontEnd, and
        // docs/plans/2026-08-04-state-layer-and-policy-slots.md 4.2). Two
        // routines may still share one generation in sequence.
        [[nodiscard]]
        auto runFrameworkRoutine(
            GenerationId generation,
            FrameworkRoutine const& routine,
            TaskRunConfig config
        ) -> Result<FrameworkRoutineReport>;

        // Binds `generation`'s project to the agent front-end and hands back the
        // session it drives. It owns a Luau VM, booted with the wider private
        // surface and the two extra published modules, and runs one
        // agent-supplied chunk per call (task/exploration-session.hpp).
        //
        // This is where the front-end mutual exclusion is enforced rather than
        // documented. A generation holds the single-open-cycle ledger, so a
        // task's wait loop and an agent's chunks driving one generation would
        // each believe they owned the one open cycle. The first of startTask,
        // runFrameworkRoutine and this to reach a generation LATCHES that
        // generation's front-end, and the others are refused for the life of the
        // generation -- whichever order they arrive in, and however many times
        // any is called. The latched value is also what the generation hands the
        // trace recorder, so a stream's attribution and the exclusion that
        // produced it are one fact.
        //
        // A second call under the SAME front-end is allowed: a generation
        // legitimately runs several tasks in sequence and would legitimately host
        // several exploration sessions. What it must never do is mix them.
        //
        // Unlike startTask this does not block: the session is returned live and
        // the caller feeds it chunks, then calls ExplorationSession::finish to
        // close the run bracket.
        [[nodiscard]]
        auto startExplorationSession(
            GenerationId generation,
            TaskRunConfig config
        ) -> Result<std::unique_ptr<ExplorationSession>>;

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
