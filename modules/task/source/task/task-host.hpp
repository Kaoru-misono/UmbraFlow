#pragma once

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>


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
    // The operator front-end's consumer of the private capability surface,
    // defined in task/operator-session.hpp. Declared here so startOperatorSession
    // can hand one back without this header depending on that one, which depends
    // on this.
    class OperatorSession;

    // The agent front-end's session, defined in task/exploration-session.hpp.
    // Declared here for the same reason OperatorSession is.
    class ExplorationSession;

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

        ProjectFingerprint liveFingerprint;

        uint64                     maximumPixelComparisons{};
        MonotonicInstant::Duration recognitionTimeout{};
        MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};

        // The per-cycle text-read budget. It is a separate dimension from
        // maximumPixelComparisons on purpose; see k_defaultMaximumReadsPerCycle
        // in task/task-context.hpp for why the two cannot share a pool.
        uint32 maximumReadsPerCycle{k_defaultMaximumReadsPerCycle};

        // The per-cycle crop budget, a third dimension on the same reasoning.
        // Only an exploration session can spend it -- a run VM has no crop verb
        // -- but it lives here rather than on the exploration spec because a run
        // config is where every recognition and delivery bound is stated, and a
        // budget kept somewhere else would be the one a reader had to go looking
        // for.
        uint32 maximumCropsPerCycle{k_defaultMaximumCropsPerCycle};

        // There is no page-wait budget here. How long a task waits for a page
        // and how often it re-observes are decided by the task, in Luau, where
        // the wait loop now lives; a host-side fallback would be a value nothing
        // reads.

        // Where this run's umbraflow-trace/v3 stream is written. One run writes
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

    // One trusted routine the host itself supplies, run through the same bracket
    // a project task runs through.
    //
    // WHAT MAKES IT TRUSTED, precisely: the source is a string literal in this
    // binary rather than a file in the project, so it is not addressed as
    // (project, name), it is never read from disk, and no project can supply,
    // replace or shadow it. Everything else about the run is identical to a
    // task's -- the same VM, the same private capability surface, the same
    // trace bracket, the same generation and the same cancellation -- because a
    // routine that ran under weaker guarantees would be measuring a system the
    // product does not have.
    //
    // Lifetime contract: both views must outlive the runFrameworkRoutine call.
    // Every caller in this repository satisfies that with a string literal.
    struct FrameworkRoutine final
    {
        // Labels the run in the trace and in compile diagnostics. It goes into
        // run.started's task name, so a routine's evidence stream is told apart
        // from a task's by what ran rather than by which verb was called.
        std::string_view name{};

        std::string_view source{};
    };

    // What one framework routine's run produced.
    struct FrameworkRoutineReport final
    {
        TaskRunReport run{};

        // The number the routine's chunk returned, which for a routine -- unlike
        // for a task -- is its answer: `umbra-flow check` returns how many things
        // the falsification matrix found wrong. It is zero for a run that failed
        // before returning, and the report's own failure is what says so.
        double answer{};
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
    // itself -- the facts read out of the page model, the run and generation
    // identities, the trace recorder, the engine session, the task context and
    // the VM -- so the same run is reachable from a CLI, from a resident host,
    // and from a test.
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

        // Reads <projectRoot>/page-model.toml for the two things the host needs
        // before a VM exists -- the geometry the model was authored at, and the
        // element and page names a script may spell -- and registers them as one
        // generation. Nothing observable happens here: a bad project fails before
        // any target is bound and before any trace file is opened.
        //
        // It reads no generated/annotations.runtime.toml. That manifest went
        // with the v4 authoring line
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
        // states it.
        //
        // A live front-end measures this from the window it bound and hands it
        // back as TaskRunConfig::liveFingerprint, which is what makes the
        // engine's compatibility check mean something. A front-end whose frames
        // come from the project's OWN screens has no window to measure, and the
        // project's own fingerprint is the honest answer for it: those screens
        // were captured at that geometry, which is exactly what makes them the
        // screens this model is about.
        [[nodiscard]]
        auto projectFingerprint(
            GenerationId generation
        ) -> Result<ProjectFingerprint>;

        // Runs one of the host's own trusted routines against `generation`'s
        // project and reports how it ended and what it answered.
        //
        // It is startTask with the two steps that exist to police an untrusted
        // script replaced: the source comes from `routine` instead of from
        // <projectRoot>/tasks/<name>.luau, and its chunk hash is taken from those
        // bytes. Everything observable is the same -- the front-end claim, the
        // resource closure pass, the run bracket, the seed, the engine session,
        // the VM's two environments -- so a routine cannot quietly enjoy a wider
        // surface than a task, and the resource pass still runs because a
        // first-party routine obeying the uf-literal rule is worth proving rather
        // than assuming.
        //
        // It claims the TASK front-end, so a generation an operator already drives
        // refuses this exactly as it refuses startTask, and a routine and a task
        // may share one generation in sequence.
        [[nodiscard]]
        auto runFrameworkRoutine(
            GenerationId generation,
            FrameworkRoutine const& routine,
            TaskRunConfig config
        ) -> Result<FrameworkRoutineReport>;

        // Binds `generation`'s project to an operator front-end and hands back the
        // session it drives. The operator is a SIBLING consumer of the same private
        // capability surface a task's Luau framework consumes, never a route into
        // Luau; see task/operator-session.hpp for what that means and what it
        // deliberately does not include -- which is now most of it, since the
        // model-dependent verbs retired with the model.
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

        // Binds `generation`'s project to the agent front-end and hands back the
        // session it drives.
        //
        // It is startOperatorSession's sibling in everything except what the
        // session then does: this one owns a Luau VM, booted with the wider
        // private surface and the two extra published modules, and runs one
        // agent-supplied chunk per call. See task/exploration-session.hpp for
        // what that widening is and where it is confined.
        //
        // It claims the ANNOTATION front-end, which is the third value of the
        // exclusion the other two already obey: a generation a task run or an
        // operator already drives refuses this, and this refuses them. That value
        // existed before this verb did -- the m0-demo input agent stamped it on
        // its own answer stream -- and reaching the host is what finally gives it
        // a generation to latch.
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
